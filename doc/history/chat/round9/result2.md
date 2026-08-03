# T-032 Round 9 Session 2 Result — StorageRuntime 并发、错误与 shutdown 硬化

## 0. 结论

**PASS**。34 storage runtime contract tests passed (27 existing + 7 new)，20 次连跑稳定。HOST profile 10/10 CTest 全过，无 CUDA/libnvm/gRPC/yaml 依赖。

## 1. 锁/credit/progress gate 精确模型

### 1.1 Registry mutex

```
mutable std::mutex registry_mutex_;
```

保护所有 registry 数据：`memory_entries_`、`target_entries_`、`io_entries_`、gen counters、`terminal_result_count_`、`state_` 转换、`progress_gates_` map。

**持锁调用 DataPath 的场景**（安全，因为 DataPath 虚方法不回调 Runtime → 无死锁）：
- `close()` → `DataPath::close()`
- `unregister_memory()` / `free_memory()` → `DataPath::unregister_memory()`
- `submit_component_backed_()` → `DataPath::submit()` + `DataPath::register_memory()` + `DataPath::release()`（orphan op 回滚）
- `refresh_component_io_()` → `DataPath::query()` + `DataPath::release()`
- `finalize_shutdown_locked_()` → `DataPath::close()` + `DataPath::unregister_memory()` + `DataPath::shutdown()`

**不持锁调用 DataPath 的场景**（热路径，在 progress gate 内）：
- `drive_progress_unlocked_()` → `DataPath::progress()`

### 1.2 Inflight credit

submit 在调用 `DataPath::submit` **之前**为每个 accepted-request candidate 增 `memory_entries_[slot].inflight_count` 和 `target_entries_[slot].inflight_count`（credit）。这确保 `close()` / `unregister_memory()` 在 submit 期间看到 `BUSY`，不会拆除正在被 DataPath 使用的 target/memory。

DataPath::submit 返回后，被 DataPath reject 的 request 的 credit 被回滚（`--inflight_count`）。被 DataPath accept 的 request 保留 credit，直到 op 终态时 `release_inflight_references_()` 释放。

### 1.3 Per-DataPath progress serialization gate

```
std::unordered_map<DataPath*, std::unique_ptr<std::mutex>> progress_gates_;
```

每个注入的 DataPath 在 `initialize_components_` 时创建一个独立 mutex。`drive_progress_unlocked_()` 对每个 DataPath 先 lock 其 gate 再调用 `progress()`。不同 DataPath 的 gate 独立 → 可并发 progress。

**关键扩展**：`submit_component_backed_()` 和 `refresh_component_io_()` 在调用 `DataPath::submit/query/release` 时也持有对应 DataPath 的 progress gate。这确保 submit/progress/query/release 在同一 DataPath 上完全串行化，满足"不以 DataPath 必然线程安全代替 Runtime 的 registry 安全"——Runtime 不假设 DataPath 内部状态（如 `ops_` map）是线程安全的。

**无死锁证明**：
- submit：registry_mutex_ → progress_gate（始终先 registry 后 gate）
- progress：progress_gate（不持 registry_mutex_，因为 drive_progress_unlocked_ 先 unlock registry 再 lock gate）
- lock ordering 一致（registry → gate），无循环等待

### 1.4 Condition variable

```
std::condition_variable io_cv_;
```

`finish_io_()` 在 op 变 terminal 时 `notify_all()`。`shutdown()` 和 `wait()` 用 `wait_for` 避免纯 busy-poll。

## 2. DataPath 失败如何映射为 Runtime terminal failure

### 2.1 refresh_component_io_ 的失败终结

```
for each sub_op in entry.data_path_operations:
    acquire progress_gate
    snapshot = DataPath::query(sub_op.op)
    release progress_gate
    if !snapshot.ok():
        failed = true; first_failure = snapshot.status()
        continue        ← 不等其他 sub-op，直接终结
    if snapshot.state == IN_FLIGHT:
        all_terminal = false
    if snapshot.state == FAILED:
        failed = true; first_failure = snapshot.status()

if !all_terminal && !failed:
    return              ← 仍在飞，等下次 progress

for each sub_op:
    acquire progress_gate
    DataPath::release(sub_op.op)    ← best-effort
    release progress_gate

finish_io_(entry, failed ? FAILED : COMPLETED, first_failure)
```

**核心保证**：DataPath::query 返回 `!ok()` 时，op 立即终结为 `FAILED`（不留在 `IN_FLIGHT`）。这满足目标 #4："DataPath progress/query/release 的失败不能让 Runtime IoHandle 永远停在 IN_FLIGHT"。

### 2.2 release 失败的处理

`refresh_component_io_` 对 `DataPath::release` 返回值做 best-effort 处理（忽略错误）。op 已经被 `finish_io_` 标记为 terminal，Runtime 的 inflight credit 已释放。DataPath 侧的 op 资源如果 release 失败，是 DataPath 的内部问题——Runtime 不重试，不阻塞。

## 3. Shutdown / 析构 timeout 的资源策略

### 3.1 shutdown(timeout_ms)

```
lock registry_mutex_
if state == STOPPED: return OK
state = DRAINING
if count_inflight_io_() == 0: return finalize_shutdown_locked_()
if timeout_ms == 0: return TIMEOUT  ← 不释放任何资源
loop:
    drive_progress_unlocked_(lock)   ← unlock → progress(gate) → lock → refresh
    if count_inflight_io_() == 0: return finalize_shutdown_locked_()
    if deadline reached: return TIMEOUT  ← 保持 DRAINING，资源全保留
    io_cv_.wait_for(lock, 1ms, ...)
```

- `shutdown(0)` 遇 inflight → `TIMEOUT`，**不释放任何资源**，state 保持 `DRAINING`
- `shutdown(>0)` 在 deadline 内 drain；超时 → `TIMEOUT`，保持 `DRAINING`
- 全部 op terminal 后 `finalize_shutdown_locked_()` 按 target → memory → DataPath 顺序释放
- `DRAINING` 状态下 `query/wait/release_io` 仍可推进

### 3.2 析构

```
~StorageRuntime():
    if state != STOPPED:
        shutdown(0)              ← best-effort drain
    if state != STOPPED:
        return                   ← 仍有 inflight → 保守泄漏，不 free
    lock registry_mutex_
    free runtime-owned memory    ← 仅在 STOPPED 后执行
```

**保守策略**：如果析构时仍有 inflight op（shutdown(0) 超时），**不释放任何内存**（leak），绝不 UAF。这满足目标 #5："析构不得为了释放内存绕过这个规则"。

## 4. 并发测试清单与结果

### 新增测试（28-34）

| # | 场景 | 验证点 | 结果 |
|---|---|---|---|
| 28 | 8 线程并发 submit/wait/release | 多线程安全，无 UAF/crash | PASS ×20 |
| 29 | submit 后并发 close+unregister | inflight credit → BUSY | PASS ×20 |
| 30 | 两 IoHandle 并发 query（block progress） | progress gate → max concurrent=1 | PASS ×20 |
| 31 | DataPath query 注入失败 | op → FAILED（不永久 IN_FLIGHT） | PASS ×20 |
| 32 | shutdown(0) → DRAINING → drain → shutdown → STOPPED | shutdown retry 语义 | PASS ×20 |
| 33 | terminal result 上限 backpressure | RESOURCE_EXHAUSTED → release → submit OK | PASS ×20 |
| 34 | 析构时 inflight op（manual mode） | 不 crash、不 UAF | PASS ×20 |

### fake DataPath 注入点

```
std::atomic<bool> fail_query       → query() 返回 DEVICE_ERROR
std::atomic<bool> fail_progress    → progress() 返回 DEVICE_ERROR
std::atomic<bool> fail_release     → release() 返回 DEVICE_ERROR
std::atomic<bool> manual_mode      → progress() 不自动 complete
std::atomic<int>  progress_concurrent / progress_max_concurrent
std::atomic<bool> block_progress_flag → progress() 阻塞直到 cleared
```

### 20 次重复

```
run 1-20: all PASS
```

### ThreadSanitizer

环境不支持（`libtsan` 缺失，`-fsanitize=thread` 链接失败）。20 次重复 + 确定性 fake seam 是当前验证手段。

## 5. 既有测试无回归

| 测试组 | 数量 | 结果 |
|---|---|---|
| 既有 stub mode (1-23) | 23 | PASS |
| 既有 component-backed (24-27) | 4 | PASS |
| 新增并发/错误 (28-34) | 7 | PASS |
| **合计** | **34** | **PASS** |

全 CTest 10/10 通过（含 data_path_contract、spi_consumer、public_api 等）。

## 6. 保留的 public API 缺口

无新增 public API 缺口。当前 `IoResult`（`IoState` + `Status`）足以表达 terminal failure。`IoSnapshot`（仅 `IoState`）在 query 非阻塞路径足够；terminal status 通过 `wait()` 的 `IoResult` 观察。

如果未来需要 per-request terminal result（而非 per-batch `IoHandle`），需要扩展 `IoResult` 为 per-request 数组——当前不阻塞，记录为已知缺口。

## 7. 修改文件

| 文件 | 改动 |
|---|---|
| `tutti/include/tutti/storage_runtime.h` | 加 `registry_mutex_`、`io_cv_`、`progress_gates_`；所有 public 方法加锁；submit credit 模型；refresh_component_io_ query 失败→FAILED；drive_progress_unlocked_ 替代 bounded_progress_；析构保守化 |
| `tests/storage_runtime_contract/storage_runtime_contract_test.cpp` | fake DataPath 加错误注入 + progress 探针；新增 test 28-34 |
| `tests/storage_runtime_contract/CMakeLists.txt` | 加 `Threads::Threads` 链接 |
| `chat/round9/result2.md` | 本文件 |

未修改：`LocalNvmeDataPath`、resolver、binding、libnvm、kernel、public/SPI header（`status.h`/`io_types.h`/`data_path.h`）、`tutti/CMakeLists.txt`。

## 8. 环境验证

```
HOST profile configure: 无 CUDA/libnvm/gRPC/yaml 查找 ✓
HOST profile build: 10 targets 全部编译/链接成功 ✓
HOST profile CTest: 10/10 passed ✓
git diff --check: clean ✓
linter diagnostics: 0 ✓
20x repeat: all PASS ✓
```

## PASS
