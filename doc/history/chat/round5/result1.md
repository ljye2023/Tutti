# T-016 Worker Result

## 1. Modified Files

- `/data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h` — four gap fixes
- `/data/home/ryeqiu/Tutti/tests/storage_runtime_contract/storage_runtime_contract_test.cpp` — updated for new submit signature + 8 new tests
- `/data/home/ryeqiu/Tutti/tests/storage_runtime_contract/CMakeLists.txt` — unchanged (already had `-Werror`)

No other files were modified.

## 2. Pre-fix Gap 2 Evidence

Command (old single-request API):
```bash
/opt/rh/gcc-toolset-13/root/usr/bin/c++ ... /tmp/gap2_repro.cpp -o /tmp/gap2_repro && /tmp/gap2_repro
```

Pre-fix output:
```
memory_offset=1<<30 length=1<<40: accepted=1
```

The out-of-bounds request was incorrectly accepted.

Post-fix: the same source no longer compiles (single-request overload removed). With the new batch API, the equivalent test (`submit(&req, 1, ctx)`) returns `initial_states[0].state == REJECTED` with `OUT_OF_RANGE`. This is verified by test 20 and test 21 in the contract test suite.

## 3. New `submit` Signature and Result Types

### Public surface (in `storage_runtime.h`)

```cpp
enum class IoRequestState {
    ACCEPTED,
    REJECTED,
};

struct IoRequestInitialState {
    IoRequestState state = IoRequestState::REJECTED;
    Status status;
};

struct IoSubmitOutcome {
    Status status;
    std::optional<IoHandle> io;
    std::vector<IoRequestInitialState> initial_states;
};

IoSubmitOutcome submit(const IoRequest* requests,
                       std::size_t count,
                       const HostSubmitContext& context);
```

No single-request overload remains. Callers pass `count == 1` for single requests.

## 4. Five Invariants Implementation

1. **`io == nullopt` <=> zero accepted**: In `submit()`, after validation, if `accepted_count == 0`, the function returns without creating an IoEntry; `io` stays `nullopt`. For `count == 0`, it returns immediately with `io == nullopt`.

2. **`io != nullopt` <=> at least one accepted**: If `accepted_count > 0`, an IoEntry is created and `outcome.io` is set. The overall `status` may be non-OK (partial failure), but `io` is present.

3. **`initial_states.size() == count`**: The vector is resized to `count` at the start of validation and populated one entry per request in input order.

4. **Accepted requests queryable**: The IoEntry is created and accessible via `query()`/`wait()` using the returned IoHandle.

5. **`count == 0` => `io == nullopt`, status OK, empty initial_states**: Explicitly handled as an early return.

## 5. Bounds Validation Implementation (Overflow-safe)

```cpp
Status validate_request_(const IoRequest& req) const {
    if (req.length == 0) return INVALID_ARGUMENT;
    if (!validate_memory_(req.memory)) return INVALID_ARGUMENT;
    if (!validate_target_(req.target)) return INVALID_ARGUMENT;

    // Memory bounds — overflow-safe: never computes offset + length
    if (req.memory_offset > mem_size) return OUT_OF_RANGE;
    if (req.length > mem_size - req.memory_offset) return OUT_OF_RANGE;

    // Target bounds — same pattern
    if (req.target_offset > tgt_size) return OUT_OF_RANGE;
    if (req.length > tgt_size - req.target_offset) return OUT_OF_RANGE;

    return OK;
}
```

Key: checks `offset > size` first, then `length > size - offset`. The subtraction `size - offset` is safe because the first check guarantees `offset <= size`. Never computes `offset + length`.

Each request is validated independently. A rejected request gets `REJECTED` + error status but does not prevent other requests from being accepted.

## 6. Gap 3: `const` Removal

- `query()`: changed from `Result<IoSnapshot> query(const IoHandle&) const` to `Result<IoSnapshot> query(const IoHandle&)`
- `wait()`: changed from `WaitOutcome wait(const IoHandle&, std::uint64_t) const` to `WaitOutcome wait(const IoHandle&, std::uint64_t)`
- A private `bounded_progress_()` no-op hook was added as a placeholder for future progress driving.
- `WaitOutcome` semantics unchanged (observation timeout does not cancel operation).
- `query_memory()` and `query_target()` remain `const` (they are pure observers).

## 7. Gap 4: Test Backdoor Convergence

**Chosen approach**: Option 1 — moved `testing_force_complete_io_` to `private`, added `friend struct ::tutti::testing::StorageRuntimeTestAccess`.

### Implementation

```cpp
namespace testing { struct StorageRuntimeTestAccess; }

class StorageRuntime {
    // ...
private:
    friend struct ::tutti::testing::StorageRuntimeTestAccess;
    Status testing_force_complete_io_(...);
};

namespace testing {
struct StorageRuntimeTestAccess {
    static Status force_complete_io(StorageRuntime& rt, ...) {
        return rt.testing_force_complete_io_(...);
    }
};
}
```

### Evidence that ordinary consumers cannot call it

The method `testing_force_complete_io_` is `private`. The only non-member that can access it is `StorageRuntimeTestAccess` (via `friend`). A consumer that does not use this struct cannot call the method. The test file includes a commented-out line that would fail to compile if uncommented:

```cpp
// rt->testing_force_complete_io_(io, tutti::IoState::COMPLETED);
```

## 8. New Test Assertions (8 tests, #15-22)

15. **Batch success**: 4 valid requests -> status OK, io present, 4 ACCEPTED, queryable.
16. **Partial commit (core)**: 4 requests, index 2 out-of-bounds -> status non-OK, io present + valid, indices 0/1/3 ACCEPTED, index 2 REJECTED with OUT_OF_RANGE, accepted part queryable.
17. **Zero accepted**: all invalid -> io == nullopt, status non-OK, all REJECTED.
18. **Count zero**: io == nullopt, status OK, empty initial_states.
19. **Length zero**: length == 0 -> REJECTED with INVALID_ARGUMENT.
20. **Out of range**: memory OOR -> OUT_OF_RANGE; target OOR -> OUT_OF_RANGE.
21. **Overflow safety**: near-UINT64_MAX offsets/lengths rejected without wrap.
22. **Non-const + test access**: static_assert query/wait are member functions; call on non-const reference; test backdoor only via friend struct.

## 9. Existing 14 Tests Still Pass

Test binary output:
```
All 22 storage runtime contract tests passed.
```

Sub-test count: 22 (14 original + 8 new). All pass.

Original tests updated for new `submit` signature: all `submit(req, ctx)` calls changed to `submit(&req, 1, ctx)` via `submit_one()` helper. All `testing_force_complete_io()` calls changed to `force_complete()` helper which routes through `StorageRuntimeTestAccess`.

## 10. Configure / Build / ctest Results

### Configure
```
-- The CXX compiler identification is GNU 13.1.1
-- Configuring done (0.2s)
-- Generating done (0.0s)
```
No CUDA / gRPC / yaml-cpp / libnvm / FIEMAP / NVMe dependency discovery.

### Build (with -Werror)
```
[100%] Built target tutti_storage_runtime_contract_test
```
Zero warnings, zero errors.

### ctest
```
1/1 Test #1: tutti_storage_runtime_contract_test ...   Passed    0.01 sec
100% tests passed, 0 tests failed out of 1
```

## 11. Proofs

### Public contract zero changes
```
git diff --stat -- tutti/include/tutti/status.h tutti/include/tutti/io_types.h tutti/include/tutti/memory_types.h
(empty output)
```

### No SPI dependency
```
grep -n 'spi/' tutti/include/tutti/storage_runtime.h
(no output)
```

### No single-request overload
```
grep -nE 'submit\(const IoRequest&' tutti/include/tutti/storage_runtime.h
(no output)
```

## 12. Public-boundary Guard

```
grep -nEiw 'libnvm|nvme|fiemap|grpc|yaml|prp|sgl|lba|cid|doorbell|iova|rkey|fd|extent' \
  tutti/include/tutti/storage_runtime.h
```
Result: no matches (exit code 1).

## 13. File Boundary and Whitespace Checks

### git diff --check
```
git diff --check -- tutti/include/tutti/storage_runtime.h
exit code: 0 (no whitespace errors)
```

### Trailing whitespace
```
tutti/include/tutti/storage_runtime.h:                              0
tests/storage_runtime_contract/CMakeLists.txt:                      0
tests/storage_runtime_contract/storage_runtime_contract_test.cpp:   0
```

### EOF newline
All files: OK.

### File boundary
Only modified:
- `tutti/include/tutti/storage_runtime.h`
- `tests/storage_runtime_contract/storage_runtime_contract_test.cpp`
- `chat/round5/result1.md` (this file)

`CMakeLists.txt` was already correct (had `-Werror`). No other files touched.

## 14. Final Status

`PASS`

All success criteria met:
1. `submit()` is batch, returns `IoSubmitOutcome`; no single-request overload.
2. Five invariants hold (verified by tests 15-18).
3. Partial-commit test passes; accepted part queryable (test 16).
4. Bounds validation covers zero-length, memory OOR, target OOR (tests 19-20).
5. Per-request independent validation; single OOR doesn't fail batch (test 16).
6. `wait()`/`query()` non-const; `WaitOutcome` semantics unchanged (test 22).
7. Test backdoor private + friend; evidence in test 22 commented-out line.
8. All 22 sub-tests pass (14 original + 8 new).
9. Three public contract headers unchanged.
10. No SPI includes; no direct CUDA SDK include.
11. HOST standalone configure/build/ctest passes with `-Werror`.
12. Only allowed files modified.
13. No module/daemon/IO operations.
14. Whitespace checks pass.

## 总指挥验收

验收结论：`PASS`。四处缺口全部真实闭合，无返工项。

### 独立实证（我写了探测程序，不只看 worker 报告）

```text
[oob]      io_present=0 state=REJECTED code=2
[wrap]     state=REJECTED code=2
[partial]  overall_ok=0 io_present=1 n=4 -> AARA  accepted_queryable=1
```

逐条解读：

- **`[oob]`** —— 上一轮我实测被静默接受的那个精确用例（4096 字节缓冲、`memory_offset=1<<30`、`length=1<<40`）现在返回 `REJECTED` + `code=2`（`OUT_OF_RANGE`），且 `io_present=0`，同时验证了「零接受 ⟹ `io == nullopt`」这条不变量。缺口 2 确实修好了。
- **`[wrap]`** —— 我构造了 `memory_offset=0xFFFFFFFFFFFFFF00`、`length=0x200` 的回绕用例：若代码写成 `offset + length > size` 会溢出成小数值而被误判合法。实测 `REJECTED` + `OUT_OF_RANGE`，防溢出写法真实生效。
- **`[partial]`** —— 4 个 request、index 2 越界：整体状态非 OK、`io` **仍存在**、`initial_states` 恰好 4 项、模式 `AARA`（只有 index 2 被拒）、且被接受部分可 `query()`。这正是架构第 678/680 行要求的「第 K 个 request 失败」契约，Runtime 层终于能表达了。

### 源码级核验

- `submit` 只有批量签名（第 406 行），单请求重载已彻底移除 —— 避免了两套 partial-commit 路径。
- `query()`（第 499 行）与 `wait()`（第 509 行）的 `const` 已去掉，为未来驱动有界 progress 留出空间；`WaitOutcome` 语义未回退。
- 防溢出写法确认为「先判 `offset > size`，再判 `length > size - offset`」，全程不计算 `offset + length`。
- **测试后门收敛已用否定编译实验证实**（超出 prompt 要求）：

```text
error: 'tutti::Status tutti::StorageRuntime::testing_force_complete_io_(...)'
       is private within this context
note: declared private here  (storage_runtime.h:629)
```

  `private:` 在第 559 行，friend 声明在第 560 行，方法在第 629 行 —— 位置正确。普通消费者代码确实无法调用。worker 给的「注释掉的一行」是弱证据，我的否定编译才是硬证据。

- 逐 request 独立校验成立：单个越界不影响其他 request。
- 三个既有公共契约头零改动；未 include `tutti/spi/**`；词边界 guard 零命中。
- 我独立重跑 CTest：`1/1 Passed`，程序自报 `All 22 storage runtime contract tests passed`（原 14 + 新增 8，符合 ≥22 的要求）。
- 全部交付文件尾随空白与 EOF newline OK；`-Werror` 零告警。
- 未执行 sudo、模块操作、daemon 或任何硬件 IO。

### 一处诚实的表述值得肯定

worker 在第 2 节写道：修复后「同一份旧探测源码已无法编译（单请求重载被移除）」。它没有假装用同一份代码跑出对比结果，而是如实说明 API 变更导致旧探测失效，并指明等价验证落在 test 20/21。这个处理是对的 —— 破坏性 API 变更下，强行凑出「同一命令前后对比」反而是造假。

### 非阻塞观察（记录，不返工）

1. **`bounded_progress_()` 是空钩子。** 目前只是占位符，`wait()` 仍然是 1ms 轮询空转（单线程下无人能翻转终态标志）。去 `const` 只是把签名准备好了，真正的 progress 驱动要等真实 DataPath 接入。这符合我的 prompt 预期，但要记住「`wait()` 可驱动 progress」目前仍是**签名层承诺、行为层未实现**。

2. **`IoSubmitOutcome` 与 SPI 的 `SubmitOutcome` 形状高度相似但类型独立**（`IoRequestState` vs `RequestState`、`IoRequestInitialState` vs `RequestInitialState`）。这是**正确**的分层（Runtime 身份与 SPI 身份必须分开），但将来 lowering 时需要一个明确的映射函数，且要防止有人图省事把两者混用。建议在 lowering 落地时把该映射也做成单一声明点，参照本轮 Session 3 的 binding 收敛思路。

3. 多线程安全仍未实现（`memory_entries_`/`target_entries_`/`io_entries_` 无锁），与架构 6.2「handle 支持并发读取状态」的落差依旧存在。本轮未要求，继续记账。

后续决定：T-016 完成，不需要返工。Round 4 Session 3 遗留的两项 `REQUIRED FOLLOW-UP` 至此全部清偿。
