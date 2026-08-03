# TASK T-069 — Round 13 Session 2：第一批退役（无生产消费者的 legacy 树）

> **并行执行**：本 session 可与 Session 3 并行，必须遵守 `chat/round13/PARALLEL.md` 的行级分治与联合验证协议。编辑期不得自行构建验证。

## 前置条件

- Session 1 决议表通过验收；本批范围为决议表中标记「删除」且无生产消费者的树（预期：`nvme_storage/`、`block_storage/`、`io_engine/`、`coordinator/`、`adapters/kv_cache/`，以 S1 决议为准）。

## 目标

删除第一批 legacy 树及其构建接线，root build 与 standalone build 保持可用，全部契约零回归。

## 允许修改/创建

- 删除 S1 决议的树；根 `CMakeLists.txt` 对应接线移除
- legacy `tests/` 中引用被删树的测试（一并移除对应接线；其余保留）
- `doc/history/`（归档被删树的设计文档，不删历史）
- `chat/round13/result2.md`

## 禁止范围

- 不动 `memory/`、`device_manager/`、`backends/local/NVMeService/`、`examples/`（属 S3）。
- 不动 standalone `tutti/` 任何源码（如发现 standalone 依赖被删树，停下记录 gap，不得绕过）。
- 不执行模块/daemon/mount 操作；不提交 Git。

## 必须实现的行为

1. 按 S1 决议逐树 `git rm -rf`；同步移除根 CMake 的 `add_subdirectory` 与所有引用（include_directories、target_link、install 规则）。
2. legacy `tests/` 清理后剩余目标全部可链接通过（`layer1_smoke_test` 的 pre-existing 失败允许按 S1 决议一并处理并记录）。
3. 文档归档：被删树对应的设计/验证文档移入 `doc/history/`（命名规范沿用既有），不丢历史。
4. 每删一棵树即验证一次（增量可发现问题归属）：root build configure+核心 target build、standalone HOST/CUDA build+ctest。

## 测试要求

- 全部完成后：HOST ctest 14/14、CUDA ctest 134/134（或当前基线）、两硬件契约 735/0 + 115/0、memfs 契约 5/5。
- `git grep` 证明 standalone 无被删树引用。

## 验收

- `chat/round13/result2.md`：删除清单（git rm 输出）、每步验证记录、归档清单、全量回归输出。
- 总指挥复跑三端构建与两硬件契约，抽查无残留引用。

## 后续依赖

- S3（资产迁移类退役）依赖本批完成。
