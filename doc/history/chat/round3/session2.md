# TASK T-009

你是一名资深 C++ storage runtime API 工程师。你只负责冻结 Tutti Phase 1 的最小公共 memory 类型：`MemoryKind`、`MemoryOwnership` 和 `MemoryView`。你看不到任何其他上下文，本 prompt 已包含完整接口契约、边界和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 已完成基线

前一轮已经建立：

- `tutti/include/tutti/status.h`
- `tutti/include/tutti/io_types.h`
- `tutti/include/tutti/cuda_like.h`
- `tutti_cuda_like` / `tutti_api`

其中 `io_types.h` 已经提供 `MemoryHandle`。本任务补充的是 **public memory view 的 position + ownership + expected identity** 语义，不实现 `MemoryRegistry`、DMA mapping 或 data-path registration。

# 架构契约

Memory 的位置与所有权必须正交：

```text
MemoryKind:
  HOST
  PINNED_HOST
  DEVICE
  MANAGED

MemoryOwnership:
  RUNTIME_OWNED
  CALLER_OWNED
```

关键约束：

- `EXTERNAL` 不再是 `MemoryKind`；
- caller 分配的 GPU memory 是 `DEVICE + CALLER_OWNED`；
- 本版本不提供 CUDA IPC/shared memory import API；
- 不加入 `IMPORTED` ownership；
- public memory 类型不得包含 DMA IOVA、PRP、rkey、fd 或 `backend_private`；
- profile identity 不能用闭集 vendor enum，因为新 profile 不应修改公共 noun；最小 source-level 表达使用非 opaque 的 `std::string`。

`MemoryView` 至少包含：

- address
- size
- 可选 expected CUDA-like profile name
- 可选 expected device id
- 可选 expected memory kind

它是 public API 值类型，不是 DMA registration record。

# 任务目标

新增公共头：

`/data/home/ryeqiu/Tutti/tutti/include/tutti/memory_types.h`

在 `namespace tutti` 中提供：

```cpp
enum class MemoryKind
enum class MemoryOwnership
struct MemoryView
```

要求 header-only、C++17、hardware-free、与其他现有公共类型可组合。

# 冻结接口

## `MemoryKind`

必须恰好包含：

```cpp
HOST
PINNED_HOST
DEVICE
MANAGED
```

## `MemoryOwnership`

必须恰好包含：

```cpp
RUNTIME_OWNED
CALLER_OWNED
```

## `MemoryView`

字段固定：

```cpp
struct MemoryView {
    void* address;
    std::uint64_t size;
    MemoryKind expected_kind;
    MemoryOwnership ownership;
    std::int32_t expected_device_id;
    std::string expected_profile;
};
```

语义：

- `address` 为 memory view 的起始地址；
- `size` 单位为 bytes；
- `expected_kind` 是 caller 期望 Runtime 验证的位置；
- `expected_device_id < 0` 表示未指定 expected device；
- `expected_profile` 为空字符串表示未指定 expected profile；
- `ownership` 描述 allocation owner，不描述 memory 是否来自另一个进程；
- 本值类型不执行 CUDA pointer query 或 validation；未来 `StorageRuntime` 负责验证。

本任务**故意不用** `std::optional`，保持最小 aggregate 值类型；不要扩展字段。若你认为 aggregate 与 task wording 的“可选”语义不清，通过 explicit sentinel（negative device id、empty profile）表达，并写进 header 注释和测试。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/include/tutti/memory_types.h`
- `/data/home/ryeqiu/Tutti/tests/memory_types_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/memory_types_contract/memory_types_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round3/result2.md`

其中 `chat/round3/result2.md` 保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round3-session2/`

该目录位于既有 ignored `build/` 下，不计入源码文件允许列表。

禁止修改或创建任何其他文件。尤其禁止修改：

- 根或 `tutti/` 的现有 `CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/status.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/io_types.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/cuda_like.h`
- 任意 Runtime、DataPath、Resolver、accelerator、NVMe、libnvm、kernel 文件
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round3/result2.md` 外的任何文件

禁止提交 Git commit。

# 头文件依赖限制

`memory_types.h` 只允许 include 完成值类型所需的 C++ 标准库头；至少可包含 `<cstdint>`、`<string>`。

明确禁止 include 或提及：

```text
cuda
hip
maca
musa
libnvm
nvme
grpc
yaml
backends/
io_engine/
device_manager/
tutti/cuda_like.h
```

Memory 位置语义必须与 vendor runtime 解耦；`expected_profile` 是字符串，不是 vendor enum。

# Contract test 要求

测试 target 与 CTest 名固定：

```text
tutti_memory_types_contract_test
```

测试为普通 C++17 可执行程序，不使用 GTest。

至少覆盖：

1. 四个 `MemoryKind` 可用且互不混淆；
2. 两个 `MemoryOwnership` 可用且互不混淆；
3. 不存在 `MemoryKind::EXTERNAL`、`MemoryKind::IMPORTED` 或 `MemoryOwnership::IMPORTED`（以注释+禁止字段 guard 的方式验证，不要写无法编译的引用）；
4. `MemoryView` 六个字段按冻结类型和顺序可赋值/读取；
5. 默认或显式构造的 unset expected profile/device 可用空字符串/负数表达；
6. `address` 可以是 `void*`，`size` 是 `std::uint64_t` bytes；
7. aggregate copy/move 语义；
8. 不包含 DMA IOVA、PRP、rkey、fd 或 `backend_private` 字段；
9. `MemoryView` 可与 `Result<MemoryView>` 组合；
10. `MemoryView` 可与 `MemoryHandle` 在同一 TU 中组合，不混淆 memory identity 与 data-path registration。

测试源码只 include：

```cpp
#include <tutti/memory_types.h>
#include <tutti/status.h>
#include <tutti/io_types.h>
```

以及标准库头。

Standalone CMake 必须：

- `project(... LANGUAGES CXX)`；
- C++17；
- target-scoped include `/data/home/ryeqiu/Tutti/tutti/include`；
- target-scoped 定义 `TUTTI_USE_HOST=1`，以满足 `io_types.h` 经由 `cuda_like.h` 的 profile 检查；
- 不查找 CUDA 或任何第三方 SDK；
- `enable_testing()` 并注册 CTest。

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
rm -rf /data/home/ryeqiu/Tutti/build/round3-session2
```

## 2. Standalone configure

```bash
cmake -S /data/home/ryeqiu/Tutti/tests/memory_types_contract \
  -B /data/home/ryeqiu/Tutti/build/round3-session2 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

要求 configure 日志不出现 CUDA、gRPC、yaml-cpp、libnvm 或 NVMe dependency discovery。

## 3. Build 和 CTest

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round3-session2 \
  --target tutti_memory_types_contract_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round3-session2 \
  --output-on-failure \
  -R '^tutti_memory_types_contract_test$'
```

要求 1/1 PASS。

## 4. Public-boundary guard

```bash
grep -nEi 'cuda|hip|maca|musa|libnvm|nvme|grpc|yaml|backends/|io_engine/|device_manager/|iova|rkey|backend_private|PRP' \
  /data/home/ryeqiu/Tutti/tutti/include/tutti/memory_types.h
```

必须无输出。

再检查 `expected_profile` 不是 enum，且没有 `EXTERNAL`/`IMPORTED` 成员。

## 5. Hygiene

```bash
git diff --check -- tutti/include/tutti/memory_types.h
```

对所有新增文件额外检查尾随空白和 EOF newline。确认本 session 只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. position 与 ownership 正交；
2. `MemoryKind` 无 `EXTERNAL`；`MemoryOwnership` 无 `IMPORTED`；
3. `MemoryView` 严格为六个冻结字段；
4. profile identity 使用字符串，不阻塞未来 community profile；
5. public 类型不泄漏 DMA/transport 私有字段；
6. 与 `Status/Result` 和 `MemoryHandle` 可组合；
7. HOST standalone configure/build/ctest 通过，不需要 CUDA SDK；
8. 未修改允许列表外文件；
9. 未执行任何模块、daemon 或 IO 操作；
10. 空白检查通过。

# 结果落盘要求

完成任务和验收后，必须把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round3/result2.md`

至少包含：

1. 修改/新增文件列表
2. 实际 public surface
3. expected profile/device/kind 的 unset 语义
4. 与旧 `EXTERNAL` 语义的拆分说明
5. configure/build/ctest 结果
6. public-boundary guard 结果
7. 文件边界与空白检查结果
8. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 不得预留或编写“总指挥验收”内容；总指挥会在结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round3/result2.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
