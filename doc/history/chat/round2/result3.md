# T-006 Status Contract — Result

## 1. 修改/新增文件列表

| 文件 | 操作 |
|---|---|
| `tutti/include/tutti/status.h` | 新增 |
| `tests/status_contract/CMakeLists.txt` | 新增 |
| `tests/status_contract/status_contract_test.cpp` | 新增 |
| `chat/round2/result3.md` | 新增（本文件） |

未修改任何允许列表外的文件。

## 2. StatusCode / Status / Result\<T\> 实际 public surface

### StatusCode

```cpp
enum class StatusCode {
    OK,
    INVALID_ARGUMENT,
    OUT_OF_RANGE,
    NOT_FOUND,
    UNSUPPORTED,
    NOT_READY,
    BUSY,
    RESOURCE_EXHAUSTED,
    TIMEOUT,
    DEVICE_ERROR,
    DATA_LOSS,
    INTERNAL,
};
```

强类型 enum，共 12 个稳定分类。`OK` 表示成功，其余均为错误。不含裸 `int`/`bool`，不含任何 vendor 私有码。

### Status

```cpp
class Status {
public:
    Status();                                    // 默认 OK
    static Status Ok();                          // 显式成功 factory
    Status(StatusCode code, std::string message);// 错误构造

    bool ok() const noexcept;
    StatusCode code() const noexcept;
    const std::string& message() const noexcept;
};
```

- 默认构造为 OK，message 为空。
- `Status::Ok()` 返回成功状态。
- copy/move 为默认值语义，正常工作。
- 不暴露第三方或设备私有类型。
- 不抛异常表达普通错误。

### Result\<T\>

```cpp
template <typename T>
class Result {
public:
    Result(T value);                             // 隐式成功构造
    explicit Result(Status status);              // 显式失败构造

    static Result Success(T value);              // 命名成功 factory
    static Result Failure(Status status);        // 命名失败 factory

    bool ok() const noexcept;
    bool has_value() const noexcept;
    const Status& status() const noexcept;

    T& value() &;                                // mutable 访问
    const T& value() const&;                     // const 访问
    T&& value() &&;                              // move 访问
};
```

- 成功路径：持有 T 值，status 为 OK。
- 失败路径：无 T 值，status 为非 OK。
- 支持 `int`、`std::string` 等普通值类型。
- 支持 `std::unique_ptr<int>` 等 move-only 值类型。
- `value()` 前置条件：`has_value()` 为 true；在错误路径调用 `value()` 属于契约违反。

## 3. 非法状态组合的处理方式

非法组合："OK status 但无 T 值"。

处理方式：**确定性归一为非 OK `INTERNAL`**。

当 `Result(Status status)` 构造函数或 `Result::Failure(Status status)` factory 收到 `status.ok() == true` 的输入时，内部 `normalize_failure_` 函数将其替换为：

```cpp
Status(StatusCode::INTERNAL,
    "Result<T> constructed with OK status but no value; "
    "normalized to INTERNAL");
```

测试 `test_illegal_ok_without_value` 验证了两种路径（命名 factory 和显式构造函数）都会归一为 `INTERNAL`，且 `ok()` 返回 false、`has_value()` 返回 false。

不静默保留非法状态，不抛异常。

## 4. move-only 支持说明

`Result<T>` 内部使用 `std::optional<T>` 存储值。`std::optional<T>` 正确传播 T 的 move 语义。

- 构造：`Result(T value)` 按值接收，通过 `std::move` 转入 `std::optional`。对于 `std::unique_ptr<int>`，传入 rvalue 时触发 move 构造。
- 访问：`T&& value() &&` 返回右值引用，允许 move-out。
- 测试 `test_result_unique_ptr_success_and_move` 验证：构造 `Result<std::unique_ptr<int>>` 后，通过 `std::move(r).value()` 成功 move-out，提取的 `unique_ptr` 非空且值正确。

`Result<T>` 本身也是 move-constructible / move-assignable（编译器生成的隐式 move 语义），因为 `std::optional<T>` 和 `Status` 都是可 move 的。

## 5. configure / build / ctest 结果

### Configure

```
$ cmake -S /data/home/ryeqiu/Tutti/tests/status_contract \
  -B /data/home/ryeqiu/Tutti/build/round2-session3 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

-- The CXX compiler identification is GNU 13.1.1
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: /opt/rh/gcc-toolset-13/root/usr/bin/c++ - skipped
-- Detecting CXX compile features - done
-- Configuring done (0.2s)
-- Generating done (0.0s)
-- Build files have been written to: /data/home/ryeqiu/Tutti/build/round2-session3
```

日志中无 CUDA、gRPC、yaml-cpp、libnvm、NVMe dependency discovery。

### Build

```
$ cmake --build /data/home/ryeqiu/Tutti/build/round2-session3 \
  --target tutti_status_contract_test -j8

[ 50%] Building CXX object CMakeFiles/tutti_status_contract_test.dir/status_contract_test.cpp.o
[100%] Linking CXX executable tutti_status_contract_test
[100%] Built target tutti_status_contract_test
```

零 warning（`-Wall -Wextra -Werror`）。

### CTest

```
$ ctest --test-dir /data/home/ryeqiu/Tutti/build/round2-session3 \
  --output-on-failure -R '^tutti_status_contract_test$'

Internal ctest changing into directory: /data/home/ryeqiu/Tutti/build/round2-session3
Test project /data/home/ryeqiu/Tutti/build/round2-session3
    Start 1: tutti_status_contract_test
1/1 Test #1: tutti_status_contract_test .......   Passed    0.00 sec

100% tests passed, 0 tests failed out of 1
Total Test time (real) =   0.00 sec
```

1/1 PASS。

## 6. dependency guard 结果

```
$ grep -nEi 'cuda|hip|maca|musa|libnvm|nvme|grpc|yaml|backends/|io_engine/|device_manager/' \
  /data/home/ryeqiu/Tutti/tutti/include/tutti/status.h

(exit code 1, no output)
```

无输出，PASS。

## 7. 文件边界与空白检查结果

### git diff --check

```
$ git diff --check -- tutti/include/tutti/status.h

(no output, exit code 0)
```

### 尾随空白 + EOF newline 检查（全部新增文件）

```
tutti/include/tutti/status.h                    — PASS
tests/status_contract/CMakeLists.txt            — PASS
tests/status_contract/status_contract_test.cpp  — PASS
```

### 文件边界

```
$ git status --short -- tutti/include/tutti/status.h tests/status_contract/ chat/round2/result3.md

?? tests/status_contract/
?? tutti/include/tutti/status.h
```

仅触碰允许列表内文件。`chat/round2/result3.md` 为本文件（写入后出现）。未修改根或 `tutti/` 的任何现有 CMakeLists.txt、cuda_like.h、io_types.h 或其他禁止文件。

## 8. 最终结论

```
PASS
```

全部 9 项成功标准均满足：

1. StatusCode 包含且只围绕 12 个稳定分类。
2. Status 明确区分 OK/错误并保留 message。
3. Result\<T\> 无非法 OK-without-value 状态（归一为 INTERNAL）。
4. move-only value contract 通过（`Result<std::unique_ptr<int>>` 构造 + move-out）。
5. 头文件完全 hardware-free、header-only、C++17。
6. Standalone configure/build/ctest 通过（1/1 PASS）。
7. 未修改允许列表外文件。
8. 未执行任何模块、daemon 或 IO 操作。
9. 空白检查通过。

## 总指挥验收

验收结论：`PASS`。

独立核验结果：

- `StatusCode` 完整包含规范要求的 12 个稳定分类，没有引入设备或传输私有码。
- `Status` 默认及 `Status::Ok()` 均为成功状态；非 OK code/message 保持值语义。
- `Result<T>` 使用 `std::optional<T>` 表达值存在性，成功与失败状态互斥；OK-without-value 会确定性归一为 `INTERNAL`。
- `Result<int>`、`Result<std::string>` 和 move-only `Result<std::unique_ptr<int>>` contract 均有测试覆盖。
- 公共头只依赖 C++ 标准库；hardware/private dependency guard 无匹配。
- `tutti_status_contract_test` 二进制存在，CTest 日志和 cost data 均确认测试执行并通过。
- `status.h` 与 `io_types.h` 的组合语法检查通过，`Result<IoHandle>` 可用于后续 Runtime contract。
- 文件边界、尾随空白、EOF newline、IDE/linter 检查全部通过。
- 未执行模块、daemon、block device 或硬件 IO 操作。

后续决定：T-006 完成，不需要返工。
