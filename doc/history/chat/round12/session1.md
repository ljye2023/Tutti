# TASK T-064 — Round 12 Session 1：MockDataPath 契约套件（contract kit）

## 前置条件

- Round 11 S1-S3 已验收；阅读 `Roadmap.md` Phase 6 deliverable「`MockDataPath` covering open/register/submit/progress/completion/error」与 gate。
- 现状：可用的 DataPath 测试替身散落在测试文件内部（`tests/storage_runtime_contract/storage_runtime_contract_test.cpp` 的 `RuntimeFakeDataPath`、`tests/data_path_contract/` 的 mock），无法被第三方扩展作者复用。

## 目标

建立一个可复用的 `MockDataPath` 契约套件：覆盖 open/register/submit/progress/completion/error 全生命周期，带可控注入点，作为社区扩展者的标准测试工具与参考实现。

## 允许修改/创建

- `tutti/testing/`（新建：kit 头文件/实现，header-only 优先）
- `tests/`（迁移现有 fake 到 kit；新增 kit 自身契约测试）
- `tutti/CMakeLists.txt`（测试接线）
- `chat/round12/result1.md`

## 禁止范围

- 不修改 `tutti/include/tutti/**` public/SPI 头与任何生产代码（kit 是测试设施，不进生产 target）。
- 不改变现有测试的语义与断言数量（迁移必须等价）。
- 不需要硬件；不提交 Git。

## 必须实现的行为

1. **kit 位置与形态**：`tutti/testing/mock_data_path.h`（.cpp 可选），只依赖 public/SPI 头；不带任何 libnvm/CUDA 依赖，HOST profile 可编译。
2. **全生命周期覆盖**：open/close、registration_domain、register/unregister、submit（含 partial commit）、progress（可驱动状态机）、query、release、shutdown drain。
3. **可控注入点**（至少）：per-request 拒绝、op 失败（FAILED + Status）、progress 不推进（模拟 hang）、能力位自定义；注入点 API 有注释示例。
4. **迁移**：`RuntimeFakeDataPath` 与 data_path_contract 的 mock 迁移到 kit，既有测试断言数不减少；重复 fake 定义消除。
5. **kit 自证契约**：新增 kit 自身的契约测试（kit 满足 DataPath SPI 的全部既有 contract 断言——证明"参考实现"本身合规）。

## 测试要求

- HOST ctest 全绿且断言数 ≥ 迁移前；CUDA profile build 不引入 kit 的硬件依赖。
- kit 文件自身不得 include 任何私有头（header_hygiene 思路检查）。

## 验收

- `chat/round12/result1.md`：kit API 摘要、注入点清单、迁移等价性证据（前后断言数）、HOST/CUDA 构建与 ctest。
- 总指挥复核：kit 只依赖 public/SPI；迁移无测试语义漂移。

## 后续依赖

- Session 3（sample 扩展）可复用本 kit；与 Session 2（profile 契约）可并行。
