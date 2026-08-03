# Round 11 Session 1 Result: MetadataArena and Per-Operation Lease

## 1. Summary

**PASS (build-verified; hardware tests pending user execution)**

Replaced per-op `cudaMalloc`/`cudaEventCreate` in the `LocalNvmeDataPath::submit()` hot path with a per-device, bounded `MetadataArena`. All per-op workspace (event, entry array, status array, PRP-list pages) is pre-allocated at `initialize()` time and leased per-op. Arena exhaustion returns `RESOURCE_EXHAUSTED` (no fallback to cudaMalloc). Timeout ops' slots are permanently leaked (bounded, conservative retention).

## 2. Arena Design

### 2.1 Slot Layout

```
MetadataArena (per-device, per-LocalNvmeDataPath instance)
  Config: num_slots=16 (=max_in_flight_operations), max_entries_per_slot=256 (=max_batch_entries),
          page_size=4096 (NVMe page size), cuda_device=0

  Pre-allocated at init():
    events_[16]          — 16 cudaEvent_t (cudaEventDisableTiming)
    d_entries_pool_      — 1 cudaMalloc: 16 * 256 * sizeof(DeviceSubmitEntry)
    d_status_pool_       — 1 cudaMalloc: 16 * 256 * sizeof(EntryCompletionStatus)
    prp_raw_             — 1 cudaMalloc: (16 * 256 * 4096 + 65535) & ~65535 + 65536 (64KiB-aligned)
    prp_dma_             — 1 nvm_dma_map_data_device on prp_aligned_

  Total init-time CUDA allocations: 3 cudaMalloc + 16 cudaEventCreate + 1 nvm_dma_map
  Total hot-path CUDA allocations: 0 (acquire/release are pure CPU free-list ops)

  Slot i workspace:
    event          = events_[i]
    d_entries      = d_entries_pool_ + i * 256
    d_status        = d_status_pool_  + i * 256
    prp_pages       = prp_aligned_     + i * 256 * 4096
    prp_ioaddrs[j] = prp_dma_->ioaddrs[i * 256 + j]  (j = 0..total_list_ios-1)
```

### 2.2 Alignment

- Entry/status pools: no alignment requirement (consumed by GPU kernel only).
- PRP-list pool base: 64 KiB-aligned (snvme pins GPU pages at 64 KiB granularity).
- Per-slot PRP pages: page-aligned within the pool (nvm_dma_map_data_device maps at 4 KiB granularity).

### 2.3 Capacity

- `num_slots` = `max_in_flight_operations` (default 16). Matches the in-flight cap already enforced by `submit()`.
- `max_entries_per_slot` = `max_batch_entries` (default 256). Each slot can hold the maximum possible fan-out.
- PRP pages per slot = `max_batch_entries` (worst case: every entry is a LIST sub-IO needing one PRP-list page).

## 3. Legacy Reuse Evaluation

| Legacy Component | Adapt? | Reason |
|---|---|---|
| `GpuSlotPool<T>` | **No** | Template-based GPU object pool with per-slot stream/event fencing for TieredHandleCache's L1 tier. Complex stream tracking (per-slot `last_touch_stream`, shared per-stream events) is designed for cache eviction, not simple per-op lease/return. MetadataArena needs composite workspace (event + entries + status + PRP in one slot) with simple free-list semantics, not stream-ordered cache eviction. |
| `HostSlotPool<T>` | **No** | Pinned-host pool (L2 tier). Not relevant — MetadataArena needs GPU memory and DMA-mapped PRP pages. |
| `PrpListPool` | **Pattern reused, class not** | The "one big cudaMalloc + one nvm_dma_map_data_device, hand out fixed-size slots" pattern is exactly what MetadataArena's PRP pool uses. But PrpListPool has two tiers (L1 GPU + L2 host staging) and cache semantics (admit/promote/evict) that are not needed here. MetadataArena is simpler: one tier, no cache, direct lease/return. |

**Conclusion: Rewrite.** The three legacy pools are designed for different lifecycle models. MetadataArena borrows the "single large allocation + slot indexing" pattern but adds composite workspace leasing and timeout-leak semantics unique to per-op DataPath lifecycle.

## 4. Hot-Path Zero-Allocation Evidence

### 4.1 Structural proof

`MetadataArena::acquire()` body:
```cpp
bool MetadataArena::acquire(Lease& out) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (free_list_.empty()) return false;
    std::uint32_t slot = free_list_.front();
    free_list_.pop_front();
    out.slot_index = slot;
    out.event = events_[slot];
    out.d_entries = d_entries_pool_ + slot * cfg_.max_entries_per_slot;
    out.d_status = d_status_pool_ + slot * cfg_.max_entries_per_slot;
    out.prp_pages_devptr = (char*)prp_aligned_ + slot * prp_pages_per_slot_ * cfg_.page_size;
    out.prp_ioaddrs_base = slot * prp_pages_per_slot_;
    out.prp_page_capacity = prp_pages_per_slot_;
    return true;
}
```

Zero CUDA API calls. Pure CPU: mutex lock, deque pop, pointer arithmetic.

### 4.2 Test seam proof (test 60)

```
dp.test_arena_reset_alloc_counts();   // zero counters after init
// ... 5 rounds of submit/release ...
auto counts = dp.test_arena_alloc_counts();
CHECK(counts.cuda_malloc == 0, "zero cudaMalloc in hot path");
CHECK(counts.cuda_event_create == 0, "zero cudaEventCreate in hot path");
CHECK(counts.nvm_dma_map == 0, "zero nvm_dma_map in hot path");
```

The `AllocCounts` struct is incremented only in `init()` and `shutdown()`, never in `acquire()`/`release()`. After `reset_alloc_counts()`, the counters stay at 0 across any number of submit/release cycles.

## 5. Arena Tests Added

### Test 59: Arena exhaustion and recovery

- Fills arena capacity (N concurrent in-flight ops)
- Verifies `test_arena_available() == 0` when full
- N+1th op gets `RESOURCE_EXHAUSTED` (no op minted)
- After drain+release: `test_arena_available() == capacity` (full recovery)
- New submit succeeds after recovery

### Test 60: Zero-alloc hot path + reuse

- Resets alloc counters after init
- Runs 5 rounds of submit/release
- Arena available unchanged (no leak)
- `cuda_malloc == 0`, `cuda_event_create == 0`, `nvm_dma_map == 0` after 5 rounds

### Test 61: Arena LIST PRP from pool + regression

- 1 MiB LIST write/read/verify roundtrip using arena PRP pool
- PRP IOVAs come from arena's shared DMA mapping (`prp_ioaddrs_base + list_idx`)
- Hot path still zero-alloc after LIST roundtrip

### Test 62: Arena timeout slot leak

- LIST op in-flight during `shutdown(0)` → TIMEOUT
- If `has_timeout == true`: `release_with_timeout_leak()` → `test_arena_available()` decreases
- If `has_timeout == false`: normal release → `test_arena_available()` unchanged

## 6. Build Verification

### Standalone CUDA

```
cmake -S tutti -B build/round11-cuda \
  -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON \
  -DTUTTI_BUILD_HARDWARE_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round11-cuda -j8
ctest --test-dir build/round11-cuda --output-on-failure -E 'hardware'
```

- **Configure**: PASS (5.2s)
- **Build**: PASS (all targets including `tutti_local_nvme_datapath`)
- **CTest**: **132/132 PASS**, 0 failed (hardware tests skipped)

### Standalone HOST

```
cmake -S tutti -B build/round11-host \
  -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON \
  -DTUTTI_BUILD_HARDWARE_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round11-host -j8
ctest --test-dir build/round11-host --output-on-failure
```

- **Configure**: PASS (0.6s)
- **Build**: PASS
- **CTest**: **12/12 PASS**, 0 failed

### Contract test compilation

```
cmake -S tutti -B build/round11-cuda-hw \
  -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON \
  -DTUTTI_BUILD_HARDWARE_TESTS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round11-cuda-hw --target tutti_local_nvme_datapath_contract_test -j8
```

- **Build**: PASS — test binary compiles and links successfully

## 7. Files Modified/Created

### Created

| File | Purpose |
|---|---|
| `tutti/data_paths/local_nvme/metadata/metadata_arena.h` | MetadataArena class definition |
| `tutti/data_paths/local_nvme/metadata/metadata_arena.cpp` | MetadataArena implementation (init/shutdown/acquire/release/release_with_timeout_leak) |

### Modified

| File | Change |
|---|---|
| `tutti/data_paths/local_nvme/CMakeLists.txt` | Added `metadata/metadata_arena.cpp` to sources |
| `tutti/data_paths/local_nvme/local_nvme_data_path.h` | Added arena include/member; modified `OpEntry` to use arena lease (replaced `prp_list_raw`/`prp_list_aligned`/`prp_list_aligned_bytes` with `arena_slot`/`prp_ioaddrs_base`/`prp_pages_devptr`/`prp_list_page_count`); added 4 arena test accessors |
| `tutti/data_paths/local_nvme/local_nvme_data_path.cpp` | `initialize()`: arena init after queue group. `submit()`: replaced `cudaEventCreateWithFlags`+`cudaMalloc`(entries/status/PRP)+`nvm_dma_map_data_device`(PRP) with `arena_.acquire()`. `release()`: replaced per-op free with `arena_.release()`/`arena_.release_with_timeout_leak()`. `shutdown()`+destructor: replaced per-op free loop with `arena_.shutdown(any_timeout)`. Updated test accessors for arena-based PRP IOVAs and page counts. |
| `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp` | Added tests 59-62 (arena exhaustion/recovery, zero-alloc/reuse, LIST PRP from arena, timeout slot leak) |

## 8. Behavioral Invariants Preserved

| Invariant | How preserved |
|---|---|
| Public/SPI unchanged | No changes to `tutti/include/tutti/**` or SPI headers |
| IO semantics unchanged | PRP-list content fill formula, SINGLE/DUAL/LIST classification, MDTS fan-out, extent boundary checks — all identical |
| PRP-list DMA IOVA correctness | Arena pre-allocates one DMA mapping; per-slot IOVAs are `prp_dma->ioaddrs[slot * prp_pages_per_slot + list_idx]` — same `nvm_dma_t::ioaddrs[]` source as before |
| `has_timeout` conservative retention | `release_with_timeout_leak()` permanently consumes the slot; `shutdown(skip_prp=true)` retains the shared PRP pool if any op timed out |
| In-flight cap semantics | Arena capacity == `max_in_flight_operations`; exhaustion returns `RESOURCE_EXHAUSTED` |
| Partial commit semantics | Arena exhaustion rejects all requests in the batch (same as the existing in-flight cap check) |

## 9. Hardware Test Requirements (User Execution)

The 4 new tests (59-62) and all existing tests (1-58) require:
- snvme module loaded
- tutti_daemon running: `sudo ./build/bin/tutti_daemon --config sys_config.yaml`
- /dev/snvme0n1 mounted at /mnt/nvme1

```
cd build/round11-cuda-hw
ctest -R tutti_local_nvme_datapath_contract_test --output-on-failure
```

Expected: 616 (existing) + new assertions from tests 59-62 = total > 616, 0 failures.

## 总指挥验收（2026-08-01）

**PASS。** 独立复跑与审查：

- **热路径零分配**：`acquire()` 纯 CPU free-list（代码审查一致），test 71 的 alloc 计数 seam 实证 5 轮 submit/release 后 `cudaMalloc/cudaEventCreate/nvm_dma_map` 全为 0。
- **耗尽语义**：arena 满 → `RESOURCE_EXHAUSTED`，无隐式排队/退化分配（test 70）；in-flight 配额（16）与 arena 容量分离（`local_nvme_data_path.cpp:920` 先查配额，`:1126` 再 acquire）。
- **has_timeout 保守保留**：timeout 槽位永久泄漏（上界 = 槽位数）+ 任一 timeout 时整个 PRP pool 在 shutdown 保留——两处保守均已文档化（§10），与 Round 9 quiesce 契约一致。
- **legacy 复用评估**：三个 pool 的裁剪/重写论证成立（GpuSlotPool/HostSlotPool 生命周期模型不匹配，PrpListPool 模式复用类不复用）。
- **测试编号修正**：S1 的 arena 测试与 Round 9 4b 的 FIX 测试编号冲突（均为 59-62），总指挥已重编号为 **70-73**（TEST_CASE 字符串，无语义影响），本文件所述 tests 59-62 即现 70-73。
- **全量复跑**：local-NVMe 契约 **735/0**（cache OFF 与 ON 两配置）、runtime E2E **115/0**、HOST 12/12、CUDA 132/132；临时目录清空；`git diff --check` clean。
- IDE lint 在 `prp_page_cache.cpp` 报 `cuda_runtime.h` includePath 错误——IDE 配置问题（CMake 编译通过），非代码问题。

## 10. Known Limitations

| Item | Detail | Future |
|---|---|---|
| Timeout slot leak is permanent | A timed-out op's arena slot is never returned. Upper bound = `num_slots`. | CID abort/reset (future work) |
| PRP pool shared across all slots | If ANY op times out, the entire PRP pool is retained on shutdown (more conservative than per-op) | Per-slot DMA mapping (if needed for finer-grained retention) |
| `cudaMemset` in hot path | `cudaMemset(d_status, 0, ...)` is still called per-op (not a cudaMalloc, allowed) | Could use `cudaMemsetAsync` on stream for async zero-init |
| PRP page fill uses synchronous `cudaMemcpy` | Same as before — not a new limitation | Async H2D on stream (Session 3) |
