# TASK T-015

你是一名资深 CMake / 构建边界工程师。你只负责为 Tutti 仓内 SPI 头建立一个可消费、可安装的 `tutti_spi` target，并用一个消费者测试证明它无需硬编码绝对路径。你看不到任何其他上下文，本 prompt 已包含完整现状、边界和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 已确认的现状问题

## 1. `tutti/spi/` 完全没有被任何 CMake target 纳入

已核实：仓库内**没有任何** `CMakeLists.txt` 引用 `tutti/spi`。目前存在两个 SPI 头：

```text
/data/home/ryeqiu/Tutti/tutti/spi/storage_target_resolver.h
/data/home/ryeqiu/Tutti/tutti/spi/data_path.h
```

它们既不被任何 target 暴露，也不被安装。

## 2. 现有 contract test 靠硬编码绝对路径才能编译

现有 SPI contract test 的 CMake 里写着：

```cmake
target_include_directories(<test> PRIVATE
    /data/home/ryeqiu/Tutti
    /data/home/ryeqiu/Tutti/tutti/include)
```

绝对路径不可移植。根因就是缺少一个承载 SPI usage requirements 的 target。

## 3. 既有的对照实现（你应当对齐它的风格）

`tutti/CMakeLists.txt` 已有两个 INTERFACE target，可作为范式：

- 第 105-106 行：`tutti_cuda_like`（INTERFACE，由 profile 模块通过 `tutti_configure_cuda_like()` 注入 `TUTTI_USE_<PROFILE>` 定义与 vendor 依赖）
- 第 116-121 行：`tutti_api`（INTERFACE，无源码；`target_include_directories` 用 `$<BUILD_INTERFACE:...>/include` + `$<INSTALL_INTERFACE:include>`；`target_link_libraries(tutti_api INTERFACE tutti_cuda_like)`）
- 第 287-292 行：`install(TARGETS tutti_api EXPORT tutti_targets ...)`

# 任务目标

## 1. 新增 `tutti_spi` INTERFACE target

在 `tutti/CMakeLists.txt` 中新增一个 INTERFACE target `tutti_spi`，语义为「消费仓内 SPI 头所需的全部 usage requirements」。

要求：

- 无源码文件（纯 INTERFACE）；
- include interface 必须让消费者可以写 `#include <tutti/spi/data_path.h>` 和 `#include <tutti/spi/storage_target_resolver.h>`。注意 SPI 头位于 `tutti/spi/` 而公共头位于 `tutti/include/tutti/`，两者根目录不同，因此 `tutti_spi` 的 include 根应当是**仓库根**（即 `tutti/` 的上一级），使 `<tutti/spi/...>` 可解析；
- 必须使用 generator expression 区分 build 与 install interface，**不得**硬编码绝对路径；
- SPI 头依赖公共头（`data_path.h` include 了 `<tutti/status.h>` 与 `<tutti/io_types.h>`；`storage_target_resolver.h` include 了 `<tutti/status.h>`），因此 `tutti_spi` 必须 `INTERFACE` link `tutti_api` 以继承公共 include 路径与 profile 宏；
- **不要**在 `tutti_spi` 上重复定义 `TUTTI_USE_*` 宏，也**不要**让它直接 link CUDA target —— 这些必须经 `tutti_api` → `tutti_cuda_like` 继承而来；
- 全部使用 target-scoped 命令。**禁止** `add_definitions()` 或全局 `include_directories()`。

## 2. 安装

- 把 `tutti_spi` 加入既有的 `tutti_targets` export 集合（与 `tutti_api` 同一集合，风格对齐第 287-292 行）；
- 安装 `tutti/spi/` 下的头文件到与 install interface 一致的位置，使安装后 `#include <tutti/spi/data_path.h>` 仍可解析。

## 3. 门控约束

`tutti_spi` **不得**被 `TUTTI_BUILD_HARDWARE_STACK` 门控。SPI 头是 hardware-free 的纯 source-level 契约，在 HOST profile 下也必须可用。与 `tutti_api` 一致，无条件定义。

## 4. 新增消费者测试（证明 target 真的够用）

新增 `tests/spi_consumer/`，其 CMake **只 link `tutti_spi`**，并且：

- **禁止**出现任何绝对路径；
- **禁止**手工 `target_include_directories` 添加仓库路径；
- **禁止**手工 `target_compile_definitions` 添加 `TUTTI_USE_*`；

必须证明这三样都能从 `tutti_spi` 的 usage requirements 自动获得。这是本任务的核心验收点。

# 关于并发 worker 的重要约束

**另有 worker 正在修改 `tutti/spi/data_path.h`**，具体是把该文件内的 `enum class MemoryKind { HOST, DEVICE }` 重命名为 `enum class DataPathMemoryKind`。

因此你的消费者测试**绝对不要引用** `data_path.h` 中的内存种类枚举（无论叫 `MemoryKind` 还是 `DataPathMemoryKind`），也不要引用 `DataPathMemoryView::kind` 字段。

安全可引用的稳定类型（这些不在改名范围内）：

- `tutti::DataPathTarget` / `DataPathMemory` / `DataPathOp`（`valid()`、`==`、`!=`）
- `tutti::DataPathCapabilities`（`name`、`source_api_version`、各 `bool` 与 `std::uint64_t` 字段）
- `tutti::RegistrationDomainKey`
- `tutti::SubmitOutcome` / `RequestInitialState` / `RequestState`
- `tutti::ProgressBudget` / `ProgressResult` / `DataPathSnapshot` / `OpState`
- `tutti::DataPathConfig` / `RegistrationScope` / `ProgressModel`
- `tutti::ResolveOptions` / `ResolvedTarget`（默认空壳、`valid()`、各 metadata accessor）

你的测试只需要证明**「这些 SPI 类型能被 include 并使用」**，不需要实现 fake DataPath 或 fake Resolver —— 那些已有专门的 contract test 覆盖，不要重复。保持这个消费者测试**小而聚焦于构建边界**。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/spi_consumer/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/spi_consumer/spi_consumer_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round4/result4.md`

其中 `chat/round4/result4.md` 保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round4-session4/`

该目录位于既有 ignored `build/` 下，不计入源码文件允许列表。

禁止修改或创建任何其他文件。尤其禁止修改：

- `/data/home/ryeqiu/Tutti/tutti/spi/**`（**另有 worker 正在改，绝对不要碰**）
- `/data/home/ryeqiu/Tutti/tutti/include/**`
- `/data/home/ryeqiu/Tutti/CMakeLists.txt`（根 CMake，不是本任务）
- `/data/home/ryeqiu/Tutti/tutti/cmake/**`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tests/` 下除 `tests/spi_consumer/` 外的任何目录（**尤其** `tests/data_path_contract/`、`tests/storage_runtime_contract/`，均属其他 worker）
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round4/result4.md` 外的任何文件
- 任意 accelerator、NVMe、libnvm、kernel 文件

禁止提交 Git commit。

## 对 `tutti/CMakeLists.txt` 的改动纪律

这是共享文件，必须外科手术式改动：

- 只新增 `tutti_spi` 的定义与安装，以及必要的头文件安装规则；
- **不要**重排、重构或「顺手改进」既有内容；
- **不要**改动 `tutti_api`、`tutti_cuda_like`、`tutti_types` 的现有定义；
- **不要**改动 `TUTTI_ACCELERATOR` / `TUTTI_BUILD_HARDWARE_STACK` 相关逻辑；
- **不要**改动既有的 `add_subdirectory` 顺序或 `BUILD_TESTING` 分支；
- 保持与文件现有注释风格一致。

# 依赖限制

`tests/spi_consumer/spi_consumer_test.cpp` 只允许 include：

```cpp
#include <tutti/spi/data_path.h>
#include <tutti/spi/storage_target_resolver.h>
```

以及标准库头。

**禁止** include CUDA SDK 头、`tutti/include/**` 下的头（那些应经 `tutti_api` 由 SPI 头间接带入，正是要验证的传递性）、或任何 `backends/`、`io_engine/`、`device_manager/` 路径。

# 两种测试组织方式（二选一）

## 方式 A（推荐）：作为 `tutti/` 构建树的子目录

在 `tutti/CMakeLists.txt` 的 `BUILD_TESTING` 分支中 `add_subdirectory` 指向 `tests/spi_consumer`，与现有 `tests/cuda_like` 的纳入方式保持一致（请先阅读现有写法再照做）。

此时 `tests/spi_consumer/CMakeLists.txt` 不需要自己的 `project()`，直接 `add_executable` + `target_link_libraries(<test> PRIVATE tutti_spi)` + `add_test`。

## 方式 B：standalone

若方式 A 与既有结构冲突，可退化为 standalone CMake，但那样就无法验证 `tutti_spi` 的 usage requirements 传递性（standalone 下拿不到 `tutti_spi` target）。**因此如果你选方式 B，本任务的核心价值就丢失了**，必须在结果中说明为何不得不退化。

**优先选方式 A。**

# 安全限制

绝对禁止：

```text
sudo
insmod
rmmod
modprobe
```

也禁止启动 daemon/client、访问 `/dev/nvme*`、执行 CUDA 调用或任何硬件 IO。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 清理专用 build 目录

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round4-session4
```

## 2. HOST profile configure（不需要 CUDA SDK）

```bash
cmake -S /data/home/ryeqiu/Tutti/tutti \
  -B /data/home/ryeqiu/Tutti/build/round4-session4 \
  -DTUTTI_ACCELERATOR=HOST \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

要求 configure 成功，且日志中不出现 CUDA / gRPC / yaml-cpp / libnvm / NVMe dependency discovery（HOST profile 会把 `TUTTI_BUILD_HARDWARE_STACK` 强制为 OFF）。

## 3. Build 与 CTest

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round4-session4 \
  --target tutti_spi_consumer_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round4-session4 \
  --output-on-failure \
  -R '^tutti_spi_consumer_test$'
```

要求 1/1 PASS。

（测试 target 与 CTest 名固定为 `tutti_spi_consumer_test`。）

## 4. 验证 usage requirements 真的传递了

从生成的 compile command 中确认消费者获得了 include 路径与 profile 宏：

```bash
grep -o -- '-DTUTTI_USE_HOST=1' /data/home/ryeqiu/Tutti/build/round4-session4/compile_commands.json | head -1
grep -c 'spi_consumer_test.cpp' /data/home/ryeqiu/Tutti/build/round4-session4/compile_commands.json
```

在结果中贴出该消费者的完整编译命令行，证明 `-DTUTTI_USE_HOST=1` 与仓库 include 路径均来自 `tutti_spi`，而非测试自己手写。

## 5. 验证消费者 CMake 无绝对路径、无手工注入

```bash
grep -nE '/data/home/ryeqiu|include_directories|compile_definitions|TUTTI_USE_' \
  /data/home/ryeqiu/Tutti/tests/spi_consumer/CMakeLists.txt
```

必须无输出。

## 6. 验证未使用全局 CMake 命令

```bash
grep -RInE '(^|[^[:alnum:]_])(add_definitions|include_directories)[[:space:]]*\(' \
  /data/home/ryeqiu/Tutti/tests/spi_consumer
```

必须无输出。

## 7. 验证未触碰 SPI 头与其他 worker 的目录

```bash
git status --short --untracked-files=all -- tutti/spi tests/data_path_contract tests/storage_runtime_contract | cat
git diff --stat -- tutti/spi/ | cat
```

`git diff --stat -- tutti/spi/` 必须为空（你没改 SPI 头）。若 `git status` 显示 `tutti/spi/` 或其他测试目录有变化，那是并发 worker 的产物，**不是你的**，在结果中说明即可，但你必须能证明自己没动它们。

## 8. Install 冒烟（验证 install interface 正确）

```bash
cmake --install /data/home/ryeqiu/Tutti/build/round4-session4 \
  --prefix /data/home/ryeqiu/Tutti/build/round4-session4/_install

find /data/home/ryeqiu/Tutti/build/round4-session4/_install -name '*.h' -path '*spi*'
```

要求安装后能找到 SPI 头，且其相对路径使 `#include <tutti/spi/data_path.h>` 在 install tree 下成立。

若 install 因既有无关 target 报错，记录真实错误并说明是否属于本任务范围；不得为此去改其他 target。

## 9. Hygiene

```bash
git diff --check -- tutti/CMakeLists.txt
```

对所有新增文件额外检查尾随空白和 EOF newline。确认本 session 只触碰允许列表。

同时贴出

```bash
git diff -- tutti/CMakeLists.txt
```

的完整 diff，证明改动是外科手术式的、未重构既有内容。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. `tutti_spi` 为无源码 INTERFACE target，使用 generator expression 区分 build/install interface，无硬编码绝对路径；
2. `tutti_spi` 经 `INTERFACE` link `tutti_api` 继承公共 include 与 profile 宏，未重复定义 `TUTTI_USE_*`，未直接 link CUDA；
3. `tutti_spi` 未被 `TUTTI_BUILD_HARDWARE_STACK` 门控；
4. `tutti_spi` 已加入 `tutti_targets` export，SPI 头被安装且 install tree 下 include 路径成立；
5. `tests/spi_consumer` 只 link `tutti_spi`，无绝对路径、无手工 include、无手工 profile 宏；
6. 消费者的真实编译命令证明 usage requirements 已传递；
7. HOST profile configure/build/ctest 通过，不需要 CUDA SDK；
8. `tutti/spi/**` 零改动；
9. `tutti/CMakeLists.txt` 的改动是纯新增，未重构既有内容；
10. 未使用 `add_definitions()` 或全局 `include_directories()`；
11. 未修改允许列表外文件；
12. 未执行任何模块、daemon 或 IO 操作；
13. 空白检查通过。

如果并发 worker 对 `tutti/spi/data_path.h` 的改名导致你的消费者测试编译失败，**说明你引用了不该引用的枚举**。请改为只引用本 prompt「安全可引用的稳定类型」清单中的类型，不要去改 SPI 头，也不要等待对方。

# 结果落盘要求

完成任务和验收后，必须把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round4/result4.md`

至少包含：

1. 修改/新增文件列表
2. `tutti_spi` 的完整定义（贴出实际 CMake 代码）与设计理由（include 根为何是仓库根、为何 link `tutti_api`）
3. 安装规则与 install 冒烟结果
4. 选择方式 A 还是 B，及理由
5. 消费者测试引用了哪些 SPI 类型（须避开改名中的枚举）
6. configure / build / ctest 结果
7. 消费者的完整真实编译命令行（证明 usage requirements 传递）
8. 消费者 CMake 无绝对路径 / 无手工注入的验证结果
9. `tutti/spi/**` 零改动的证明
10. `tutti/CMakeLists.txt` 的完整 diff
11. 文件边界与空白检查结果
12. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 如果命令失败，写入真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round4/result4.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
