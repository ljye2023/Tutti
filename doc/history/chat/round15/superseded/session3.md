# TASK T-078 — Round 15 Session 3：StripedDataPath——单 kernel 融合提交（单 launch 跨多设备）

## 前置条件

- Session 1/2 验收通过；阅读 `tutti/data_paths/local_nvme/io/{submit_one.cuh,submit_one.cu,device_target.h,prp_builder.h}`、legacy `io_engine/src/local_nvme/nvme_batch_xfer_kernel.cu`（`third_pkgs/Tutti`，融合 kernel 先例）与 commit `10602fc` 的 stripe 语义。
- **设计决议（maintainer 定，2026-08-02）：单 kernel 融合提交，禁止 host 侧 fan-out 成 N 次 launch。** legacy 已证明可行性：同一 GPU 可 P2P 映射多台 NVMe 的 BAR/队列，一个 kernel 写多台设备 doorbell。完成侧因此更简单：单 kernel = 单 event = caller stream 单 fence。

## 目标

实现 `StripedDataPath`（单 launch 跨 N 设备）：直接持有 N 台设备的 ctrl 与 queue group，共享 workspace 池对每设备各建一份 DMA 映射；融合 kernel 内按 shard 解析并提交到对应设备队列。对 Session 2 的 striped payload 提供完整 DataPath SPI；调用方经 Runtime 看到普通 TargetHandle。

## 允许修改/创建

- `tutti/data_paths/striped_local_nvme/`（新建 package：fused DataPath + fused kernel）
- `tutti/data_paths/local_nvme/io/`（**允许**把 `resolve_lba`/提交原语抽成共享 device 头供融合 kernel 复用；不得改变 LocalNvmeDataPath 行为，抽离后既有 735 断言全过）
- `tests/`（契约测试）
- `tutti/CMakeLists.txt`（一行接线，BUILD_TESTING 块内 `include(CTest)` 之后）
- `chat/round15/result3.md`

## 禁止范围

- 零 core 改动（public/SPI/Runtime 不动）；不改变 `LocalNvmeDataPath` 行为语义。
- 不执行模块/daemon/mount/mkfs 操作；不提交 Git。

## 必须实现的行为

1. **控制面装配**：构造注入 N 个设备描述（pci/ns/backing）；initialize() 对每设备完成 ctrl attach + queue group 创建（复用 control/ 与 libnvm 原语，与 LocalNvmeDataPath 同一套 bring-up 路径）；任一设备失败整体回滚。
2. **共享 workspace + 每设备 DMA 映射**：一个 entries/status/PRP 池（arena 模式复用 Round 11 设计），同一批物理页对 **每台设备各做一次** `nvm_dma_map_data_device`，得到 `ioaddrs[dev][page]`；op 租约/超时保守规则与 Round 9/11 一致。
3. **融合 kernel**：
   - device table：N 个 queue group 的设备侧指针数组（GPU 可见）；
   - entry = `{dev_idx, shard_off, len, mem_iova_base}`；thread 按 `dev_idx` 取设备队列，复用共享头里的 `resolve_lba` + 提交原语，写该设备 doorbell、bounded poll 该设备 CQ；
   - 保留 Round 9 全部语义：SQE 全零初始化、per-entry completion status（result=1/2/3 分类）、CQ poll 预算、NVMe error dword3 透传。
4. **submit（一次 launch）**：逻辑 (target_offset,length) 按 stripe 公式切分为 entries（跨 unit 边界必切分），一次 H2D + **一次 `cudaLaunchKernel`** + caller stream 一次 event record（fence）。result 中必须给出 launch 计数断言（每 submit == 1，与 N 无关）。
5. **open/close/registration**：open 解析 striped payload 建 N 个 shard 设备 target workspace（handle cache 可复用 Round 11 模式）；`registration_domain()` 返回组合 key；`register_memory` 对每设备各建映射（同一 buffer × N IOVA 表），返回 composite handle；失败回滚。
6. **聚合语义**：op 状态由 per-entry status 聚合（全部成功才 COMPLETED；任一失败 FAILED；bytes 只计成功），与既有 DataPath 语义同构；timeout 继承保守规则（含 fused kernel 内任一设备 CQ 超时 → has_timeout，对应 shard 的 PRP 映射不提前解除）。

## 测试要求

- mock-free 真实硬件（双盘）：striped WRITE→READ 逐字节校验（SINGLE/DUAL/LIST、跨 shard 边界、非 unit 对齐长度）；
- **单 launch 断言**：instrumentation（launch 计数 seam）证明每 submit 恰好 1 次 launch，N=1 与 N=2 一致；
- 跨盘并行：双盘 striped 大 READ 加速比 >1.3×（单盘对照）；
- 单 shard 异常（越界/故障注入）隔离与 partial commit 如实上报；
- 既有 735+115 基线零回归。

## 验收

- `chat/round15/result3.md`：装配结构、融合 kernel 设计、共享池×N 映射论证、单 launch 证据、并行加速比、回归输出。
- 总指挥复核：共享头抽离无 LocalNvmeDataPath 行为漂移；单 launch 与 fence 语义；hardware 复跑。

## 后续依赖

- S4（硬件契约与门禁）依赖本 session。
