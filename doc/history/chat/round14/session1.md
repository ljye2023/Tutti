# TASK T-072 — Round 14 Session 1：main 文档体系移植与 Roadmap 调和

## 前置条件

- 阅读 main 分支 commit `a4e4d43`（`third_pkgs/Tutti`）：README/CONTRIBUTING 重写、Roadmap 改 4 产品方向、`doc/architecture/key-designs.md`、`doc/tutti-arch.png`。
- 已核实：`key-designs.md` 与 `tutti-arch.png` 不在重构树；key-designs.md 的全部代码指针指向已退役 legacy 路径（io_engine/、memory/、nvme_storage/、block_storage/、device_manager/）。

## 目标

把 main 的产品文档体系移植进重构树，所有代码指针改写为新架构真实路径；调和"产品 Roadmap（4 方向）"与"实施 Roadmap（Phase 0-7，已全部关闭）"的关系。

## 允许修改/创建

- `README.md`、`CONTRIBUTING.md`、`doc/architecture/key-designs.md`、`doc/tutti-arch.png`、`Roadmap.md`、`doc/history/`
- `chat/round14/result1.md`

## 禁止范围

- 不修改任何源码与测试。
- 不丢失重构实施历史：实施 Roadmap（当前 Roadmap.md）必须归档，不得直接覆盖删除。
- 不提交 Git。

## 必须实现的行为

1. **README**：以 main 新版为主体（GPU-centric KV-cache 定位、GeminiFS 引用、架构图、features 矩阵），但内容必须与重构后事实一致：提到的组件/路径/构建命令逐条核对新架构（libnvm→`tutti/device_manager/nvme/libnvm/`、snvme→`kernel_modules/`、daemon→`build/bin/tutti_daemon`、构建→root build 或 standalone profile）；不实表述修正或删除。
2. **key-designs.md**：五个性能设计保留论述，代码指针全部改写为新架构等价位置（如 `nvme_batch_xfer_kernel.cu`→`data_paths/local_nvme/io/submit_one.cu`、tiered cache→`metadata/handle_workspace_cache.h`/`prp_page_cache.h`、slot pool→`metadata/metadata_arena.h`）；无等价物的标注「该设计在重构后以 X 形式体现/未保留」并说明。
3. **Roadmap 调和**：main 的产品 Roadmap（4 方向：Kernels/加速卡/AI 应用/社区）成为新的主 `Roadmap.md`；当前实施 Roadmap（Phase 0-7 全关闭）归档为 `doc/history/roadmap-refactor-v0.2.md`（命名遵循既有规范，顶部注明完成日期与验收基线 735/115/14/134）。
4. **CONTRIBUTING**：main 新版移植，测试阶梯改为新架构事实（`ctest` 标签、hardware-free/hardware 分层、门禁脚本）。
5. `tutti-arch.png` 复制进 `doc/`。

## 测试要求

- 纯文档任务：只需确认 markdown 内引用的路径/命令在重构树中真实存在（脚本化 grep 验证清单贴入 result）。
- 构建与测试零影响（跑一次 HOST ctest 证明无意外改动）。

## 验收

- `chat/round14/result1.md`：移植清单、指针改写对照表（旧→新路径）、Roadmap 调和方案、链接/命令有效性验证输出。
- 总指挥复核：抽查指针全部可达；README 表述与新架构事实一致。

## 后续依赖

- 与 Session 2 可并行。
