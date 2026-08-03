# TASK T-051 — Round 11 Session 2：TieredHandleCache 与 DMA-correct PrpPageCache 迁移

## 前置条件

- Session 1 完成（MetadataArena 就位）；阅读 `Roadmap.md` Phase 4 对应 deliverable、`memory/include/tiered_handle_cache.h`（644 行，两级：CPU pinned L2 + GPU L1，inclusive LRU）、`memory/include/prp_page_cache.h`（426 行，host-pinned L2 + GPU-DMA L1）。
- 现状：每次 `DataPath::open()` 重新构建 device target workspace（FIEMAP payload H2D）；LIST op 的 PRP-list 每次 submit 从 arena 全新分配+DMA map（Session 1 后）。

## 目标

把两个 legacy cache 以 **operation-pinned entries** 语义迁移为 `LocalNvmeDataPath` 私有原语：in-flight op 引用的 entry 不可被逐出；重复 open 同一文件复用 handle workspace；LIST PRP-list 页可缓存复用且 DMA 映射生命周期正确。

## 允许修改/创建

- `tutti/data_paths/local_nvme/**`（`metadata/` 或新子目录）
- `tests/local_nvme_datapath_contract/**`
- `chat/round11/result2.md`

## 禁止范围

- 不修改 public/SPI、resolver、binding、StorageRuntime、control/、libnvm、kernel module。
- 不把 cache 提升为 public 概念；不引入跨 DataPath 共享 cache。
- 不做异步/fence 改动（Session 3）；不做性能调优之外的 kernel 改动（Session 4）。
- 不执行模块/daemon/mount 操作；不提交 Git。

## 必须实现的行为

1. 复用评估先行：legacy 两级 cache 设计（L1 GPU/L2 host、inclusive LRU、批量 promote）是否直接适配当前 DataPath 工作集；结论与裁剪/重写理由写入 result。
2. operation-pinned：in-flight op 持有的 handle/PRP entry 被 pin，pin 计数归零前不得逐出/unmap；pin 生命周期 = op 租约生命周期（接 Session 1 arena 语义）。
3. DMA 正确性：缓存的 PRP-list 页的 DMA mapping 与其 backing 页同生共死；逐出必须先 unmap；不得出现「缓存页已 unmap 但 controller 仍在取指」的窗口（与 has_timeout 规则一致）。
4. 容量有界：两级容量可配置；逐出路径确定性；统计（hit/miss/evict/pinned）暴露为 DataPath 私有 test accessor。
5. 迁移后 `memory/` legacy 原文件不动（Phase 7 才退役）；新实现物理位于 `data_paths/local_nvme/`。

## 测试要求

保留全部既有断言并新增：

- 同文件重复 open：第二次 open 命中 cache（accessor 证明），H2D 拷贝次数不增长；
- pin 保护：构造工作集超过 L1 容量，in-flight op 的 entry 不被逐出（统计+正确性双重证据）；
- 逐出正确性：op release 后 entry 可逐出，逐出前 unmap（test accessor 观察 mapping 状态）；
- 真实 SINGLE/DUAL/LIST/cross-extent roundtrip 与混合 workload 无回归；
- cache 开启/关闭两种配置下 616+115 基线全过。

## 验收

- `chat/round11/result2.md`：复用评估结论、cache 结构（两级容量/逐出策略/pin 语义）、DMA 生命周期论证、hit/miss 统计实测、全量回归。
- 总指挥复跑两硬件契约，审查 pin/逐出/unmap 时序。
