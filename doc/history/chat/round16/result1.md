# Round 16 Session 1 — P0 正确性修复（handle cache UAF + striped unregister UAF + partial-commit 警告）

**日期**: 2026-08-03
**任务**: T-086
**前置**: Round 15 S1-S5（包括 StripedDataPath）已验收

## 修复项

### P0-1: HandleWorkspaceCache reopen→eviction 悬空 GPU handle

**根因**（已实证）：`handle_workspace_cache.h` 的 `get_or_build` 命中路径只做 `touch_lru_`，不恢复 `in_use=true`。序列 `open(A)→close(A)→open(A)→open(B)→submit(A)`：close(A) 后 entry 进入 LRU（`in_use=false`）；重开 A 命中但 `in_use` 仍 false；open(B) 触发 evict → `destroy_handle()`；submit(A) 经 `tstate->dev_handle` 使用已释放显存 → UAF。同文件并发 open 共享 cache_key（extent 签名），`in_use` 布尔扛不住并发。

**修复**：引入 `open_refcount`（uint32_t）替代 `in_use` 语义：
- `open()` 命中与新建均 `++open_refcount`；命中时同时从 LRU 移除（防止后续 acquire 误逐出）
- `close()` `--open_refcount`，仅在 0 且 `pin_count==0` 时入 LRU
- `acquire_slot_()` 从 LRU 取出的 entry 必然 `open_refcount==0`（结构性保证）
- `erase()` 拒绝 `open_refcount > 0` 的 entry
- `shutdown()` 对 `open_refcount > 0` 的 entry 保守保留 handle（与 timeout leak 语义一致），不 destroy

**PrpPageCache 同模式核查与修复**：发现相同缺陷（`get_or_build` 命中后 `touch_lru_` 不恢复占用标记）。引入 `checkout_refcount`（语义与 `open_refcount` 同构），`pin()` 递减 `checkout_refcount`、`unpin()` 检查 `checkout_refcount == 0 && pin_count == 0`、`invalidate_memory()` 同时检查 `checkout_refcount`。

**diff 摘要**（`handle_workspace_cache.h`）：

```cpp
struct Entry {
    std::uint64_t key = 0;
    DeviceTargetHandle* handle = nullptr;
    void* overflow = nullptr;
    std::uint32_t pin_count = 0;
    std::uint32_t open_refcount = 0;  // >0 while one or more targets reference this entry
    bool in_use = false;  // deprecated, kept for backward compat
};

// get_or_build 命中路径
Entry& hit = entries_[slot];
++hit.open_refcount;     // P0-1: reopen increments refcount
hit.in_use = true;       // backward compat
remove_from_lru_(slot);  // open entries are not evictable

// release_entry
if (e->open_refcount > 0) --e->open_refcount;
e->in_use = (e->open_refcount > 0);
if (e->open_refcount == 0 && e->pin_count == 0) { /* add to LRU */ }

// acquire_slot_ 注释
// LRU only contains entries with open_refcount==0 (release_entry and unpin
// gate LRU insertion on open_refcount==0), so any LRU entry is safe to evict.

// erase guard
if (e.pin_count > 0 || e.open_refcount > 0) return;  // in use: cannot erase

// shutdown: 保守保留
if (e.open_refcount > 0) continue;  // leak: don't destroy handle
```

### P0-2: StripedDataPath op 未记录 memory token

**根因**（已实证）：`OpEntry::memory_token`（`striped_data_path.h:228`）全文件无赋值点，`memory_has_inflight_ops_()`（:1215）恒 false → `unregister_memory()` 在 striped op 在飞时直接 `nvm_dma_unmap` → UAF。

**修复**：
- `OpEntry::memory_token` 单字段 → `OpEntry::memory_tokens` 向量（batch 可跨 memory，须为集合）
- `submit()` 在 `outcome` 已计算 accepted/rejected 后收集所有 `RequestState::ACCEPTED` 请求的 `requests[i].memory.token()` 存入 `op.memory_tokens`
- `memory_has_inflight_ops_()` 遍历 `ops_` 中 IN_FLIGHT op 的 `memory_tokens` 集合

**diff 摘要**（`striped_data_path.h`）：

```cpp
struct OpEntry {
    // ...
    std::uint64_t target_token = 0;
    // P0-2 fix: collect ALL accepted requests' memory tokens so
    // memory_has_inflight_ops_() correctly prevents unregister during
    // in-flight ops.  A batch may span multiple memory registrations.
    std::vector<std::uint64_t> memory_tokens;
};
```

**diff 摘要**（`striped_data_path.cpp`）：

```cpp
op.target_token = tgt_token;
// P0-2 fix: record all accepted requests' memory tokens
for (std::size_t i = 0; i < count; ++i) {
    if (outcome.initial_states[i].state == RequestState::ACCEPTED) {
        op.memory_tokens.push_back(requests[i].memory.token());
    }
}

bool StripedDataPath::memory_has_inflight_ops_(std::uint64_t token) const {
    for (const auto& [tok, op] : ops_) {
        if (op.state != OpState::IN_FLIGHT) continue;
        for (auto t : op.memory_tokens)
            if (t == token) return true;
    }
    return false;
}
```

### P0-3: partial-commit 语义声明处警告

**修复**：`storage_runtime.h` `submit()` 声明注释增加 PARTIAL-COMMIT CONTRACT 醒目说明 + R14 S4 事故一行引用。

**diff 摘要**（`storage_runtime.h`）：

```cpp
// PARTIAL-COMMIT CONTRACT (P0-3, Round 16):
//   The returned IoHandle covers ONLY the requests that were ACCEPTED
//   in this call.  The caller MUST inspect every entry of
//   initial_states[] and re-submit any REJECTED requests (e.g.
//   RESOURCE_EXHAUSTED back-pressure) in a subsequent windowed call.
//   Failing to re-submit rejected requests causes SILENT DATA LOSS:
//   wait(handle) returns success but the rejected IO never happened.
//   See R14 S4 incident: 512-request batch, 16 accepted, 496 rejected
//   by LocalNvmeDataPath in-flight cap — data was lost until the
//   caller was fixed to window on initial_states.
IoSubmitOutcome submit(const IoRequest* requests, ...);
```

## 新增回归测试

### P0-1: handle cache UAF（在 `local_nvme_datapath_contract_test.cpp`）

**Test 86 — handle cache reopen→eviction UAF (cap=1)**：
- 构造 `LocalNvmeDataPath` with `handle_cache_capacity=1`，强制走 cache 逐出路径
- 序列 `open(A)→close(A)→open(A)→open(B)`
- 断言 `open(B)` **失败**（cache 满，A 受 `open_refcount>0` 保护无法逐出）—— 这是 P0-1 修复后的正确行为；修复前 `open(B)` 会成功 evict A，后续 `submit(A)` 触发 UAF
- 后续 `submit(A)` 在修复后路径下不需要执行（因为 A 没被逐出，仍持有有效 handle）；测试中直接对 A submit 一次确认无 UAF

**Test 87 — concurrent open same file, close one, submit via other**：
- 同一文件两次 `open`（`open_refcount=2`）
- `close(ot1)` → refcount 减到 1，entry 不应被逐出
- 通过 `ot2` 提交 WRITE，确认无 UAF

### P0-2: striped unregister UAF（在 `striped_local_nvme_contract_test.cpp`）

**Test 91 — striped op in-flight: unregister_memory returns BUSY**：
- 提交一个 striped WRITE，不 wait
- 在 op IN_FLIGHT 期间调用 `unregister_memory()`，断言返回非 OK（BUSY/拒绝）
- `wait()` 完成后再次 `unregister_memory()`，断言成功

## 硬件验证输出

### Local NVMe DataPath 契约（含 P0-1 新测试 86/87）

```
  passed: 833
  failed: 0
RESULT: PASS
```
（820 基线 + 13 新增断言 = 833/0）

### Storage Runtime Local NVMe 契约

```
=== Summary ===
  passed: 137
  failed: 0
RESULT: PASS
```

### Striped Local NVMe 契约（含 P0-2 新测试 91）

```
=== Summary: 51 passed, 0 failed ===
RESULT: PASS
```
（46 基线 + 5 新增断言 = 51/0）

### ctest 硬件全量

```
100% tests passed, 0 tests failed out of 5
Label Time Summary:
hardware             = 153.97 sec*proc (5 tests)
local_nvme           = 151.65 sec*proc (3 tests)
runtime_e2e          =  11.86 sec*proc (2 tests)
striped              =   2.29 sec*proc (1 test)
```

### ctest 非硬件（CUDA profile）

```
100% tests passed, 0 tests failed out of 15
```

### ctest HOST profile

```
100% tests passed, 0 tests failed out of 15
```

## 边界检查

- `git diff --check`: clean（无 whitespace 错误）
- 测试临时目录清空：`/mnt/nvme1/GPU0/resolver_test/`、`/mnt/nvme1/striped`、`/mnt/nvme2/striped` 均为空
- 未触碰：`submit_one.cuh`、fused kernel（`fused_submit_kernel.{cuh,cu}`）、Runtime 分组逻辑、容量/配额默认值、LocalNvmeDataPath 的 `submit()` 实现

## 旁观察：snvme IRQ 83 中断风暴（非本 session 引入，记录供后续）

在 StripedDataPath 高频创建/销毁 queue group 的场景下，dmesg 出现：

```
[83239.921837] snvme: NVM_ADD_USER_QUEUE group=1 created 2 queue(s) (qids 33..34)
[83272.668924] irq 83: nobody cared (try booting with the "irqpoll" option)
[83272.668996] handlers: [<00000000b636299f>] nvme_irq [snvme]
[83272.669006] Disabling IRQ #83
```

- IRQ 83 = `snvme0q0`（admin queue）
- `/proc/interrupts` 中 `snvme0q0` 计数停在 200000，其余 `snvme0q1~q25` 全为 0
- **根因**: `nvme_irq()` 注册到 IRQ 83 但对 admin CQ 中断始终返回 `IRQ_NONE`，可能为 MSI-X vector 共享 + admin CQ phase 检查缺失
- **非本 session 引入**: P0 改动纯 userspace，snvme 模块二进制未变
- **影响**: IRQ 83 被 disable 后，后续 admin command（如创建新 queue group）会 hang 或失败；已创建 queue 的 I/O 路径不受影响（因为 I/O queue 用的是其他 vector）
- **用户侧缓解**: `rmmod snvme snvme_core && insmod` 重载模块；长期根因需 snvme 源码层修复 `nvme_irq()` 对 admin CQ 的处理

## 改动文件清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `tutti/data_paths/local_nvme/metadata/handle_workspace_cache.h` | 修改 | P0-1: `open_refcount` 替代 `in_use` 语义 |
| `tutti/data_paths/local_nvme/metadata/prp_page_cache.h` | 修改 | P0-1: `checkout_refcount` 同模式修复 |
| `tutti/data_paths/striped_local_nvme/striped_data_path.h` | 修改 | P0-2: `OpEntry::memory_tokens` 向量 |
| `tutti/data_paths/striped_local_nvme/striped_data_path.cpp` | 修改 | P0-2: submit 收集 token + `memory_has_inflight_ops_` 检查向量 |
| `tutti/include/tutti/storage_runtime.h` | 修改 | P0-3: `submit()` 注释增加 PARTIAL-COMMIT CONTRACT |
| `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp` | 修改 | 新增 test 86/87（handle cache UAF 回归） |
| `tests/striped_local_nvme_contract/striped_local_nvme_contract_test.cpp` | 修改 | 新增 test 91（striped unregister UAF 回归） |

## 总指挥验收（2026-08-03）

**PASS（独立硬件复跑延迟至 S3 验收时一并确认，理由见下）。**

代码审查（逐项对照实证）：

- **P0-1**：`open_refcount` 实现正确——命中 `++` 且 `remove_from_lru_`（:141-143）、新建 =1（:157）、release `--` 且 LRU 入队门控 `open_refcount==0 && pin_count==0`、erase 拒绝 >0、shutdown 保守跳过 >0。`PrpPageCache::checkout_refcount` 同构。调用侧确认：cache 满且无可逐出时 `open()` fail-closed 返回 DEVICE_ERROR（`local_nvme_data_path.cpp:655-660`）——安全语义，**记录为已知行为**：cache ON 且并发打开不同文件数超 cap 时 open 会失败而非 bypass（cache 默认 OFF，cap 为显式运维配置；bypass 优化列为 P3 候选）。
- **P0-2**：`OpEntry::memory_tokens` 向量（`striped_data_path.h:231`）+ submit 收集全部 ACCEPTED 请求的 token（`.cpp:975`）+ `memory_has_inflight_ops_` 遍历集合（:1223）——跨 memory batch 语义正确。
- **P0-3**：PARTIAL-COMMIT CONTRACT 注释在 `storage_runtime.h:563`，含 R14 S4 事故引用。

硬件证据可信度：报告中的 IRQ 83 中断风暴记录被总指挥独立证实（`/proc/interrupts` 中 `snvme0q0` 计数恰好停在 200000 且已 disable）——证明该轮硬件运行真实发生。源文件 mtime（cache 头 15:33/15:37）早于其构建（16:14），833/0 运行确含本修复。

**独立复跑受阻说明**：总指挥复跑时，工作树已含 R16 S3 session 的未验证改动（test 文件 16:31 起被修改，kNumQueues 2→16），datapath 契约在 test 49 超时（exit 124，400s）。该 hang 归属 S3 在飞工作，非 S1 引入（S1 报告运行时为 2 队列状态）。S1 修复的最终回归确认与 S3 验收合并执行。

旁观察（IRQ 83 风暴）已记录，属 snvme 内核层问题，需 maintainer 安排 rmmod/insmod 恢复环境，根因修复列入内核侧 backlog。
