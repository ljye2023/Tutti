# TASK T-079 — Round 15 Session 4：striped 多设备硬件契约与门禁

## 前置条件

- Session 1-3 全部验收通过；双盘环境就绪（snvme0+snvme1，`/mnt/nvme1`+`/mnt/nvme2` 已挂载）。

## 目标

以端到端硬件契约证明 striped 抽象：`striped://` URI → 一个 TargetHandle → 跨双盘 WRITE/READ 回读正确；并形成可复跑门禁。

## 允许修改/创建

- `tests/striped_local_nvme_contract/**`（新建，hardware label）
- `scripts/round15_gate.sh`（新建）
- `chat/round15/result4.md`

## 禁止范围

- 零 core 改动；不执行模块/daemon/mount/mkfs 操作；不提交 Git。

## 必须实现的行为（契约场景）

1. **E2E 主路径**：经公开 StorageRuntime（StripedResolver + StripedDataPath + 2×LocalNvmeDataPath）open 一个 striped 文件 → GPU buffer 一次注册 → WRITE 全量逻辑空间 → 清 buffer → READ 回读 → 逐字节校验（覆盖 SINGLE/DUAL/LIST 尺寸、跨 shard 边界的 offset/length 组合）。
2. **stripe 分布验证**：读取各 shard backing file 的物理内容（或经 resolver 断言），证明数据确实按 unit round-robin 落到两块盘（不是全写一块）。
2b. **单 launch 与跨盘并行验收**（maintainer 硬性要求）：instrumentation 证明经 Runtime 的 striped submit 只产生 **1 次 kernel launch**（与 shard 数无关）；双盘 striped 大 READ 加速比 >1.3×（单盘对照），证明 kernel 内跨盘并行。
3. **并发**：双 stream 对同一 striped target 交错 IO；多 striped target 并行。
4. **异常**：单 shard 越界请求拒绝不影响正常请求（partial commit）；stripe unit 非对齐 URI 拒绝。
5. **生命周期**：in-flight 时 close 拒绝（BUSY）；drain 后正常 close/unregister/shutdown。
5b. **重启持久化（KV 关键场景）**：striped WRITE 后完整 teardown（close/unregister/shutdown），随后以**全新 Runtime + 全新 Resolver/DataPath 实例**（模拟进程重启）重新 open 同一 URI，READ 回读逐字节校验——证明 backing 文件 extent 稳定、数据跨"重启"可恢复。覆盖单 shard 与跨 shard 两类 offset。
6. **门禁脚本**：HOST/CUDA build + 非硬件 ctest + striped 硬件契约 + 既有 735/115 基线，汇总输出。

## 测试要求

- 契约断言全过；双盘临时目录运行后清空；dmesg 无异常。
- 环境缺第二盘时硬件契约整体 SKIP（明确打印），不得 fail。

## 验收

- `chat/round15/result4.md`：契约输出、stripe 分布证据、门禁输出。
- 总指挥复跑门禁与两基线契约；全过后 Round 15 关闭，多设备能力正式纳入基线。
