# TASK T-066 — Round 12 Session 3：sample 扩展全流程证明（新 Resolver+Binding 不加 core 改动）

## 前置条件

- Session 1 完成（MockDataPath kit 可复用）；阅读 `Roadmap.md` Phase 6 gate 原文：「a community contributor can add a feature primarily by adding a new package/profile/tests/docs plus one line in the registry/CMake profile list, without modifying Runtime public storage nouns or algorithms」。

## 目标

以一个全新的最小 sample 扩展（Resolver+Binding 对，建议纯内存/null 语义，如 `memfs`）走完社区贡献者全流程，实证：只新增 package+tests+docs+一行接线，零 core 改动。

## 允许修改/创建

- `tutti/resolvers/<sample>/`、`tutti/bindings/<sample>/`（新建 sample package）
- `tests/`（sample 契约测试）
- `tutti/CMakeLists.txt`（仅一行 add_subdirectory/注册）
- `doc/`（sample 扩展指南一页）
- `chat/round12/result3.md`

## 禁止范围

- **零 core 改动**：不得修改 `tutti/include/tutti/**`、StorageRuntime、既有 resolver/binding/DataPath 的任何文件（若发现必须改 core 才能完成，停下来记录为 gap，不得绕过）。
- sample 不得成为第二个生产 backend（纯示例语义，文档明确标注 sample-only）。
- 不需要硬件；不提交 Git。

## 必须实现的行为

1. **sample 语义**：最小但真实——`memfs://` URI → 固定内存"设备"上的字节区间（纯 host 内存模拟），Resolver 产出 pair-private payload，Binding 声明 DataPath key 兼容性（可声明兼容 Session 1 的 MockDataPath，或自带极简 DataPath 亦可——但推荐复用 kit 以同时证明 kit 价值）。
2. **pair-private payload 不污染 Runtime**：payload 类型只存在于 sample package 内；`tutti/include/tutti/**` 与 Runtime 源文件无任何 sample 类型引用（grep 证据）。
3. **一行接线**：`tutti/CMakeLists.txt` 仅新增一行（add_subdirectory 或 feature 注册），sample 即可被构建与测试。
4. **StorageRuntime 端到端（host 内存）**：用 sample resolver + MockDataPath 经公开 `StorageRuntime` 完成 open→register→submit(write/read)→query→release→close，数据回读正确。
5. **扩展指南**：`doc/` 一页：新增一个 Resolver/Binding/DataPath 需要哪些文件、放哪里、在哪注册一行、如何跑契约——以本 sample 为实例。

## 测试要求

- sample 契约测试：URI 解析、payload 生命周期（lease 释放）、边界拒绝、经 Runtime 的 E2E 回读；hardware-free，纳入 BUILD_TESTING。
- 既有全部测试零回退（HOST/CUDA）。

## 验收

- `chat/round12/result3.md`：sample 文件清单（证明"只新增"）、一行接线 diff、pair-private 不污染 grep 证据、Runtime E2E 输出、扩展指南链接。
- 总指挥复核：`git status` 确认 core 零改动；一行接线属实；指南可独立跟随。

## 后续依赖

- Session 4（feature 开关与门禁）依赖本 session。
