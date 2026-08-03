> **状态：已暂缓（2026-08-01，maintainer 决定）。** Framework Adapter 当前版本不实施，本文件保留作为未来 prompt 基线。

# TASK T-063 — Round 12 Session 4：真实硬件 KV 验证与 Phase 5 门禁

## 前置条件

- Session 1-3 全部验收通过；硬件环境就绪（snvme module + tutti_daemon + /mnt/nvme1，见既定环境要求）。

## 目标

在真实 StorageRuntime→LocalFileResolver→LocalNvmeDataPath 栈上验证 adapter 的 KV save/load 端到端正确性，并以可重复门禁关闭 Phase 5 的 C++ adapter 部分。

## 允许修改/创建

- `tests/`（新增 adapter 硬件契约，hardware label）
- `scripts/phase5_adapter_gate.sh`（新建）
- `Roadmap.md`（Phase 5 C++ adapter 部分标记；Python/真实 framework 对接显式标注为未来工作）
- `chat/round12/result4.md`

## 禁止范围

- 不修改 core；不做 Python connector/vLLM plugin（明确属于下一独立任务）。
- 不执行模块/daemon/mount 操作；不提交 Git。

## 必须实现的行为

1. **硬件 KV E2E**：GPU 分配模拟 KV 池（多 block × 多 layer × K/V），adapter 一次注册后执行 save（GPU→file）→ 清空 GPU 池 → load（file→GPU）→ 逐 block/layer 校验数据。覆盖 SINGLE/DUAL/LIST 各至少一种尺寸。
2. **混合与并发**：多 request 并发 save/load、部分失败注入（越界 block id）、abort+drain 在真实栈上的行为。
3. **门禁脚本**：HOST ctest（含 adapter mock 契约）→ CUDA build → adapter 硬件契约 → 两既有硬件契约（735/115 基线）→ 汇总表。
4. **独立编译证明**：adapter target 不链接 LocalNvmeDataPath 符号（只经 StorageRuntime 注入），`ldd`/`nm` 或 CMake 依赖图证据。

## 测试要求

- 硬件契约断言全过；resolver_test 无残留；dmesg 无异常。
- 既有 735+115 基线无回归。

## 验收

- `chat/round12/result4.md`：硬件 E2E 输出（各尺寸/并发/失败注入）、门禁脚本输出、独立编译证据、Roadmap diff。
- 总指挥重跑门禁与硬件契约，抽查 offset 公式与 drain 时序；全过后宣布 Phase 5 C++ adapter 关闭，并明确记录：真实 vLLM Python connector 对接为 Phase 5 剩余工作。
