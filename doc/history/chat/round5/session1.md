# TASK T-016

你是一名资深 C++ API 设计工程师。你只负责把 Tutti `StorageRuntime` 公共门面的四处契约缺口一次性补齐。你看不到任何其他上下文，本 prompt 已包含完整现状、缺口证据、正确语义和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 已完成基线（**只读，禁止修改**）

以下公共契约已冻结，必须复用，**不得改动其中任何一个字节**：

## `tutti/include/tutti/status.h`

```cpp
enum class StatusCode {
    OK, INVALID_ARGUMENT, OUT_OF_RANGE, NOT_FOUND, UNSUPPORTED,
    NOT_READY, BUSY, RESOURCE_EXHAUSTED, TIMEOUT, DEVICE_ERROR,
    DATA_LOSS, INTERNAL,
};
```

以及 `Status`（`code()`、`ok()`、`Status::Ok()`、`Status(code, message)`）和 `Result<T>`（`ok()`、`has_value()`、`value()`、`status()`、`Result<T>::Success(...)`、`Result<T>::Failure(Status)`）。

## `tutti/include/tutti/io_types.h`

```cpp
class StorageRuntime;   // 前向声明（第 19 行）

// MemoryHandle / TargetHandle / IoHandle：phantom-tag 强类型，
// 私有构造 {runtime_id, slot, generation}，
// friend class ::tutti::StorageRuntime;  是唯一 minting 边界（第 50 行）

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

## `tutti/include/tutti/memory_types.h`

```cpp
enum class MemoryKind { HOST, PINNED_HOST, DEVICE, MANAGED };
enum class MemoryOwnership { RUNTIME_OWNED, CALLER_OWNED };
struct MemoryView {
    void* address; std::uint64_t size; MemoryKind expected_kind;
    MemoryOwnership ownership; std::int32_t expected_device_id;
    std::string expected_profile;
};
```

# 现状：`tutti/include/tutti/storage_runtime.h` 已存在

上一轮已建立该门面，包含：`RuntimeState`、`RuntimeConfig`、`CudaLikeProfileInfo`、`DeviceInfo`、`DeviceCapabilities`、`MemorySpec`、`MemoryAllocation`、`MemoryInfo`、`OpenOptions`、`TargetInfo`、`IoState`、`IoSnapshot`、`IoResult`、`WaitOutcome`、`StorageRuntime`。

以下语义**已实现且已验收通过，必须保持不回退**：

- `create()` → `RUNNING`；`shutdown()` → `STOPPED`（超时则留 `DRAINING`）；
- `STOPPED` 后所有 handle 失效；`DRAINING` 下 handle 仍可 query/wait/release；
- handle 三重校验 `runtime_id` / `active` / `generation`；generation 单调递增不复用；
- 跨 Runtime handle 被拒（`NOT_FOUND`）；
- BUSY：memory 有 inflight → `free_memory`/`unregister_memory` 返回 `BUSY`；target 有 inflight → `close` 返回 `BUSY`；IO 非 terminal → `release_io` 返回 `BUSY`；
- 所有权对称：`free_memory` 只收 `RUNTIME_OWNED`，`unregister_memory` 只收 `CALLER_OWNED`，错配 `INVALID_ARGUMENT`；
- terminal result 不自动淘汰；`terminal_result_count_` 达上限时 `submit` 返回 `RESOURCE_EXHAUSTED`；
- `WaitOutcome{observation_status, optional<IoResult>}`，观察超时不取消 operation；
- `MemoryInfo` 不含 IOVA / rkey / data-path pointer；
- 头文件不 include `tutti/spi/**`，不直接 include CUDA SDK。

现有 contract test 有 14 个子测试，全部通过。**你必须保证这 14 项在改动后依然通过。**

# 你要修的四处缺口（已实测确认）

## 缺口 1：`submit()` 是单请求，无法表达 batch partial-commit

当前签名：

```cpp
Result<IoHandle> submit(const IoRequest& request, const HostSubmitContext& context);
```

目标架构在 Runtime 层明确要求批量与部分提交：

- 「batch 允许部分执行，但任何已经发出的工作都不会失去 owner、lease 或完成观察路径。」
- 「contract test 必须覆盖『第 K 个 request 发出后提交失败』。」
- Runtime 只要收到任一有效底层 operation 就返回 `IoHandle`，并把其余 request 记录为 per-request failure。

单请求签名在结构上无法表达「4 个 request、第 3 个之后失败」，也无处安放 per-request 初始状态。

注意当前的怪状：**下层 SPI 比上层 Runtime 表达能力更强**。`tutti/spi/data_path.h`（**只读，不要改**）已有：

```cpp
enum class RequestState { ACCEPTED, REJECTED };
struct RequestInitialState { RequestState state; Status status; };
struct SubmitOutcome {
    Status status;
    std::optional<DataPathOp> op;
    std::vector<RequestInitialState> initial_states;
};
```

Runtime 必须能把这种部分提交结果如实转达调用方。

### 要求

`submit()` 改为批量，并返回能表达部分提交的结果类型。建议形状（字段名可微调，语义不可变）：

```cpp
enum class IoRequestState { ACCEPTED, REJECTED };

struct IoRequestInitialState {
    IoRequestState state;
    Status status;              // OK for ACCEPTED；错误码+消息 for REJECTED
};

struct IoSubmitOutcome {
    Status status;                                   // 整体状态
    std::optional<IoHandle> io;                      // 见下述不变量
    std::vector<IoRequestInitialState> initial_states;
};

IoSubmitOutcome submit(const IoRequest* requests,
                       std::size_t count,
                       const HostSubmitContext& context);
```

必须成立的不变量：

1. `io == nullopt` ⟺ **零个** request 被不可撤销地接受（`status` 仍可为非 OK，例如全部被拒）；
2. `io != nullopt` ⟺ 至少一个 request 被接受且仍可观察，**即使 `status` 表示部分失败**；
3. `initial_states.size() == count`，且顺序与输入一一对应；
4. 已接受的 request 必须可通过返回的 `IoHandle` 被 `query()` / `wait()` 观察到；
5. `count == 0` 时返回 `io == nullopt`、`status` OK、`initial_states` 为空。

**不要**保留单请求重载。调用方需要单请求时传 `count == 1`。理由：两个重载会让 partial-commit 语义出现两套路径，是未来 bug 的温床。

## 缺口 2：`submit()` 不校验 request bounds

实测（当前行为，属缺陷）：

```text
注册 4096 字节缓冲，提交 memory_offset=1<<30, length=1<<40
  -> submit accepted = 1   （应为 0）
提交 length=0
  -> submit accepted = 1   （应为 0）
```

目标架构要求 `submit()` 第一步就「验证 runtime 状态、handle generation 和 **request bounds**」。

### 要求

对每个 request 校验，失败者标记 `REJECTED` 并给出精确 Status：

- `length == 0` → `INVALID_ARGUMENT`；
- `memory_offset + length` 超出该 `MemoryHandle` 注册/分配的 `size` → `OUT_OF_RANGE`；
- `target_offset + length` 超出该 `TargetHandle` 的 `logical_size` → `OUT_OF_RANGE`；
- 加法必须防溢出（例如先判 `offset > size` 再判 `length > size - offset`，**不要**写 `offset + length > size`）；
- `memory` 或 `target` handle 无效 → `INVALID_ARGUMENT`。

**校验必须逐 request 独立进行**：一个 request 越界只让该 request 变 `REJECTED`，不得导致整批失败。这正是缺口 1 的批量语义要支撑的场景。

## 缺口 3：`wait()` / `query()` 被标记 `const`

当前 `WaitOutcome wait(...) const` 和 `Result<IoSnapshot> query(...) const`。

目标架构要求「`query()` 非阻塞，**可以驱动一次有界 progress**」「`wait()` 通过有界 progress/退避循环等待」。`const` 方法无法驱动 progress。

实测当前 `wait()` 的行为：单线程下没有任何东西能翻转终态标志，1ms 轮询循环纯属空转 —— `wait(io, 300)` 实测耗时 316ms 后返回 `TIMEOUT`。

### 要求

- 去掉 `wait()` 与 `query()` 的 `const` 限定；
- 保留现有 `WaitOutcome` 语义不变（观察超时不取消 operation）；
- 不要求真正实现 progress 驱动（无真实 DataPath），但签名必须为未来留出空间；
- 可以保留一个内部 `bounded_progress_()` 私有钩子作为占位，但**不要**在里面写假的推进逻辑。

如果你希望保留一个只读观察入口，可以额外提供 `const` 版本的 `peek(io)`；这是可选项，不做也可以。

## 缺口 4：`testing_force_complete_io()` 位于 public 区段

当前它在 `public:` 区（约第 462 行），是生产门面上的测试后门，且是驱动 IO 终态的唯一手段。

### 要求

把它从常规 public surface 收敛出去。**允许的做法（任选其一）**：

1. 改为 `private`，并声明一个测试专用 friend，例如：

```cpp
namespace testing { struct StorageRuntimeTestAccess; }
// class 内：
friend struct ::tutti::testing::StorageRuntimeTestAccess;
```

  然后在头文件中提供 `StorageRuntimeTestAccess` 的静态转发函数供测试使用；

2. 用 `#if defined(TUTTI_ENABLE_TEST_HOOKS)` 包裹该方法，并让 contract test 的 CMake target-scoped 定义该宏。

**禁止**的做法：直接删掉它（那样 IO 永远无法到终态，14 个既有子测试会失效）。

无论选哪种，必须保证：**不定义测试宏 / 不通过 friend 的普通消费者代码，无法调用该方法**。你需要用一个编译期或说明性证据说明这一点。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h`
- `/data/home/ryeqiu/Tutti/tests/storage_runtime_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/storage_runtime_contract/storage_runtime_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round5/result1.md`

其中 `chat/round5/result1.md` 保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round5-session1/`

该目录位于既有 ignored `build/` 下，不计入源码文件允许列表。

禁止修改或创建任何其他文件。尤其禁止修改：

- `/data/home/ryeqiu/Tutti/tutti/include/tutti/status.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/io_types.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/memory_types.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/cuda_like.h` 及 `gpu_vendor/**`
- `/data/home/ryeqiu/Tutti/tutti/spi/**`（**另有 worker 可能正在移动这些文件，绝对不要碰**）
- `/data/home/ryeqiu/Tutti/tutti/bindings/**`（另一 worker 的目录）
- 根或 `tutti/` 的任何 `CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tests/` 下除 `tests/storage_runtime_contract/` 外的任何目录
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round5/result1.md` 外的任何文件

禁止提交 Git commit。

# 依赖限制

`storage_runtime.h` 只允许 include：

```cpp
#include <tutti/status.h>
#include <tutti/io_types.h>
#include <tutti/memory_types.h>
```

以及 C++17 标准库头。

**禁止** include `tutti/spi/**`（公共门面不得依赖仓内 SPI）。**禁止**直接 include `<cuda.h>` / `<cuda_runtime.h>`（`cudaStream_t` 已由 `io_types.h` 间接提供）。

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

也不要出现 PRP、SGL、LBA、CID、doorbell、IOVA、rkey、fd、extent、descriptor 等私有名词（注释中说明「本层不涉及什么」除外）。

# Contract test 要求

target 与 CTest 名保持不变：

```text
tutti_storage_runtime_contract_test
```

普通 C++17 可执行程序，不用 GTest，不用 CUDA SDK。

## 必须保留的既有覆盖（14 项，改动后仍须通过）

create/RUNNING、shutdown/STOPPED 后 handle 失效、register 参数校验、unregister 后失效、generation 不复用、所有权对称性、open/close、BUSY（memory 与 target）、`WaitOutcome` 三态、`release_io` BUSY→OK、terminal 不淘汰、跨 Runtime 拒绝、DRAINING 下可 query/wait/release、`MemoryInfo` 无私有字段。

其中 `submit` 相关调用需按新签名改写（传 `count == 1`）。

## 必须新增的覆盖

1. **批量成功**：提交 4 个合法 request → `status` OK、`io` 有值、`initial_states.size() == 4` 且全 `ACCEPTED`；
2. **第 K 个失败的部分提交**（核心）：构造 4 个 request，令第 3 个（0-based index 2）因**可预见原因**被拒（例如它越界），断言：
   - `status` 非 OK；
   - `io` **仍有值**且 `valid()`；
   - `initial_states.size() == 4`，index 0/1/3 为 `ACCEPTED`，index 2 为 `REJECTED`；
   - 被接受的部分仍可 `query()` 到；
3. **零发出**：全部 request 都非法 → `io == nullopt`、`status` 非 OK、`initial_states` 全 `REJECTED`；
4. `count == 0` → `io == nullopt`、`status` OK、`initial_states` 空；
5. **bounds 校验**：分别覆盖 `length == 0`（`INVALID_ARGUMENT`）、memory 越界（`OUT_OF_RANGE`）、target 越界（`OUT_OF_RANGE`）；
6. **溢出安全**：`memory_offset` 或 `length` 取接近 `UINT64_MAX` 的值时被正确拒绝，且**不发生回绕误判为合法**；
7. **`wait()`/`query()` 非 const**：用 `static_assert` 或对非 const 引用调用来证明签名已改；
8. **测试后门不可从普通 public surface 调用**：给出编译期证据（例如注释说明 + 一段被注释掉的、若解注释则编译失败的代码，或用 SFINAE 检测该成员不可直接访问）。

测试源码只允许 include `<tutti/storage_runtime.h>`（以及已冻结的三个公共头、标准库头）。**不得** include `tutti/spi/**`。

Standalone CMake 必须：`project(... LANGUAGES CXX)`；C++17；target-scoped include `/data/home/ryeqiu/Tutti/tutti/include`；target-scoped 定义 `TUTTI_USE_HOST=1`（若选缺口 4 的方案 2，再加测试宏）；`-Wall -Wextra -Werror`；不查找 CUDA 或任何第三方 SDK；`enable_testing()` 并注册 CTest。

# 安全限制

绝对禁止 `sudo` / `insmod` / `rmmod` / `modprobe`；禁止启动 daemon/client、访问 `/dev/nvme*`、执行 CUDA 调用或任何硬件 IO。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 先记录修复前的缺口证据

在改动前，用一个临时探测程序（写到 `/tmp`，**不要**放进仓库）复现缺口 2：

```bash
# 注册 4096 字节缓冲，提交 memory_offset=1<<30 length=1<<40，观察是否被接受
```

把真实输出记录到结果文件。改完后同一探测必须显示被拒绝。

## 2. 清理与 configure

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round5-session1

cmake -S /data/home/ryeqiu/Tutti/tests/storage_runtime_contract \
  -B /data/home/ryeqiu/Tutti/build/round5-session1 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

要求日志无 CUDA / gRPC / yaml-cpp / libnvm / FIEMAP / NVMe dependency discovery。

## 3. Build 与 CTest

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round5-session1 \
  --target tutti_storage_runtime_contract_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round5-session1 \
  --output-on-failure -R '^tutti_storage_runtime_contract_test$'
```

要求 `-Werror` 零告警，1/1 PASS，且子测试总数 **不少于 22**（原 14 + 新增 8）。

## 4. 既有公共契约零改动

```bash
git diff --stat -- tutti/include/tutti/status.h tutti/include/tutti/io_types.h tutti/include/tutti/memory_types.h
```

必须为空。

## 5. 未依赖 SPI

```bash
grep -n 'spi/' /data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h
```

必须无输出。

## 6. 确认单请求重载未残留

```bash
grep -nE 'submit\(const IoRequest&' /data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h
```

必须无输出（只应存在批量签名）。

## 7. Public-boundary guard（词边界）

```bash
grep -nEiw 'libnvm|nvme|fiemap|grpc|yaml|prp|sgl|lba|cid|doorbell|iova|rkey|fd|extent' \
  /data/home/ryeqiu/Tutti/tutti/include/tutti/storage_runtime.h
```

如有命中须逐条说明是注释还是真实类型/字段。

## 8. Hygiene

```bash
git diff --check -- tutti/include/tutti/storage_runtime.h
```

对所有改动文件额外检查尾随空白与 EOF newline。确认只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. `submit()` 为批量签名，返回类型能表达部分提交；无单请求重载残留；
2. 五条 `IoSubmitOutcome` 不变量全部成立；
3. 「第 K 个 request 被拒」的 contract test 通过，且被接受部分仍可 query；
4. bounds 校验覆盖零长度、memory 越界、target 越界，且防溢出；
5. 逐 request 独立校验，单个越界不导致整批失败；
6. `wait()` / `query()` 已去 `const`；`WaitOutcome` 语义未回退；
7. 测试后门已收敛出普通 public surface，并有证据；
8. 既有 14 项覆盖全部保留且通过，子测试总数 ≥ 22；
9. 既有三个公共契约头零改动；
10. 未 include `tutti/spi/**`，未直接 include CUDA SDK；
11. HOST standalone configure/build/ctest 在 `-Werror` 下通过；
12. 未修改允许列表外文件；
13. 未执行任何模块、daemon 或 IO 操作；
14. 空白检查通过。

如果某项在无真实 DataPath 前提下无法真实测出，明确记录原因，但**不得**弱化契约、删除断言或伪造状态来让测试变绿。

# 结果落盘要求

完成任务和验收后，把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round5/result1.md`

至少包含：

1. 修改文件列表
2. 修复前缺口 2 的真实探测输出
3. 新 `submit` 签名与结果类型的实际 public surface
4. 五条不变量的实现方式
5. bounds 校验的实现方式（含防溢出写法）
6. 缺口 3 改动说明（去 const 的影响面）
7. 缺口 4 选用方案及「普通消费者无法调用」的证据
8. 新增 8 项测试的断言清单
9. 既有 14 项仍通过的证据（子测试总数）
10. configure / build / ctest 结果（含 `-Werror`）
11. 既有公共契约零改动、未依赖 SPI、无单请求重载残留的证明
12. public-boundary guard 结果
13. 文件边界与空白检查结果
14. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 命令失败就写真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round5/result1.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
