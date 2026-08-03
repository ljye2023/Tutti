# TASK T-084 — Round 15（重做）Session 5：StripedDataPath——单 kernel 融合提交

> **先读 `chat/round15/BASELINE.md`**。前置：S3（分组）、S4（容量）已验收。S2 的 `StripedLocalNvmePayload`（`striped://` URI → N 子目标 + stripe 元数据）已就绪可用。**本 session 必须当次完成硬件验证——"编译通过"不算交付（上次 hang 的教训）。**

## 设计（maintainer 已核定，单 launch 硬性要求）

- **kernel**：一次 `cudaLaunchKernel`；entry 带 `dev_idx`；device table（GPU 可见数组）持 N 台设备的 `DeviceTargetHandle*`；thread 按 `dev_idx` 取 handle → `resolve_lba` → acquire_queue → issue（写该设备 doorbell）→ CQ poll——全部复用共享原语。
- **workspace**：entries/status 池按 MetadataArena 模式（**禁止 per-op cudaMalloc 简化交付**）；同一批物理页对每台设备各做一次 DMA 映射（`ioaddrs[dev][page]`）；PRP SINGLE/DUAL/LIST 全路径（**禁止只交付 SINGLE/DUAL**）。
- **完成**：单 kernel = 单 event = caller stream 单 fence；聚合语义与 Runtime 同构（全成才 COMPLETED，任一失败 FAILED，bytes 只计成功 entry）；result 码 0/1/2/3（含 timeout 保守规则：has_timeout 时 PRP 映射不提前解除）。
- **控制面**：构造注入 N 个设备描述；`initialize()` 每设备走与 LocalNvmeDataPath 相同 bring-up（libnvm attach + daemon queue group），任一失败整体回滚；`effective_mdts = min(所有设备)`。
- **共享原语抽离**：`submit_one.cuh` 的 device 区段抽为 `io/nvme_submit_primitives.cuh`（函数逐字节不变），抽离后**同 session** 复跑 799 契约证明零回归。
- **stripe 切分**：host 侧按公式 `shard=(off/unit)%N`、`shard_off=(off/(unit·N))·unit+(off%unit)` 切 entries（跨 unit 边界必切分），unit 强制 4KiB 对齐。

## REQUIRED 1：实现

新建 `tutti/data_paths/striped_local_nvme/`（类声明/实现/fused kernel/launcher/CMake）；共享头抽离；`tutti/CMakeLists.txt` 一行接线（在 `include(CTest)` 之后）。

## REQUIRED 2：当次硬件验证（双盘环境，必须全部通过才算完成）

新建 `tests/striped_local_nvme_contract/`（编号 82 起，不与 S4 冲突时向后排）：

1. **roundtrip**：striped WRITE → READ 逐字节校验（位置相关 pattern），覆盖单 shard 与跨 shard offset；
2. **单 launch**：每 submit launch 计数==1（与 N 无关，N=1/2 均验证）；
3. **跨盘并行**：双盘 striped 大 READ（每 shard ≥64MiB）加速比 >1.3×（单盘对照）；
4. **stripe 分布**：断言数据按 unit round-robin 落两块盘（读 backing 文件物理内容或 resolver 断言）；
5. **生命周期**：in-flight close 拒绝（BUSY）、drain 后正常 close/unregister/shutdown、resolver_test 目录清空；
6. **回归**：799(+S4 新增)/0 + 115/0 + HOST/CUDA 非硬件 ctest 全绿。

## 边界

- 只准新增 striped 包 + 测试目录 + 共享头抽离 + 一行 CMake；LocalNvmeDataPath 行为零变更（抽离逐字节）。
- **禁止**：改 Runtime/public API/SPI 签名；改 quota/容量默认值；per-op cudaMalloc 简化；PRP LIST 留尾。
- 若硬件验证中发现 fused kernel hang（CQ poll 空转），**禁止**用增大 budget 掩盖——必须定位 doorbell/queue 映射根因并写入 result。

## 结果文件

`chat/round15/result5.md`：装配结构、共享头抽离证据、六项硬件验证输出、回归证据、**改动文件清单**。
