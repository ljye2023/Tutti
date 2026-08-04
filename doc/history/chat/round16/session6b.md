# TASK — Round 16 Session 6b：tiered 元数据恢复（设计已裁定，按裁定移植）

> ## 进度基线（2026-08-04 总指挥核定——已完成部分勿重复）
>
> 1. **模板移植已完成（~30%）**：`tutti/data_paths/local_nvme/metadata/` 下 `tiered_handle_cache.h`（644 行，与 legacy 逐行一致）、`gpu_slot_pool.h`（482）、`host_slot_pool.h`（160）、`prp_list_pool.h`（171）——namespace 已适配 `tutti::data_paths::local_nvme`，git untracked。**勿重新移植**。
> 2. **集成 0%**：上述文件未在任何处 include/接线（CMake、DataPath 均无引用）——这是本 session 的剩余主体。
> 3. 树当前健康（全量构建 rc=0，S6 REQUIRED 0 已验收态）。
> 4. **环境已升级**：operator 已 reload 内核模块 `io_queue_depth=1024`（/sys/module/snvme/parameters/io_queue_depth 已确认 1024），4 盘已挂载、daemon 在线。门禁直接按 1024 深度跑；测试/示例的 `queue_depth` 构造参数请同步改为 1024（或 `TUTTI_TEST_QDEPTH` env，默认 1024）。
> 5. 前次停止原因：session 上下文耗尽，非设计阻塞（T 类型裁定见下）。

**日期：** 2026-08-04（总指挥签发）
**前置：** S6 REQUIRED 0 已验收（kernel 单路径化）。本 session 继续 S6 未完成的三层，**T 类型的设计问题已由总指挥裁定（见下），不再重新设计**。
**maintainer 最高指令不变：** 完整恢复 legacy 逻辑、不改原本逻辑、`third_pkgs/` 只读、session 自证、门禁收尾一次。

---

## 设计裁定（总指挥，2026-08-04——替代 S6 result 中的次优提案）

S6 session 担心 `DeviceTargetHandle` 含 GPU 指针（`extents_overflow`、`d_qps`）"不能简单 memcpy"——**只对一半**：

1. **`d_qps` 按值 memcpy 即可**：queue pair 结构体由 DataPath/controller 持有、生命周期内不可移动，指针值稳定，拷贝往返安全。legacy 的 `NvmeFileDeviceHandle` 同样内嵌 queue 指针，memcpy 成立正是同一原理。
2. **`extents_overflow`：L2 记录保存"handle 结构体 + overflow 内容字节"**（两者都是纯数据）。downgrade = 内容拷入 L2 host 缓冲 + 释放 GPU 分配；promote = 池分配 + memcpy 回灌。**这就是 legacy "promote 只需一次 memcpy"的语义**——restore，不是 rebuild。S6 提议的"L2 存 FIEMAP、promote 重跑 build_device_target"**不采用**（rebuild ≠ restore，且徒增延迟）。
3. KV 现实：fallocate 连续文件 1-2 extent，handle 内联自足，形状 = legacy 的 192B/entry。
4. 结论：`T` = `{DeviceTargetHandle handle; overflow blob}` 的小包装，直接套 legacy 模板形状。

## 工作项（按依赖序）

1. **基础设施**：移植 `GpuSlotPool`（GPU 槽位 + stream-fenced 回收）与 `HostSlotPool`（host-pinned 槽位）——`third_pkgs/Tutti/memory/include/gpu_slot_pool.h`、`host_slot_pool.h`。
2. **PRP page cache 双层化**：移植 `prp_page_cache.h`（L2 host-pinned 内容 + L1 GPU DMA 工作集 + `ensure_resident_batch` 批量 promote + **scatter kernel 一次 patch 全部 prp2** + event-fenced slot reuse）。现有单层 `metadata/prp_page_cache.h` 的 `checkout_refcount`（R16 S1 P0 修复）语义叠加保留。
3. **handle cache 双层化**：移植 `tiered_handle_cache.h`，`T` 按上述裁定；L1 现有 `open_refcount` 语义叠加保留（在用 entry 不可 downgrade）；batch promote = 一次连续 memcpy。
4. **MetadataArena / StripedArena 对齐 stream-fenced slot reuse**（event 栅栏异步回收时机对齐 legacy）。
5. **容量压测契约**（编号接 97 后）：① L1 容量 < 文件数时 reopen 走 memcpy 回灌（分配/拷贝计数证明，**非** cudaMalloc 重建）；② N 冷 entry promote = 1 次连续 memcpy；③ scatter patch 调用 = 1/batch；④ 正确性逐字节。

## REQUIRED 附带项：queue_depth 1024 对齐

maintainer 定：`io_queue_depth=64` 是防呆默认，**生产要 1024**。**operator 已完成 reload（当前内核 io_queue_depth=1024）**。libnvm/daemon 自动跟随 `ctrl->q_depth`（唯一事实源），零生产代码改动。本 session 附带：测试/示例的 `queue_depth` 构造参数（写死 64，纯簿记）改为 1024（或 `TUTTI_TEST_QDEPTH` env 默认 1024），门禁按 1024 跑。

## 验收（session 自证）

1. 容量压测契约 4 项全过（计数证据）；2. 全量门禁一次（含 qdepth=1024 配置下，若 operator 已 reload）；3. KV sim Phase H 26/26 + 带宽对照（qdepth 64 vs 1024 各一轮，若环境允许）；4. result：改动清单、legacy 行号对齐表、计数证据。

## 硬约束

同 S5/S6：third_pkgs 只读、公共 API 语义不破、O_DIRECT、防缠结（改动清单）、cache 默认值（OFF）不动、临时 debug 收尾摘除。
