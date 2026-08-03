# TASK T-014

你是一名资深 C++ API 设计工程师。你只负责冻结 Tutti `StorageRuntime` 公共门面的**接口壳与生命周期语义**，不实现任何真实 IO。你看不到任何其他上下文，本 prompt 已包含完整架构语义、已有基线、边界和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 已完成基线（**只读，禁止修改**）

以下公共契约已冻结，你必须复用，**不得改动其中任何一个字节**：

## `tutti/include/tutti/status.h`

```cpp
enum class StatusCode {
    OK, INVALID_ARGUMENT, OUT_OF_RANGE, NOT_FOUND, UNSUPPORTED,
    NOT_READY, BUSY, RESOURCE_EXHAUSTED, TIMEOUT, DEVICE_ERROR,
    DATA_LOSS, INTERNAL,
};
```

以及 `Status`（含 `code()`、`ok()`、`Status::Ok()`、`Status(code, message)`）和 `Result<T>`（含 `ok()`、`has_value()`、`value()`、`status()`、`Result<T>::Success(...)`、`Result<T>::Failure(Status)`）。

## `tutti/include/tutti/io_types.h`

```cpp
class StorageRuntime;   // 已前向声明（第 19 行）

// MemoryHandle / TargetHandle / IoHandle：phantom-tag 强类型，
// 私有构造 {runtime_id, slot, generation}，
// friend class ::tutti::StorageRuntime;  是唯一 minting 边界（第 50 行）
// 均有 valid()，默认 invalid，互不转换

enum class IoDirection { READ, WRITE };

struct IoRequest {
    IoDirection   direction;
    MemoryHandle  memory;
    std::uint64_t memory_offset;
    TargetHandle  target;
    std::uint64_t target_offset;
    std::uint64_t length;
};

enum class ExecutionDomain { HOST_EXECUTION, DEVICE_EXECUTION };

struct HostSubmitContext {
    ExecutionDomain execution_domain;
    std::int32_t    device_id;
    cudaStream_t    stream;
};
```

**关键：`StorageRuntime` 已经是这三个 handle 的 friend。** 这意味着你定义的 `StorageRuntime` 类必须精确命名为 `tutti::StorageRuntime`，才能获得 mint 权限。

## `tutti/include/tutti/memory_types.h`

```cpp
enum class MemoryKind { HOST, PINNED_HOST, DEVICE, MANAGED };
enum class MemoryOwnership { RUNTIME_OWNED, CALLER_OWNED };

struct MemoryView {
    void*         address;
    std::uint64_t size;
    MemoryKind    expected_kind;
    MemoryOwnership ownership;
    std::int32_t  expected_device_id;
    std::string   expected_profile;
};
```

# 架构契约（目标架构第 6-7 章）

## StorageRuntime 的职责边界

`StorageRuntime` 是应用门面 / handle registry / 路由 / 分组 / backpressure / completion 汇聚点。

它**不负责**、也**不理解**：PRP、CQ、descriptor、queue、FIEMAP、CUDA kernel、任何具体 storage descriptor 或 DataPath kernel。

## 公共 API 概念形状

```text
lifecycle
  create(RuntimeConfig, injected components) -> Result<StorageRuntime>
  shutdown(timeout) -> Status

discovery
  query_cuda_like_profile() -> CudaLikeProfileInfo
  list_devices() -> Result<DeviceInfo[]>
  query_device_capabilities(device_id) -> Result<DeviceCapabilities>

memory
  allocate_memory(spec) -> Result<MemoryAllocation {handle, address, size}>
  free_memory(handle) -> Status
  register_memory(view) -> Result<MemoryHandle>
  unregister_memory(handle) -> Status
  query_memory(handle) -> Result<MemoryInfo>

target
  open(uri, options) -> Result<TargetHandle>
  close(target) -> Status
  query_target(target) -> Result<TargetInfo>

IO
  submit(requests, submit_context) -> Result<IoHandle>
  query(io) -> Result<IoSnapshot>
  wait(io, timeout) -> WaitOutcome
  release_io(io) -> Status
```

不要求逐字采用该形式，但**错误返回与生命周期语义必须一致**。

## 必须精确表达的语义（这是本任务的核心）

### 1. `wait()` 返回 `WaitOutcome`，不是 `Result<IoResult>`

观察超时与执行超时必须区分：

- **观察超时：** `wait(timeout)` 返回 `WaitOutcome{observation_status=TIMEOUT, result=nullopt}`；operation 仍可能在飞，**不释放资源，不取消 operation**；
- **执行超时：** 只有 DataPath 已停止后续 DMA 或已隔离资源后，operation 才进入 terminal timeout failure，并在 `IoResult` 中返回失败。

```text
WaitOutcome
  observation_status   OK | TIMEOUT | invalid-handle Status
  optional IoResult    只在 operation 已终态时存在
```

必须能同时表达「API 调用错误」和「operation 状态」，**禁止**用单个 `IoResult` 混合两者。这是本任务最容易做错的一点。

### 2. Handle 生命周期

- `MemoryHandle` / `TargetHandle` / `IoHandle` 在 close/unregister/release 后**确定性失效**；
- handle generation **不得复用**；
- 跨 Runtime 的 handle 必须被拒绝。

### 3. BUSY 语义（不隐式阻塞）

- memory 有 inflight operation 时，`free_memory()` 与 `unregister_memory()` 返回 `BUSY`，**不隐式阻塞、不提前 unmap/free**；
- target 有 inflight IO 时 `close()` 返回 `BUSY`，**不静默延迟关闭**；
- `release_io()` 在 inflight 时返回 `BUSY`。

### 4. allocate/register 与 free/unregister 的所有权对称性

- `free_memory()` **只接受** runtime-owned allocation；
- `unregister_memory()` **只接受** caller-owned registration；
- 用错配对必须返回 `INVALID_ARGUMENT`。

### 5. terminal result 不自动淘汰

- 未调用 `release_io()` 的 terminal result **不得**自动淘汰；
- terminal result 达到配置上限时，新的 `submit()` 通过 `RESOURCE_EXHAUSTED` 施加 backpressure。

### 6. Runtime 状态机

```text
create -> RUNNING -> shutdown -> DRAINING -> STOPPED
```

- `shutdown()` 成功进入 `STOPPED` 后，统一使该 Runtime 的所有公共 handle 失效；
- 若 shutdown 观察超时，Runtime 停留在 `DRAINING`，此时 handle **仍可** query/wait/release。

### 7. 公共 API 不泄漏私有信息

`query_memory()` 等公共 API **禁止**返回 DMA IOVA、PRP、rkey 或任何 data-path registration pointer。

# 任务目标

新增公共头：

`/data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h`

在 `namespace tutti` 中定义 `StorageRuntime` 类及其配套值类型，至少包括：

```text
RuntimeState            (RUNNING / DRAINING / STOPPED)
RuntimeConfig
CudaLikeProfileInfo
DeviceInfo
DeviceCapabilities
MemorySpec
MemoryAllocation        {MemoryHandle, void* address, std::uint64_t size}
MemoryInfo
OpenOptions
TargetInfo
IoState                 (IN_FLIGHT / COMPLETED / FAILED)
IoSnapshot
IoResult
WaitOutcome
StorageRuntime
```

## 本任务是「接口壳 + 语义契约」，不是完整实现

明确要求：

- `StorageRuntime` 的方法必须**可编译、可调用**，但允许返回 `UNSUPPORTED` 或在最小内存簿记上工作；
- **禁止**接入真实 Resolver、真实 DataPath、CUDA 分配或任何硬件；
- **禁止** include `tutti/spi/**` 下的任何头。公共门面头不得依赖仓内 SPI；未来的组合由 `.cpp` 实现或独立 assembly 头负责，不在本任务；
- 可以（也建议）用一个内部最小 registry 让 handle 的 mint / 失效 / generation 不复用 / BUSY 这些**语义**被真实测出来。这是本任务的价值所在：语义必须可测，而不是只写注释。

## 关于 header-only 还是分离 .cpp

允许两种做法：

1. header-only（inline 实现最小簿记）；
2. `storage_runtime.h` + `tutti/src/storage_runtime.cpp` 分离。

如果选方案 2，你**只能**新建 `.cpp` 于允许列表内指定路径，且必须让 contract test 能直接编译该 `.cpp`（standalone CMake 里把它加入 `add_executable` 源列表即可，**不要**去改 `tutti/CMakeLists.txt`）。

推荐方案 1，理由：避免触碰构建系统，减少与其他 worker 的冲突面。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h`
- `/data/home/ryeqiu/Tutti/tutti/src/storage_runtime.cpp`（仅当你选择分离实现；否则不要创建）
- `/data/home/ryeqiu/Tutti/tests/storage_runtime_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/storage_runtime_contract/storage_runtime_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round4/result3.md`

其中 `chat/round4/result3.md` 保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round4-session3/`

该目录位于既有 ignored `build/` 下，不计入源码文件允许列表。

禁止修改或创建任何其他文件。尤其禁止修改：

- `/data/home/ryeqiu/Tutti/tutti/include/tutti/status.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/io_types.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/memory_types.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/cuda_like.h` 及 `gpu_vendor/**`
- `/data/home/ryeqiu/Tutti/tutti/spi/**`（另有 worker 正在改 `data_path.h`，**绝对不要碰**）
- 根或 `tutti/` 的任何 `CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tests/` 下除 `tests/storage_runtime_contract/` 外的任何目录
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round4/result3.md` 外的任何文件
- 任意 accelerator、NVMe、libnvm、kernel 文件

禁止提交 Git commit。

# 依赖限制

`storage_runtime.h` 只允许 include：

```cpp
#include <tutti/status.h>
#include <tutti/io_types.h>
#include <tutti/memory_types.h>
```

以及完成该门面所需的 C++17 标准库头。

**特别注意：**

- **禁止** include `tutti/spi/data_path.h` 或 `tutti/spi/storage_target_resolver.h`；
- 由于 `io_types.h` 经 `cuda_like.h` 需要 profile 宏，编译时必须定义 `TUTTI_USE_HOST=1`（见 CMake 要求）；
- **禁止**直接 include `<cuda.h>` / `<cuda_runtime.h>`；如需 `cudaStream_t`，它已由 `io_types.h` 间接提供。

明确禁止 include 或提及：

```text
libnvm
nvme
fiemap
grpc
yaml
backends/
io_engine/
device_manager/
```

也不要在公共头中出现 PRP、SGL、LBA、CID、doorbell、IOVA、rkey、fd、extent、descriptor 等私有实现名词（除注释中说明「本层不涉及什么」）。

# Contract test 要求

测试 target 与 CTest 名固定：

```text
tutti_storage_runtime_contract_test
```

测试为普通 C++17 可执行程序，不使用 GTest，不使用 CUDA SDK。

至少覆盖以下语义（每条都要有真实断言，不能只靠注释）：

1. `create()` 成功后状态为 `RUNNING`；
2. `shutdown()` 后状态为 `STOPPED`，且此后所有公共 handle 失效；
3. `register_memory()` 返回有效 `MemoryHandle`；对 `address == nullptr` 或 `size == 0` 的 `MemoryView` 返回 `INVALID_ARGUMENT`；
4. `unregister_memory()` 后同一 handle 确定性失效（再次使用返回非 OK，且**不是** crash）；
5. generation 不复用：unregister 后再 register，新 handle 与旧 handle **不相等**，旧 handle 不会「复活」；
6. 所有权对称性：对 caller-owned registration 调 `free_memory()` 返回 `INVALID_ARGUMENT`；对 runtime-owned allocation 调 `unregister_memory()` 返回 `INVALID_ARGUMENT`；
7. `open()` 返回有效 `TargetHandle`（允许用 stub/内存中假 target，不接真实 resolver）；`close()` 后失效；
8. **BUSY 语义**：存在 inflight IO 时 `close(target)` 返回 `BUSY`；memory 有 inflight 时 `unregister_memory()` / `free_memory()` 返回 `BUSY`；
9. **`WaitOutcome` 区分观察超时与终态**：
   - 对 in-flight operation 用极小 timeout 调 `wait()` → `observation_status == TIMEOUT` 且 `result == nullopt`，且该 operation **仍未被取消**（随后仍可 query）；
   - operation 终态后 `wait()` → `observation_status == OK` 且 `result` 有值；
   - 对 invalid handle 调 `wait()` → `observation_status` 为非 OK 的 handle 错误，且 `result == nullopt`；
10. `release_io()` 在 inflight 时返回 `BUSY`；terminal 后返回 OK 且 handle 失效；
11. terminal result 未 release 时不自动淘汰（连续多次 `query()` 仍返回同一终态）；
12. 跨 Runtime handle 被拒绝：由 Runtime A mint 的 handle 传给 Runtime B 时返回非 OK（`INVALID_ARGUMENT` 或 `NOT_FOUND`），且不 crash；
13. shutdown 观察超时留在 `DRAINING` 时，handle 仍可 query/wait/release（若你的最小实现无法产生该状态，明确记录该限制并说明为何不可测，**不要伪造**）；
14. `query_memory()` 返回的 `MemoryInfo` 中不含任何 IOVA / rkey / data-path pointer 字段（用 `static_assert` 或字段清单断言表达）。

测试源码只允许 include：

```cpp
#include <tutti/storage_runtime.h>
```

以及标准库头。（如需，也可 include 已冻结的 `status.h` / `io_types.h` / `memory_types.h`，但**不得** include `tutti/spi/**`。）

Standalone CMake 必须：

- `project(... LANGUAGES CXX)`；
- C++17；
- target-scoped include `/data/home/ryeqiu/Tutti/tutti/include`；
- target-scoped 定义 `TUTTI_USE_HOST=1`；
- `-Wall -Wextra -Werror`；
- **不查找** CUDA 或任何第三方 SDK；
- `enable_testing()` 并注册 CTest。

# 安全限制

绝对禁止：

```text
sudo
insmod
rmmod
modprobe
```

也禁止启动 daemon/client、访问 `/dev/nvme*`、执行 FIEMAP、CUDA 调用或任何硬件 IO。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 清理专用 build 目录

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round4-session3
```

## 2. Standalone configure

```bash
cmake -S /data/home/ryeqiu/Tutti/tests/storage_runtime_contract \
  -B /data/home/ryeqiu/Tutti/build/round4-session3 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

要求 configure 日志不出现 CUDA、gRPC、yaml-cpp、libnvm、FIEMAP 或 NVMe dependency discovery。

## 3. Build 和 CTest

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round4-session3 \
  --target tutti_storage_runtime_contract_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round4-session3 \
  --output-on-failure \
  -R '^tutti_storage_runtime_contract_test$'
```

要求 `-Werror` 下零告警，1/1 PASS。

## 4. 确认既有公共契约零改动

```bash
git diff --stat -- tutti/include/tutti/status.h tutti/include/tutti/io_types.h tutti/include/tutti/memory_types.h
```

必须为空输出。

## 5. 确认未依赖 SPI

```bash
grep -n 'spi/' /data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h
```

必须无输出。

## 6. Public-boundary guard（词边界）

```bash
grep -nEiw 'libnvm|nvme|fiemap|grpc|yaml|prp|sgl|lba|cid|doorbell|iova|rkey|fd|extent' \
  /data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h
```

本 guard 使用 `-w` 词边界以避免子串误报。如有命中必须逐条说明是注释还是真实类型/字段。

## 7. Hygiene

```bash
git diff --check -- tutti/include/tutti/storage_runtime.h
```

对所有新增文件额外检查尾随空白和 EOF newline。确认本 session 只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. `StorageRuntime` 覆盖 lifecycle / discovery / memory / target / IO 五组 API；
2. 类名精确为 `tutti::StorageRuntime`，从而与 `io_types.h` 既有 friend 声明匹配，能 mint 三种 handle；
3. `WaitOutcome` 正确区分观察超时与执行终态，且观察超时不取消 operation；
4. handle 在 close/unregister/release 后确定性失效，generation 不复用；
5. 跨 Runtime handle 被拒绝且不 crash；
6. BUSY 语义在 memory 与 target 两处均成立，且不隐式阻塞；
7. allocate/free 与 register/unregister 的所有权配对被强制；
8. terminal result 不自动淘汰；
9. 公共 API 不泄漏 IOVA / rkey / data-path pointer；
10. 公共头未 include `tutti/spi/**`，未直接 include CUDA SDK；
11. 既有三个公共契约头零改动；
12. HOST standalone configure/build/ctest 在 `-Werror` 下通过，不需要 CUDA SDK；
13. 未修改允许列表外文件；
14. 未执行任何模块、daemon 或 IO 操作；
15. 空白检查通过。

如果某条语义在「不接真实 DataPath」的前提下无法真实测出（例如 `DRAINING` 状态），**明确记录该限制并说明原因**，但不得为了让测试变绿而弱化契约、删除断言或伪造状态。

# 结果落盘要求

完成任务和验收后，必须把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round4/result3.md`

至少包含：

1. 修改/新增文件列表（并说明选了 header-only 还是分离 .cpp，以及理由）
2. `StorageRuntime` 及配套值类型的实际 public surface
3. `WaitOutcome` 的精确语义与实现方式
4. handle 失效 / generation 不复用 / 跨 Runtime 拒绝的实现方式
5. BUSY 语义的实现方式与覆盖范围
6. 所有权对称性的实现方式
7. 本轮**未**实现或**无法测**的语义清单（含原因）
8. configure / build / ctest 结果（含 `-Werror`）
9. 既有公共契约零改动的证明
10. 未依赖 SPI 的证明
11. public-boundary guard 结果
12. 文件边界与空白检查结果
13. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 如果命令失败，写入真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round4/result3.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
