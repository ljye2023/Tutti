> **状态：已暂缓（2026-08-01，maintainer 决定）。** Framework Adapter 当前版本不实施，本文件保留作为未来 prompt 基线。

# TASK T-062 — Round 12 Session 3：abort/drain 与 KV block 回收安全

## 前置条件

- Session 2 验收通过；阅读 `Roadmap.md` Phase 5 的「abort/shutdown drains and observes terminal state before framework resource reuse」。

## 目标

闭合 adapter 的生命周期安全：abort 语义明确、shutdown 前 drain 并观察终态、KV block 只在 IO terminal 后才允许回收复用。

## 允许修改/创建

- `tutti/adapters/**`
- `tests/`（adapter 测试）
- `chat/round12/result3.md`

## 禁止范围

- 不修改 core；不引入新 public 概念（若发现 core 缺语义，停下来在 result 记录，不得绕过）。
- 不需要硬件；不提交 Git。

## 必须实现的行为

1. **abort(request_id)**：语义明确化——已 terminal 的不可 abort（明确返回）；in-flight 的标记 abort-requested，**等待终态观察后**才算 abort 完成（Runtime 无取消语义，adapter 不得假装取消成功）。
2. **block 回收安全**：任何 {block, layer} 切片在其全部引用 IO terminal 之前，`can_reuse(block)` 必须为 false；terminal 后才 true。double-free/提前回收在测试中注入并断言被拒绝。
3. **shutdown drain**：adapter `shutdown(timeout)` 等待全部 in-flight terminal（含 abort-requested），超时如实返回；drain 后才允许 `unregister_memory` 与析构。
4. **异常路径**：adapter 析构时若仍有未 drain 的 in-flight，按文档化顺序尽力 drain（bounded），并明确记录泄漏边界（不静默）。

## 测试要求（mock/hardware-free）

- abort 三态：terminal 拒绝 / in-flight 观察终态后完成 / 不存在 id 拒绝；
- 回收安全：in-flight block `can_reuse==false`，terminal 后 true；提前回收注入被拒；
- drain：多 request 混合（COMPLETED/FAILED/aborted）下 shutdown 全观察后返回；
- 析构安全：未 drain 析构不死锁、不 use-after-free（ASAN 或计数 seam 证明）。

## 验收

- `chat/round12/result3.md`：abort/drain 语义文档、测试输出、HOST ctest 全绿。
- 总指挥复核：回收安全与 drain 时序无窗口；abort 语义无"假取消"。

## 后续依赖

- Session 4（真实硬件验证与门禁）依赖本 session。
