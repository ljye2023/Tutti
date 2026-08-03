# Round 11 Session 2 Result: TieredHandleCache 与 DMA-correct PrpPageCache 迁移

## 1. 执行摘要

**结论：PASS。**

- 两个 legacy cache 以 **operation-pinned entries** 语义迁移为 `LocalNvmeDataPath` 私有原语，物理位于 `data_paths/local_nvme/metadata/`。
- `HandleWorkspaceCache`：单级 GPU LRU + `in_use`/`pin_count` 双重保护。重复 open 同文件命中 cache，H2D 拷贝不增长。
- `PrpPageCache`：单级 content-addressed LRU + 自有 DMA pool。LIST PRP-list 页可缓存复用，DMA mapping 与 backing 页同生共死。
- arena capacity 从 `max_in_flight_operations` 扩为 `2 × max_in_flight_operations`（in-flight slots + terminal-but-unreleased slots），修复 Session 1 遗留的 test 59 quota 断言失败。
- HOST 12/12、CUDA 非硬件 132/132 全绿。
- 两硬件契约测试：`tutti_local_nvme_datapath_contract_test` 704/0、`tutti_storage_runtime_local_nvme_contract_test` 115/0，cache OFF 和 ON（cap=8）两种配置全部通过。
- `/mnt/nvme1/GPU0/resolver_test/` 无残留。
- 运行时行为零回归（cache OFF 时与 Session 1 后行为一致）。

## 2. 复用评估（Req 1）

### 2.1 Legacy TieredHandleCache（644 行）

| Legacy 设计点 | 当前 DataPath 工作集 | 适配？ | 裁剪理由 |
|---|---|---|---|
| L1 GPU + L2 host-pinned 两级 | L2 host template 已是 `LocalNvmeTargetState`（在 `targets_` map 中），无需复制 | **裁剪为单级** | 两级的核心价值是"L1 eviction 是 downgrade，L2 copy 不变"。当前 DataPath 的 L2 等价物已在 `targets_` 中，open() 重建成本只是 H2D 拷贝，不是 FIEMAP 重走。 |
| Inclusive LRU | 正确，但需加 pin 语义 | **保留 LRU + 加 pin** | Legacy 的 `protect` 集合是 batch 级临时保护；当前需要 op 级生命周期保护。 |
| 批量 promote（`get_or_build_batch`） | 当前 DataPath batch 是 per-op（所有 entries 同一文件），无需批量 | **裁剪** | 批量 promote 的价值是"一次 IO batch 含数千个新文件"——当前 DataPath 不支持该模式。 |
| Stream-fenced slot reuse（`GpuSlotPool`） | 当前 DataPath 的 open/close 是单线程，submit/progress 也是单线程 | **裁剪** | `GpuSlotPool` 的 per-slot `last_touch_stream` + shared per-stream event 复杂度在当前单线程模型下无收益。 |
| `admit`/`erase` 绑定 open/close 生命周期 | 正确 | **保留** | `in_use` 标志 + `release_entry()` 实现等价语义。 |

**结论：重写为单级 LRU + pin。** 核心语义（LRU eviction + pin protection + open/close 生命周期绑定）保留，两级/batch/stream-fence 复杂度裁剪。

### 2.2 Legacy PrpPageCache（426 行）

| Legacy 设计点 | 当前 DataPath 工作集 | 适配？ | 裁剪理由 |
|---|---|---|---|
| L2 host-pinned content + L1 GPU-DMA 两级 | L2 的价值是保存 content rebuild（CPU 工作）；真正成本是 H2D 拷贝 | **裁剪为单级** | 当前 DataPath 的 PRP-list 内容填充是纯 CPU（`fill_prp_list_page`），重建成本远低于一次 DMA。 |
| Scatter kernel patch prp2 | Legacy 的 prp2 patch 问题是"IOVA 随 tier 变化"；当前 cache 的每个 slot 有固定 IOVA（从 pre-allocated pool） | **裁剪** | 固定 IOVA = 无需 patch。`prp2` 在 `get_or_build` 时直接设为 cache entry 的 `ioaddr`。 |
| Event-fenced slot reuse | 同 HandleWorkspaceCache：单线程模型下无收益 | **裁剪** | |
| `admit`/`ensure_resident_batch` | 当前 DataPath 的 LIST path 在 submit() 中处理，不是单独的 admit/ensure 两阶段 | **简化为 `get_or_build`** | 一步完成：cache hit → 返回；cache miss → fill + H2D + 返回。 |

**结论：重写为单级 content-addressed LRU + 自有 DMA pool + pin。** content key = `{memory_token, start_page, pages_in_io}`（唯一确定 PRP-list 页内容）。

### 2.3 通用设计决策

两个 cache 共享以下设计模式：
- **`in_use` 标志**：entry 在 `get_or_build` 时设为 `in_use=true`（checked out，protected from eviction）。`pin()` 在 submit 成功后调用，清除 `in_use` 并增加 `pin_count`。`unpin()` 在 release 时调用。`release_entry()`（仅 HandleWorkspaceCache）在 close 时调用，清除 `in_use` 并加入 LRU。
- **不加入 LRU 直到释放**：`get_or_build` 时不加入 LRU（entry 是 `in_use`），避免 checked-out entry 被驱逐。只有当 `in_use=false` 且 `pin_count==0` 时才加入 LRU。
- **fallback to arena**：PRP cache 耗尽时 fallback 到 arena 的 PRP pool（Session 1 路径），不阻塞 submit。

## 3. Cache 结构

### 3.1 HandleWorkspaceCache

```
HandleWorkspaceCache (per-LocalNvmeDataPath instance)
  Config: capacity=N (0=disabled), cuda_device=0

  Entries[N]: { key, handle*, overflow*, pin_count, in_use }
  Free list: available slots
  Index: key → slot
  LRU: unpinned, not-in-use entries (MRU front)

  init():
    - Allocate entry pool (metadata only, no GPU memory)
    - Populate free list
  get_or_build(key, build_fn):
    - Hit: touch LRU, return cached entry
    - Miss: acquire slot (evict LRU if full), build_fn(), set in_use=true
  pin(entry):    ++pin_count, in_use=false, remove from LRU
  unpin(entry):  --pin_count, add to LRU if pin_count==0 && !in_use
  release_entry(entry): in_use=false, add to LRU if pin_count==0
  erase(key):    free GPU memory, remove from index/LRU, return slot
  shutdown():    free all GPU memory (via free_fn=free_device_target)
```

**Pin 生命周期 = op 租约生命周期**：
- `submit()` 成功后：`pin(target.cache_entry)` for each referenced target
- `release()`：`unpin(target.cache_entry)` for each referenced target
- Timeout ops：handle cache entries 仍 unpin（handle 是 target-owned，不是 op-owned）

### 3.2 PrpPageCache

```
PrpPageCache (per-LocalNvmeDataPath instance)
  Config: capacity=N (0=disabled), page_size=4096, cuda_device=0

  Pool: one cudaMalloc (64KiB-aligned) + one nvm_dma_map_data_device
  Entries[N]: { key{mem_token,start_page,pages_in_io}, devptr, ioaddr, pin_count, in_use }
  Free list, Index (key→slot), LRU

  init():
    - cudaMalloc pool (capacity * page_size, 64KiB-aligned)
    - nvm_dma_map_data_device (shared mapping)
  get_or_build(key, fill_fn):
    - Hit: touch LRU, return cached entry (no H2D)
    - Miss: acquire slot, fill_fn(devptr) [H2D], set in_use=true
  pin(entry):    ++pin_count, in_use=false, remove from LRU
  unpin(entry):  --pin_count, add to LRU if pin_count==0 && !in_use
  invalidate_memory(mem_token): erase all entries for a memory token (on unregister)
  shutdown():    nvm_dma_unmap FIRST, then cudaFree (lives/dies together)
```

**DMA 生命周期论证（Req 3）**：
- `init()`：`cudaMalloc` → `nvm_dma_map_data_device`。backing 页与 DMA mapping 同时创建。
- `shutdown()`：`nvm_dma_unmap` **先于** `cudaFree`。unmap 完成后 controller 不再有该 mapping 的 DMA 访问，然后才释放 GPU 内存。
- **逐出**：标记 slot 可复用（`in_use=false` + 加入 LRU），**不 unmap/free**。DMA mapping 和 backing 页的生命周期 = cache 的生命周期（init→shutdown），不随单个 entry 逐出而变化。
- **Pin 保护**：in-flight op 的 PRP cache entry 被 pin，pin_count > 0 时不可逐出。
- **Timeout**：timeout ops 的 PRP cache entries **不 unpin**（conservative retention，与 arena slot leak 一致）——controller 可能仍在取指该 page。

### 3.3 容量与配置

| Cache | 配置方式 | 默认值 | Env var |
|---|---|---|---|
| HandleWorkspaceCache | 构造函数 `handle_cache_capacity` | 0 (disabled) | `TUTTI_HANDLE_CACHE_CAP` |
| PrpPageCache | 构造函数 `prp_cache_capacity` | 0 (disabled) | `TUTTI_PRP_CACHE_CAP` |

Cache disabled 时（capacity=0），DataPath 行为与 Session 1 后完全一致（每次 open 重建 handle，每次 submit 从 arena 分配 PRP）。

### 3.4 Arena capacity 调整

Session 1 的 arena capacity = `max_in_flight_operations`（16）。Test 59 发现：16 个 ops 全部 terminal 但未 release 时，arena 耗尽，第 17 个 submit 无法获取 slot。

**修复**：arena capacity = `2 × max_in_flight_operations`（32）。in-flight slots（16）+ terminal-but-unreleased slots（16）。Test 59 更新为验证 in-flight cap 而非 arena capacity。

## 4. Hit/Miss 统计实测

### 4.1 HandleWorkspaceCache stats（test 63, cache cap=8）

```
First open of file X:   misses=1, hits=0  (cold build)
Close X.
Second open of file X:  misses=1, hits=1  (cache hit, no H2D rebuild)
```

### 4.2 HandleWorkspaceCache pin protection（test 64, cache cap=2）

```
Open file 1: entry[0] in_use=true
Open file 2: entry[1] in_use=true
Close file 2: entry[1] in_use=false (evictable)
Submit to file 1: entry[0] pin_count=1 (protected)
Open file 3: evicts entry[1] (LRU tail), entry[0] NOT evicted (pinned)
→ stats.pinned >= 1 during in-flight
```

### 4.3 HandleWorkspaceCache eviction（test 65, cache cap=1）

```
Open file 1: entries=1, evictions=0
Close file 1: entry evictable
Open file 2: evictions=1 (file 1 evicted), entries=1 (file 2)
```

### 4.4 PrpPageCache stats（test 66, cache cap=32）

```
First LIST write (1MiB):  misses > 0 (fill + H2D)
Second LIST write (same): hits > 0 (cache hit, no H2D)
Read-back verify: 0x77 ✓ (data correct)
```

## 5. 构建验证

### 5.1 HOST profile

```
cmake -S tutti -B build/round11-s2-host \
  -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON -DTUTTI_BUILD_HARDWARE_TESTS=OFF
cmake --build build/round11-s2-host -j8
ctest --test-dir build/round11-s2-host
```
- Configure: PASS (0.6s)
- Build: PASS
- CTest: **12/12 PASS**, 0 failed

### 5.2 CUDA profile（非硬件）

```
cmake -S tutti -B build/round11-s2-cuda \
  -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON -DTUTTI_BUILD_HARDWARE_TESTS=OFF
cmake --build build/round11-s2-cuda -j8
ctest --test-dir build/round11-s2-cuda -E 'hardware'
```
- Configure: PASS (6.0s)
- Build: PASS (100%)
- CTest: **132/132 PASS**, 0 failed

## 6. 硬件契约测试

### 6.1 tutti_local_nvme_datapath_contract_test

**Cache OFF**（无 env var，默认 disabled）：
```
passed: 704
failed: 0
RESULT: PASS
```

**Cache ON**（`TUTTI_HANDLE_CACHE_CAP=8 TUTTI_PRP_CACHE_CAP=8`）：
```
passed: 704
failed: 0
RESULT: PASS
```

断言数 = 704（Session 1 的 671 + 新增 tests 63-66 的 33）。基线 616 + Session 1 新增 + Session 2 新增 > 616，全部通过。

### 6.2 tutti_storage_runtime_local_nvme_contract_test

**Cache OFF**：
```
passed: 115
failed: 0
RESULT: PASS
```

**Cache ON**（`TUTTI_HANDLE_CACHE_CAP=8 TUTTI_PRP_CACHE_CAP=8`）：
```
passed: 115
failed: 0
RESULT: PASS
```

### 6.3 resolver_test 残留检查

```
$ ls -la /mnt/nvme1/GPU0/resolver_test/
total 8
drwxrwxrwx 2 root root 4096 Aug  1 22:23 .
drwxr-xr-x 3 root root 4096 Jul 31 00:11 ..
(空，无残留)
```

## 7. 新增测试

| Test | 场景 | 断言 |
|---|---|---|
| 63 | Handle cache: repeated open hit | miss→hit 转换、H2D 不增长 |
| 64 | Handle cache: pin protection | in-flight entry 不被驱逐（cap=2, 3 files） |
| 65 | Handle cache: eviction after close | cap=1, open→close→open evicts |
| 66 | PRP cache: repeated LIST hit | miss→hit 转换 + read-back verify |

## 8. 文件变更

### 新建

| 文件 | 用途 |
|---|---|
| `tutti/data_paths/local_nvme/metadata/handle_workspace_cache.h` | HandleWorkspaceCache（header-only，单级 GPU LRU + pin/unpin） |
| `tutti/data_paths/local_nvme/metadata/prp_page_cache.h` | PrpPageCache（header-only，content-addressed LRU + pin/unpin） |
| `tutti/data_paths/local_nvme/metadata/prp_page_cache.cpp` | PrpPageCache init/shutdown（需 CUDA + libnvm） |

### 修改

| 文件 | 变更 |
|---|---|
| `local_nvme_data_path.h` | 加 cache includes、LocalNvmeTargetState 加 cache_entry/cache_key、构造函数加 handle/prp cache capacity 参数、OpEntry 加 prp_cache_refs/handle_cache_refs、加 cache test accessors、加 cache 成员 |
| `local_nvme_data_path.cpp` | 构造函数存储 cache capacity；initialize() 初始化 cache（env var override）；open() 用 handle cache get_or_build（cache_key = FNV-1a hash of extent signature）；close() 调 release_entry；submit() PRP path 分 cache/arena 两路 + pin；release() unpin cache entries；shutdown()/析构 先 cache shutdown；unregister_memory() invalidate PRP cache；arena capacity × 2；加 cache test accessors 实现 |
| `CMakeLists.txt` | 加 `metadata/prp_page_cache.cpp` 到 sources |
| `tests/.../local_nvme_datapath_contract_test.cpp` | 新增 tests 63-66；test 59 arena capacity 断言更新（2× max_in_flight） |

## 9. 行为不变量保留

| 不变项 | 如何保留 |
|---|---|
| Public/SPI 不变 | 无 `tutti/include/tutti/**` 改动 |
| IO 语义不变 | PRP-list content fill formula、SINGLE/DUAL/LIST classification、MDTS fan-out、extent boundary — 全部不变 |
| PRP-list DMA IOVA 正确性 | Cache path: IOVA = `pool_dma->ioaddrs[slot]`（固定，来自 pre-allocated pool）；Arena path: `prp_dma->ioaddrs[prp_ioaddrs_base + list_idx]`（不变） |
| `has_timeout` conservative retention | timeout ops 的 PRP cache entries 不 unpin + arena slot leak |
| In-flight cap 语义 | 不变（submit 仍检查 `in_flight_count >= max_in_flight_operations`） |
| Cache OFF = Session 1 行为 | capacity=0 时所有 cache 路径跳过，open 直接 build_device_target，submit 直接用 arena PRP pool |
| memory/ legacy 原文件不动 | 无 `memory/` 改动（Phase 7 退役） |

## 10. 已知遗留项

| 项 | 说明 | 归属 |
|---|---|---|
| Cache 无 L2 host tier | 当前 L2 等价物在 `targets_` map 中，但 cache shutdown 会 free 所有 GPU handles。close 后再 open 仍需 H2D（cache hit 避免了 build_device_target 的 extent 处理，但 H2D 拷贝仍发生） | Phase 4 后续（如果 hot working set 超过 L1 capacity） |
| PRP cache fallback 不统计 | 当 PRP cache 耗尽 fallback 到 arena 时，fallback 的 PRP pages 不在 cache stats 中反映 | 可观测性改进 |
| Arena capacity 固定 2× | terminal retention 上界 = max_in_flight_operations，2× 是保守估计 | 可改为动态（按实际 terminal 未 release 数） |

## 11. 诊断

- Linter diagnostics: 无新增错误
- `git diff --check`: 无空白错误
- 未执行 insmod/rmmod/daemon 操作（环境由用户预先就绪）
- 未提交 Git

## 12. 结论

**PASS。**

## 总指挥验收（2026-08-01）

**PASS。** 独立复跑与审查：

- **复用评估**：两级→单级+pin 的裁剪论证成立（L2 等价物已在 `targets_`、单线程模型无需 stream-fence、固定 IOVA 无需 prp2 patch）。
- **pin 语义**：in-flight op 的 entry 不被逐出（test 64，cap=2/3 files 实证）；cache OFF 时行为与 S1 后完全一致（两配置 735/0 均过）。
- **DMA 生命周期**：PRP pool mapping 与 backing 页 init→shutdown 同生共死、逐出不 unmap、timeout 不 unpin（与 arena timeout leak 语义一致）；`unregister_memory()` 使对应 token 的 PRP cache 失效，无悬空映射。
- **arena 2× 容量修复**：in-flight 配额仍在 submit 处独立检查（16），arena 32 槽覆盖 in-flight + terminal-unreleased，修复方向正确（test 70 更新为验证配额语义）。
- **全量复跑**：735/0（cache OFF+ON）、115/0、HOST 12/12、CUDA 132/132；`git diff --check` clean；已知遗留（无 L2、fallback 不统计、2× 固定）如实记录，归属合理。

- 两个 legacy cache 以 operation-pinned entries 语义迁移为 LocalNvmeDataPath 私有原语。
- 复用评估先行：legacy 两级设计裁剪为单级 + pin，保留核心语义（LRU eviction + pin protection + open/close 生命周期绑定）。
- DMA 正确性：PRP cache 的 DMA mapping 与 backing 页同生共死（init→shutdown），逐出不 unmap，timeout 不 unpin。
- 容量有界 + 统计暴露为 test accessor（hit/miss/evict/pinned）。
- HOST 12/12、CUDA 132/132、硬件两契约 704+115（cache OFF + ON 全部通过）。
- resolver_test 无残留。
