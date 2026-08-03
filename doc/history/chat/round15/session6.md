# TASK T-085 — Round 15（重做）Session 6：striped Runtime E2E + 重启持久化 + Round 门禁

> **先读 `chat/round15/BASELINE.md`**。前置：S5（StripedDataPath）已硬件验收。

## REQUIRED 1：Runtime 级 striped E2E（经 public API，双盘硬件）

在既有 striped 契约测试目录中增补（编号续排）：

1. **全 public 路径**：`rt.open("striped://name?devs=/mnt/nvme1,/mnt/nvme2&unit=65536")` → 普通 `TargetHandle` → register/submit/wait/release/close 全流程（调用方零 striped 感知断言：代码中不出现 striped 类型）；
2. **block 编址**：`block_id × block_size` 逻辑偏移写读回验（KV pool 使用模型）；
3. **重启持久化**：striped WRITE 后完整 teardown（close/unregister/shutdown），**全新 Runtime + 全新 Resolver/DataPath 实例**重开同一 URI，READ 逐字节校验（单 shard 与跨 shard 两类 offset）——KV 持久化关键场景；
4. **故障语义**：某 shard 非法请求 per-request 拒绝，其余 shard 正常完成（partial commit）。

## REQUIRED 2：Round 15 门禁

- 全量：datapath 契约（含多设备 78-81 与 S4/S5 新增）、runtime E2E 115、striped 契约全套、HOST/CUDA 非硬件å ctest——全绿；
- 临时文件：双盘 `resolver_test/`、`striped_test/`、`kvlw_*` 全空；
- `git diff --check` clean；
- 更新 `doc/extending_tutti.md`：striped 包作为第二个社区扩展示例（一段 + 链接），一行接线位置警告保持。

## 结果文件

`chat/round15/result6.md`：E2E 输出、门禁证据、**改动文件清单**。Round 15 由总指挥据此做关闭验收。
