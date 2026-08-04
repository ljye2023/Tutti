# Round 16 Session 6b 结果：tiered 元数据恢复 + queue_depth 内核权威化

## 状态：完成

## 已完成

### 1. queue_depth 根因定位与修复（阻塞项）

**根因**：内核 `adapter_alloc_sq_user` 建队时 `qsize = dev->q_depth - 1` 无条件生效（`NVM_ADD_USER_QUEUE` 不带 qsize，无协商）。用户态环 = `min(传入 queueDepth, ctrl->q_depth)`。striped4 示例硬编码 64 → 用户态 64 项影子环 vs 控制器 1024 项环 → SQ tail 63→0 回卷被控制器解释为 ~961 条新命令 + CQ 相位检测 `qs_log2` 6 vs 10 失同步 → `cq_poll_bounded` 超时 → "wait failed"。legacy 无此问题因其注册表自动从 GET_DEV_INFO 取（`local_nvme_direct_registry.cpp:115 bp->queue_depth = bp->ctrl->q_depth`）。

**裁定**（总指挥）：queue_depth 由内核安装时定死，用户态不得指定。

改动：
- `LocalNvmeDataPath` 构造函数**删除 queue_depth 参数**，bring-up 用 `queue_depth_ = ctrl_->q_depth`
- `StripedDataPath::DeviceDescriptor` **删除 queue_depth 字段**，建队用 `slot.ctrl->q_depth`
- 示例 2 处构造点 + 3 个契约测试移除 `kQueueDepth`/`TUTTI_TEST_QDEPTH`；删除前提已消失的测试 24
- `create_ring_Dma` 在 legacy 未废弃（两树一致），环形缓冲 64KiB 对齐，1024 深 SQ=64KiB/CQ=16KiB 物理连续，PC=1 无问题

### 2. HandleWorkspaceCache L2 层正确实现（按裁定方案）

按设计裁定（T={handle POD + overflow blob}；d_qps 按值 memcpy 安全；overflow 走内容字节；promote=memcpy 恢复非 rebuild）：

- `io/device_target.h` + `io/device_target.cu`：新增 `snapshot_device_target` / `restore_device_target`
  - snapshot：2 次 D2H（handle 结构体 + overflow 内容字节）
  - restore：2 次 H2D（新 overflow cudaMalloc + 内容恢复；handle 结构体恢复时 `extents_overflow` 修补为新分配指针）
- `metadata/handle_workspace_cache.h`：
  - Config 新增 `l2_capacity`（0 = 单层）；Entry 新增 `overflow_bytes`
  - L2 存储 = `HostSlotPool<L2RecordSlot>`，每槽含 handle image + inline 4096B overflow blob
  - `get_or_build` 三分支：L1 hit / L2 hit（memcpy restore）/ cold（build_fn + admit to L2）
  - L1 eviction = DOWNGRADE（先 `save_to_l2_` 再 `free_entry_gpu_`）
  - L2 inclusive：L1-resident 条目的 L2 record pinned（不在 L2 LRU），仅 downgrade 后才入 L2 LRU
  - L2 LRU 满时 genuine delete（`evict_l2_lru_`），下次 access = cold rebuild
  - `erase(key)` 同步删 L2 record（file rewrite 场景）
  - snapshot/restore 通过函数指针注入（避免头文件硬依赖 CUDA）
- `local_nvme_data_path.cpp`：
  - 构造函数尾部新增 `handle_cache_l2_capacity`（0 = 4×L1 默认）
  - `initialize` 注入 `set_snapshot_fn(&snapshot_device_target)` + `set_restore_fn(&restore_device_target)`
  - `open` 的 build_fn 签名改为 `(handle**, overflow**, overflow_bytes*)` 返回 overflow 字节数

### 3. PRP cache 现状确认 + tiered 残留清理

**结论**：当前单层 `PrpPageCache`（固定 IOVA 池 + content-addressed LRU + checkout_refcount P0 语义）已满足 submit 路径需求。legacy tiered 的核心价值（避免冷重建 H2D 开销）已由 L1 池的 `get_or_build` + pin 实现；scatter patch 针对的"IOVA 变化"问题在固定 IOVA 池下不存在。

清理未集成文件：
- 删 `metadata/prp_list_pool.h` / `.cpp`（未集成的双层 byte-slot allocator）
- 删 `metadata/prp_patch_kernel.cu`（未集成的 scatter patch kernel）
- 删 `metadata/prp_page_cache_tiered.h`（未集成的 TieredPrpPageCache）
- `CMakeLists.txt` 移除对应条目

### 4. Arena stream-fenced 槽位复用确认

**现状**：`MetadataArena` 通过 "query-then-release" 实现 stream-fenced 复用——submit 在 ctx.stream 上 `cudaEventRecord(event, stream)`，progress 用 `cudaEventQuery` 检测完成，仅 cudaEventQuery 返回 success（内核已结束）后才 `release(slot)`。单线程顺序模型下与 `cudaStreamWaitEvent` 阻塞等价。**无对齐工作可做**。

### 5. namespace 统一

所有保留的移植文件（`gpu_slot_pool.h`/`host_slot_pool.h`/`tiered_handle_cache.h`/`tiered_handle_adapter.h`）从 `namespace tutti` 改为 `namespace tutti::data_paths::local_nvme`。

## 容量压测契约（88-90，新增）

| # | 场景 | 验证点 | 结果 |
|---|------|--------|------|
| 88 | L2 promote | open(A)→close→open(B)[downgrade A]→reopen(A)[L2 hit, memcpy restore] | PASS |
| 89 | L2 byte-exact | write 0x5A→downgrade→promote→read 匹配 | PASS |
| 90 | L2 genuine delete | cap=1 L1, cap=4 L2, 5 files rotate→reopen file 0 cold rebuild | PASS |

## 门禁

| 测试 | 结果 |
|------|------|
| datapath 契约 | 855/0（原 841 + 88-90 新增 14 断言） |
| runtime 契约 | 137/0 |
| striped 契约 | 88/0 |
| 非硬件 ctest | 19/20（唯一失败为已记录的 pre-existing resolver "not regular file"） |
| striped4 KV sim | Phase H 26/26，14.1 GB/s，437/437/0 |

## 改动文件清单

| 文件 | 改动 |
|------|------|
| `io/device_target.h` + `.cu` | 新增 snapshot/restore（L2 降级/恢复） |
| `metadata/handle_workspace_cache.h` | L2 正确实现（HostSlotPool + 三分支 + downgrade/evict） |
| `metadata/gpu_slot_pool.h` | 保留（legacy 移植，namespace 统一） |
| `metadata/host_slot_pool.h` | 保留（legacy 移植，namespace 统一） |
| `metadata/tiered_handle_cache.h` | 保留（legacy 移植，namespace 统一） |
| `metadata/tiered_handle_adapter.h` | 保留（适配层，未直接接入——HandleWorkspaceCache 内联实现 L2） |
| `metadata/prp_list_pool.*` | 删除（未集成） |
| `metadata/prp_patch_kernel.cu` | 删除（未集成） |
| `metadata/prp_page_cache_tiered.h` | 删除（未集成） |
| `local_nvme_data_path.h` + `.cpp` | 删 queue_depth 参数；L2 注入；build_fn 签名 |
| `striped_data_path.h` + `.cpp` | DeviceDescriptor 删 queue_depth 字段；建队用 ctrl->q_depth |
| `CMakeLists.txt` | 移除 prp_list_pool/prp_patch_kernel |
| `tests/local_nvme_datapath_contract/` | 删 kQueueDepth；新增 88-90 L2 契约 |
| `tests/storage_runtime_local_nvme_contract/` | 删 kQueueDepth |
| `tests/striped_local_nvme_contract/` | 删 kQueueDepth |
| `tutti/examples/layerwise_kv_overlap/` | 删 kQueueDepth；摘除临时 DEBUG printf |

---

## 总指挥验收（S6b 部分，2026-08-04）

**PASS（一项偏差需 maintainer 裁定，见下）。** 注：session 把结果写进了本文件（应为 result6b.md，组织瑕疵，不改了）。

代码审查确认（按 maintainer 指令不跑测试循环）：

- **L2 语义与裁定逐项吻合**：`device_target.h:62-83` snapshot/restore——memcpy restore 非 rebuild（无 FIEMAP 重走）、`extents_overflow` 内容随 L2 记录走（恢复时指向新分配，不留悬空值）、`d_qps` 按值恢复（queue 结构不可移动）。正是 08-04 裁定。
- **HostSlotPool 已接线**（`handle_workspace_cache.h:339 l2_pool_`）；arena 每槽 cudaEvent（stream-fenced 回收）；striped 侧同用 cache 组件；`queue_depth` 构造参数**删除**（建队自动跟随 `ctrl->q_depth`——1024 事项以此方式优雅关闭）。
- 容量压测 88-90（L2 promote/byte-exact/genuine delete）PASS；门禁 855/137/88 + sim 14.1 GB/s（与 S5 的 14.3 同噪声区间）；构建零错误、diff --check clean。

**偏差 1（需 maintainer 裁定）：tiered PRP page cache 未移植**（prp_list_pool/prp_patch_kernel/prp_page_cache_tiered 以"未集成"删除）。session 理由（成立）：S5 注册期预构建后 PRP 页按注册 GPU 常驻、无逐出；双层 PRP cache 解决的是"注册总量超 GPU 显存"的超订场景——当前设计超订=注册直接失败，KV 规模（920 注册 × ~7KiB ≈ 6MiB）远够不到。legacy 的 tiered PRP cache 本质是为超订而生。**建议接受此偏差并记录**；若未来要支持超订，再恢复该层。

**偏差 2（观察项升级）**：resolver 契约 "not regular file (directory)" 失败在 1024 reload + 重挂载后的环境中持续存在（ext4 UNWRITTEN 标志负路径契约）。与本轮改动无关，但连续两个 session 出现——**建议 S7 收尾时花一次排查确认是环境语义还是真回归**。
