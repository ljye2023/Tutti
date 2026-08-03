# TASK T-074 — Round 14 Session 3：legacy bugfix/perf 在新架构的等价性核查与防御测试

## 前置条件

- Session 2 完成；阅读 main commits `10602fc`（stripe 粒度 bug）与 `859953c`（shard-slot 重写跳过）。
- 已核实的映射背景：
  - `10602fc` 的 bug 类：legacy 多 shard striping 用 sub-IO(MDTS) 粒度 round-robin，tensor>MDTS 时落到错误 shard/offset（resolve_lba 失败 + 静默 K/V 交叉污染）。**新架构无 striping 层**：单文件 target，entry 直接携带虚拟偏移，kernel `resolve_lba` 按 extent 逐 entry 解析。
  - `859953c` 的意图：消除每 batch 重复的 shard 指针 H2D 重写。**新架构 Round 11 S2 handle_workspace_cache 已内嵌该意图**（target workspace 缓存+pin，submit 只 H2D entries/PRP/status 清零）。

## 目标

以代码走查 + 防御性测试证明：两个 legacy 问题的等价物在新架构中不存在（或已被更优机制覆盖），形成书面核查结论，防止未来回归。

## 允许修改/创建

- `tests/local_nvme_datapath_contract/**`（防御性测试）
- `chat/round14/result3.md`

## 禁止范围

- 原则上不改生产代码；若核查发现真实缺陷，停下来记录，不得顺手修（走独立 follow-up）。
- 不执行模块/daemon/mount 操作；不提交 Git。

## 必须实现的行为

1. **10602fc 等价性核查**：
   - 走查新路径「单请求长度 > MDTS → 多 entry fan-out」的 entry 生成（`local_nvme_data_path.cpp` 分段逻辑）与 kernel 端 `resolve_lba`（`submit_one.cuh`）：证明每个 entry 的虚拟偏移独立正确解析，跨 extent 边界行为正确；
   - 防御测试：长度 > MDTS 且跨 extent 的请求，WRITE 后逐字节 READ 回读校验（覆盖 fan-out 各 entry 落到正确 LBA 的证据）；若现有 tests 43/44 已充分覆盖，引用并说明覆盖边界，只补缺口。
2. **859953c 等价性核查**：
   - 走查 submit 热路径的 H2D 写集合（entries/PRP/status），证明 target workspace 不在每 submit 重写；
   - 防御测试：重复 submit 同一 target，断言 handle workspace 的 H2D 拷贝计数不增长（用既有 cache 统计 accessor 或 allocator seam）。
3. **书面结论**：两个 commit 的问题陈述 → 新架构机制 → 为何不适用/已覆盖 → 防御测试证据，写入 result3.md。

## 测试要求

- 新增/引用的防御测试在真实硬件通过；既有 735+115 断言零回归。

## 验收

- `chat/round14/result3.md`：走查记录（文件/行级）、防御测试输出、等价性结论。
- 总指挥复核：走查逻辑独立推演一遍；防御测试复跑。

## 后续依赖

- 无硬依赖；与 Session 4 可并行（硬件运行错峰）。
