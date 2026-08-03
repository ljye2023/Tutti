> **状态：已暂缓（2026-08-01，maintainer 决定）。** Framework Adapter 当前版本不实施，本文件保留作为未来 prompt 基线。

# TASK T-061 — Round 12 Session 2：Adapter 核心——KV 读写路径与一次注册

## 前置条件

- Session 1 验收通过（设计已冻结）；阅读 `chat/round12/result1.md` 的 layout 公式与状态机。

## 目标

实现 adapter 的核心数据面：整块 KV 一次 `register_memory`，按 {block, layer, K/V} 切片提交 READ/WRITE，`IoHandle` 正确映射回 framework request id，全部只经 StorageRuntime public API。

## 允许修改/创建

- `tutti/adapters/**`
- `tests/`（adapter 测试）
- `chat/round12/result2.md`

## 禁止范围

- 不修改 core（同 Session 1）；不引入 framework 依赖；不做 Python 绑定。
- 不做性能优化（batching 策略保持简单正确即可）。
- 不需要硬件（mock DataPath + host 内存即可验证逻辑）；不提交 Git。

## 必须实现的行为

1. **一次注册**：adapter 初始化时对整块 KV 分配调用一次 `StorageRuntime::register_memory`；任何 block/layer 切片请求不再触发注册（计数断言）。
2. **切片提交**：`save(block_ids, layer_range)` / `load(...)` 展开为 per-切片 `IoRequest`，offset 严格按 Session 1 冻结公式；批量提交遵守 Runtime partial-commit 语义（部分拒绝向上层如实返回）。
3. **id 映射**：每个保存/加载请求获得可查询的句柄；`poll(request_id)` / `wait(request_id)` 正确反映终态；终态前 block 不得标记可复用。
4. **错误传播**：Runtime/DataPath 的 FAILED/TIMEOUT 映射为 adapter 层的明确错误（含 request id 上下文），不吞错。
5. **注销**：adapter 析构/shutdown 时 `unregister_memory`（无 in-flight 后），顺序正确。

## 测试要求（全部 mock/hardware-free）

- offset 公式：首/末 block、首/末 layer、K/V 两路、非对齐拒绝；
- 一次注册计数断言；大 block 列表（> 单批容量）正确分批且 id 一一对应；
- partial commit：注入单切片拒绝，其余成功，上层看到精确的成功/失败集合；
- 错误传播：注入 FAILED，poll/wait 返回带 request id 的错误；
- 生命周期：unregister 在 in-flight 时拒绝（BUSY），drain 后成功。

## 验收

- `chat/round12/result2.md`：API 摘要、测试输出、HOST ctest 全绿。
- 总指挥复核：核对 offset 公式与 Session 1 冻结设计一致；partial commit 与 id 映射无边界漏洞。

## 后续依赖

- Session 3（abort/drain 语义）依赖本 session 核心。
