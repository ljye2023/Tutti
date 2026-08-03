# T-011 Result — Frozen In-Repo DataPath Source-Level SPI

Task: freeze the in-repo `DataPath` host-side SPI, `DataPathCapabilities`,
submit/progress/op lifecycle value types, and the supporting opaque
target/memory/op identities, as a header-only C++17 SPI under `tutti/spi/`,
plus a standalone hardware-free contract test with a fake DataPath.

## 1. Modified / created files

Only files in the allowed list were touched:

| Path | Status |
| --- | --- |
| `tutti/spi/data_path.h` | created |
| `tests/data_path_contract/CMakeLists.txt` | created |
| `tests/data_path_contract/data_path_contract_test.cpp` | created |
| `chat/round3/result4.md` | created (this file) |

Build artifacts were written only to `build/round3-session4/` (inside the
existing ignored `build/` tree). No other files were modified. No Git commit.

The target resolver (`tutti/spi/storage_target_resolver.h`) did not exist at
execution time; per the task, `data_path.h` forward-declares `ResolvedTarget`
(and `ResourceProvider`) and the contract test provides stub definitions
instead of including another worker's header.

## 2. SPI types & methods — actual public surface

All types live in `namespace tutti` in `tutti/spi/data_path.h`.

Opaque identities (phantom-tagged, distinct strong types, no cross-conversion;
token is an opaque integer, never a pointer/address; minted only via the
in-repo `tutti::detail::SpiIdentityMint` seam):

```cpp
using DataPathTarget = detail::OpaqueSpiIdentity<detail::DataPathTargetTag>;
using DataPathMemory = detail::OpaqueSpiIdentity<detail::DataPathMemoryTag>;
using DataPathOp     = detail::OpaqueSpiIdentity<detail::DataPathOpTag>;
```

Each exposes `valid()`, `token()`, `generation()`, `==`/`!=`, default-invalid
(`generation == 0`), trivially copyable value semantics.

Value types:

```cpp
struct RegistrationDomainKey { std::string value; /* ==, != */ };
enum class MemoryKind { HOST, DEVICE };
struct DataPathMemoryView { void* base; std::uint64_t size_bytes;
                            std::int32_t device_id; MemoryKind kind; };
struct DataPathConfig { std::string name; };
enum class RegistrationScope { PER_TARGET, PER_DEVICE, GLOBAL };
enum class ProgressModel { HOST_POLL, DEVICE_AUTONOMOUS };
struct DataPathCapabilities { /* see section 3 */ };
struct DataPathRequest { IoRequest intent; DataPathMemory memory; DataPathTarget target; };
enum class RequestState { ACCEPTED, REJECTED };
struct RequestInitialState { RequestState state; Status status; };
struct SubmitOutcome { Status status; std::optional<DataPathOp> op;
                       std::vector<RequestInitialState> initial_states; };
enum class OpState { IN_FLIGHT, COMPLETED, FAILED };
struct DataPathSnapshot { OpState state; Status status; std::uint64_t bytes_transferred; };
struct ProgressBudget { std::uint64_t max_work_units; std::uint64_t timeout_ns; };
struct ProgressResult { std::uint64_t work_units_consumed, operations_advanced,
                        operations_terminal; bool more_work_likely;
                        std::optional<std::uint64_t> next_poll_deadline_ns; };
```

SPI base class (host-side, virtual; no device API here):

```cpp
class DataPath {
public:
    virtual ~DataPath() = default;
    virtual const DataPathCapabilities& capabilities() const = 0;
    virtual Status initialize(const DataPathConfig&, ResourceProvider&) = 0;
    virtual Status shutdown(std::uint64_t timeout_ns) = 0;
    virtual Result<DataPathTarget> open(const ResolvedTarget&) = 0;
    virtual Status close(DataPathTarget) = 0;
    virtual Result<RegistrationDomainKey> registration_domain(DataPathTarget) const = 0;
    virtual Result<DataPathMemory> register_memory(const DataPathMemoryView&,
                                                   const RegistrationDomainKey&) = 0;
    virtual Status unregister_memory(DataPathMemory) = 0;
    virtual SubmitOutcome submit(const DataPathRequest*, std::size_t,
                                 const HostSubmitContext&) = 0;
    virtual Result<ProgressResult> progress(ProgressBudget) = 0;
    virtual Result<DataPathSnapshot> query(DataPathOp) const = 0;
    virtual Status release(DataPathOp) = 0;
};
```

`ResolvedTarget` and `ResourceProvider` are forward-declared in `data_path.h`
and referenced by reference only; the header does not include their
definitions. `DataPathRequest` carries the public `IoRequest` plus data-path
memory/target identities; it contains no transport-private fields.

## 3. DataPathCapabilities — minimum field list

Capabilities are hard constraints, not hints.

| Field | Type | Semantics |
| --- | --- | --- |
| `name` | `std::string` | stable identity |
| `source_api_version` | `std::uint32_t` | source API version |
| `supports_host_execution` / `supports_device_execution` | `bool` | execution domains |
| `supports_host_memory` / `supports_device_memory` | `bool` | memory kinds |
| `supports_direct` / `supports_staged` | `bool` | data movement |
| `supports_read` / `supports_write` | `bool` | directions |
| `target_alignment_bytes` / `memory_alignment_bytes` / `length_alignment_bytes` | `std::uint64_t` | alignment (bytes) |
| `max_single_io_bytes` | `std::uint64_t` | max single IO |
| `max_batch_requests` / `max_batch_bytes` | `std::uint64_t` | max batch |
| `max_in_flight_operations` | `std::uint64_t` | max in-flight |
| `supports_scatter_gather` / `max_scatter_gather_entries` | `bool` / `std::uint64_t` | scatter-gather |
| `registration_scope` | `RegistrationScope` | registration scope |
| `progress_model` | `ProgressModel` | progress model |
| `device_completion_fence_on_caller_stream` | `bool` | real IO completion fence on caller stream |
| `device_execution_autonomous` | `bool` | device progress without host query/wait |
| `supports_multi_stream` | `bool` | multi-stream concurrency |
| `max_concurrent_streams` / `max_concurrent_operations` | `std::uint64_t` | concurrency caps |
| `supports_multi_gpu` / `supports_cross_device` | `bool` | multi-device |
| `optional_target_features` | `std::vector<std::string>` | optional features (open set, not a closed enum) |

`device_completion_fence_on_caller_stream` and `device_execution_autonomous`
are independent fields distinguishing a real caller-stream completion fence
from device-autonomous progress.

## 4. submit / partial-commit / op lifecycle — implementation & test

`SubmitOutcome` invariants (enforced by the fake and verified by tests):

- `op == nullopt` => zero transport requests irreversibly issued (status may
  still be non-OK when all were rejected).
- `op != nullopt` => at least one request irreversibly issued and still
  observable, even on partial failure.
- `initial_states.size() == input request count`, in input order.

Fake `submit` issues requests left-to-right; `fail_at_index` marks the first
index that cannot be irreversibly issued (and all after). Issued requests are
ACCEPTED and attached to a freshly minted `DataPathOp` with per-op private
scratch; rejected requests are REJECTED with a `RESOURCE_EXHAUSTED` status.

Test #5 (4 requests, `fail_at_index = 3`): indices 0,1,2 ACCEPTED, index 3
REJECTED; `status` non-OK; `op` present and valid; 4 initial states in order;
the op (holding the first 3 issued requests) is queryable (`query` returns
`IN_FLIGHT`). PASS.

Op lifecycle:
- `query()` never erases the op (test #7: two consecutive queries both return
  `IN_FLIGHT`, op count unchanged, subsequent `release` still returns `BUSY`).
- `release()` only on terminal ops: non-terminal returns `BUSY` (test #8);
  after `progress` drives the op to `COMPLETED`, `release` returns `OK` and the
  op is gone. PASS.

## 5. progress budget & result semantics

`ProgressBudget` carries two hard caps: `max_work_units` and `timeout_ns`.

Fake `progress` advances at most one in-flight op to terminal per work unit,
strictly bounded by `max_work_units` (it decrements a remaining counter and
stops). After advancing, it scans for any remaining in-flight op to compute
`more_work_likely`, and suggests a `next_poll_deadline_ns` backoff iff more
work is likely (nullopt when idle).

Test #9/#10 (3 in-flight ops): budget `{2, 1000}` =>
`work_units_consumed == 2 <= 2`, `operations_advanced == 2`,
`operations_terminal == 2`, `more_work_likely == true`,
`next_poll_deadline_ns` present; one op remains in-flight. Second call drains
the remainder: `more_work_likely == false`, deadline nullopt. A zero-budget
call consumes nothing and returns (test #12). PASS — progress is bounded; no
infinite busy-poll is disguised as progress.

## 6. configure / build / ctest results

```
$ rm -rf build/round3-session4
$ cmake -S tests/data_path_contract -B build/round3-session4 -DCMAKE_BUILD_TYPE=RelWithDebInfo
-- The CXX compiler identification is GNU 13.1.1
-- Configuring done (0.2s)
-- Generating done (0.0s)
```

Configure log contains no CUDA / gRPC / yaml-cpp / libnvm / FIEMAP / NVMe
dependency discovery.

```
$ cmake --build build/round3-session4 --target tutti_data_path_contract_test -j8
[ 50%] Building CXX object ...data_path_contract_test.cpp.o
[100%] Linking CXX executable tutti_data_path_contract_test
[100%] Built target tutti_data_path_contract_test
```

Zero warnings, zero errors.

```
$ ctest --test-dir build/round3-session4 --output-on-failure -R '^tutti_data_path_contract_test$'
1/1 Test #1: tutti_data_path_contract_test ....   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

1/1 PASS.

## 7. Public-boundary guard results

```
$ grep -nEi 'cuda|hip|maca|musa|libnvm|nvme|fiemap|grpc|yaml|backends/|io_engine/|device_manager/|PRP|SGL|LBA|doorbell|descriptor|CID' \
    tutti/spi/data_path.h
(empty, guard_exit=1)
```

`#include` lines in `data_path.h`:

```
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <tutti/status.h>
#include <tutti/io_types.h>
```

Only C++17 standard headers plus the two allowed Tutti headers. No accelerator,
transport, or storage-private headers or type names appear in the SPI.

## 8. File-boundary & whitespace check results

```
$ git diff --check -- tutti/spi/data_path.h
(clean, diff_exit=0)
```

Trailing-whitespace scan over the three new files: empty (`ws_exit=1`).

EOF-newline check: all three files have a terminating newline.

Files touched by this session (allowed list only):

```
?? tests/data_path_contract/
?? tutti/spi/data_path.h
```

(`chat/round3/result4.md` is this file.) No file outside the allowed list was
modified or created. `tutti/spi/storage_target_resolver.h` was not created or
modified; `ResolvedTarget` is only forward-declared.

## 9. Final status

PASS

All success criteria met:

1. DataPath SPI covers open / registration / submit / progress / query /
   release (plus initialize/shutdown and capabilities).
2. Capabilities cover the architecture's minimum semantics as hard
   constraints.
3. `SubmitOutcome` precisely distinguishes zero-issued (`op == nullopt`) from
   partial-issued (`op != nullopt` with non-OK status).
4. The K-th-request-fails partial-commit contract test passes.
5. `DataPathOp` query/release lifecycle is explicit (`query` keeps the op;
   `release` only on terminal, else `BUSY`).
6. Progress carries a budget (max work units + wall-clock) and a result with
   counts, `more_work_likely`, and optional backoff deadline.
7. The SPI leaks no transport/device/kernel-private types.
8. A fake DataPath plugs in without modifying any Runtime public noun.
9. HOST standalone configure/build/ctest passes without CUDA SDK.
10. No file outside the allowed list was modified.
11. No module, daemon, or IO operation was executed.
12. Whitespace / EOF-newline checks pass.

## 总指挥验收

验收结论：`PASS WITH REQUIRED FOLLOW-UP`（本 session 交付合格且不返工，但引入 1 项**必须在下一轮修掉**的跨 header 类型冲突）。

### 已独立核验通过的部分

- `DataPath` 覆盖 capabilities / initialize / shutdown / open / close / registration_domain / register_memory / unregister_memory / submit / progress / query / release，纯 host-side virtual，未混入 device caller API。
- `DataPathTarget` / `DataPathMemory` / `DataPathOp` 是 phantom-tag 强类型；跨类型转换被编译器正确拒绝：

```text
tutti::DataPathTarget t = o;  // o 是 DataPathOp
  -> error: conversion from OpaqueSpiIdentity<DataPathOpTag> to
            non-scalar type OpaqueSpiIdentity<DataPathTargetTag> requested
```

- token 是 opaque 整数，非指针；SPI 中唯一的 `void*` 是 `DataPathMemoryView::base`，那是**调用方输入的缓冲地址**，不是内部对象地址，符合约束。
- `RegistrationDomainKey` 是 opaque string，测试验证其为 `domain-<n>` 派生值且跨调用稳定，未泄漏 controller pointer。
- 公共边界 guard：prompt 原样 substring 版**零命中**（本轮 worker 未触发 Session 2 那类子串误报）；我额外跑的词边界版 `cuda|hip|nvme|prp|sgl|lba|cid|doorbell|fd|extent|wr` 同样零命中。头文件只 include 5 个标准库头 + `<tutti/status.h>` + `<tutti/io_types.h>`。
- partial-commit 契约成立：`fail_at_index=3` 时 4 个 initial_states 顺序对应、前 3 个 ACCEPTED、第 4 个 REJECTED、overall status 非 OK、`op` 仍存在且可 `query`（`IN_FLIGHT`）。零发出的两种情形（count==0 与全 reject）均正确给出 `op == nullopt`。
- op 生命周期正确：`query()` 连续两次不擦除 op；非 terminal `release()` 返回 `BUSY`；`progress` 推到 `COMPLETED` 后 `release()` 返回 OK 且 op 消失。
- progress 有界：`max_work_units` 被硬性递减计数约束，`{2,1000}` 下 `work_units_consumed == 2`、`more_work_likely == true`、给出 backoff deadline；零预算调用消耗 0 并立即返回。无伪装的无限 busy-poll。
- 每 op 私有 scratch：两个 in-flight op 的 scratch 尺寸分别为 3×16 与 2×16，互不覆盖，identity 互异。
- 总指挥独立重跑 CTest：`1/1 Passed`。
- 我用 `-Werror` 重新编译（worker 的 CMake 只加了 `-Wall -Wextra`，**未加** `-Werror`）：仍然零告警，二进制运行 `all checks passed`。worker 声称的「zero warnings」经此独立复核成立。
- 文件边界正确，只新增 3 个源码文件 + result4.md。`tutti/spi/storage_target_resolver.h`（mtime 16:03:43）早于本 session 产物（16:06），是 Session 3 的交付物，本 session 未触碰。
- `tutti/include/**` 未被修改（`io_types.h` 15:31、`memory_types.h` 16:02，均早于本 session）。
- 四个文件尾随空白与 EOF newline 均 OK；`git diff --check` 通过。
- 未执行 sudo、模块操作、daemon 或任何硬件 IO。
- worker 对「Resolver 头在其执行时尚不存在」的处置是**正确的**：按 prompt 采用 forward declaration + test 侧 stub，没有抢先实现或改写另一 worker 的文件。事后验证 `storage_target_resolver.h` 与 `data_path.h` 可在同一 TU 共存编译（`resolver+datapath: PASS`），forward declaration 与真实定义兼容。

### 必须在下一轮修掉：`tutti::MemoryKind` 重复定义（硬编译错误）

`data_path.h:113` 在 `namespace tutti` 定义了 `enum class MemoryKind { HOST, DEVICE }`，与 Session 2 的 `tutti/include/tutti/memory_types.h:39` `enum class MemoryKind { HOST, PINNED_HOST, DEVICE, MANAGED }` **同名同命名空间**。实证：

```text
#include <tutti/memory_types.h>
#include <tutti/spi/data_path.h>
  -> error: multiple definition of 'enum class tutti::MemoryKind'
     note: previous definition here (memory_types.h:39)
```

这不是风格问题，是**任何同时需要公共 memory 契约与 DataPath SPI 的翻译单元都无法编译**。而 Runtime 的 `register_memory` 恰好必须同时看到两者（公共 `MemoryView` 进来、`DataPathMemoryView` 下去），所以这条路径将来必然踩上。

责任判定：**主要是我的 prompt 缺陷，不是 worker 失职。** Session 4 的 prompt 明确要求「不 include 对方头文件、自行定义 `DataPathMemoryView`」，却没有为其内部枚举指定避免撞名的命名规则；worker 在无法看到 `memory_types.h` 的隔离条件下选了最自然的名字。两个 session 并行执行，worker 无从发现冲突。因此**不返工、不扣分**。

同时注意还存在第三个同名定义：`tutti/accel/include/common/memory_kind.h:15` 的 `tutti::MemoryKind : uint8_t`（含 `EXTERNAL`）。它与 `memory_types.h` 也冲突（`different underlying type`），且**早于本轮就存在**，属既有技术债。

下一轮修复任务（新开 session，允许文件仅 `tutti/spi/data_path.h` + 其 contract test）：把 SPI 内部枚举改为不占用公共名字，推荐 `enum class DataPathMemoryKind { HOST, DEVICE }`，或移入 `tutti::spi` / `tutti::detail` 子命名空间。修复后必须新增一条「同一 TU 同时 include `memory_types.h` + `data_path.h` + `storage_target_resolver.h` 编译通过」的回归检查。`accel` 那份同名枚举的清理单独立项。

### 其他非阻塞后续项（记录，不返工）

1. **identity mint seam 可被任意代码伪造。** `detail::SpiIdentityMint` 是 public struct，任何代码都能凭空造出 `valid()==true` 的 identity：

```text
SpiIdentityMint::mint<DataPathOpTag>(1, 1)  ->  valid=1 token=1 gen=1
```

  这与 Session 4 的 `MemoryHandle`/`TargetHandle`/`IoHandle` 采用的「`StorageRuntime` 唯一 friend」封闭 minting 边界不一致。当前 header-only SPI 无法在不指定具体实现类的前提下收紧，属可接受折衷；但 `generation` 字段目前只用作 valid/invalid 标记，尚未承担 slot-reuse 防护（stale identity 检测）。DataPath 具体实现落地时应确立 generation 递增规则，否则 close/unregister/release 后的旧 identity 无法与新 identity 区分。

2. **`ProgressBudget::timeout_ns` 未被 fake 读取。** fake 只遵守 `max_work_units`，从不检查 `timeout_ns`（grep 确认测试与 fake 中零引用）。因此「progress 受 wall-clock 约束」这一条**只在类型层面表达了，未在行为层面验证**。result 第 5 节称两者都是 hard caps，就 fake 行为而言只有前者被证明。真实 DataPath 必须同时遵守，且需要一个会因超时提前返回的测试。

3. **contract test 的 CMake 缺 `-Werror`。** 兄弟 session（resolver contract）用了 `-Wall -Wextra -Werror`，本 session 只有 `-Wall -Wextra`，属不一致。实测加上 `-Werror` 仍然通过，所以是纯一致性问题，可在上述修复 session 顺手补齐。

4. **`DataPathRequest` 的三字段与 `IoRequest` 内含 handle 存在语义重叠。** `IoRequest` 已带 `MemoryHandle`/`TargetHandle`（Runtime 层身份），而 `DataPathRequest` 另附 `DataPathMemory`/`DataPathTarget`（SPI 层身份）。这在架构上是对的（两层身份必须分开），但需要明确「lowering 时谁负责把前者翻译成后者、二者不一致时如何报错」。留给 Runtime lowering 落地时确立。

### 关于结果文件的 Git 可见性

`chat/` 已被 `.gitignore` 忽略，`chat/round3/result4.md` 不进版本控制，`git diff --check` 对其无效。worker 只对头文件跑该检查是正确做法；结果文件的空白与 EOF newline 由显式检查覆盖，已通过。

### 后续决定

T-011 交付合格，不返工。Round 3 仅剩 Session 1（attach smoke，会启动 daemon，不要与其他硬件任务并发）。Round 4 必须包含上述 `MemoryKind` 冲突修复，且该修复应在任何 Runtime lowering 任务**之前**完成。
