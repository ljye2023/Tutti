> **状态：已暂缓（2026-08-01，maintainer 决定）。** Framework Adapter 当前版本不实施，本文件保留作为未来 prompt 基线。

# TASK T-060 — Round 12 Session 1：Framework Adapter 设计冻结与 Mock 契约

## 前置条件

- Round 11 S1-S3 已验收（735/0、115/0）；阅读 `Roadmap.md` Phase 5、`TUTTI_TARGET_ARCHITECTURE.md` FrameworkAdapter 相关章节、`tutti/include/tutti/storage_runtime.h`。
- 现状：public StorageRuntime 已可工作（Round 8-11）；`adapters/kv_cache/` 是 legacy（基于旧 io_engine/Coordinator，本 round 不续用其代码，仅可参考其 KV offset 语义注释）；本机有 `~/vllm-env`（Python venv，可用于锁定 framework 版本）。

## 目标

冻结首个真实 Framework Adapter 的设计并建立 mock-first 契约：明确 framework 选择、KV layout 语义、request 生命周期模型；用测试 DataPath（非硬件）证明 adapter 只依赖 StorageRuntime public API。

## 允许修改/创建

- `tutti/adapters/`（新建子树）
- `tests/`（新增 adapter 契约测试目录）
- `tutti/CMakeLists.txt`（测试接线）
- `chat/round12/result1.md`

## 禁止范围

- 不修改 core：`tutti/include/tutti/**`、SPI、StorageRuntime、LocalNvmeDataPath、resolver、binding 一律不动。
- 不在 core 中引入任何 vLLM/LMCache 类型；adapter 也不得 include framework 头（C++ 层保持框架无关，Python 绑定属后续）。
- 不实现真实 framework 集成（Python connector、vLLM plugin 属 Session 4 之后的独立任务，本 round 只到 C++ adapter + 门禁）。
- 不需要硬件（本 session 全部 mock）；不提交 Git。

## 必须实现的行为

1. **framework 选择论证**：vLLM vs LMCache 二选一（建议 vLLM：`vllm-env` 可锁定版本 + 既有 Mooncake/vLLM 经验）。`pip show vllm`（或对应包）锁定版本号写入 result。选择必须基于证据，不允许"两个都做"。
2. **设计冻结**（写入 result1.md 与 adapter 头文件注释）：
   - KV layout：block/layer/K/V 的内存排布假设（从选定 framework 的实际 layout 出发，写明版本依据）；
   - `memory_offset` 切片公式：整块 KV 一次注册，如何用 offset 表达 {block, layer, K/V}；
   - request 生命周期：submit→in-flight→terminal→block 可复用的状态机；
   - `IoHandle` → framework request id 的映射方式。
3. **C++ adapter 骨架**：`VllmAdapter`（内部名；外部协议名 `TuttiKVConnector` 仅在必要时出现于注释）——构造注入 `StorageRuntime&`，只调用其 public API。
4. **Mock 契约测试**：测试 DataPath（可使用既有 RuntimeFakeDataPath 模式）替换 LocalNvmeDataPath 时，adapter 源码零修改编译运行通过；覆盖 layout 公式、切片 offset、handle→id 映射、非对齐/越界拒绝。

## 测试要求

- hardware-free；纳入 `BUILD_TESTING`（HOST profile 可跑）。
- 明确断言：整块注册一次（register_memory 调用计数）、切片请求不触发重复注册、offset 公式边界（首/末 block、首/末 layer）。

## 验收

- `chat/round12/result1.md`：framework 选择与锁定版本证据、设计冻结文档、mock 契约测试输出、HOST ctest 全绿。
- 总指挥复核：adapter include 图中无 core 内部头、无 framework 类型；mock 互换证明成立。

## 后续依赖

- Session 2（核心实现）依赖本 session 的冻结设计。
