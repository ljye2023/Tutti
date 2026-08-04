# TASK — Round 16 Session 6：完整恢复 legacy 逻辑（二）：tiered 元数据（handle cache L1/L2 + PRP page tier + slot pool + batch promote + scatter patch）

**日期：** 2026-08-03（总指挥签发）
**前置：** S5（注册期预构建）落地。同一批 metadata 文件，严格串行。
**maintainer 最高指令：** 完整恢复历史逻辑、不修改原本逻辑；`third_pkgs/` 只读；总指挥不做测试循环，session 自行验证。

---

## REQUIRED 0：kernel 单路径化（消除双路径分支，对齐 legacy 唯一形态）

maintainer 问题"需要支持两种路径吗"的定案：**动态路径也经 descriptor 承载**。具体：`submit_one_kernel`/`fused_submit_kernel` 删除 `if (e.prp_entry != nullptr)` 分支，永远从描述符指针读 prp1/prp2/data_length（legacy 唯一形态）；动态（未声明 io_granularity/形状不匹配）路径在 submit 时把现算的描述符写入 **arena 每槽描述符池**（容量 = max_batch_entries × 24B，预分配）+ 一次 H2D，entry 统一携带描述符指针。host 工作量与现 inline 填值相同、H2D 字节量相当（entry 缩小），公共 API 通用性零损失。分支从 kernel 中永久消失。

## 目标

把 legacy 的**双层元数据管理**完整恢复到新架构，替换现有的单层简化实现。三层恢复：

### 1. handle cache 双层化（对齐 `third_pkgs/Tutti/memory/include/tiered_handle_cache.h`）

- **L2 host-pinned 大容量**：保存完整模板（FIEMAP/extent 结果等全部 GPU 工作区内容的主机副本）；**L1 GPU 小容量**：当前工作集。
- **eviction = DOWNGRADE**：L1 逐出只释放 GPU slot，L2 副本保留；再次 promote = **一次 memcpy 回灌**，不是完整重建（cudaMalloc + 重算）。
- **batch promote**：冷子集一次 host build + 一次连续 L2 写 + **一次连续 L1 cudaMemcpyAsync**（连续段，不打散成逐 entry H2D）。
- 现有 `open_refcount`（R16 S1 P0 修复）语义保留并叠加在 L1 层（在用的 entry 不可 downgrade）。

### 2. PRP page cache 双层化（对齐 `third_pkgs/Tutti/memory/include/prp_page_cache.h`）

- L2 host-pinned 保存 PRP 页**内容**（注册时一次 admit）；L1 GPU DMA 工作集。
- `ensure_resident_batch`：批量 promote；evict = 只 downgrade。
- **prp2 修补用一个 scatter kernel 一次完成**（绝不打散成多次 H2D）——对齐 legacy `ensure_prp_pages_resident` 的 patch 机制。
- event-fenced slot reuse。

### 3. slot pool 语义（对齐 legacy `GpuSlotPool`/`HostSlotPool`）

- stream-fenced slot reuse（cudaEvent 栅栏异步回收）；现有 `MetadataArena` 的 acquire/release 语义对齐补齐（event fence 时机、批量连续段 promote）。

## 验收（session 自行验证并报告）

1. 容量压测契约（编号接着 S5 新增之后）：L1 容量 < 文件数场景下，reopen/promote 路径走 memcpy 回灌（**不是** cudaMalloc 重建）——用分配/拷贝计数证明；正确性逐字节校验。
2. batch promote：N 个冷 entry 的 promote = 1 次连续 memcpy（计数证据），不是 N 次。
3. scatter patch：prp2 修补 kernel 调用次数 = 1/batch（计数证据）。
4. 契约一次：全量门禁；KV sim Phase H 26/26。
5. result：改动清单、legacy 行号对齐表、计数证据、带宽对照。

## 硬约束

同 S5：`third_pkgs/` 只读；公共 API 语义不破；O_DIRECT；防缠结；默认值（cache OFF）不动——双层机制是 cache ON 时的行为；临时 debug 收尾摘除。
