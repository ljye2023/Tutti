# TASK T-027

你是一名资深 CUDA/NVMe C++ 工程师。你的任务是**分层抽象 + 代码搬运**：在 Session 1 的 queue group + device target 基础上，从 main 典型路径搬出一个**最小单块真实 IO slice**，把 `LocalNvmeDataPath::submit/progress/query/release` 从显式失败变成真实 4KiB NVMe write/read/verify。

# 任务定位

- 基线：`main@4862157d50c8a7004cdeb166dda630ab1ef4561a`。
- 必读：`MAIN_IO_PATH.md`。
- 搬运源：
  - `main:nvme_storage/include/nvme_storage_device.cuh`（`resolve_lba`, `submit_read_one`, `submit_write_one`）
  - `main:nvme_storage/include/queue_acquire_helper.cuh`（queue acquire、SQE、doorbell、CQ poll）
  - `main:io_engine/src/local_nvme/nvme_batch_xfer_kernel.cu`（one-thread-per-entry launch 形状）
  - `main:io_engine/src/local_nvme/local_nvme_io_engine.cpp`（H2D → launch → stream/event completion）
- 当前落点：Session 1 完成后的 `tutti/data_paths/local_nvme/`。

**只取 main 路径的最小 4KiB slice，不发明新 submit 模型。** 本 session 只支持：DEVICE_EXECUTION、DEVICE memory、每个 request 恰好一个 4KiB block、batch count=1。其他输入明确 `UNSUPPORTED/INVALID_ARGUMENT`，留给 Session 3 搬 batch/MDTS fan-out。

不要评审/修复 main 的 `QueueAcquireHelper`、SQE/CQ 或 kernel 策略；原样搬运。

# 前置条件

必须在 Session 1 PASS 后执行；若 queue group/device target 文件不存在，报告 BLOCKED，不从头重写。

环境由负责人保持：daemon、`/dev/ssnvme0`、`/mnt/nvme1`、GPU 均在。你不启停、不挂载。

# 1. Private device submit 搬运

在 `tutti/data_paths/local_nvme/io/` 增加建议文件：

```text
submit_one.cuh       # private device helpers，搬 resolve_lba + submit_*_one
submit_one.cu        # one-request kernel + host launcher
```

## Device request

定义 private、GPU-visible 的最小 entry（不进入 SPI/public）：

```text
device target pointer
prp1（一个 4KiB page 的 controller DMA address）
target_offset（byte）
length = 4096
direction
```

kernel 一 thread 处理一 entry，按 main 调 `submit_read_one` / `submit_write_one`；helper 内部 `resolve_lba → acquire_queue → issue_nvme_cmd → CQ poll`，kernel 返回表示真正 IO poll 已结束。

**不要** include/复用旧 `nvme_storage/**`；把需要的 private struct/helper 代码搬入新 package，include 仅 libnvm/CUDA/private local files。

# 2. MemReg 机械扩展

当前 `MemReg` 只存 `nvm_dma_t*`。为 request bounds/PRP1 计算，存入注册时的：

```text
base
size_bytes
kind
device_id
dma
identity generation
```

给 memory_offset=aligned offset：

```text
page_index = memory_offset / controller_page_size (本环境 4096)
prp1 = dma->ioaddrs[page_index]
```

先校验（无溢出）：

```text
memory_offset <= size
length <= size - memory_offset
memory_offset % 4096 == 0
length == 4096
```

本 session 只接受 DEVICE memory；HOST memory registration 保持可用，但 submit HOST memory 返回 `UNSUPPORTED`（main 典型 GPU kernel 路径用 device pointer）。

# 3. Target/request 校验

submit 前完整校验：

- initialized + queue group + target device handle ready；
- `requests != nullptr`, `count == 1`；count 0 返回零发出，不 crash；
- `ctx.execution_domain == DEVICE_EXECUTION`，stream 非 null，device_id 与 queue group cuda device 一致；
- DataPathTarget/DataPathMemory token+generation 有效；
- `target_offset <= file_size` 且 `length <= file_size-target_offset`（无溢出）；
- target_offset/length/memory_offset 都 4096 对齐；length 恰好 4096；
- request 不跨 extent（device helper source 同样要求）。

失败时 `SubmitOutcome`：`op=nullopt`、`initial_states.size()==count`、对应 REJECTED + 结构化错误。未发 IO。

# 4. Per-op owner 与 SPI lifecycle

main 的 blocking bool 没有 operation owner；新 SPI 必须机械适配成 `DataPathOp`：

```text
OpState + Status + bytes
private device entry allocation
cudaEvent_t completion_event
stream（borrowed）
强引用/身份记录：target token/gen + memory token/gen
```

## submit

1. 所有 validation 完成；
2. 为**本 op**独立 cudaMalloc device entry（禁止共享 scratch）；
3. H2D entry 到 caller stream；
4. launch kernel 到同 stream；
5. record event 到同 stream（kernel 内部 poll CQ，event 只有在真实 IO 后才 signal）；
6. mint `DataPathOp`，存 op table；
7. 返回 ACCEPTED + op。

## progress

按 `ProgressBudget` 有界处理：

- `max_work_units=0` → 消耗 0，立即返回；
- 每 query 一个 event 算 1 work unit，不能超过 cap；
- `timeout_ns` 为本次调用 wall-clock cap；
- `cudaEventQuery == success` → op COMPLETED、bytes=4096；not-ready 保持 IN_FLIGHT；其他 CUDA error → FAILED；
- 不无限 busy-poll。

## query

只读快照，不销毁 op；返回当前 IN_FLIGHT/COMPLETED/FAILED。

## release

- IN_FLIGHT → BUSY；
- terminal → cudaFree entry、destroy event、erase op；
- 重复 release → NOT_FOUND。

## shutdown/close/unregister

- op 未 terminal 时，close target/unregister memory 必须 BUSY（op 持有它们）；
- shutdown 按 timeout 尝试 drain；超时返回 TIMEOUT，不可释放仍被 GPU 使用的资源；
- 终态/release 后才允许 target/memory 释放。

这是把 main 的 stream completion 生命周期适配到已冻结 SPI 所必需，不是重写数据面。

# 5. Capabilities

本 session 完成后如实更新：

```text
supports_device_execution = true
supports_device_memory = true
supports_direct = true
supports_read/write = true
target/memory/length alignment = 4096
max_single_io_bytes = 4096
max_batch_requests = 1
max_batch_bytes = 4096
progress_model = HOST_POLL
device_completion_fence_on_caller_stream = true
```

`device_execution_autonomous=true`：main kernel 自己 poll CQ，不依赖 host progress 才推进；可以置 true并说明依据。`supports_host_execution` 对 IO 应如实 false（控制代码在 host 不等于支持 HOST_EXECUTION IO）。multi-stream 先 false，后续验证再开。

# 6. 真实 E2E 测试

扩展现有 local_nvme contract test，保留所有既有断言，新增：

1. 在 `/mnt/nvme1/GPU0/resolver_test/` 创建一个 4KiB（或更大但测试 offset 0）的普通文件：fallocate + 全量写入 + fsync；
2. 用 `LocalFileResolver` 产 `ResolvedTarget`，DataPath open；
3. cudaMalloc write/read buffer（64KiB 对齐策略按 main memory source；可 overallocate+align），register 两块；
4. write buffer 填固定 pattern；submit WRITE 4KiB；循环调用**有界 progress**直到 query terminal；release；
5. READ 4KiB 到 read buffer；同样 progress/query/release；
6. D2H 比较 pattern，必须完全一致；
7. 删除测试文件；
8. 输出真实 target LBA、PRP1/ioaddr、op token、progress work units、bytes transferred。

负向测试：未对齐、越界、count>1、HOST_EXECUTION/null stream、HOST memory submit、release in-flight、close/unregister in-flight、无效身份。

**只写测试文件，不碰其他数据。**

# 7. 你只能修改/创建

- Session 1 创建的 `tutti/data_paths/local_nvme/**`
- `tests/local_nvme_datapath_contract/CMakeLists.txt`
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`（若需改为 `.cu`，允许 rename，但结果说明）
- `chat/round8/result2.md`

构建只能写 `build/round8-session2*`。

禁止修改旧 source、SPI/public/binding/resolver、libnvm 源码、其他 tests/CMake。

# 8. 安全

禁止模块、bind/unbind、mkfs/mount/umount、启停 daemon、打开块设备节点。允许 client API/CUDA/DMA map/queue 和**仅对测试文件映射出的 LBA**做 write/read/verify。禁止固定 LBA、raw-device IO、触碰 `/mnt/nvme4`。

# 9. 验收

```bash
rm -rf build/round8-session2
cmake -S tests/local_nvme_datapath_contract -B build/round8-session2 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round8-session2 -j8
ctest --test-dir build/round8-session2 --output-on-failure
```

要求：

1. 真实 file-backed 4KiB write/read/verify PASS；
2. `submit/progress/query/release` 全部真实，不再 UNSUPPORTED；
3. per-op 独立资源，无共享 scratch；
4. event 只在 kernel 内 CQ poll 完成后 signal；
5. 负向 validation/ownership 测试全过；
6. capabilities 与实现一致；
7. Round 7 + Session 1 既有测试无回退；
8. 环境/RAID/daemon/module 状态不变；
9. 文件边界与 hygiene 正常。

# 结果落盘

写 `chat/round8/result2.md`：source→port 对照、机械适配、op owner、validation、capabilities、真实 E2E 输出（pattern/LBA/PRP/op/progress）、负向测试、环境边界、PASS/BLOCKED。

不要寒暄、不要提交 commit、不要写总指挥验收。
