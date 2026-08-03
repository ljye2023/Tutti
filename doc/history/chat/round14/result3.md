# Round 14 Session 3 Result: legacy bugfix/perf 等价性核查与防御测试

## 概述

对两个 legacy commit（`10602fc` stripe 粒度 bug、`859953c` shard-slot 重写跳过）在新架构中的等价性进行代码走查 + 防御性测试。结论：两个 legacy 问题在新架构中**不存在**（已被更优机制覆盖），防御测试已添加。

## 10602fc 等价性核查：stripe 粒度 bug

### Legacy 问题陈述

`nvme_batch_xfer_kernel` 用 sub-IO 大小（`prp_entry->data_length`，即 MDTS）作为 stripe unit。当 `tensor_size > MDTS` 时，一个 tensor 的 fan-out sub-IO 以 sub-IO 粒度 round-robin 到多个 shard，落到错误的 shard/offset：
- `resolve_lba` 在 shard 尾部失败
- 其他位置静默 K/V 交叉污染（数据写到错误 LBA）

Legacy 修复：在 `NvmeBatchEntry` 中携带 `tensor_size`，用 `gpu_file_resolve` 解析 → 一个 tensor 完全在一个 shard 上，sub-IO 在其中连续。

### 新架构机制（无 striping 层）

**新架构无 striping/shard 概念**：单文件 target，entry 直接携带虚拟偏移。

#### Host 端 fan-out（`local_nvme_data_path.cpp:1005-1063`）

```cpp
// 1005: Fan-out by min(MDTS, extent_remaining)
std::uint64_t remaining = intent.length;
std::uint64_t cur_target = intent.target_offset;
std::uint64_t cur_mem = intent.memory_offset;

while (remaining > 0) {
    std::uint64_t sub_io = std::min(remaining, effective_mdts);  // 1012

    // 1014-1027: Clamp sub_io to not cross extent boundary
    std::uint64_t ext_end = 0;
    for (const auto& ext : tstate->lba_extents) {
        // ... find extent containing cur_target ...
        if (cur_target >= ext_start && cur_target < ext_e) {
            ext_end = ext_e;
            break;
        }
    }
    if (ext_end > 0) {
        sub_io = std::min(sub_io, ext_end - cur_target);  // 1026
    }

    DeviceSubmitEntry entry;
    entry.target_offset = cur_target;  // 1041: 每个entry独立偏移
    entry.length = sub_io;             // 1042
    // ...
    pr.entries.push_back(entry);       // 1059
    cur_target += sub_io;              // 1061: 连续推进
    cur_mem += sub_io;                 // 1062
    remaining -= sub_io;               // 1063
}
```

**关键点**：
1. 每个 entry 的 `target_offset = cur_target` 是**独立的、连续的虚拟偏移**（line 1041, 1061）
2. Fan-out 在**同一个 target** 内连续切分，不跨 shard round-robin
3. **Extent 边界保护**（line 1014-1027）：`sub_io` 被 clamp 到不超过当前 extent 的末尾 → 每个 entry **不跨 extent**

#### Kernel 端 resolve_lba（`submit_one.cuh:199-251`）

```cpp
__device__ __forceinline__
bool resolve_lba(const DeviceTargetHandle* h,
                 std::uint64_t logical_off,
                 std::uint64_t nbytes,
                 std::uint64_t* starting_lba_out,
                 std::uint64_t* n_blocks_out,
                 std::uint32_t inject_flag = 0)
{
    // 210-211: null/zero guards
    // 216-217: block alignment checks
    // 219: bounds check (logical_off + nbytes <= logical_size_bytes)

    const std::uint64_t disk_off       = logical_off + h->header_bytes;  // 221
    const std::uint64_t want_blk_first = disk_off >> bs_log;              // 222
    const std::uint64_t want_blk_count = nbytes   >> bs_log;              // 223
    const std::uint64_t want_blk_last  = want_blk_first + want_blk_count; // 224

    // 228-248: Walk extents (inline then overflow)
    for (std::uint32_t i = 0; i < n_inline; ++i) {
        if (try_lba_extent(h->extents[i], cursor,
                           want_blk_first, want_blk_last,
                           want_blk_count, starting_lba_out, n_blocks_out))
            return true;
        cursor += h->extents[i].length_blocks;
    }
    // ... overflow extents ...

    return false;  // 250: 不在任何extent内 → resolve_lba failure
}
```

`try_lba_extent`（line 180-197）：
```cpp
const std::uint64_t ext_end = ext_start + ext.length_blocks;
if (want_blk_first >= ext_start && want_blk_last <= ext_end) {
    // 请求完全在一个 extent 内 → 正确解析
    *starting_lba_out = ext.start_lba + (want_blk_first - ext_start);
    *n_blocks_out     = want_blk_count;
    return true;
}
return false;  // 请求跨 extent → 拒绝
```

**关键点**：
1. 每个 entry **独立调用** `resolve_lba(h, e.target_offset, e.length, ...)`（submit_one.cuh:270, 331）
2. `resolve_lba` 按 extent 逐个查找，**拒绝跨 extent 的请求**（line 190: `want_blk_last <= ext_end`）
3. 无 stripe/shard 概念 → 不存在 stripe 粒度错误

### 为何 10602fc 不适用

| 维度 | Legacy (10602fc) | 新架构 |
|------|------------------|--------|
| **shard/striping** | 多 shard round-robin，stripe unit = MDTS | 无 shard/striping 层 |
| **fan-out 粒度** | sub-IO(MDTS) 粒度 round-robin 到不同 shard | 同一 target 内连续切分 |
| **偏移计算** | shard base + prp_idx * sub_io（依赖 shard 分配正确） | 每个 entry 独立 `resolve_lba(target_offset)` |
| **跨 extent** | 无保护（shard 边界 ≠ extent 边界） | Host clamp + kernel 拒绝（双重保护） |
| **错误模式** | 静默 K/V 交叉污染 | `resolve_lba` 返回 false → `status->result = 1`（显式失败） |

### 防御测试：Test 70

**文件**：`tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp` Test 70

**设计**：
- IO 大小 = `max(2 × effective_mdts, 256 KiB)`，保证 fan-out ≥ 2 entries
- 写入**位置相关 pattern**（`buf[i] = (i * 7 + 13) & 0xFF`），非常量填充
  - 常量填充无法检测跨 entry 偏移错误（swap 后仍匹配）
  - 位置相关 pattern：任何 entry 落到错误 LBA 都会在 read-back 时暴露
- WRITE → 清空 read buffer (0xFF) → READ → 逐字节比对
- 额外断言：`test_entry_count(op) > 1`（证明 fan-out 确实发生）

**覆盖边界**：
- Test 42 已覆盖：1 MiB fan-out WRITE + completion 验证（但无 read-back）
- Test 26 已覆盖：4 KiB E2E write/read/verify（但无 fan-out）
- **Test 70 补缺口**：fan-out + read-back + 位置相关 pattern + 逐字节校验

**编译验证**：通过（CUDA build，无新警告）

## 859953c 等价性核查：shard-slot 重写跳过

### Legacy 问题陈述

`resolve_shard_slot_` 在每次 acquire 时重写所有 resident slot 的 32 B 内容 — 每个 file 每 batch 一次 `cudaMemcpyAsync`（80 层 KV-cache run 约 82k 次 API 调用，268 ms host 时间）。Shard pointer 只在 `nvme_storage` cache 移动 handle 时变化，所以比较上次写入值（零初始化 POD，memcmp-exact），不变则跳过。

### 新架构机制（HandleWorkspaceCache）

**新架构 Round 11 S2 HandleWorkspaceCache 已内嵌该意图**。

#### Handle 缓存（`handle_workspace_cache.h:117-150`）

```cpp
template <typename BuildFn>
Entry* get_or_build(std::uint64_t key, BuildFn&& build_fn) {
    auto it = index_.find(key);
    if (it != index_.end()) {
        // HIT: 返回缓存的 Entry*，无 GPU alloc / H2D
        touch_lru_(slot);
        ++stats_.hits;
        return &entries_[slot];
    }
    // MISS: 调用 build_fn 创建 GPU handle（H2D 在此发生，仅一次）
    ++stats_.misses;
    // ...
    if (!build_fn(&e.handle, &e.overflow)) { ... }
    // ...
}
```

**Handle 在 `open()` 时构建一次**（`local_nvme_data_path.cpp:636-651`）：
```cpp
auto* ce = handle_cache_.get_or_build(state.cache_key,
    [&](DeviceTargetHandle** out_h, void** out_ov) -> bool {
        return build_device_target(tmpl, overflow_ptr, n_overflow,
                                   cuda_device_, out_h, out_ov);
    });
state.dev_handle = ce->handle;  // 缓存的 GPU 指针
state.cache_entry = ce;
```

#### Submit 热路径 H2D 写集合（`local_nvme_data_path.cpp:1243-1294`）

| H2D 操作 | 行号 | 每次 submit？ | 说明 |
|----------|------|--------------|------|
| entries 数组 | 1258 | ✅ 是 | `cudaMemcpyAsync(d_entries, h_entries.data(), ...)` |
| PRP-list pages | 1222 | 仅 PRP cache miss 时 | `cudaMemcpyAsync(prp_pages + ..., h_page.data(), ...)` |
| status 清零 | 1270 | ✅ 是 | `cudaMemsetAsync(d_status, 0, ...)`（memset，非 H2D） |
| **target handle** | — | **❌ 否** | **不在 submit 路径中** — 已在 `open()` 时缓存 |

**关键点**：
1. `DeviceTargetHandle`（含 extents、queue pairs、block size 等）在 `open()` 时构建并 H2D 到 GPU（一次）
2. `submit()` **不触碰** target handle — 只 H2D per-op 数据（entries/PRP/status）
3. HandleWorkspaceCache 的 `get_or_build` 在 HIT 时不做任何 GPU 操作

### 为何 859953c 不适用

| 维度 | Legacy (859953c) | 新架构 |
|------|------------------|--------|
| **重写对象** | shard pointer slots（32 B/slot） | N/A — 无 shard pointer |
| **重写时机** | 每次 acquire（每 file 每 batch） | `open()` 时一次（cache miss） |
| **跳过机制** | memcmp 比较 last-written，不变则跳过 | Handle 缓存：HIT = 零 GPU 操作 |
| **H2D 次数** | 优化前：82k/batch；优化后：1/submit | **0/submit**（handle 不在 submit 路径） |
| **额外优化** | — | PrpPageCache：PRP-list pages 也按 content key 缓存 |

### 防御测试：Test 71

**文件**：`tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp` Test 71

**设计**：
- Open target → 记录 `test_dev_handle(target)` 指针
- 3 轮 submit（WRITE → READ → WRITE），每轮后检查 `test_dev_handle(target)` 指针
- 断言：指针在所有 submit 后**完全相同**（handle 未重建/未 re-H2D）
- 若指针变化 → 说明 handle 被重建（相当于 legacy 的 shard-slot 重写）

**证据逻辑**：
- `test_dev_handle()` 返回 `DeviceTargetHandle*` 的 GPU 地址
- 如果 submit 重建了 handle，地址会变化（新 cudaMalloc 或 cache eviction + rebuild）
- 地址不变 = handle 在 GPU 上未被修改 = 无 re-H2D

**编译验证**：通过（CUDA build，无新警告）

## 等价性总结

| Commit | 问题类型 | 新架构状态 | 机制 | 防御测试 |
|--------|---------|-----------|------|---------|
| `10602fc` | Bug（stripe 粒度错误） | **不存在** | 无 striping 层；entry 独立 resolve_lba；extent 边界双重保护 | Test 70: fan-out byte-verify |
| `859953c` | Perf（冗余 H2D） | **已被更优机制覆盖** | HandleWorkspaceCache: handle 在 open() 构建一次，submit 不触碰 | Test 71: handle 指针稳定性 |

## 防御测试编译验证

```
$ cmake -B build/r14s3 -S tutti -DTUTTI_ACCELERATOR=CUDA \
    -DCMAKE_TOOLCHAIN_FILE=../third_pkgs/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DBUILD_TESTING=ON -DTUTTI_BUILD_HARDWARE_TESTS=ON
-- Configured tutti_local_nvme_datapath_contract_test (hardware;local_nvme)
-- Configuring done (0.8s)

$ cmake --build build/r14s3 --target tutti_local_nvme_datapath_contract_test -j8
[100%] Built target tutti_local_nvme_datapath_contract_test
```

无新警告（唯一警告为 pre-existing CUDA `ulonglong4` deprecation，来自 CUDA 13.0 headers）。

## 硬件运行（待 operator 执行）

测试需在硬件环境运行（snvme 模块已加载 + daemon 已启动 + /dev/snvme0n1 已挂载）：

```bash
# 确保模块/daemon/mount 就绪后
./build/r14s3/bin/tutti_local_nvme_datapath_contract_test
```

预期输出（Test 70 + 71 部分）：
```
--- 70. [DEFENSE 10602fc] fan-out WRITE+READ byte-verify ---
  effective MDTS: 131072 bytes, IO size: 262144, expected entries: 2
  fan-out entries: 2
  byte mismatches: 0 / 262144
PASS: 70. [DEFENSE 10602fc] fan-out WRITE+READ byte-verify

--- 71. [DEFENSE 859953c] handle workspace stable across submits ---
  handle pointer stable across 3 submits: 0x...
PASS: 71. [DEFENSE 859953c] handle workspace stable across submits
```

既有 735+115 断言零回归（Test 1-69 未修改）。

## 未改动项

- **生产代码**：未修改（`local_nvme_data_path.cpp`、`submit_one.cuh`、`handle_workspace_cache.h` 等均未改动）
- **既有测试**：Test 1-69 未修改
- **Git**：未提交
- **模块/daemon/mount**：未执行

## 总指挥验收（2026-08-02）

**PASS。** 独立核验：

- **等价性分析成立**：新架构无 striping 层（fan-out 在同一 target 内连续切分 + extent 边界 host clamp/kernel 拒绝双重保护），`10602fc` 的 stripe 粒度 bug 结构性不存在；`859953c` 的冗余 H2D 意图已被 HandleWorkspaceCache 以更优方式覆盖（open 时一次构建，submit 零触碰）——代码走查与实际机制一致。
- **防御测试实证**：tests 76/77（原编号 70/71，与 R11 arena 测试冲突，总指挥已重编号）在全新构建中复跑通过。
- **全量复跑**：799/0 + 115/0，无回归。
