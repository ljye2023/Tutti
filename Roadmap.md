# Tutti Roadmap（post-refactor）

**生成日期：** 2026-08-03
**来源：** 四份独立评审合成（`doc/review/`：REFACTOR_AUDIT、ASSESSMENT、ASSESSMENT_R15、REVIEW）
**状态：** 待 maintainer review
**基线：** refact @ 26f1f7e+8de7c5f；契约 datapath 820/0、runtime 137/0、striped 46/0、非硬件 15/15

> 旧 Roadmap（Phase 0-7）已随重构收官删除。本文档只列评审发现的**真实缺陷与未决事项**，每条标注来源评审与验收方式。标记 ◆ 的为需要 maintainer 拍板的决策项，其余为确定性修复。

---

## P0 — 正确性缺陷（稳定版前必须修复）

### P0-1 HandleWorkspaceCache reopen→eviction 悬空 GPU handle
- **来源：** 全部四份评审一致确认（AUDIT §4-6、ASSESSMENT P0、R15 F-001、REVIEW §3）；**总指挥已对照实际代码实证**
- **实证机理**（与评审的 "open_refcount" 表述略有出入，以代码为准）：`get_or_build` 命中路径（`handle_workspace_cache.h:122-128`）只做 `touch_lru_`，**不恢复 `in_use=true`**。于是 `open(A)→close(A)→open(A)` 后 entry 保持 `in_use=false` 可逐出；`open(B)` 触发 evict → `destroy_handle()`；`submit(A)` 经 `tstate->dev_handle` 用已释放显存 → UAF。同文件并发 open 共享 cache_key（extent 签名），`in_use` 布尔本就扛不住——正确解是 open 引用计数
- **影响面：** cache 默认 OFF（`TUTTI_HANDLE_CACHE_CAP=0`），仅 cache ON 配置触发（tests 63-66、任何设 cap 的部署）
- **修复：** Entry 引入 `open_refcount`：`open()` 命中/新建均 ++；`close()` --；evict 仅当 `pin_count==0 && open_refcount==0`；`shutdown()` 按 open_refcount 结清；同思路核查 `PrpPageCache`（同模式）
- **验收：** 回归 `open(A)→close(A)→open(A)→open(B)→submit(A)`（cap=1，cache ON）字节级正确 + 同文件并发 open 场景；820/137/46/15 全绿

### P0-2 StripedDataPath op 未记录 memory token，direct unregister 可提前解除 in-flight DMA
- **来源：** ASSESSMENT P0-B（R15 F-008 旁证）；**总指挥已实证**：`OpEntry::memory_token`（`striped_data_path.h:228`）只有声明与默认值，全文件无赋值点，`memory_has_inflight_ops_()`（:1215）恒 false
- **现象：** `StripedDataPath::submit()` 未把 accepted request 的 memory token 写入 `OpEntry`，`memory_has_inflight_ops_()` 恒 false；`unregister_memory()` 在 striped op 在飞时直接 `nvm_dma_unmap`
- **修复：** 按 `DataPathMemoryToken` 收集 accepted tokens 存入 OpEntry；`memory_has_inflight_ops_()` 检查该集合
- **验收：** 2 个回归（unregister in-flight striped memory 拒绝/延迟；drain 后成功）+ 全套基线

### P0-3 submit 的 partial-commit 语义未在声明处警告
- **来源：** REVIEW P2-3（本轮 R14 S4 真实事故根因——示例静默丢 904/920 请求）
- **修复：** 在 `storage_runtime.h` submit 声明注释明确：返回值只覆盖"本轮提交"的请求，`initial_states` 必须逐一检查，拒绝请求须重投
- **验收：** 文档化即完成（半天，防数据丢失，建议与 P0-1/2 同批）

---

## P1 — 发布准备（对外使用前完成）

| # | 事项 | 来源 | 要点 | 验收 |
|---|---|---|---|---|
| P1-1 | striped:// `name` 路径组件校验 | R15 P1 | resolve 期拒绝 `.`/`..`/`/`/NUL/空（fail-closed 对齐 LocalFileResolver），不静默 sanitize | 非法输入负向契约 |
| P1-2 ◆ | 构建入口统一 | 全部评审 | 根 build（daemon 产线）vs standalone（开发测试）并存且 driver 参数漂移；**决策**：superbuild 统一 or standalone-only + 根 build 仅 daemon | 单一命令可复现全部工件；README 重写 Quick Start |
| P1-3 ◆ | GPU 可移植性定位 | AUDIT §11、R15、REVIEW | 目标架构承诺 MACA/MUSA 但只有 CUDA/HOST；**决策**：写死 NVIDIA-only or 立项 vendor abstraction；最小动作：`data_paths/` 6 处直接 `#include <cuda_runtime.h>` 改走 `cuda_like.h` 统一报错 | 定位写入 README + 目标架构 D5 消解 |
| P1-4 | kernel 双 baseline 行为对齐 | AUDIT §9 | 5.4 树 3 个修复（queue DMA、__free_pages、rq_data_dir）未回填 5.15；5.15 从未 insmod | 修复回填 + 每 baseline insmod 冒烟门禁 |
| P1-5 | 测试 seam 收敛（开源卫生） | AUDIT §8 | `LocalNvmeDataPath` 约 30 个 `test_*` 公有方法混入生产头 | `TestAccess` friend 模式或等价收敛，公有面只留真 API；契约全绿 |

---

## P2 — 契约完整性与打磨

| # | 事项 | 来源 | 要点 |
|---|---|---|---|
| P2-1 ◆ | batch open 决策 | AUDIT §10、REVIEW P2-2 | open 走 ioctl 无快路径；**决策**：实现 `open_batch()`（sync driver `populate_multi` 已有但仅 test seam 可达）or 明确不支持并写契约 |
| P2-2 ◆ | 跨 striped-target batch 上限 | R15 P2 | 每 submit 仅 1 个 striped target（超出 per-request 拒绝）；**决策**：保留上限写契约 or (target,shard) 去重表 |
| P2-3 | 注释/文档漂移清理 | 全部评审 | handle_workspace_cache.h batch/concurrency 注释、MetadataArena::Config num_slots 注释（现=in-flight 语义）、PORTING.md 旧路径、示例变量名/头注释/单文件拆分、tests/ 布局说明 |
| P2-4 | 硬件测试目录参数化 | REVIEW P2-1 | `/mnt/nvme1,2` 硬编码 → env var |

---

## P3 — 机会型优化（不阻塞）

submit fan-out per-request vector 池化（AUDIT §7）；`submit_read_one/write_one` 合并（opcode 参数）；striped fan-out 部分 entry 拒绝时的回滚防御（当前不可达但脆弱）；`aggregate_completion_status_` 全成功快路径跳过 entries D2H；`find_free_*_slot_` 线性扫描 → free 栈；`QueueAcquireHelper` 常量偏移混淆简化；simulator 真三重叠指标化（6 指标框架已含）；**kernel 队列/性能测量**（队列 2→16 扫描、双盘 ~2× 差距定位——maintainer 2026-08-03 暂缓，保留候选）。

---

## 建议阶段（每阶段独立验收）

```text
Phase A（稳定性门禁）: P0-1 + P0-2 + P0-3
  Gate: 两项 P0 回归测试全红→绿；820/137/46/15 不劣化
Phase B（发布准备）:   P1-1、P1-2◆、P1-3◆、P1-4、P1-5
  Gate: 单一构建命令可复现；README/目标架构文档同步（current-structure.md §3 偏差全部消解）
Phase C（契约决策）:   P2-1◆、P2-2◆、P2-3、P2-4
Phase D（打磨）:       P3 全项，按价值择取
```

**显式暂缓（maintainer 决策，非待办）：** Framework Adapter（`doc/history/chat/round12/deferred-adapter/`）；kernel strategy 性能基准（见 P3 末项）。
