# TASK T-071 — Round 13 Session 4：Phase 7 门禁与 Roadmap 关闭

## 前置条件

- Session 1-3 全部验收通过；阅读 `Roadmap.md` Phase 7 gate 原文：「no duplicate production implementation remains; clean standalone build and hardware-free tests still pass; performance baseline preserved or improved」。

## 目标

以可重复门禁关闭 Phase 7：无重复生产实现、三端构建干净、契约全绿、性能对比不劣化、文档归档完成。

## 允许修改/创建

- `scripts/phase7_gate.sh`（新建）
- `Roadmap.md`（Phase 7 标记 + 版本快照规则执行）
- `doc/history/`（最终归档）
- `chat/round13/result4.md`

## 禁止范围

- 不新增功能；不修改 standalone 生产源码。
- 不执行模块/daemon/mount 操作；不提交 Git。

## 必须实现的行为

门禁脚本按序执行并输出汇总表：

1. **无重复实现**：`git grep`/`find` 证明 libnvm、snvme、NVMeService、device_manager、io_engine、memory 实现各剩一份（列出唯一路径清单）。
2. **三端构建**：root build configure+生产 target（libnvm/nvmeservice/tutti_daemon/modules）通过；standalone HOST clean configure+build+ctest；standalone CUDA clean configure+build+ctest（非硬件）。
3. **契约全绿**：HOST 全量、CUDA 非硬件全量、memfs 契约；operator 环境下两硬件契约 ≥735/115。
4. **性能对比**：复跑 S1 基线脚本，与退役前基线对比（允许 ±10% 噪声带），记录数据；劣化超过阈值则失败并回查。
5. **Roadmap 关闭**：Phase 7 deliverable 标记完成；按版本规则把当前 roadmap 快照归档 `doc/history/`（命名遵循既有规范）。

## 测试要求

- 门禁脚本连续两次全绿；`git status` 干净度检查（退役后无孤儿引用、无未跟踪垃圾目录）。
- dmesg/resolver_test 环境检查同既有标准。

## 验收

- `chat/round13/result4.md`：门禁输出、唯一实现清单、性能对比数据、Roadmap diff。
- 总指挥重跑门禁与硬件契约；全过后宣布 Phase 7 关闭——`Roadmap.md` Active Roadmap 全部 phase 完成（Phase 4 性能基准与 Phase 5 Framework Adapter 为 maintainer 决策的显式暂缓项，保持记录）。
