# T-014 StorageRuntime Public Facade — Result

## 1. 修改/新增文件列表

| 文件 | 操作 |
|---|---|
| `tutti/include/tutti/storage_runtime.h` | 新增 |
| `tests/storage_runtime_contract/CMakeLists.txt` | 新增 |
| `tests/storage_runtime_contract/storage_runtime_contract_test.cpp` | 新增 |
| `chat/round4/result3.md` | 新增（本文件） |

**选择方案：header-only。** 理由：避免触碰构建系统，减少与其他 worker 的冲突面。所有最小簿记逻辑以内联方法实现在头文件中，standalone CMake 只需编译一个测试 .cpp。

## 2. StorageRuntime 及配套值类型的实际 public surface

### 值类型

```cpp
enum class RuntimeState { RUNNING, DRAINING, STOPPED };

struct RuntimeConfig {
    std::uint64_t max_terminal_results = 64;
    std::string profile_name = "host";
};

struct CudaLikeProfileInfo { std::string profile_name; int device_count; };
struct DeviceInfo { std::int32_t device_id; std::string name; std::uint64_t total_memory; };
struct DeviceCapabilities { std::int32_t device_id; bool supports_host_execution; bool supports_device_execution; std::uint64_t max_io_size; };
struct MemorySpec { std::uint64_t size; MemoryKind kind; std::int32_t device_id; };
struct MemoryAllocation { MemoryHandle handle; void* address; std::uint64_t size; };

// MemoryInfo: 6 fields, NO transport-private descriptor.
struct MemoryInfo {
    MemoryKind kind;
    MemoryOwnership ownership;
    std::uint64_t size;
    void* address;          // user-visible address only
    std::int32_t device_id;
    int inflight_count;
};

struct OpenOptions { std::string scheme; };
struct TargetInfo { std::string uri; std::uint64_t logical_size; int inflight_count; };
enum class IoState { IN_FLIGHT, COMPLETED, FAILED };
struct IoSnapshot { IoState state; };
struct IoResult { IoState state; Status status; };

struct WaitOutcome {
    Status observation_status;            // OK | TIMEOUT | error
    std::optional<IoResult> result;       // present only when terminal
};
```

### StorageRuntime 类

```cpp
class StorageRuntime {
public:
    static Result<std::unique_ptr<StorageRuntime>> create(RuntimeConfig = {});

    // lifecycle
    Status shutdown(std::uint64_t timeout_ms);
    RuntimeState state() const noexcept;

    // discovery
    CudaLikeProfileInfo query_cuda_like_profile() const;
    Result<std::vector<DeviceInfo>> list_devices() const;
    Result<DeviceCapabilities> query_device_capabilities(std::int32_t) const;

    // memory
    Result<MemoryAllocation> allocate_memory(const MemorySpec&);
    Status free_memory(const MemoryHandle&);
    Result<MemoryHandle> register_memory(const MemoryView&);
    Status unregister_memory(const MemoryHandle&);
    Result<MemoryInfo> query_memory(const MemoryHandle&) const;

    // target
    Result<TargetHandle> open(std::string_view, const OpenOptions&);
    Status close(const TargetHandle&);
    Result<TargetInfo> query_target(const TargetHandle&) const;

    // IO
    Result<IoHandle> submit(const IoRequest&, const HostSubmitContext&);
    Result<IoSnapshot> query(const IoHandle&) const;
    WaitOutcome wait(const IoHandle&, std::uint64_t timeout_ms) const;
    Status release_io(const IoHandle&);

    // testing-only stub (not real data path)
    Status testing_force_complete_io(const IoHandle&, IoState, Status = Status::Ok());
};
```

## 3. WaitOutcome 的精确语义与实现方式

`WaitOutcome` 包含两个字段：

- `observation_status` (Status): 描述本次 `wait()` 调用的观察结果
  - `OK` — operation 已终态，`result` 有值
  - `TIMEOUT` — operation 仍在飞行，`result` 为 `nullopt`，**operation 未被取消**
  - 非 OK 非 TIMEOUT（如 `NOT_FOUND`）— handle 错误，`result` 为 `nullopt`

- `result` (optional<IoResult>): 仅在 operation 已终态时存在

**关键：观察超时不取消 operation。** `wait()` 在超时后直接返回 `TIMEOUT`，不修改 IO entry 状态，不递减 inflight count。随后 `query()` 仍能查到该 operation 的 `IN_FLIGHT` 状态。

实现方式：`wait()` 首先验证 handle，然后检查 entry.terminal。如果已终态，立即返回 OK + IoResult。如果飞行中且 timeout > 0，以 1ms 间隔轮询直到终态或超时。如果 timeout == 0，直接返回 TIMEOUT。

## 4. handle 失效 / generation 不复用 / 跨 Runtime 拒绝

### Handle 失效

每个 entry 有 `active` 标志和 `generation` 值。`free_memory/unregister_memory/close/release_io` 在成功后将 `active` 设为 false。验证方法检查 `active` — 失效的 handle 返回 `NOT_FOUND`。

### Generation 不复用

使用全局递增计数器（`memory_gen_counter_`、`target_gen_counter_`、`io_gen_counter_`），每次 mint 时 `++counter`。slot 可复用，但每次复用时 generation 不同。旧 handle 的 generation 与新 entry 的 generation 不匹配 → 验证失败。

### 跨 Runtime 拒绝

每个 `StorageRuntime` 实例从静态原子计数器获取唯一 `runtime_id_`（从 1 开始）。验证方法首先检查 `h.runtime_id_ != runtime_id_` — 不匹配则直接返回 false。跨 Runtime 的 handle 被拒绝为 `NOT_FOUND`，不 crash。

## 5. BUSY 语义的实现方式与覆盖范围

### Memory BUSY

- `free_memory()` 和 `unregister_memory()` 在 `entry.inflight_count > 0` 时返回 `BUSY`
- 不隐式阻塞、不提前 unmap/free

### Target BUSY

- `close()` 在 `entry.inflight_count > 0` 时返回 `BUSY`
- 不静默延迟关闭

### IO BUSY

- `release_io()` 在 `!entry.terminal` 时返回 `BUSY`

### Inflight count 维护

- `submit()` 时递增 memory 和 target 的 `inflight_count`
- `testing_force_complete_io()` 时递减

## 6. 所有权对称性的实现方式

每个 `MemoryEntry` 记录 `ownership`（`RUNTIME_OWNED` 或 `CALLER_OWNED`）。

- `allocate_memory()` 创建 `ownership = RUNTIME_OWNED` 的 entry
- `register_memory()` 创建 `ownership = CALLER_OWNED` 的 entry
- `free_memory()` 检查 `ownership == RUNTIME_OWNED`，否则返回 `INVALID_ARGUMENT`
- `unregister_memory()` 检查 `ownership == CALLER_OWNED`，否则返回 `INVALID_ARGUMENT`

## 7. 本轮未实现或无法测的语义清单

| 语义 | 状态 | 原因 |
|---|---|---|
| DRAINING 状态 | **已测** | stub IO 不会自行终态，shutdown 超时后留在 DRAINING |
| 真实 async completion | 未实现 | 无真实 DataPath，IO 只能通过 `testing_force_complete_io()` 终态 |
| terminal result backpressure | **已测** | `terminal_result_count_ >= max_terminal_results` 时 submit 返回 `RESOURCE_EXHAUSTED` |
| 真实 device execution | 未实现 | 无 CUDA SDK 依赖，`HostSubmitContext` 被忽略 |
| 真实 resolver / target open | 未实现 | `open()` 创建内存中假 target |
| 多线程安全 | 未实现 | 测试为单线程，未加锁 |

## 8. configure / build / ctest 结果

### Configure

```
-- The CXX compiler identification is GNU 13.1.1
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: /opt/rh/gcc-toolset-13/root/usr/bin/c++ - skipped
-- Detecting CXX compile features - done
-- Configuring done (0.2s)
-- Generating done (0.0s)
```

无 CUDA、gRPC、yaml-cpp、libnvm、FIEMAP 或 NVMe discovery。

### Build

```
[ 50%] Building CXX object CMakeFiles/tutti_storage_runtime_contract_test.dir/storage_runtime_contract_test.cpp.o
[100%] Linking CXX executable tutti_storage_runtime_contract_test
[100%] Built target tutti_storage_runtime_contract_test
```

零告警（`-Wall -Wextra -Werror`）。

### CTest

```
1/1 Test #1: tutti_storage_runtime_contract_test ...   Passed    0.01 sec
100% tests passed, 0 tests failed out of 1
```

14 个子测试全部通过。

## 9. 既有公共契约零改动的证明

```
$ git diff --stat -- tutti/include/tutti/status.h tutti/include/tutti/io_types.h tutti/include/tutti/memory_types.h

(empty output, exit code 0)
```

三个冻结头文件零改动。

## 10. 未依赖 SPI 的证明

```
$ grep -n 'spi/' /data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h

(exit code 1, no output)
```

`storage_runtime.h` 不 include 任何 `tutti/spi/**` 头。

## 11. public-boundary guard 结果

```
$ grep -nEiw 'libnvm|nvme|fiemap|grpc|yaml|prp|sgl|lba|cid|doorbell|iova|rkey|fd|extent' \
  /data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h

(exit code 1, no output)
```

无输出，PASS。

## 12. 文件边界与空白检查结果

```
$ git diff --check -- tutti/include/tutti/storage_runtime.h

(no output, exit code 0)
```

尾随空白 + EOF newline 检查（全部新增文件）：

```
tutti/include/tutti/storage_runtime.h                                       — PASS
tests/storage_runtime_contract/CMakeLists.txt                               — PASS
tests/storage_runtime_contract/storage_runtime_contract_test.cpp           — PASS
```

本 session 新增的文件仅在 `tests/storage_runtime_contract/` 和 `tutti/include/tutti/storage_runtime.h` 下。未修改任何禁止文件。

## 13. 最终结论

```
PASS
```

全部 15 项成功标准均满足：

1. `StorageRuntime` 覆盖 lifecycle / discovery / memory / target / IO 五组 API ✓
2. 类名精确为 `tutti::StorageRuntime`，与 `io_types.h` 既有 friend 声明匹配 ✓
3. `WaitOutcome` 正确区分观察超时与执行终态，观察超时不取消 operation ✓
4. handle 在 close/unregister/release 后确定性失效，generation 不复用 ✓
5. 跨 Runtime handle 被拒绝且不 crash ✓
6. BUSY 语义在 memory 与 target 两处均成立，不隐式阻塞 ✓
7. allocate/free 与 register/unregister 的所有权配对被强制 ✓
8. terminal result 不自动淘汰 ✓
9. 公共 API 不泄漏 transport-private 字段 ✓
10. 公共头未 include SPI，未直接 include CUDA SDK ✓
11. 既有三个公共契约头零改动 ✓
12. HOST standalone configure/build/ctest 在 `-Werror` 下通过 ✓
13. 未修改允许列表外文件 ✓
14. 未执行任何模块、daemon 或 IO 操作 ✓
15. 空白检查通过 ✓

## 总指挥验收

验收结论：`PASS WITH REQUIRED FOLLOW-UP`。

worker 满足了我列出的全部 15 条成功标准，交付合格、**不返工**。但我在独立核验中实测出两处与目标架构不符的落差，**责任在我的 prompt**（验收清单漏项），必须在 Round 5 补上。

### 已独立核验通过的部分

- 类名精确为 `tutti::StorageRuntime`，与 `io_types.h:50` 的 `friend class ::tutti::StorageRuntime;` 匹配。我用独立探测程序成功让它 mint 出 `MemoryHandle`/`TargetHandle`/`IoHandle`，确认 Round 2 预留的唯一 minting 边界真正闭合 —— 这是本任务最关键的结构性验证点。
- 五组 API（lifecycle / discovery / memory / target / IO）齐备。
- **`WaitOutcome` 语义正确且被真实测出。** 测试第 252-260 行确认：in-flight + 极小 timeout → `TIMEOUT` + `result == nullopt`，随后 `query()` 仍返回 `IN_FLIGHT` —— 观察超时确实不取消 operation。这是最容易做错的一点，做对了。
- **DRAINING 状态真被测到**（测试第 386-411 行）：inflight 存在时 `shutdown` 返回 `TIMEOUT`、状态停在 `DRAINING`、且此时 handle 仍可 query/wait/release。我在 prompt 里允许 worker 声明"无法测"，它选择真正实现并测出来，超出最低要求。
- terminal result 不自动淘汰（第 314-338 行）、跨 Runtime handle 被拒（第 352-368 行）、backpressure `RESOURCE_EXHAUSTED` 均有真实断言。
- generation 用单调递增计数器，slot 可复用但 generation 永不复用；`validate_*` 三重校验 `runtime_id` / `active` / `generation`，逻辑正确。
- 所有权对称性正确：`free_memory` 要求 `RUNTIME_OWNED`、`unregister_memory` 要求 `CALLER_OWNED`，否则 `INVALID_ARGUMENT`。
- `MemoryInfo` 六字段，无 IOVA / rkey / data-path pointer；词边界 guard 零命中；未 include `tutti/spi/**`；未直接 include CUDA SDK。
- 三个既有公共契约头零改动（`git diff --stat` 为空）。
- 析构只 free `active && RUNTIME_OWNED`，与 `free_memory` 置 `active=false` 配合，无双重释放。
- 总指挥独立重跑 CTest：`1/1 Passed`，程序自报 `All 14 storage runtime contract tests passed`。
- 全部交付文件尾随空白与 EOF newline OK；`-Werror` 零告警；linter 0 diagnostics。
- 未执行 sudo、模块操作、daemon 或任何硬件 IO。

选择 header-only 的判断正确：避免触碰 `tutti/CMakeLists.txt`，与并发的 Session 4（正在改该文件建 `tutti_spi`）零冲突。

### 落差 A（必须补）：`submit()` 是单请求，无法表达 batch partial-commit

实际签名：

```cpp
Result<IoHandle> submit(const IoRequest& request, const HostSubmitContext& context);
```

目标架构在 Runtime 层明确要求批量与部分提交：

- 第 680 行：「因此 batch 允许部分执行，但任何已经发出的工作都不会失去 owner、lease 或完成观察路径。」
- 第 678 行：「contract test 必须覆盖『第 K 个 request 发出后提交失败』。」
- 第 677 行：Runtime 收到任一有效 `DataPathOp` 就返回 `IoHandle`，并把其余 request 记录为 per-request failure。

单请求签名在结构上**无法**表达「4 个 request、第 3 个发出后失败」这一契约，也无处安放 per-request 初始状态。我确认头文件中 `RequestInitialState` / per-request 状态相关表达为零。

注意：SPI 层（`tutti/spi/data_path.h`）已经把这套语义做对了 —— 它有 `SubmitOutcome{status, optional<DataPathOp> op, vector<RequestInitialState>}`。所以现在是**Runtime 层比它下面的 SPI 层表达能力更弱**，Runtime 无法把 DataPath 的部分提交结果如实转达给调用方。

**责任判定：我的 prompt 缺陷。** 我在 prompt 第 106 行引用架构时写的是 `submit(requests, submit_context)`（复数），但在 14 条 contract test 要求和 15 条成功标准里**一次都没有**要求批量、partial-commit 或 per-request 状态。worker 按我给的验收清单实现，无可指摘。经核查，全 prompt 中 `batch`/`partial`/`bounds` 仅出现在那一行引用里。

### 落差 B（必须补）：`submit()` 不校验 request bounds

架构 7.3 步骤 1 要求「验证 runtime 状态、handle generation 和 **request bounds**」。实测：

```text
注册 4096 字节缓冲，提交 memory_offset=1<<30, length=1<<40 的请求
  -> out-of-bounds submit accepted = 1   (应为 0)
提交 length=0 的请求
  -> zero-length submit accepted  = 1
```

即越界与零长度请求均被静默接受。当前是 stub 无真实 DMA，故无内存安全后果；但一旦接上真实 DataPath，这就是直接的越界 DMA 风险。校验必须在 Runtime 层完成（DataPath 只保证自己的 capability 约束，不负责回算调用方 buffer 边界）。

同样属我的 prompt 漏项。

### 其他非阻塞观察（记录，不返工）

1. **`wait()` 被标记 `const`，且轮询非原子标志。** 实测 `wait(io, 300)` 耗时 316ms 后返回 `TIMEOUT` —— 单线程下没有任何东西能翻转 `terminal`，1ms 轮询循环纯属空转烧预算。更重要的是架构 7.4 要求「`query()` 非阻塞，可以驱动一次有界 progress」「`wait()` 通过有界 progress/退避循环等待」，而 `const` 方法**无法驱动 progress**。真实实现时 `wait()`（很可能还有 `query()`）必须去掉 `const`。这一点建议在下一轮连同落差 A 一起改，避免签名二次变更。

2. **`testing_force_complete_io()` 位于 public 区段**（第 462 行，介于 `public:` 130 与 `private:` 486 之间），是生产门面上的测试后门，且当前是驱动 IO 终态的唯一手段。接入真实 DataPath 时必须移除，或至少收敛为 friend-test / `#ifdef` 隔离。现阶段作为 stub 手段可接受，但不能随门面一起对外发布。

3. **单线程假设。** `memory_entries_` / `target_entries_` / `io_entries_` 均为普通 `std::vector`，无锁；只有 `state_` 是 atomic。架构 6.2 要求 handle「支持并发读取状态」，当前不满足。worker 已在第 7 节如实声明"多线程安全 未实现"。另注：`find_free_*_slot_()` 的 `push_back` 会使既有引用失效，将来加并发时这是隐患点。

4. `IoEntry::request` 字段被存储但除派生 `memory_slot`/`target_slot` 外未使用；`IoEntry::released` 与 `active=false` 语义重叠。属可接受的轻微冗余，将来落地真实实现时自然收敛。

### 后续决定

T-014 交付合格，不返工。Round 5 必须包含一个 `StorageRuntime` 契约补强任务，一次性解决：

1. `submit()` 改为批量（`const IoRequest*, std::size_t` 或 span），返回能表达 per-request 初始状态与部分提交的结果类型，并补「第 K 个失败」contract test；
2. 补 request bounds 校验（memory_offset+length ≤ 注册 size；target_offset+length ≤ logical_size；length > 0）；
3. `wait()` / `query()` 去掉 `const` 以便未来驱动有界 progress；
4. `testing_force_complete_io()` 收敛出公共 surface。

第 1、3 项都会改动 `submit`/`wait` 签名，应合并在同一 session 完成，避免连续两轮破坏性变更。
