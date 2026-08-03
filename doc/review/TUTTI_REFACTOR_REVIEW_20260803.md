# Tutti 重构评审报告

- 日期：2026-08-03（当日二次核对：Round 15 S1-S6 全部关闭后的最终态已纳入，见 §3.4、§6、§7 更新）
- 评审对象：重构后的 `/data/home/ryeqiu/Tutti/tutti`（工作区快照，git 未提交）
- 对比基线：原版 `third_pkgs/Tutti`（代表文件 `examples/adapters/kv_cache_layerwise_overlap.cu`）、`third_pkgs/Mooncake`（cuda_alike 编译底座）
- 目标依据：`COMMUNITY_MEETING_001.md`、`doc/design/*`、`TUTTI_REFACTOR_TAKEOVER.md`、`TUTTI_TARGET_ARCHITECTURE.md`、`chat/round10-15`
- 评审方法：全量源码走读 + 双 profile 全新构建 + 全量契约测试 + 硬件实测（daemon 运行中，/mnt/nvme1 为 snvme 数据盘）

---

## 1. 结论摘要

**重构方向正确，主干目标已达成，工程质量明显高于旧树；存在 3 个能力缺口和若干打磨项，无架构性错误。**

| 目标 | 判定 | 说明 |
|------|------|------|
| 上层/底层分离 | ✅ 达成 | Runtime 只依赖 SPI 纯抽象；组件显式注入；公共头零 CUDA 依赖（有测试保证） |
| 跨设备编译（CUDA-like） | ✅ 主干达成 | CUDA/HOST 双 profile 均实测构建+测试通过；MACA/MUSA 为显式 `#error` 占位 |
| 跨内核 | ✅ 达成（硬件栈侧） | 双内核树 + compat 单元 + UAPI ABI 握手 fail-closed（Round 10 成果，本次未重测模块） |
| 合理抽象（不过度） | ✅ 达成 | SPI 极小（DataPath 9 方法 / Resolver 1 方法）；memfs 样例证明不动核心即可扩展 |
| 关键优化保留 | ◐ 大部分保留 | batch IO 保留且增强；batch 打开缺失；L1/L2 池简化为单层；striped 已闭环（R15 S5/S6） |
| 接入应用 / 访存范围扩展 | 按要求未做 | 适配层（kv_cache adapter）与范围扩展均未做，符合本次约束 |

**实测证据**（本次评审新跑，非引用历史结果）：

- CUDA profile（`build/review`，含 R15 全部改动重新配置编译）：编译 100% 通过；**全量 ctest 20/20 通过**，其中硬件 5 项：resolver、datapath 契约（820 断言，73.0s）、runtime E2E（137 断言，10.7s）、striped 契约（46 断言，2.5s）、layerwise 示例（67.1s）。
- HOST profile（`build/review-host`，`-DTUTTI_FEATURE_LOCAL_NVME=OFF`）：无 CUDA 工具链路径编译通过，15/15 测试通过（striped 包被 feature 开关正确跳过）。**"同一棵树无 CUDA 可编"这一核心承诺属实。**
- layerwise 示例小规模实测（128 chunk、8 层、32K ctx、/mnt/nvme1）：读 **6.3 GB/s**、写 0.6 GB/s（小批量 host-bound）、三流 overlap 38%。对比：旧示例读 4.33 GB/s（同样几何），旧 io_engine 直连峰值 7.8 GB/s。新栈在公共 API 路径上已超过旧示例，距直连峰值仍有 ~20% 空间。R15 S4 完整 HY3（80 层/512 chunk）验收口径：读 **6.9 GB/s**、每层每方向恰好 1 submit/1 kernel launch（instrumentation 硬断言）、overlap 36%。
- striped 双盘独立复跑（`tutti_striped_local_nvme_contract_test`）：46/0；单盘读 5.09 GB/s vs 双盘 striped 读 **7.23 GB/s**，加速比 **1.42×**（>1.3× 门槛）；N=1/N=2 均恰好 1 submit/1 launch；round-robin 落盘、重启持久化（双独立环境实例逐字节）、partial commit 故障语义全部复证。

---

## 2. 架构符合度分析

### 2.1 分层与依赖方向（符合目标）

```
tutti/include/tutti/            公共 API（tutti_api）+ SPI（tutti_spi）
  storage_runtime.h (1586 行)    唯一公共门面，header-only，厂商中立
  io_types/memory_types/status   值类型，零依赖
  cuda_like.h                    厂商选择器（37 行）
  spi/data_path.h                DataPath SPI（9 个纯虚方法）
  spi/storage_target_resolver.h  Resolver SPI（1 个纯虚方法）
tutti/bindings/                  配对私有 payload 契约（ext4_local_nvme / striped_local_nvme / memfs）
tutti/resolvers/                 local_file / striped_file / memfs(样例)
tutti/data_paths/                local_nvme（生产）/ 测试替身
tutti/device_manager/            硬件栈（vendored libnvm、snvme 模块、daemon）
tutti/cmake/accelerators/        CUDA.cmake / HOST.cmake
```

依赖方向单向：components → SPI/API，Runtime 不认识任何具体后端。`tutti/include/tutti/**` 实测无任何 `cuda_runtime.h` 引用，`tests/header_hygiene` 把这一点固化为契约测试。memfs 样例（resolver + binding + DataPath 全部作为扩展存在）证明第三方可以在不修改核心头的情况下加后端——这正是"上下层分离"的实操证据。

与 `TUTTI_TARGET_ARCHITECTURE.md` 的对照：数据面四组件（StorageRuntime/DataPath/StorageTargetResolver/DeviceManager）中前三者按设计落地；**DeviceManager 公共门面已在 2026-08-03 的孤儿清理中删除**（`tutti/data_paths/local_nvme/control/` 无消费者），生产 DataPath 改为 client-only 方式直连 libnvm ioctl（`nvm_ctrl_attach_client` + `nvm_create_group`/`nvm_add_user_queue`），daemon 只负责设备 bring-up（bind、建块设备），不在 IO 路径上。这是合理的简化，但意味着目标架构文档需要更新（见 §6 问题 D6）。

### 2.2 跨设备编译底座（CUDA-like）

机制与 Mooncake 的 `cuda_alike.h` 同构且略严格：

- `tutti/include/tutti/cuda_like.h`：按 `TUTTI_ACCELERATOR_{CUDA,MACA,MUSA,HOST}` 选择；MACA/MUSA 为 `#error` 显式占位；HOST 映射到 `gpu_vendor/host.h`（手卷 event/stream 状态机，非空壳）。
- `cmake/accelerators/CUDA.cmake`：`COMPILE_AS_CUDA ON` + 强制 `-x cu`（所有 TU 按 CUDA 编译，`.cpp` 也不例外）；`HOST.cmake`：纯 C++17 + `-x c++` + stub `cuda_runtime.h` include 路径。
- 每厂商一个 `Tutti-ACC.cmake` 导出编译选项；`--coverage` 在 CUDA profile 下 `message(FATAL_ERROR)` 阻止 flag 泄漏。
- 已固化测试：`tests/cuda_like`（双 profile 各跑一遍，断言行为一致）。

**与 Mooncake 的三点实质差异：**

1. **编译域选择不同**。Mooncake host 侧全部是 `.cpp` + 原生编译器 + `#define` 映射；Tutti 选择"所有 TU 按 CUDA 编译"。代价：消费方编译时间变长，公共头必须能被 device 编译（公共类型不能用 pImpl，见 `io_types.h` 注释）。收益：上层可以直接在任意 TU 写 kernel，接口里可以出现 `__device__` 类型。对 GPU-direct 存储库这个取舍成立，但应在 README 写明该权衡（目前只在 takeover 文档里）。
2. **device 侧无厂商抽象**。Mooncake 有 `device_ops.cuh` 的 `mc_*` 原子/内存序函数让 kernel 零 `#ifdef`；Tutti 的 IO kernel（`submit_one.cuh`）在 `__CUDACC__` 下直接包含 libnvm device 头（`nvm_cmd.h`/`queue.h` 等，本身即 CUDA-only）。按 TAKEOVER 策略 b)（NVMe DataPath 属厂商相关实现）这可以接受，但意味着"同一 kernel 源跨厂商"对 IO kernel 不成立，且 `data_paths/` 下 5 个文件直接 `#include <cuda_runtime.h>`（`local_nvme_data_path.cpp`、`metadata_arena.cpp`、`prp_page_cache.cpp`、`submit_one.cu`、`device_target.cu`、`nvme_queue_group.cu`），换 profile 时得到的是"头文件找不到"而非统一的不支持错误。建议这些文件改经 `cuda_like.h`，让失败信息一致（工作量小）。
3. **MACA/MUSA 未实证**。占位 `#error` 是正确的诚实做法，但"加新加速器 ~30 行"的承诺没有任何一次真实工具链验证过，HOST 是唯一非 CUDA 证据。

### 2.3 跨内核

用户态与内核版本解耦；内核侧为双树（`snvme-5.4.241-1-tlinux4-0017`、`snvme-5.15.0-public`）+ compat 单元（唯一含 `LINUX_VERSION_CODE` 的翻译单元）+ `peer_memory_ops` 隔离 GPU pinning（Round 10 Session 4 成果）。libnvm 与模块间有 ABI 握手（`TUTTI_SNVME_ABI_VERSION=1`，旧模块 fail-closed ENODEV）。本次评审未重编/加载模块（按约定由用户手动），用户态与现加载模块配合全部测试通过。

---

## 3. 关键优化保留情况（逐项核对）

### 3.1 Batch IO 操作 —— ✅ 保留且语义增强

热路径与旧栈同构，关键环节全部保留：

- 公共 `rt->submit(requests, count, ctx)` 接受跨 target 混合批次，按 DataPath 合并为**一次** `DataPath::submit()`（Round 15 S3，修复了此前按 (data_path,target) 分组导致的 kernel 串行化）。
- DataPath 内 fan-out：每请求按 `min(MDTS, extent 剩余)` 拆成 ≤4096 个 `DeviceSubmitEntry`，一次 H2D + **一次 kernel launch**（`submit_one_kernel`，256 线程/块，一线程一 entry），与旧 `nvme_batch_xfer_kernel` 同形。
- kernel 内：`resolve_lba`（GPU 常驻 extent 表，8 inline + overflow）→ SQE → bounded CQ poll → 写 per-entry `EntryCompletionStatus`（0 成功/1 映射失败/2 超时/3 NVMe 错误），比旧版多了有界轮询与逐 entry 错误归因。
- 完成检测走 `cudaEventRecord` on caller stream，`progress()` 只 `cudaEventQuery`，**提交路径零 host 同步**（唯一例外：`cudaEventRecord` 本身失败时兜底 `cudaStreamSynchronize`，注释详尽）。
- 背压语义：in-flight op 上限 16、`max_batch_entries`、batch 字节上限三重闸，拒绝逐请求落在 `initial_states`，构成 partial-commit 契约（见 §6 问题 P2-3 的文档警告建议）。R15 S4 已将三重闸参数化为构造函数尾随可选参数（`max_in_flight_operations`/`max_batch_requests`/`max_request_bytes_override`，0=原默认公式），默认值 16/256 未动，并新增 submit/launch 计数 seam 使"每层 1 submit/1 launch"成为可硬断言的契约（simulator instrumentation 实测 437 次调用全部单轮）。

### 3.2 Batch 文件打开 —— ❌ 未保留（最主要的能力回退）

旧 `open_files_batch`（一次并行 FIEMAP + 批量缓存提升）在新公共 API 中**没有对应物**，只有逐文件 `rt->open()`。冷启动打开 N 个文件 = N 次串行 `open/fstat/FIEMAP ioctl` +（缓存未命中时）`cudaMalloc` + H2D。新示例 Phase C 就是 128 次循环打开。

部分补偿：`HandleWorkspaceCache` 以 extent 签名（FNV-1a over controller/nsid/size/LBA 表）为 key，**重复打开同一物理布局零 CUDA 调用**——对"每层反复打开同一批文件"的 KV 场景，稳态开销已消除；但首次打开仍是串行冷启动。建议：提供 `open_batch()`（或在 open 路径加并行 FIEMAP + 批量 build），优先级 P1。

### 3.3 L1/L2 元数据内存池 —— ◐ 以简化形态保留

旧两层体系（`TieredHandleCache` 644 行、`PrpPageCache` 426 行、`GpuSlotPool`/`HostSlotPool`、`PrpListPool`、`IoSliceTable`）被替换为三个单层组件，热路径"零分配"目标达成且有测试证明：

| 新组件 | 替代 | 机制 |
|--------|------|------|
| `MetadataArena` | GpuSlotPool/PrpListPool/IoSliceTable | init 预分配 `2×max_in_flight` 槽（event+entry 数组+status 数组+PRP 页池，PRP 整体 DMA-mapped）；submit 租槽 O(1) 零 CUDA 调用（`AllocCounts` 测试缝证明）；超时 op 的槽位保守永久泄漏（防在飞命令 DMA 复用页） |
| `HandleWorkspaceCache` | TieredHandleCache | 单层 GPU LRU + pin/unpin + in_use 保护；内容寻址（extent 签名） |
| `PrpPageCache` | 两层 PrpPageCache | 单层内容寻址（`{mem_token,start_page,pages}`）GPU LRU，整块 DMA 映射同生共死；命中省 H2D |

设计理由写在 `chat/round11/result2.md` 与各头文件。三个保留意见：

1. **理由（1）已过时**：`handle_workspace_cache.h` 注释称"batch is per-op (all entries target the same file)"，Round 15 S3 后 batch 明确跨 target——需要更新注释并重新确认单层容量模型在多文件批次下的结论（示例用 capacity=4096 规避，但容量打满时 `get_or_build` 返回 nullptr → **open 直接失败**，旧两层设计会降级 L2/重建，行为变更需显式告知）。
2. 旧 L2 host tier 省下的 H2D 在新模型中由"miss 时 caller stream 上 async H2D"承担，单次代价低，可接受。
3. `MetadataArena::Config` 注释 `num_slots = 16 // = max_in_flight_operations` 与实际 `2×max_in_flight`（`local_nvme_data_path.cpp:371`）语义不符，易误导容量规划。

### 3.4 GPU file ↔ NVMe file 映射 —— ✅ 单盘增强，条带已闭环（R15 S5/S6）

- **单盘 ✅（比旧版更严格）**：`LocalFileResolver` fail-closed 五项校验（S_ISREG、st_dev  backing 设备同一性、FIEMAP unsafe flags 全拒、extent 无缝覆盖 [0,size)、namespace_base 对齐），fd lease + 明确的部署契约（禁止 truncate/hole-punch/COW）。`DeviceTargetHandle` GPU 常驻 extent 表与旧 `NvmeFileDeviceHandle` 一致（8 inline + overflow）。
- **条带 ✅（Round 15 S5/S6 交付并硬件验收，本次评审独立复跑通过）**：`StripedResolver`（`shard=(off/unit)%N`）+ `striped_local_nvme` binding + **`StripedDataPath`** 全链路闭环：
  - 单次 `cudaLaunchKernel` 融合提交 N 盘：host 侧 stripe 切分（`shard=(off/unit)%N` + extent 边界 clamp）+ PRP 三路径（SINGLE/DUAL/LIST，复用 `prp_builder.h` 的 `classify_prp`/`fill_prp_list_page`，无平行实现）→ 1×H2D entries + 1×H2D device-table + 1×launch + 1×event。fused kernel 极简（139 行），通过 `using` 复用从 `submit_one.cuh` 逐字节抽离的共享原语 `nvme_submit_primitives.cuh`（resolve_lba/队列获取/SQE/CQ 有界轮询），`LocalNvmeDataPath` 行为零变更（820/0+137/0 抽离前后不变）。
  - `StripedArena` 镜像 `MetadataArena` 设计（4 池预分配、submit 路径零 cudaMalloc、超时槽保守泄漏），差异点注释清楚（PRP 池需按设备各映射一次，因 IOVA 域不同）。
  - 每 shard 独立 DMA 映射（同一 GPU buffer × N 张 IOVA 表）；SPI 多 target 契约以显式容量上限诚实履行（设备表定容 N，第二批 target 逐请求 RESOURCE_EXHAUSTED，非静默假设）。
  - E2E 语义完整：公共 API 调用点零 striped 感知（测试 87）、block 编址 KV-pool 模型乱序回读（88）、重启持久化（89）、单 shard 非法请求 partial commit（90）。
  - **实测**（本次评审独立复跑）：双盘读 7.23 GB/s vs 单盘 5.09 GB/s = **1.42×** 加速，N=1/N=2 均恰好 1 launch。
- **遗留差异（不影响闭环，属范围决策）**：旧系统"GPU↔NVMe 亲和性优先选 shard"（`gpu_file_resolve.h`）在新设计中无等价物——当前是纯 round-robin，无 NUMA/亲和性感知；如需亲和性，是 StripedResolver/StripedDataPath 之上的后续增强。另外 `StripedDataPath` 暂无 handle cache/PRP cache（open 总是 N×cudaMalloc+H2D，submit 每 LIST entry 一次 H2D）——对 striped 目标数量少的使用模型是合理简化，已在头文件写明。

### 3.5 与旧代表示例 `kv_cache_layerwise_overlap.cu` 的对标

新 `tutti/examples/layerwise_kv_overlap/` 用**纯公共 StorageRuntime API** 复刻了同一场景（80 层、3 流 `read(L+1)||compute(L)||write(L-1)`、SGEMM 校准、windowed submit 正确处理 partial-commit），且作为 ctest 硬件测试 #19 固化。API 映射：

| 旧（Coordinator 世界） | 新（Runtime 世界） |
|---|---|
| `Coordinator::bootstrap` (SERVICE_CLIENT/IN_PROCESS) | `StorageRuntime::create` + 显式组件注入 |
| `GpuFile` + BlockStorage 创建/日志 | 预分配 ext4 文件 + `rt->open("file://...")`（创建/增长语义刻意移出，由部署契约保证） |
| `coord.allocate_device` + `register_tensor` | 用户自管 `cudaMalloc` + `rt->register_memory` |
| `KvCacheIoAdapter::batched_read/write` | `rt->submit` + `wait` + `release_io`（windowed 重投） |
| `coord.sync_file` | 无（NVMe 写完成即设备持久，无日志需刷） |
| 64 文件 × 64 tensor 单批单 kernel | 128 target × 256 tensor 单批单 kernel（实测通过） |

旧示例走的 `KvCacheIoAdapter`（应用适配层）按本次约束未重做，属预期缺口。

---

## 4. 架构合理性评估

### 值得肯定的设计

1. **SPI 极小化与 payload 配对私有**：binding 头（`kPayloadTypeId` + `view_payload`）让 Resolver/DataPath 各自演化，Runtime 零后端知识——这是全架构最成功的一笔。
2. **错误处理纪律**：partial-commit 逐请求归因；in-flight credit 先授后回滚；close/unregister 遇在飞 op 返回 BUSY；generation 句柄防 stale；超时槽位保守泄漏并注释"为什么不是 bug"。
3. **测试缝（test seam）工程化**：注入 launch 失败/event 失败/query 错误/resolve_lba 失败/NVMe CQ 错误，把"不可达的兜底路径"变成可测契约（799 断言的 datapath 契约测试）。这在开源存储项目里属于上游水平。
4. **并发模型注释化**：registry 锁内/锁外调用的判定、per-DataPath progress gate 的无死锁论证全部写在代码旁，新人可审计。
5. **header-only Runtime**：公共门面单头文件，`#include <tutti/storage_runtime.h>` 即可用，接入成本极低。

### 风险与问题清单（按优先级）

**P1 —— 能力缺口**

- **P1-1 batch 打开缺失**：见 §3.2。建议公共 API 增加 `open_batch`（并行 FIEMAP + 批量 device handle 构建），或在 DataPath `open` 路径做内部批处理。
- ~~P1-2 StripedDataPath 缺席~~ **已解决（R15 S5/S6）**：见 §3.4，fused 单 launch 多盘提交已交付并硬件验收，本次评审独立复跑通过。
- **P1-3 MACA/MUSA 全未实证**：建议至少把 `data_paths/` 下 6 处直接 `#include <cuda_runtime.h>` 改经 `cuda_like.h`，统一不支持错误；并在有真实工具链时跑通一次再加"30 行接入"的宣传。

**P2 —— 接口与文档一致性**

- **P2-1 `LocalNvmeDataPath` 约 30 个 `test_*` 公共方法**污染类接口（占头文件近半）。Runtime 已用更干净的 `testing::StorageRuntimeTestAccess` 模式，DataPath 应对齐（如 `LocalNvmeDataPathTestAccess` 友元 struct），开源后这些测试缝会成为"事实 API"。
- **P2-2 设计注释漂移**：`handle_workspace_cache.h` 的单层化理由（1）已被 R15 S3 推翻；`MetadataArena::Config` 的 `num_slots` 注释与实际 2× 公式不符。
- **P2-3 partial-commit 陷阱警告不够显著**：`rt->submit` 返回 `io.has_value()==true` **不代表全部接受**（被拒请求在 `initial_states`），上层漏遍历即静默丢数据。示例的 `windowed_submit_wait` 是正确示范，但 `storage_runtime.h` 的 `submit` 声明处没有显著警告——这是上层最容易踩的坑，应在公共头以 `WARNING` 级别注释写明。
- **P2-4 顶层文档漂移**：`TUTTI_REFACTOR_ASSESSMENT_20260803.md` 仍描述"16 文件公共 API + Device Manager 三行门面 + 两个示例"，与孤儿清理后的树不符；`TUTTI_REFACTOR_TAKEOVER.md` 的 `tutti/api/` 目录名未采用。建议一次文档对齐，避免新贡献者按过期地图施工。

**P3 —— 打磨项**

- **P3-1** submit fan-out 每请求一个 `std::vector<DeviceSubmitEntry>` 堆分配 + flatten 二次拷贝（`local_nvme_data_path.cpp:950-1266`），大批次下是 O(请求数) 次分配；可用 arena/池化或两遍计数预分配。
- **P3-2** `submit_read_one`/`submit_write_one` 除 opcode 外完全重复（R15 S5 后已迁至共享头 `nvme_submit_primitives.cuh`，被 `submit_one_kernel` 与 `fused_submit_kernel` 共用），应合并为带 opcode 参数的单个函数——两个 kernel 共用后重复的危害加倍。
- **P3-2b（新增，R15 S5 带入）** `StripedDataPath::submit` 的 fan-out 循环中，若某请求在 fan-out 中途被拒（当前唯一中站点是 PRP 越界检查，实测不可达——前置 bounds 检查已排除），**已 push 的部分 entry 会随批次提交**（请求却报告 REJECTED）。当前不可达故无实际危害，但属于脆弱结构：建议在请求级 fan-out 前先记录 `h_entries.size()`，中途拒绝时回滚到该位置。
- **P3-2c（新增，R15 S5 观察）** 两个 DataPath 的 `aggregate_completion_status_` 在全部成功的快路径上也 D2H 拷贝 entries 数组（仅为逐 entry 累计 `bytes_transferred`，全成功时该值恒等于 `total_bytes`）；可在全成功时跳过 entries D2H，仅失败路径拷贝。完成路径每 op 一次、非热路径，故列 P3。
- **P3-3** `QueueAcquireHelper::acquire_queue` 的 `(blockDim.x*32u + threadIdx.x) % num_queues` 中 `blockDim.x*32u` 是常数偏移（对 2 的幂队列数等价于 `threadIdx.x % num_queues`），从旧代码逐字继承，功能正确但令人困惑，建议简化或注释。
- **P3-4** `find_free_{memory,target,io}_slot_` 线性扫描 O(n)，句柄表膨胀后退化；可维护空闲栈。
- **P3-5** 测试源在仓库根 `tests/`、注册逻辑在 `tutti/CMakeLists.txt`（以 `../tests/xxx` 引用），根 `tests/CMakeLists.txt` 是"legacy removed"空壳——布局别扭，建议要么测试移入 `tutti/tests/` 要么注册上移。
- **P3-6** 示例头注释宣称"All data-plane operations use ONLY the public StorageRuntime API"，但实际 forward-declare 了 DataPath 内部的 `launch_fill_pattern`（DMA-visible 填充，规避 cudaMemset 的 L2 一致性问题——本身是真实的硬件教训，保留合理），注释表述应放宽为"IO 路径只用公共 API"。

---

## 5. 代码质量与命名（开源视角）

**总体：达到"敢开源"的水平线，注释文化是其最大亮点。**

命名抽查结论（覆盖公共 API、SPI、DataPath、metadata、kernel）：

- 类型名望文知义且风格统一：`MetadataArena`/`HandleWorkspaceCache`/`PrpPageCache`/`DeviceTargetHandle`/`DeviceSubmitEntry`/`EntryCompletionStatus`；枚举分层一致（`RuntimeState`/`IoState`/`OpState`/`RequestState` 公共/SPI 镜像对应，转换函数显式命名 `to_public_op_state`）。
- 常量集中且有单位后缀：`kMaxSlots`、`kScheme`、`kDeviceTargetInlineExtents`、`stripe_unit`、`max_in_flight_operations`、`namespace_base_bytes`。
- 布尔/标志不滥用魔法数：`inject_flag` 的 bit0/bit1 有注释；`CompletionMode{EVENT, STREAM_QUERY}` 枚举化。
- 注释解释"为什么"且带历史教训（pageable host H2D 的阻塞语义、event record 失败后的唯一安全恢复、timeout 槽位为何保守泄漏、DMA 映射与 backing page 同生共死），这对开源项目的新贡献者极其友好。
- 一处易混点：`IoRequest`（公共）与 `DataPathRequest`（SPI）都含 `intent/memory/target` 但 `memory/target` 类型不同（句柄 vs SPI 身份），首次阅读需适应；属 public/SPI 隔离的固有代价，建议在 `io_types.h` 加一段对照注释。

无碍开源但建议发布前处理：P2-1（test_* 接口）、P2-4（文档漂移）、README/CONTRIBUTING 与当前树的同步（README 仍含 `tuttictl mt op` 等已删工具的描述）。

---

## 6. 本次验证记录

| 项 | 命令/配置 | 结果 |
|----|-----------|------|
| CUDA profile 构建 | `cmake -B build/review -S tutti -DTUTTI_ACCELERATOR=CUDA -DTUTTI_BUILD_HARDWARE_TESTS=ON -DBUILD_TESTING=ON -DCMAKE_TOOLCHAIN_FILE=...vcpkg.cmake`（R15 全部改动落地后重新配置） | 100% 通过，零新增告警 |
| 全量 ctest（R15 终态） | `ctest -j4` | **20/20 通过**（73.0s）：非硬件 15 项 + 硬件 5 项（resolver、datapath 820 断言、runtime E2E 137 断言、striped 46 断言、layerwise） |
| HOST profile | `-DTUTTI_ACCELERATOR=HOST -DTUTTI_FEATURE_LOCAL_NVME=OFF` | 构建 + 15/15 通过，全程无 CUDA；striped 包被 feature 开关正确跳过 |
| layerwise 实测 | `tutti_layerwise_kv_overlap --data-dir /mnt/nvme1/GPU0 --layers 8 --ctx-tokens 32768` | 读 6.3 GB/s，overlap 38%，数据校验通过 |
| striped 实测 | `tutti_striped_local_nvme_contract_test`（双盘 /mnt/nvme1+/mnt/nvme2） | 46/0；双盘读 7.23 GB/s vs 单盘 5.09 GB/s（1.42×）；N=1/N=2 均 1 submit/1 launch |

（首轮核对时 R15 S5/S6 尚未落地，当时结果为 19/19；上表为 R15 全部关闭后的二次核对结果。）

环境前提：daemon 运行中（`build/bin/tutti_daemon --config sys_config.yaml`）、`/dev/snvme0n1` 挂载 `/mnt/nvme1`。`/mnt/nvme4` 为 md0/xfs，非 snvme 设备，不能作为 direct-IO 数据盘（resolver 的 backing 设备校验会 fail-closed 拒绝），仅适合作构建/ scratch 空间——本次未使用。

## 7. 改进路线建议（按投入产出排序）

1. **补 partial-commit 公共警告**（P2-3，半天，防上层丢数据）。
2. **统一 test seam 为 TestAccess 模式 + 更新漂移注释/文档**（P2-1/P2-2/P2-4，1-2 天，开源前必做）。
3. **`data_paths` 直引 `cuda_runtime.h` 改经 `cuda_like.h`**（P1-3 前半，半天，为跨厂商铺路）。
4. **`open_batch`**（P1-1，含契约测试，2-3 天，消除唯一明确的性能回退）。
5. P3 打磨项随周边改动顺手做（P3-1/P3-2/P3-2b/P3-3 优先）。

（原第 5 项"StripedDataPath 重做"已由 R15 S5/S6 完成并验收，从待办移除。）

---

*评审人：CodeBuddy（Kimi-K3）；评审基线：工作区快照 2026-08-03，构建目录 `build/review`（CUDA）与 `build/review-host`（HOST）保留可复现。*
