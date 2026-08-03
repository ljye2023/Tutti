# T-007 Result — Frozen Phase 1 Public IO Nouns

Task: freeze the minimal public IO value types (`MemoryHandle`, `TargetHandle`,
`IoHandle`, `IoDirection`, `IoRequest`, `ExecutionDomain`, `HostSubmitContext`)
as a header-only C++17 public header, plus a standalone hardware-free contract
test.

## 1. Modified / created files

Only files in the allowed list were touched:

| Path | Status |
| --- | --- |
| `tutti/include/tutti/io_types.h` | created |
| `tests/io_types_contract/CMakeLists.txt` | created |
| `tests/io_types_contract/io_types_contract_test.cpp` | created |
| `chat/round2/result4.md` | created (this file) |

Build artifacts were written only to `build/round2-session4/` (inside the
existing ignored `build/` tree). No other files were modified. No Git commit.

## 2. Three handles — public surface & internal construction boundary

All three are distinct strong types produced from a phantom-tagged class
template `tutti::detail::OpaqueHandle<Tag>`:

```cpp
using MemoryHandle = detail::OpaqueHandle<detail::MemoryHandleTag>;
using TargetHandle = detail::OpaqueHandle<detail::TargetHandleTag>;
using IoHandle     = detail::OpaqueHandle<detail::IoHandleTag>;
```

Public surface (identical for all three):

```cpp
constexpr OpaqueHandle() noexcept = default;        // default => invalid
constexpr bool valid() const noexcept;              // generation_ != 0
constexpr bool operator==(const OpaqueHandle&) const noexcept;
constexpr bool operator!=(const OpaqueHandle&) const noexcept;
// implicitly-declared trivial copy/move ctor/assign and destructor
```

Internal construction boundary:

- Internal identity is `(runtime_id: uint32, slot: uint32, generation: uint64)`.
  `generation == 0` denotes an invalid / never-minted handle; a valid handle
  minted by the runtime always has `generation >= 1`.
- The only minting constructor is **private**:
  `OpaqueHandle(uint32 runtime_id, uint32 slot, uint64 generation)`.
- It is accessible exclusively via `friend class ::tutti::StorageRuntime;`,
  where `StorageRuntime` is **forward-declared only** (not implemented here, no
  registry, no heap allocation).
- There is **no public factory** (`from_raw()` or equivalent), so ordinary
  application code cannot forge a valid handle.
- The binary layout / field widths / offsets are **not** part of the public
  ABI; only the public contract (default-invalid, `valid()`, equality,
  copy/move) is stable.

Because the three are distinct template instantiations with no converting
constructor, they are mutually non-convertible and cannot be constructed from
`void*` or integers.

## 3. `IoRequest` / `HostSubmitContext` definition summary

```cpp
enum class IoDirection {
    READ,   // target -> memory
    WRITE,  // memory -> target
};

struct IoRequest {
    IoDirection   direction;
    MemoryHandle  memory;
    std::uint64_t memory_offset;
    TargetHandle  target;
    std::uint64_t target_offset;
    std::uint64_t length;
};

enum class ExecutionDomain {
    HOST_EXECUTION,
    DEVICE_EXECUTION,
};

struct HostSubmitContext {
    ExecutionDomain execution_domain;
    std::int32_t    device_id;
    cudaStream_t    stream;
};
```

- `IoRequest` has exactly the six frozen fields. Both `memory_offset` and
  `target_offset` are independent `std::uint64_t` fields; all offsets/length are
  in bytes. No stream, backend/data-path pointer, tensor shape, storage-private
  descriptor, retry/priority/cancel, etc.
- `HostSubmitContext` obtains `cudaStream_t` only via `<tutti/cuda_like.h>`.
  `HOST_EXECUTION` may use a null stream; `DEVICE_EXECUTION` stream validation
  is the runtime's responsibility. The value type performs no CUDA API calls.

`io_types.h` includes only `<cstdint>` and `<tutti/cuda_like.h>` (equality is
inline `constexpr`, so no extra standard header is needed).

## 4. static_assert and runtime contract coverage

`tests/io_types_contract/io_types_contract_test.cpp` — plain C++17, no GTest,
no CUDA SDK, no hardware. Tutti public types come only from `<tutti/io_types.h>`.

Compile-time (`static_assert`):

1. The three handles are distinct types (`!is_same_v` for all pairs).
2. No implicit conversion between handle types (`!is_convertible_v`, 6 pairs).
3. Cannot implicitly construct from `void*` or `int`
   (`!is_convertible_v` and `!is_constructible_v` for each handle).
5. Default-constructible + copy/move constructible/assignable
   (`is_default_constructible_v`, `is_copy_constructible_v`,
   `is_move_constructible_v`, `is_copy_assignable_v`, `is_move_assignable_v`).
7. No `sizeof`/`offsetof` pinned — only `is_object_v` asserted; layout is
   deliberately not relied upon.
9/10. `IoRequest` fields have frozen types via `decltype(IoRequest::field)`;
   both offsets are `std::uint64_t`.
11. `ExecutionDomain::HOST_EXECUTION != DEVICE_EXECUTION`.
12. `HostSubmitContext` field types (`ExecutionDomain`, `std::int32_t`,
   `cudaStream_t`).

Runtime (`main`, returns 0 on success):

4. Default-constructed handles are invalid (`!valid()`).
6. Same-type invalid handles compare equal; `!=` is false.
5. Copy/move construct and assign round-trip.
8. `IoDirection::READ`/`WRITE` assignable and comparable.
9/10. `IoRequest` all six fields assignable/readable; `memory_offset` and
   `target_offset` are independent (set 10/20, both distinct).
12. `HostSubmitContext{HOST_EXECUTION, 0, nullptr}` round-trip; null stream
   expresses HOST execution; fields reassignable.

## 5. configure / build / ctest results

Clean + standalone configure (no CUDA/gRPC/yaml/libnvm/nvme discovery):

```
$ rm -rf build/round2-session4
$ cmake -S tests/io_types_contract -B build/round2-session4 -DCMAKE_BUILD_TYPE=RelWithDebInfo
-- The CXX compiler identification is GNU 13.1.1
-- Detecting CXX compiler ABI info - done
-- Configuring done (0.2s)
-- Generating done (0.0s)
-- Build files have been written to: .../build/round2-session4
```

Configure log contains no CUDA toolkit / gRPC / yaml-cpp / libnvm / NVMe
dependency discovery.

Build:

```
$ cmake --build build/round2-session4 --target tutti_io_types_contract_test -j8
[ 50%] Building CXX object ...io_types_contract_test.cpp.o
[100%] Linking CXX executable tutti_io_types_contract_test
[100%] Built target tutti_io_types_contract_test
```

Zero warnings, zero errors.

CTest:

```
$ ctest --test-dir build/round2-session4 --output-on-failure -R '^tutti_io_types_contract_test$'
    Start 1: tutti_io_types_contract_test
1/1 Test #1: tutti_io_types_contract_test .....   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

1/1 PASS.

## 6. Public-boundary static check results

Forbidden-private-type grep on `io_types.h` (must be empty):

```
$ grep -nEi 'PRP|LBA|libnvm|nvme|StorageTarget|MemoryRegion|backend_private|void[[:space:]]*\*[[:space:]]*target_handle|backends/|io_engine/|device_manager/' \
    tutti/include/tutti/io_types.h
(empty, grep exit=1)
```

`#include` lines in `io_types.h`:

```
#include <cstdint>
#include <tutti/cuda_like.h>
```

No direct `cuda.h` / `cuda_runtime.h`. `cudaStream_t` appears only as the type
obtained via `<tutti/cuda_like.h>` (in comments and the `HostSubmitContext`
member) — the only accelerator include is `tutti/cuda_like.h`, as required.

## 7. File-boundary & whitespace check results

```
$ git diff --check -- tutti/include/tutti/io_types.h
(clean, exit=0)
```

Trailing-whitespace scan over the three new files: empty (grep exit=1).

EOF-newline check: all three files have a terminating newline.

Files touched by this session (allowed list only):

```
?? tests/io_types_contract/
?? tutti/include/tutti/io_types.h
```

(`chat/round2/result4.md` is this file.) No file outside the allowed list was
modified or created.

## 8. Final status

PASS

All success criteria met:

1. Three handles are strong distinct types, default-invalid, mutually
   non-convertible, not forgeable by application code.
2. Handle semantics reserve runtime identity + slot + generation; ABI size is
   not frozen (no `sizeof`/`offsetof` pinned).
3. `IoRequest` has exactly the six frozen storage-intent fields.
4. The two offsets are independent `std::uint64_t` (bytes).
5. `ExecutionDomain` has only `HOST_EXECUTION` / `DEVICE_EXECUTION`.
6. `HostSubmitContext` uses `cudaStream_t` only via `<tutti/cuda_like.h>`.
7. Public header leaks no storage/device private types.
8. HOST standalone configure/build/ctest passes without CUDA SDK.
9. No file outside the allowed list was modified.
10. No module, daemon, or IO operation was executed.
11. Whitespace / EOF-newline checks pass.

## 总指挥验收

验收结论：`PASS`。

独立核验结果：

- `MemoryHandle`、`TargetHandle`、`IoHandle` 是互不转换的 phantom-tag 强类型，默认 invalid，不接受整数或指针构造。
- handle 私有身份包含 runtime、slot、generation；未来 `StorageRuntime` 是唯一 friend minting 边界，未暴露 public raw factory。
- 额外组合语法检查确认未来 `StorageRuntime` 能构造三种有效 handle，并能与 `Result<IoHandle>` 组合。
- `IoRequest` 严格只有 direction、memory/offset、target/offset、length 六个字段，两个 byte offset 独立。
- `ExecutionDomain` 仅含 `HOST_EXECUTION` 与 `DEVICE_EXECUTION`；未混入未来 device caller API。
- `HostSubmitContext` 的 `cudaStream_t` 只经 `<tutti/cuda_like.h>` 获得。
- storage/device 私有类型静态 guard 无匹配；公共头没有直接 include vendor SDK。
- `tutti_io_types_contract_test` 二进制存在，CTest 日志和 cost data 均确认测试执行并通过。
- 文件边界、尾随空白、EOF newline、IDE/linter 检查全部通过。
- 未执行模块、daemon、block device 或硬件 IO 操作。

后续决定：T-007 完成，不需要返工。
