# TASK — Round 19 Session 2：striped 多 target batch（P2-2，M×N device table）

> **状态：已关闭（2026-08-04 总指挥核定）。** 主体由 R16 S4 实现（M×N device table、cap 2048、dev_idx=target×N+shard）；契约补强由 R16 S5 完成（striped 契约 95-97：2 target 单 submit/单 launch、8 target 大 batch、容量边界）；S7 验收时 striped 契约 88/0 全绿。无剩余工作。

**日期：** 2026-08-03（预生成，启动前总指挥复核）
**前置依赖：** R16 S4 落地之后（StripedDataPath/fused kernel 文件所有权）；建议在 S1（batch open）之后。
**背景文档：** Roadmap P2-2；`doc/history/chat/round16/session4.md`（S4 的根因修复成果是本任务的地基）。

---

## 背景

当前 StripedDataPath 单 submit 只接受**一个** striped target：每 op 的 GPU device table 按该 target 的 N 个 shard 定容，batch 里出现第二个 striped target 时其 requests 被 per-request 拒绝（显式容量上限，诚实但受限）。KV cache 场景每层 K/V 两个 striped 文件 → 每层 2 submit/2 launch。maintainer 模型：一次 kernel 提交几千上万 IO。

## 设计要点

1. **device table 按 M×N 定容**：op 级 device table 从"单 target 的 N shard"扩为"M 个 target × 各自 N shard"（M 上限按 workspace 容量定，如 8）；entry 携带 (target_slot, shard_idx)。
2. entry→shard 映射、resolve_lba、doorbell/CQ 路由全部按 (target_slot, shard_idx) 索引；**复用 S4 修复后的 kernel 结构**，不重起炉灶。
3. 容量语义：submit 时 M 超上限 → 诚实 per-request 拒绝（partial-commit），不静默拆分。
4. 每 target 独立的 extent 表/overflow 按 slot 管理；arena 内存账更新（M×N 表 × 每 entry 开销），默认配置不改小。

## 验收

1. 契约：单 submit 跨 2+ striped target（K/V 模型）全部 ACCEPTED、单 kernel launch（计数 seam 佐证）、逐字节校验、跨 4 盘分布正确。
2. simulator `--striped4` 模式切换为每层 1 submit/1 launch（K+V 同 batch），实测带宽不低于 S4 的 2-submit 基线（汇报对比）。
3. 回归一次：842/137/66 + 非硬件 15。
4. result：改动清单 + 契约证据 + launch 计数 + 带宽对比。

## 硬约束

- O_DIRECT 政策；partial-commit 保持；防缠结（改动清单）。
- 不改变单 target 场景的既有行为与性能（回归带宽对比）。
- 启动前总指挥复核：S4 对 fused kernel/device table 的最终改造形态（本任务的 M×N 扩展必须建立在 S4 修复后的结构上）；R18 是否已排期（若 R18 已启动则本任务提前，避免 kernel 文件争用）。
