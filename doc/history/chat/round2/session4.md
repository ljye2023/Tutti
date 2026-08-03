# TASK T-007

你是一名资深 C++ storage runtime API 工程师。你只负责冻结 Tutti Phase 1 的最小公共 IO nouns：三种强类型 handle、`IoDirection`、`IoRequest`、`ExecutionDomain` 和 `HostSubmitContext`。你看不到任何其他上下文，本 prompt 已包含完整接口契约、边界和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 架构背景

Tutti 的目标 public API 不再暴露旧实现中的：

```text
MemoryRegion*
void* target_handle
PRP
LBA
extent
fd
namespace id
rkey
framework block id
```

上层只表达 memory、target 和异步 operation 的稳定身份，以及 byte-range IO 意图。

前一轮已经建立统一 profile 入口：

```cpp
#include <tutti/cuda_like.h>
```

HOST profile 可在没有 CUDA SDK 时提供 `cudaStream_t` 等 contract 类型。

本任务只定义公共值类型，不实现 registry、Runtime、DataPath、submit/query/wait 或真实 IO。

# 任务目标

新增公共头：

`/data/home/ryeqiu/Tutti/tutti/include/tutti/io_types.h`

在 `namespace tutti` 中提供：

```text
MemoryHandle
TargetHandle
IoHandle
IoDirection
IoRequest
ExecutionDomain
HostSubmitContext
```

要求 header-only、C++17，公共 accelerator 类型只能经 `<tutti/cuda_like.h>` 获得。

# Opaque handle 契约

三种 handle：

```cpp
MemoryHandle
TargetHandle
IoHandle
```

必须满足：

1. 是互不相同的强类型；
2. 不能互相隐式转换；
3. 不能从裸整数、指针或 `void*` 隐式构造；
4. 默认构造得到 invalid handle；
5. 提供明确的 `valid()` 或等价检查；
6. 可以 copy/move，适合作为轻量值类型；
7. 支持同类型 equality/inequality；
8. 语义上包含 runtime identity、slot 和 generation，以便未来拒绝 cross-runtime/stale handle；
9. 不把内部对象地址强转为整数；
10. 不公开允许普通 application 任意伪造有效 handle 的 factory；有效 handle 只能由未来 `StorageRuntime`/内部 access 创建。

建议内部语义：

```text
runtime_id
slot
generation
```

但注意：

- 这只是当前 source-level 表达，不承诺稳定 binary layout；
- 测试禁止固定 `sizeof`、字段 offset 或编码值；
- 不加入 UUID、shared_ptr、全局 registry 或 heap allocation；
- 可以 forward declare/friend 未来 `StorageRuntime`，或使用最小内部 access seam，但不得实现 Runtime。

# `IoDirection`

名称固定：

```cpp
enum class IoDirection {
    READ,
    WRITE,
};
```

语义：

- `READ`：target → memory；
- `WRITE`：memory → target。

不得加入 FLUSH、DISCARD、COMPARE 等未要求操作。

# `IoRequest`

字段和顺序固定为：

```cpp
struct IoRequest {
    IoDirection direction;
    MemoryHandle memory;
    std::uint64_t memory_offset;
    TargetHandle target;
    std::uint64_t target_offset;
    std::uint64_t length;
};
```

所有 offset/length 单位都是 bytes。

明确禁止额外字段：

```text
stream
MemoryRegion pointer
backend/data-path pointer
tensor shape
KV block id
fd
extent
LBA
namespace id
PRP/SGL
rkey
retry/priority/cancel
```

Runtime 未来负责 bounds/alignment/capability 校验；本值类型不实现校验算法。

# `ExecutionDomain`

名称和枚举项固定：

```cpp
enum class ExecutionDomain {
    HOST_EXECUTION,
    DEVICE_EXECUTION,
};
```

本轮禁止加入：

```text
DEVICE_API
CPU_SUBMIT
GPU_SUBMIT
COOPERATIVE
AUTO
```

这里仅描述 host API 调用后，data-path work 在 host 还是 device 执行，不描述未来 device caller。

# `HostSubmitContext`

字段固定：

```cpp
struct HostSubmitContext {
    ExecutionDomain execution_domain;
    std::int32_t device_id;
    cudaStream_t stream;
};
```

要求：

- `cudaStream_t` 只能来自：

```cpp
#include <tutti/cuda_like.h>
```

- `HOST_EXECUTION` 可使用空 stream；
- `DEVICE_EXECUTION` 未来由 Runtime 校验 stream 必填；本值类型不调用 CUDA API、不做 Runtime validation；
- 不加入 event、native context、callback 或 device-side API。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/include/tutti/io_types.h`
- `/data/home/ryeqiu/Tutti/tests/io_types_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/io_types_contract/io_types_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round2/result4.md`

其中 `chat/round2/result4.md` 只用于保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round2-session4/`

该目录位于既有 ignored `build/` 下，不计入源码文件允许列表。

禁止修改或创建任何其他文件。尤其禁止修改：

- 根或 `tutti/` 的任何现有 `CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tutti/io_engine/include/io_types.h`（这是旧接口，只作迁移参照）
- `/data/home/ryeqiu/Tutti/tutti/backends/include/storage_target.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/status.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/cuda_like.h`
- 任意 Runtime/DataPath/Resolver/accelerator/NVMe/kernel 实现
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round2/result4.md` 外的任何文件

禁止提交 Git commit。

# 头文件依赖限制

`io_types.h` 只允许 include：

```cpp
#include <cstdint>
#include <tutti/cuda_like.h>
```

如果实现 equality 确实需要其他标准库头，先证明必要；不要加入容器、string、memory、variant 或第三方依赖。

明确禁止 include 或提及 storage 私有实现：

```text
backends/
io_engine/
device_manager/
libnvm
nvme
PRP
LBA
StorageTarget
MemoryRegion
```

# Contract test 要求

测试 target 和 CTest 名固定：

```text
tutti_io_types_contract_test
```

测试必须是 hardware-free 普通 C++17 可执行程序，不使用 GTest。

至少覆盖：

1. `MemoryHandle`、`TargetHandle`、`IoHandle` 是不同类型；
2. 三者不可互相隐式转换；
3. 三者不可从 `void*` 或整数隐式构造；
4. 三者 default construct 后 invalid；
5. 三者 copy/move constructible 和 assignable；
6. 同类型 invalid handle equality 正确；
7. 不固定 handle 大小或内部布局；
8. `IoDirection::READ/WRITE` 可用；
9. `IoRequest` 的六个字段可按冻结类型赋值和读取；
10. `memory_offset` 与 `target_offset` 是两个独立 `std::uint64_t`；
11. `ExecutionDomain` 只有当前两个有效枚举语义；
12. `HostSubmitContext` 在 HOST profile 下可用，空 `cudaStream_t` 可表达 HOST execution；
13. 源码只通过 `<tutti/io_types.h>` 获取公共 Tutti 类型。

使用 `static_assert` 覆盖类型性质，使用运行时检查覆盖默认值和字段语义。

Standalone CMake 必须：

- `project(... LANGUAGES CXX)`；
- C++17；
- 对测试 target 以 target-scoped 方式定义 `TUTTI_USE_HOST=1`；
- 对测试 target 加入 `/data/home/ryeqiu/Tutti/tutti/include`；
- 不查找 CUDA 或任何第三方 SDK；
- `enable_testing()` 并注册 CTest。

这是独立 contract test，禁止为了接线去修改产品 `tutti/CMakeLists.txt`。

# 安全限制

绝对禁止：

```text
sudo
insmod
rmmod
modprobe
```

也禁止启动 daemon/client、访问 `/dev/nvme*`、执行任何硬件测试或 IO。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 清理专用 build 目录

只允许清理：

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round2-session4
```

## 2. Standalone configure

```bash
cmake -S /data/home/ryeqiu/Tutti/tests/io_types_contract \
  -B /data/home/ryeqiu/Tutti/build/round2-session4 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

要求 configure 日志不出现 CUDA toolkit、gRPC、yaml-cpp、libnvm 或 NVMe dependency discovery。

## 3. Build 和 CTest

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round2-session4 \
  --target tutti_io_types_contract_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round2-session4 \
  --output-on-failure \
  -R '^tutti_io_types_contract_test$'
```

要求 1/1 PASS。

## 4. Public-boundary 静态检查

```bash
grep -nEi 'PRP|LBA|libnvm|nvme|StorageTarget|MemoryRegion|backend_private|void[[:space:]]*\*[[:space:]]*target_handle|backends/|io_engine/|device_manager/' \
  /data/home/ryeqiu/Tutti/tutti/include/tutti/io_types.h
```

必须无输出。

再检查 `io_types.h` 中 accelerator include 只能是：

```text
tutti/cuda_like.h
```

不得直接 include `cuda.h` 或 `cuda_runtime.h`。

## 5. Hygiene

```bash
git diff --check -- tutti/include/tutti/io_types.h
```

对所有新增文件额外检查尾随空白和 EOF newline。确认本 session 只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. 三种 handle 强类型、默认 invalid、不可互转/伪造。
2. handle 语义预留 runtime identity + slot + generation，但不冻结 ABI 大小。
3. `IoRequest` 严格只有冻结的六项 storage intent。
4. 两个 offset 独立且单位为 bytes。
5. `ExecutionDomain` 只有 `HOST_EXECUTION`/`DEVICE_EXECUTION`。
6. `HostSubmitContext` 仅经统一 CUDA-like 头使用 `cudaStream_t`。
7. Public header 不泄漏任何 storage/device 私有类型。
8. HOST standalone configure/build/ctest 通过且不依赖 CUDA SDK。
9. 未修改允许列表外文件。
10. 未执行任何模块、daemon 或 IO 操作。
11. 空白检查通过。

如果 private construction seam 暂时没有 consumer，选择最小设计；不要为了测试暴露 public `from_raw()` 或伪造有效 handle 的接口。

# 结果落盘要求

完成任务和验收后，必须把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round2/result4.md`

至少包含：

1. 修改/新增文件列表
2. 三种 handle 的实际 public surface 与内部构造边界
3. `IoRequest`/`HostSubmitContext` 定义摘要
4. static_assert 和运行时 contract 覆盖
5. configure/build/ctest 结果
6. public-boundary 静态检查结果
7. 文件边界与空白检查结果
8. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 不得预留或编写“总指挥验收”内容；总指挥会在结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round2/result4.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
