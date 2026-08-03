# TASK T-050 — Round 11 Session 1：MetadataArena 与 per-operation lease

## 前置条件

- Phase 3 已关闭（Round 10 全部验收 PASS）；阅读 `Roadmap.md` Phase 4、`chat/round10/result5.md`、`MAIN_MEMORY_PRP_PATH.md`。
- 现状（已核实）：`LocalNvmeDataPath::submit()` 热路径每次 op 直接 `cudaEventCreateWithFlags` + `cudaMalloc(d_entries)` + `cudaMalloc(d_status)`，LIST op 另加 `cudaMalloc(prp_raw)`（`local_nvme_data_path.cpp:1028-1135`）；`release()`/`shutdown()`/析构对应 free（含 `has_timeout` 保守保留）。
- legacy 参考资产：`memory/include/{gpu_slot_pool,host_slot_pool,prp_list_pool}.h`（仅参考，不强制复用）。

## 目标

用 per-device、有界 `MetadataArena` 取代 per-op CUDA 分配，op 的 descriptor/status/event/PRP-list workspace 全部从 arena 租约（lease），`release()` 归还；耗尽时确定性 `RESOURCE_EXHAUSTED`。不改 public/SPI，不改 IO 语义。

## 允许修改/创建

- `tutti/data_paths/local_nvme/**`（新增 `metadata/` 子目录）
- `tests/local_nvme_datapath_contract/**`
- `chat/round11/result1.md`

## 禁止范围

- 不修改 public/SPI 头、resolver、binding、StorageRuntime、control/、libnvm、kernel module。
- 不引入异步语义变化（fence/ordering 属 Session 3）；不迁移 handle/PRP page cache（属 Session 2）。
- 不执行模块/daemon/mount 操作；硬件测试用既有环境（module+daemon+/mnt/nvme1）。
- 不提交 Git。

## 必须实现的行为

1. `MetadataArena`：per-device、构造期有界（槽位数可配置，默认覆盖 `max_in_flight_operations`），预分配全部 workspace；submit 热路径零 `cudaMalloc`/`cudaEventCreate`。
2. 租约语义：op 从 arena 取得 {d_entries, d_status, event, PRP-list span} 租约；`release()` 后归还；op terminal 未 release 仍占租约（与 Round 8 in-flight 配额语义一致）。
3. 耗尽确定性：arena 满 → `submit()` 对应 request `RESOURCE_EXHAUSTED`（批内 per-request 拒绝，partial commit 语义不破），不得隐式排队或退化到 cudaMalloc。
4. `has_timeout` 保守保留规则不变：timeout op 的 PRP-list 租约不归还（注释说明 CID 降级 + 槽位泄漏上界）。
5. legacy 复用评估：`gpu_slot_pool/host_slot_pool/prp_list_pool` 是否适配本需求，结论与理由写入 result（复用、部分复用或重写均可，但必须论证）。

## 测试要求

保留全部 616+115 断言并新增：

- arena 容量=N 时第 N+1 个并发 op 得 `RESOURCE_EXHAUSTED`，drain+release 后可再提交；
- release 后槽位复用（重复 submit/release 多轮无泄漏、无 cudaMalloc 调用计数增长——可用 allocator 计数 seam 证明热路径零分配）；
- timeout op 的 PRP 租约不归还且arena 槽位按预期减少；
- 真实 SINGLE/DUAL/LIST roundtrip 与 S5 E2E 无回归。

## 验收

- `chat/round11/result1.md`：arena 设计（槽位布局/对齐/容量）、legacy 复用评估结论、热路径零分配证据、耗尽/复用/timeout 测试结果、616+115 全量回归。
- 总指挥复跑两硬件契约 + HOST/CUDA ctest，审查 arena 边界条件。
