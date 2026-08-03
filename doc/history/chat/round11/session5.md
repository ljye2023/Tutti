> **状态：已暂缓（2026-08-01，maintainer 决定）。** 当前版本不实施；Phase 4 正式门禁待 Session 4 及后续工作就绪后再启动。本文件保留作为未来 prompt 基线。注意：S1-S3 完成后 Phase 4 处于「核心语义已落地、性能测量与门禁未做」的未关闭状态，`Roadmap.md` 不得标记 Phase 4 完成。

# TASK T-054 — Round 11 Session 5：Phase 4 验收门禁

## 前置条件

- Session 1-4 全部通过总指挥验收；阅读 `Roadmap.md` Phase 4 gate 原文。

## 目标

以可重复的一键门禁关闭 Phase 4：并发正确性（arena 后无 scratch/event 覆盖）、有界 pool 背压、shutdown drain、真实异步顺序、全量回归、测量数据归档。

## 允许修改/创建

- `scripts/phase4_gate.sh`（新建）
- `tests/**`（仅压力/并发场景补充）
- `Roadmap.md`（Phase 4 标记与 Known Bugs 同步）
- `chat/round11/result5.md`

## 禁止范围

- 不改 public/SPI；不做 Phase 5（framework adapter）内容。
- 不执行模块/daemon/mount 操作（环境由 operator 预置）；不提交 Git。

## 必须实现的行为

门禁脚本按序执行并输出汇总表：

1. HOST profile clean configure+build+ctest 全绿；
2. CUDA profile build + 非硬件 ctest 全绿；
3. 两硬件契约（`local_nvme_datapath` + `storage_runtime_local_nvme`）：断言数 ≥ 当前基线，全过；
4. 并发压力：≥4 stream × ≥2 host thread 混合 submit/query/release 持续 ≥30s，op 终态全部正确、无 scratch/event 覆盖（arena slot 审计：测试前后槽位计数一致，无泄漏）；
5. 背压证明：arena 容量打满后持续 submit，拒绝率与 drain 后恢复均符合预期（不得死锁/挂起， watchdog 计时）；
6. shutdown drain：在飞 IO 下 `shutdown(timeout)` 正常 drain，无残留槽位/映射；
7. 测量归档：Session 4 的关键指标（IOPS/BW/p99/overlap）摘要写入 `Roadmap.md` Phase 4 gate 记录。

## 测试要求

- 门禁脚本全绿；连续两次运行结果一致（无 flaky）。
- 运行后 `/mnt/nvme1/GPU0/resolver_test/` 为空；dmesg 无新增异常。

## 验收

- `chat/round11/result5.md`：门禁完整输出、并发/背压/drain 场景参数与结果、Roadmap diff。
- 总指挥重跑门禁脚本与压力场景，抽查 arena 审计逻辑；全过后宣布 Phase 4 关闭、Phase 5（Framework Adapter）可启动。
