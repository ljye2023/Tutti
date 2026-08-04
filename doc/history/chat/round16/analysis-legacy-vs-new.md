# 新旧架构对比分析：S4（重构后）vs legacy kv_cache_layerwise_overlap

**日期：** 2026-08-03（总指挥）
**对象：** 新 = `tutti/` 重构栈（StorageRuntime + LocalNvmeDataPath/StripedDataPath，S4 simulator 路径）；旧 = `third_pkgs/Tutti/examples/adapters/kv_cache_layerwise_overlap.cu` + Coordinator/memory/io_engine/block_storage。
**方法：** 双侧源码精读（legacy：`kv_cache_io_adapter.cpp`、`coordinator.cu`、`memory/include/host_device_memory_subsystem.h`、`io_engine/src/local_nvme/nvme_batch_xfer_kernel.cu`；新：已验收的 R14-R16 全部实现）。

---

## 1. 内存注册

| 维度 | legacy | 新架构 |
|---|---|---|
| 入口 | `Coordinator::register_tensor(spec)`（幂等，同 buffer 重注册返回同 region） | `StorageRuntime::register_memory(MemoryView)` → 显式 token，unregister 对称 |
| DMA 映射时机 | 注册时隐式完成（nvm_dma_t 随 region 持有，unregister 释放） | 注册时一次 `nvm_dma_map`，token 复用到 unregister |
| PRP 计算时机 | **注册时**预展开：tensor 按 MDTS 粒度切成 PRPMappingEntry 子片（IoSliceView），IO 时按索引引用 | **每次 IO** 按 extent/MDTS fan-out 现算，PrpPageCache 缓存页（R16 S1 后带 checkout_refcount）；arena 每槽预分配 PRP 页 |
| 设计权衡 | 注册重、IO 轻（适合同 buffer 反复 IO） | 注册轻、IO 有少量 host fan-out（被 cache 摊薄）；换来注册语义简单 + per-IO 灵活性 |

**结论：语义等价，时机不同。** 都是"注册一次、长期复用"；legacy 把 PRP 展开前置到注册期，新架构摊到 IO 期并用 cache 收敛。KV 场景（buffer 长期不变）两者成本都只在注册/首次 IO 付一次。

## 2. 元数据管理

| 维度 | legacy | 新架构 |
|---|---|---|
| extent 解析 | block_storage `open_gpu_files_batch` 时 FIEMAP，存入 GpuFile 的 shard 表 | LocalFileResolver resolve 时 FIEMAP 一次，产出**不可变 ResolvedTarget**（pair-private payload + lease） |
| GPU 侧元数据 | Coordinator 透明 handle cache（按 GpuFileId 键）+ memory/tiered_handle_cache（L1/L2 双层） | HandleWorkspaceCache（**单层** LRU，按 extent 签名键，同文件去重；R16 S1 后 open_refcount 修 UAF） |
| stripe 元数据 | **在 kernel 里算**：`gpu_file_resolve(tensor_size, ...)` 按 tensor 粒度选 shard | **在 resolver 算**：StripedResolver open 时产出 N shard 映射，kernel 只按 entry 查表 |
| 错误行为 | kernel 内防御性 `printf + skip entry`——**静默丢数据的通道**（错只进 dmesg） | resolve/open 期 fail-closed；submit 期 per-request 状态；不存在"跳过" |

**结论：三层差异。** ①新架构把元数据变成显式契约对象（lease/RAII），legacy 藏在 Coordinator 全局表；②单层 cache 是对 legacy 双层 tiered 的刻意简化（已验收）；③**stripe 决策从 kernel 挪到 resolver**——legacy 的 kernel 内 stripe 选择正是 `10602fc` stripe 粒度 bug 的温床，新架构结构性消除该类 bug，且消掉了 kernel printf-skip 这条静默失败路径。

## 3. IO 效率

| 维度 | legacy | 新架构 |
|---|---|---|
| 每层提交 | `batched_read` → resolve_handles + submit_chunked 按 `max_entries_per_batch` 切块，每块一次 H2D + **一次 launch**（容量够时整块 = 1 launch/层） | `rt->submit` → Runtime 按 DataPath 分组合并 → 一次 H2D + **一次 launch**（R15 S3 起；S4 实测 instrumentation：437 calls = 437 rounds = 每层单 launch） |
| kernel 线程模型 | **一线程一 entry**（`blocks=(count+tpb-1)/tpb`，tid 越界即返） | **一线程一 entry**（submit_one.cuh 同源）——两侧源码逐行确认，模型相同 |
| 容量语义 | v0.1：单 tensor entries > max_entries 直接**硬失败**（不可拆分） | 容量参数化（S4），超限 per-request 拒绝 + 窗口化重投，不硬失败 |
| 实测 | ~7 GB/s（单盘，注释/历史数据） | 6.9 GB/s（单盘，S4 HY3 全量实测，盘饱和） |

**结论：打平。** 线程模型、launch 次数、H2D 次数同阶；实测带宽相等（单盘物理上限）。差异只在容量超限时的语义（硬失败 vs 窗口化）。

## 4. IO 行为

| 维度 | legacy | 新架构 |
|---|---|---|
| 完成模型 | stream 有序：submit_batch 入队即返，完成靠 cudaStreamSynchronize/event 对 | handle 异步：submit→IoHandle，wait(timeout)/query/release_io；event fence 挂 caller stream |
| 部分失败 | 无概念：bool 过/败；kernel 内 printf-skip 静默丢 entry | **partial-commit**：per-request ACCEPTED/REJECTED（背压 RESOURCE_EXHAUSTED），调用方必须遍历 initial_states 窗口化重投（R14 S4 事故的教训，已在 S4 simulator 落地为 windowed_submit_wait） |
| 读写并发 | 示例不能（共享 scratch buffer） | 能（每 op 独立 workspace，无共享 scratch；S4b 已实证交错流水线） |
| 错误报告 | bool + stderr printf | 分层 Status：observation_status（观察通道）/ per-request state（IO 结果），R11 起契约化 |
| 生命周期 | Coordinator 全局管理 | close BUSY 检查、drain、shutdown 配额结清（契约测试覆盖） |

**结论：新架构语义显著更严**（partial-commit + 分层错误 + 生命周期契约），代价是调用方义务更重（必须处理被拒请求——这是唯一一处"重构后比 legacy 难用"的点，由窗口化 helper 收敛）。

---

## 总判定

重构**没有引入性能回退**（效率打平、线程模型同源）**，语义无丢失**（注册/元数据/提交能力一一对应），且在四处**严格更好**：①stripe 决策出 kernel（消 bug 类）；②静默失败通道关闭（fail-closed）；③部分失败显式化（legacy 的 printf-skip 在 KV 场景就是丢数据）；④读写可并发。唯一增加的调用方负担是 partial-commit 的窗口化义务——已由 S4 simulator 的 `windowed_submit_wait` 提供范式。

**对 S4（striped 4 盘）的含义**：legacy 的 kernel 内 stripe（gpu_file_resolve）只支持"单文件跨 shard"模型；新架构 StripedResolver + fused kernel 是它的严格超集（N 设备表 + per-entry shard 索引）。S4 的 N=4 性能塌陷根因排查**不需要怀疑模型能力差异**——模型在 legacy 已验证可达盘速，问题定位应聚焦新 fused kernel 的实现细节（SQ/CQ 共享、跨 BAR 路径、PRP 开销），与 session4.md 的嫌疑清单一致。
