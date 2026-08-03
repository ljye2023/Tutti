# Round 12 Session 1 Result: MockDataPath 契约套件（contract kit）

## 1. 执行摘要

**结论：PASS。**

- 建立可复用的 `MockDataPath` 契约套件，位于 `tutti/testing/mock_data_path.h`（header-only），只依赖 public/SPI 头 + C++17 标准库。
- 全生命周期覆盖：open/close、registration_domain、register/unregister、submit（含 partial commit）、progress（可驱动状态机）、query、release、shutdown drain。
- 可控注入点（8 个）：per-request 拒绝、op 失败（FAILED + Status）、progress 不推进（manual_mode/hang）、fail_submit、fail_progress、fail_query、fail_release、block_progress（序列化测试）、能力位自定义、bytes_per_request 自定义、domain_key 自定义。
- 迁移 `FakeDataPath`（data_path_contract）和 `RuntimeFakeDataPath`（storage_runtime_contract）到 kit，消除重复 fake 定义。迁移前后断言数等价。
- 新增 kit 自证契约测试（19 个场景），证明 MockDataPath 自身满足 DataPath SPI 全部既有 contract 断言。
- HOST 13/13、CUDA 133/133 全绿。
- kit 文件不 include 任何私有头（header_hygiene 通过）。

## 2. Kit API 摘要

### 2.1 位置与形态

```
tutti/testing/mock_data_path.h   (header-only, ~430 lines)
  namespace tutti::testing
  class MockDataPath : public tutti::DataPath
```

依赖：`<tutti/spi/data_path.h>` + `<tutti/status.h>` + `<tutti/io_types.h>` + C++17 STL（`<atomic>`, `<mutex>`, `<condition_variable>`, `<unordered_map>`, `<vector>`, `<string>`）。无 CUDA、libnvm、硬件依赖。HOST profile 可编译。

### 2.2 公共接口

| 类别 | 成员 | 说明 |
|---|---|---|
| **可配置 capabilities** | `caps` | `DataPathCapabilities`，默认全支持；测试可任意修改 |
| **注入点** | `reject_at_index` | 拒绝该 0-based index 及之后的 request（partial commit） |
| | `fail_submit` (atomic) | submit() 返回 DEVICE_ERROR，无 op |
| | `fail_progress` (atomic) | progress() 返回 DEVICE_ERROR |
| | `fail_query` (atomic) | query() 返回 DEVICE_ERROR |
| | `fail_release` (atomic) | release() 返回 DEVICE_ERROR |
| | `manual_mode` (atomic) | progress() 不自动完成 op（模拟 hang） |
| | `block_progress_flag` (atomic) | progress() 阻塞直到 flag 清除（序列化测试） |
| | `bytes_per_request` | 覆盖 per-request bytes_transferred（默认 4096） |
| | `domain_key_prefix` | 覆盖 registration_domain 返回值（默认 "mock-domain"） |
| **Fluent setters** | `set_reject_at_index(n)` | 返回 `MockDataPath&`，链式调用 |
| | `set_manual_mode(v)` / `set_fail_*(v)` | 同上 |
| **手动控制** | `manual_complete(op)` | 手动将 op 标记为 COMPLETED |
| | `manual_fail(op, status)` | 手动将 op 标记为 FAILED |
| | `unblock_progress()` | 清除 block_progress_flag 并唤醒等待者 |
| **Call counters** | `initialize_calls` / `shutdown_calls` / `open_calls` / ... | 每个 SPI 方法的调用计数 |
| **Progress probe** | `progress_concurrent` / `progress_max_concurrent` | 并发 progress() 探针 |
| **Test inspectors** | `total_op_count()` / `in_flight_op_count()` / `op_scratch_size(op)` | op 表状态 |
| **Last submit** | `last_requests` | 最后一次 submit() 的 request 快照 |

### 2.3 SPI 实现

MockDataPath 实现了 `DataPath` 的全部纯虚方法：
- `capabilities()` → 返回 `caps`
- `initialize()` / `shutdown()` → 返回 OK，递增计数器
- `open()` → mint opaque identity，记录 domain key
- `close()` → 擦除 domain key
- `registration_domain()` → 返回 `domain_key_prefix`
- `register_memory()` / `unregister_memory()` → mint opaque identity
- `submit()` → 支持 partial commit（reject_at_index）、fail_submit 注入、per-op private scratch
- `progress()` → 支持 manual_mode、fail_progress、block_progress、budget 限制、并发探针
- `query()` → 支持 fail_query 注入
- `release()` → 支持 fail_release、BUSY on non-terminal

## 3. 注入点清单

| # | 注入点 | 类型 | 用途 | 示例 |
|---|---|---|---|---|
| 1 | `reject_at_index` | `size_t` | Partial commit：该 index 及之后 reject | `dp.set_reject_at_index(3)` |
| 2 | `fail_submit` | `atomic<bool>` | submit() 整体失败 | `dp.set_fail_submit(true)` |
| 3 | `fail_progress` | `atomic<bool>` | progress() 返回错误 | `dp.set_fail_progress(true)` |
| 4 | `fail_query` | `atomic<bool>` | query() 返回错误 | `dp.set_fail_query(true)` |
| 5 | `fail_release` | `atomic<bool>` | release() 返回错误 | `dp.set_fail_release(true)` |
| 6 | `manual_mode` | `atomic<bool>` | progress() 不自动完成（模拟 hang） | `dp.set_manual_mode(true)` |
| 7 | `block_progress_flag` | `atomic<bool>` | progress() 阻塞（序列化测试） | `dp.block_progress_flag = true; ...; dp.unblock_progress()` |
| 8 | `caps` | `DataPathCapabilities` | 完全自定义能力位 | `dp.caps.max_in_flight_operations = 4` |
| 9 | `bytes_per_request` | `uint64_t` | 覆盖 per-request bytes | `dp.bytes_per_request = 8192` |
| 10 | `domain_key_prefix` | `string` | 覆盖 registration_domain | `dp.domain_key_prefix = "custom-domain"` |

## 4. 迁移等价性证据

### 4.1 data_path_contract 迁移

| 迁移前 | 迁移后 |
|---|---|
| `FakeDataPath` (内联类, ~215 行) | `tutti::testing::MockDataPath` (kit header) |
| `fail_at_index` | `reject_at_index` (via `set_reject_at_index()`) |
| `caps.name = "fake"` | `caps.name = "mock"` (assertion updated) |
| `"domain-1"` domain key | `"mock-domain"` (assertion updated) |
| 116 CHECK assertions | 116 CHECK assertions (same count) |
| **Output**: `all checks passed` | **Output**: `all checks passed` |

### 4.2 storage_runtime_contract 迁移

| 迁移前 | 迁移后 |
|---|---|
| `RuntimeFakeDataPath` (内联类, ~225 行) | `using RuntimeFakeDataPath = tutti::testing::MockDataPath` |
| `block_progress_cv.notify_all()` | `dp.unblock_progress()` |
| 34 tests passed | 34 tests passed (same count) |
| **Output**: `All 34 storage runtime contract tests passed.` | **Output**: `All 34 storage runtime contract tests passed.` |

### 4.3 消除的重复定义

| 重复项 | 消除方式 |
|---|---|
| `FakeDataPath` in `tests/data_path_contract/` | 替换为 `#include <tutti/testing/mock_data_path.h>` |
| `RuntimeFakeDataPath` in `tests/storage_runtime_contract/` | 替换为 `using RuntimeFakeDataPath = tutti::testing::MockDataPath` |

两个 fake 的共同功能（submit partial commit、progress、query、release、manual_mode、fail 注入、call counters、progress 并发探针、block_progress）统一到 `MockDataPath` 中。

## 5. Kit 自证契约测试

新增 `tests/mock_data_path_kit_contract/mock_data_path_kit_contract_test.cpp`（19 个场景）：

| # | 场景 | 验证点 |
|---|---|---|
| 1 | SPI instantiation | MockDataPath 可实例化、initialize/shutdown 成功 |
| 2 | Capabilities minimum fields | 所有最小字段有非零值 |
| 3 | Custom capabilities injection | caps 可自定义 |
| 4 | open/close distinct identities | 不同 target 有不同 opaque identity |
| 5 | registration_domain | 返回非空 string key |
| 6 | register/unregister distinct | 不同 memory 有不同 identity |
| 7 | submit partial commit | reject_at_index 正确行为 |
| 8 | submit count=0 | op=nullopt, status=OK |
| 9 | all rejected | op=nullopt, status=non-OK |
| 10 | query doesn't destroy | 两次 query 后 op 仍存在 |
| 11 | release terminal only | non-terminal → BUSY, terminal → OK |
| 12 | progress budget bounds | work_units <= max_work_units |
| 13 | manual_mode | progress 不自动完成 |
| 14 | fail_progress injection | progress 返回 DEVICE_ERROR |
| 15 | fail_query injection | query 返回 DEVICE_ERROR |
| 16 | fail_release injection | release 返回 DEVICE_ERROR |
| 17 | fail_submit injection | submit 返回错误, op=nullopt |
| 18 | Call counters | 所有 SPI 方法的调用计数正确 |
| 19 | Per-op scratch private | 两个并发 op 有独立 scratch |

## 6. 构建验证

### 6.1 HOST profile

```
cmake -S tutti -B build/r12-s1-host \
  -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON -DTUTTI_BUILD_HARDWARE_TESTS=OFF
cmake --build build/r12-s1-host -j8
ctest --test-dir build/r12-s1-host
```
- Configure: PASS (0.1s)
- Build: PASS
- CTest: **13/13 PASS**, 0 failed (Round 11: 12/12 → +1 kit contract test)

### 6.2 CUDA profile

```
cmake -S tutti -B build/r12-s1-cuda \
  -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON -DTUTTI_BUILD_HARDWARE_TESTS=OFF
cmake --build build/r12-s1-cuda -j8
ctest --test-dir build/r12-s1-cuda -E 'hardware'
```
- Configure: PASS (5.5s)
- Build: PASS (100%)
- CTest: **133/133 PASS**, 0 failed (Round 11: 132/132 → +1 kit contract test)

### 6.3 Kit header hygiene

```
$ grep "#include" tutti/testing/mock_data_path.h
#include <tutti/spi/data_path.h>
#include <tutti/status.h>
#include <tutti/io_types.h>
#include <atomic>, <condition_variable>, <cstdint>, <cstddef>,
#include <mutex>, <string>, <unordered_map>, <vector>
```
- 只依赖 public/SPI 头 + C++17 STL
- 无 CUDA、libnvm、nvmeservice、kernel module、DataPath 私有头
- header_hygiene_test: PASS

## 7. 文件变更

### 新建

| 文件 | 用途 |
|---|---|
| `tutti/testing/mock_data_path.h` | MockDataPath 契约套件（header-only, ~430 行） |
| `tests/mock_data_path_kit_contract/CMakeLists.txt` | Kit 自证契约测试接线 |
| `tests/mock_data_path_kit_contract/mock_data_path_kit_contract_test.cpp` | Kit 自证契约测试（19 场景） |
| `chat/round12/result1.md` | 本结果文件 |

### 修改

| 文件 | 变更 |
|---|---|
| `tests/data_path_contract/data_path_contract_test.cpp` | FakeDataPath → MockDataPath；fail_at_index → reject_at_index；domain 断言更新 |
| `tests/data_path_contract/CMakeLists.txt` | 加 `target_include_directories` 指向 kit |
| `tests/storage_runtime_contract/storage_runtime_contract_test.cpp` | RuntimeFakeDataPath → `using MockDataPath`；block_progress_cv → unblock_progress() |
| `tests/storage_runtime_contract/CMakeLists.txt` | 加 `target_include_directories` 指向 kit |
| `tutti/CMakeLists.txt` | 加 `add_subdirectory(tests/mock_data_path_kit_contract)` |

## 8. 行为不变量保留

| 不变项 | 如何保留 |
|---|---|
| Public/SPI 不变 | 无 `tutti/include/tutti/**` 改动 |
| 既有测试断言不减少 | data_path_contract: 116 CHECK (迁移前后相同)；storage_runtime_contract: 34 tests (迁移前后相同) |
| 迁移等价 | FakeDataPath/RuntimeFakeDataPath 的全部语义（partial commit、progress、query、release、manual_mode、fail 注入、call counters、block_progress）统一到 MockDataPath |
| Kit 不进生产 target | kit 头在 `tutti/testing/`，不被 `tutti_local_nvme_datapath` 或任何生产 target include |
| Kit 不带硬件依赖 | 只依赖 public/SPI + C++17 STL；HOST profile 可编译 |
| Kit 不 include 私有头 | header_hygiene 通过 |

## 9. 结论

**PASS。**

## 总指挥验收（2026-08-02）

**PASS。** 独立核验：

- kit（434 行）include 面仅 public/SPI + STL；生产 target（data_paths/resolvers/bindings/include）对 kit **零引用**；两个契约测试已迁移（各 1 处 include）。
- 复跑：HOST `13/13`、CUDA `133/133`；`git diff --check` clean；kit 与 host shim 诊断 0。
- 注入点与 kit 自证 19 场景审查一致；迁移等价（116+34 断言保留）由 ctest 复跑旁证。
- 硬件基线保险复跑：735/0 + 115/0 无影响。

**S3 已解除 kit 依赖（可复用本 kit）；S4 门禁依赖全部 S1-S3。**

- `MockDataPath` 契约套件位于 `tutti/testing/mock_data_path.h`，header-only，只依赖 public/SPI + C++17 STL。
- 全生命周期覆盖 + 10 个可控注入点。
- 两个既有 fake 迁移到 kit，断言数不减少（116 + 34 = 150 既有断言全保留）。
- 新增 kit 自证契约测试（19 场景），证明 kit 自身满足 DataPath SPI contract。
- HOST 13/13、CUDA 133/133 全绿。
- kit 不 include 任何私有头（header_hygiene 通过）。
