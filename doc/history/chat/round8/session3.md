# TASK T-028

你是一名资深 CUDA/NVMe C++ 工程师。你的任务是**分层抽象 + 代码搬运**：在 Session 2 的真实 4KiB IO 闭环上，搬运 main 典型 `kv_cache_layerwise_overlap` 路径的 **batch + MDTS/extent fan-out + PRP SINGLE/DUAL/LIST + per-op workspace**，使 `LocalNvmeDataPath` 能执行真实的 K/V-like 批量 file IO。

# 任务定位

- 基线：`main@4862157d50c8a7004cdeb166dda630ab1ef4561a`。
- 必读：
  - `MAIN_IO_PATH.md`
  - `MAIN_MEMORY_PRP_PATH.md`（内存 allocation/DMA/PRP 的权威导航与废旧代码排除清单）
- 搬运源：
  - `main:adapters/kv_cache/src/kv_cache_io_adapter.cpp:28-80,102-179`（chunk/offset 语义，仅用于测试几何；DataPath 不认识 K/V）
  - `main:memory/src/host_device_memory_subsystem.cu`：`IoSliceBuildPlan`、`fill_address_descriptors`、owned PRP-list fallback
  - `main:memory/include/memory_subsystem.h`：`AddressDescriptor`/IoSlice 值类型
  - `main:io_engine/src/local_nvme/host_batch_builder.cpp`（每 sub-IO 一 entry）
  - `main:io_engine/src/local_nvme/nvme_batch_xfer_kernel.cu`（one-thread-per-entry）
  - Session 2 已搬入的 private submit helpers/op lifecycle。

**你在搬 main 已跑通的 batch 路径，不设计新的 scheduler/cache。** 本任务先搬 main 的 owned PRP-list fallback，不搬 Tiered PRP cache（那是性能优化，后续单独搬）。

# 前置条件

必须在 Session 1/2 完整 PASS、且 `chat/round8/result2.md` 的总指挥验收不再含 `REQUIRED FOLLOW-UP` 后执行；若真实 4KiB write/read/verify 或 op 生命周期仍有待修项，报告 `BLOCKED`。

环境由负责人保持 daemon + mount；你不启停、不挂载。

# 1. Request fan-out（机械适配 main host_batch_builder）

把 `DataPathRequest[]` lowering 成 private `NvmeBatchEntry[]`：

每个 request 先独立校验：

- target/memory identity generation；
- memory/target bounds（无溢出）；
- 4096 对齐；
- DEVICE_EXECUTION + stream；
- direction/read/write；
- capability/batch limits。

然后按以下边界 fan-out：

```text
remaining request bytes
controller MDTS/max_data_size
当前 target extent 剩余 bytes
memory DMA page coverage / PRP descriptor capacity
```

每个 sub-IO：

```text
device target pointer
target byte offset
memory byte offset
length
PRP descriptor
request index
```

**与 main 的差异仅是新接口必需的：** main 通过 `MemoryRegion::IoSliceTable` 预计算；当前 SPI 没有 granularity 参数，因此在 submit 时从 `MemReg.dma->ioaddrs[]` 构造 per-op descriptor。不要为此修改 SPI。

# 2. PRP descriptor 搬运

搬 `AddressDescriptor` 与 descriptor fill 逻辑到 private local package（建议 `io/prp_builder.h/.cu`）。支持：

```text
SINGLE: 1 page  → prp1, prp2=0
DUAL:   2 pages → prp1, prp2=second ioaddr
LIST:   >2 pages→ prp1, prp2=DMA address of PRP-list page
```

## LIST owned fallback

按 main `build_io_slice_table_locked` / `dma_alloc_device_data` 的 owned fallback 搬：

1. `user_bytes = LIST sub-IO 数 * NVMe page_size`，mapping size 向 **64 KiB** 取整；
2. `cudaMalloc(raw, aligned_bytes + 64 KiB)`，从 raw 内取得 **64 KiB 对齐**的 PRP-list view；raw 是 owner，aligned view 不能单独 `cudaFree`；
3. `nvm_dma_map_data_device` 注册 aligned view + aligned size，取得 `nvm_dma_t::ioaddrs[]`；
4. 每个 LIST sub-IO 独占一个 NVMe page-sized list page，host 填 data `ioaddrs[start_page+1..]`，剩余项为 0；
5. H2D 到 caller stream；
6. `descriptor.prp2 = prp_list_dma->ioaddrs[list_page_index]`，写 **DMA IOVA**，不是 CUDA virtual pointer；
7. op terminal/release 后先 `nvm_dma_unmap`，再 `cudaFree(raw)`。

禁止把 CUDA pointer 直接写进 PRP2（旧 P0-10）；禁止省略 source 的 64 KiB vaddr alignment 与 mapping-size round-up；这是 source owned fallback 的机械要求。

每个 op 独立拥有所有 entry/descriptor/PRP-list raw+aligned view/DMA/event，禁止共享 scratch。

**来源排除：** 不得参考 `tutti/backends/nvme/**` 或 `tutti/io_engine/**` 的同名/相似实现；其中存在 placeholder submit 与未 poll CQ 的 kernel。canonical source 仅以本节列出的 `main:memory/**` + `main:io_engine/src/local_nvme/**` 为准。

# 3. Batch submit 与 partial commit

扩展 Session 2 的 submit：

- 支持 `count > 1`，支持 mixed target/memory/direction；每 entry 带自己的 device target，不使用 `requests[0].target` 代表全批。
- 每 request 独立 validation/lowering；合法 request ACCEPTED，非法 REJECTED。
- 至少一个 ACCEPTED → launch accepted sub-IO，返回 `op != nullopt`；整体 status 若有拒绝则非 OK（partial commit），initial_states 与输入一一对应。
- 全拒绝/count=0 → `op == nullopt`，零发出。
- 一个 request fan-out 为多个 sub-IO，但其 initial state 仍一项。

kernel 按 main one-thread-per-entry：每 entry 调 Session 2 已搬的 submit helper，内部 CQ poll。方向必须来自 entry，允许一个 batch 混合 READ/WRITE（若 main launcher原本全批 bool方向，不足以表达新 SPI，则把 bool机械下沉为 entry 字段；这是接口必需适配）。

# 4. Op/progress/completion

沿用 Session 2 lifecycle，扩展 bytes 与资源：

- bytes_transferred 只在所有 accepted sub-IO kernel 完成后等于 accepted request bytes 总和；
- event 在同 stream、kernel 后 record；
- progress 有界 query event；
- release terminal 后释放 entries/descriptors/PRP list DMA + memory；
- close/unregister 对 in-flight 引用返回 BUSY；
- shutdown timeout 不释放 in-flight workspace。

本任务**不设计新的 CQ error channel**；main helper没有 host error channel，原样保留并在结果中一句话记录限制，不展开评审。

# 5. Capabilities

按真实实现更新：

- supports read/write/direct/device execution/device memory = true；
- alignment 4096；
- max_single_io_bytes：DataPath 可 fan-out 的最大**request** bytes（按你的明确上限填写，不虚报）；
- max_batch_requests/max_batch_bytes/max_in_flight 与实际表/配置一致；
- supports_scatter_gather：若 LIST 已实现，可按 SPI 语义说明是否置 true（PRP list 是物理 page scatter，不一定等同 public SG；说明理由）；
- supports_multi_stream 只有完成双 stream 并发测试才可 true；
- device completion/autonomous 与 main kernel CQ poll + stream event 一致。

# 6. 真实 batch E2E（模拟 main K/V 几何，不引入 Adapter 类型）

在测试文件系统创建**至少两个文件**，每个大小足够容纳多层 K/V（例如 `layers=4`, `tensor_size=1MiB`, file_size=`2*layers*tensor_size`）。

测试：

1. resolver → 两个 target → DataPath open；
2. 多块 GPU K/V buffer 注册；
3. 用 main adapter 同样公式生成 offset：
   - K `layer*2*tensor_size`
   - V `base+tensor_size`
4. batch WRITE 多 request（混合两个 target），progress/query/release；
5. batch READ 到独立 buffer，逐 byte/固定 pattern verify；
6. 覆盖 SINGLE（4KiB）、DUAL（8KiB）、LIST（>8KiB，建议 1MiB）；
7. 覆盖 MDTS fan-out（1MiB 应按 controller max 分多个 sub-IO；输出 sub-IO 数与长度）；
8. 覆盖跨 extent request：用 R7 的 A+B+扩展 A 技巧造 2 extent 文件，并提交覆盖 extent 边界的 block-aligned request，host lowering 必须拆开且 verify；
9. partial commit：同批一个合法、一个越界；op 存在、状态非 OK、合法 request 真正完成并可验证；
10. mixed target：两个文件写不同 pattern，读回不串路由；
11. 两个 CUDA stream 各 submit 一个 op（per-op workspace 不互相覆盖），都 verify 后才允许 `supports_multi_stream=true`。

输出真实：文件 extent、target offset→LBA、memory ioaddr、descriptor kind、PRP2 IOVA、fan-out entry 数、op token/bytes/progress、pattern verify。

测试只写自己创建的普通文件；删除清理。

# 7. 你只能修改/创建

- `tutti/data_paths/local_nvme/**`（Session 1/2 私有 package）
- `tests/local_nvme_datapath_contract/CMakeLists.txt`
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp/.cu`
- `chat/round8/result3.md`

构建只能写 `build/round8-session3*`。

禁止修改 main/旧 source、SPI/public、binding/resolver、libnvm 源码、其他 tests/CMake。

# 8. 安全

禁止模块/bind/mkfs/mount/daemon 操作；禁止 raw/fixed-LBA IO；只允许对 resolver 从测试文件 FIEMAP 得到的 LBA 做 write/read/verify；禁止触碰 `/mnt/nvme4`。

# 9. 验收

```bash
rm -rf build/round8-session3
cmake -S tests/local_nvme_datapath_contract -B build/round8-session3 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round8-session3 -j8
ctest --test-dir build/round8-session3 --output-on-failure
```

必须：

1. main 的 fan-out/descriptor/batch/kernel 流程有逐符号 source→port 对照；
2. SINGLE/DUAL/LIST 真实 IO 均 PASS；LIST 的 prp2 是 DMA IOVA；
3. multi-target/KV-like batch write/read/verify PASS；
4. MDTS/extent fan-out PASS；
5. partial commit SPI 不变量 PASS；
6. per-op workspace + 双 stream 不覆盖；
7. op/query/progress/release/ownership PASS；
8. 所有 R7/R8 既有测试无回退；
9. 环境与生产 RAID 不变；
10. 文件边界/hygiene 正常。

# 结果落盘

写 `chat/round8/result3.md`：main source→port 对照、fan-out/PRP owner、partial commit、capabilities、全部真实 E2E 输出/指标、环境与边界、PASS/BLOCKED。

不要寒暄、不要提交 commit、不要写总指挥验收。
