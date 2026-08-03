# TASK T-006

你是一名资深 C++ 公共 API 工程师。你只负责冻结 Tutti Phase 1 的最小硬件无关错误契约：`StatusCode`、`Status` 和 `Result<T>`。你看不到任何其他上下文，本 prompt 已包含完整接口约束、文件边界和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 架构背景

Tutti 正在从旧的 bool/裸错误码和 NVMe/CUDA 私有错误，迁移到稳定的公共 storage runtime 契约。

公共 API 不允许只返回 `bool`。本轮只建立 source-level C++17 值类型，不承诺稳定 binary ABI，不实现 Runtime、DataPath、IO completion 或日志系统。

# 任务目标

新增公共头：

`/data/home/ryeqiu/Tutti/tutti/include/tutti/status.h`

在 `namespace tutti` 中提供：

```text
StatusCode
Status
Result<T>
```

要求 header-only、C++17、hardware-free。

# 冻结的 `StatusCode`

必须是强类型 enum，并包含以下全部稳定分类；名称固定：

```cpp
OK
INVALID_ARGUMENT
OUT_OF_RANGE
NOT_FOUND
UNSUPPORTED
NOT_READY
BUSY
RESOURCE_EXHAUSTED
TIMEOUT
DEVICE_ERROR
DATA_LOSS
INTERNAL
```

约束：

- `OK` 必须表示成功。
- 不能使用裸 `int` 或 `bool` 代替。
- 不加入 CUDA error、errno、NVMe status 等实现私有枚举。
- 不主动加入 `CANCELLED`、`RETRY`、`EOF`、`PERMISSION_DENIED` 等本轮未要求分类。

# 冻结的 `Status` 最小语义

`Status` 至少提供：

```text
- 默认/显式成功状态
- 从 StatusCode + message 构造错误状态
- bool ok() const
- StatusCode code() const
- const std::string& message() const
```

要求：

- 默认构造必须是 `OK`。
- 提供清晰的成功 factory，例如 `Status::Ok()`；名称可以按现有 C++ 风格选择，但测试必须覆盖。
- 非 OK 状态可以携带可读 message。
- copy/move 必须正常。
- 不暴露第三方或设备私有 C/C++ 类型。
- 本轮不实现 native domain/code、DataPath 名称、request index；架构允许未来扩展，但当前没有真实消费方，不要过度设计。
- 不抛异常表达普通错误。

# 冻结的 `Result<T>` 最小语义

`Result<T>` 必须能表达且只能表达二选一：

```text
成功：一个 T 值 + OK status
失败：一个非 OK Status，无 T 值
```

最低要求：

- 支持普通值，例如 `int`、`std::string`。
- 支持 move-only 值，例如 `std::unique_ptr<int>`。
- 提供 `ok()` 与 `status()`。
- 提供能安全判断值是否存在的 API，例如 `has_value()`。
- 提供成功路径的值访问；至少覆盖 mutable、const 和 move 语义。
- 失败结果不能被当成成功值。
- 禁止产生“没有值但 status 为 OK”的非法状态：factory/constructor 收到这种组合时，必须拒绝或确定性归一为非 OK `INTERNAL`，不能静默保留非法状态。
- 不要求 `Result<void>`。
- 不实现 monadic helpers、callback、coroutine、exception wrapper 或宏体系。

实现应使用标准库值类型，保持最少代码。优先清晰的 `std::optional<T>` 或 `std::variant<T, Status>`，不要自行实现复杂 union 生命周期。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/include/tutti/status.h`
- `/data/home/ryeqiu/Tutti/tests/status_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/status_contract/status_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round2/result3.md`

其中 `chat/round2/result3.md` 只用于保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round2-session3/`

该目录位于既有 ignored `build/` 下，不计入源码文件允许列表。

禁止修改或创建任何其他文件。尤其禁止修改：

- 根或 `tutti/` 的任何现有 `CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/cuda_like.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/io_types.h`
- 任意 Runtime、DataPath、Resolver、accelerator、NVMe、libnvm、kernel 文件
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round2/result3.md` 外的任何文件

禁止提交 Git commit。

# 依赖限制

`status.h` 只能依赖完成该值类型所需的 C++ 标准库头。

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
```

不得依赖 `tutti/cuda_like.h`；错误模型必须完全 hardware-free。

# Contract test 要求

测试 target 名和 CTest 名固定：

```text
tutti_status_contract_test
```

测试使用普通可执行程序，不使用 GTest 或第三方库。

至少覆盖：

1. `StatusCode` 的全部 12 个稳定分类可编译、互不混淆；
2. 默认 `Status` 是 OK；
3. 显式成功 factory 是 OK；
4. 每类非 OK status 的 `code()` 正确；
5. message 可读且保持；
6. `Status` copy/move；
7. `Result<int>` 成功值；
8. `Result<std::string>` 成功值；
9. 错误 `Result<int>` 保持 code/message 且无值；
10. `Result<std::unique_ptr<int>>` 成功构造和 move-out；
11. 非法“OK status 但无值”组合不能成为合法成功结果；
12. 不在错误路径调用未定义的 `value()`；先通过 `ok()/has_value()` 验证。

测试源码只 include：

```cpp
#include <tutti/status.h>
```

以及标准库头。

Standalone `tests/status_contract/CMakeLists.txt` 必须：

- `project(... LANGUAGES CXX)`；
- 使用 C++17；
- 只为测试 target 添加 `/data/home/ryeqiu/Tutti/tutti/include`；
- `enable_testing()` 并注册 CTest；
- 不查找 CUDA、gRPC、yaml-cpp 或任何第三方包。

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
rm -rf /data/home/ryeqiu/Tutti/build/round2-session3
```

## 2. Standalone configure

```bash
cmake -S /data/home/ryeqiu/Tutti/tests/status_contract \
  -B /data/home/ryeqiu/Tutti/build/round2-session3 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

要求 configure 日志不出现 CUDA、gRPC、yaml-cpp、libnvm 或 NVMe dependency discovery。

## 3. Build 和 CTest

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round2-session3 \
  --target tutti_status_contract_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round2-session3 \
  --output-on-failure \
  -R '^tutti_status_contract_test$'
```

要求 1/1 PASS。

## 4. Public-header dependency guard

```bash
grep -nEi 'cuda|hip|maca|musa|libnvm|nvme|grpc|yaml|backends/|io_engine/|device_manager/' \
  /data/home/ryeqiu/Tutti/tutti/include/tutti/status.h
```

必须无输出。

## 5. Hygiene

```bash
git diff --check -- tutti/include/tutti/status.h
```

对所有新增文件额外检查尾随空白和 EOF newline。确认本 session 只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. `StatusCode` 包含且只围绕要求的稳定错误模型。
2. `Status` 明确区分 OK/错误并保留 message。
3. `Result<T>` 无非法 OK-without-value 状态。
4. move-only value contract 通过。
5. 头文件完全 hardware-free、header-only、C++17。
6. Standalone configure/build/ctest 通过。
7. 未修改允许列表外文件。
8. 未执行任何模块、daemon 或 IO 操作。
9. 空白检查通过。

如果接口约束有冲突，选择最小、值语义、无异常的实现，并在结果中说明；不要扩大任务范围。

# 结果落盘要求

完成任务和验收后，必须把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round2/result3.md`

至少包含：

1. 修改/新增文件列表
2. `StatusCode`/`Status`/`Result<T>` 的实际 public surface
3. 非法状态组合的处理方式
4. move-only 支持说明
5. configure/build/ctest 结果
6. dependency guard 结果
7. 文件边界与空白检查结果
8. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 不得预留或编写“总指挥验收”内容；总指挥会在结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round2/result3.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
