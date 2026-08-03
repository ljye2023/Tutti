# T-005 Session Result

## 1. 修改/新增文件列表

| 文件 | 操作 |
|------|------|
| `tutti/CMakeLists.txt` | 修改（新增 `tutti_api` target、`tests/public_api` 子目录、install export） |
| `tests/public_api/CMakeLists.txt` | 新增 |
| `tests/public_api/public_api_usage_test.cpp` | 新增 |

## 2. `tutti_api` 实现摘要

在 `tutti/CMakeLists.txt` 中 `tutti_cuda_like` 之后新增：

```cmake
add_library(tutti_api INTERFACE)
target_include_directories(tutti_api INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(tutti_api INTERFACE tutti_cuda_like)
```

- **类型**：`INTERFACE` library，无源码
- **include**：build tree 传播 `tutti/include`，install tree 传播 `include`
- **link**：通过 `INTERFACE` link 继承 `tutti_cuda_like`，不直接定义 `TUTTI_USE_*`，不直接链接 CUDA target
- **install**：加入现有 `tutti_targets` export 集合
- **不传播**：backends、io_engine、device_manager、libnvm、NVMeService、内核头路径

## 3. 测试如何证明 usage requirements 传递

`tutti_public_api_usage_test` 的 CMake 配置仅做：

```cmake
target_link_libraries(tutti_public_api_usage_test PRIVATE tutti_api)
```

- 未调用 `target_include_directories()`
- 未调用 `target_compile_definitions()`
- 未直接链接 `tutti_cuda_like` 或 CUDA target

源码仅 include `<tutti/cuda_like.h>` 和 `<cstdio>`，编译期验证：

1. `TUTTI_USE_CUDA` 与 `TUTTI_USE_HOST` 恰好定义一个（`#error` 保证）
2. HOST profile 下 `TUTTI_USE_HOST` 已定义、`TUTTI_USE_CUDA` 未定义
3. 运行期调用 `cudaGetDeviceCount()`，证明 API 通过 `tutti_api` → `tutti_cuda_like` → `cuda_like.h` 链路可用

若编译/链接成功，则 include 路径、profile 宏、CUDA-like API 均已正确传播。

## 4. HOST configure 完整结果摘要

```
-- The C compiler identification is GNU 13.1.1
-- The CXX compiler identification is GNU 13.1.1
-- Detecting C compiler ABI info - done
-- Detecting CXX compiler ABI info - done
-- Configured cuda_like_contract_test (profile-agnostic contract test)
-- Tutti: added tests/cuda_like (contract test)
-- Configured tutti_public_api_usage_test (public API usage test)
-- Tutti: added tests/public_api (public API usage test)
-- Configuring done (0.5s)
-- Generating done (0.0s)
```

- `TUTTI_BUILD_HARDWARE_STACK:BOOL=OFF` ✓
- 无 `Found CUDA`、`CUDAToolkit`、`gRPC`、`yaml-cpp`、`libnvm` ✓
- `tutti_api` target 存在 ✓
- `tutti_public_api_usage_test` target 存在 ✓
- `cuda_like_contract_test` target 仍存在 ✓

## 5. 新测试 build/ctest 结果

### Build

```
[ 50%] Building CXX object tests_public_api/CMakeFiles/tutti_public_api_usage_test.dir/public_api_usage_test.cpp.o
[100%] Linking CXX executable ../bin/tutti_public_api_usage_test
[100%] Built target tutti_public_api_usage_test
```

### CTest

```
Test project /data/home/ryeqiu/Tutti/build/round2-session2
    Start 2: tutti_public_api_usage_test
1/1 Test #2: tutti_public_api_usage_test ......   Passed    0.00 sec

100% tests passed, 0 tests failed out of 1
Total Test time (real) =  0.00 sec
```

## 6. 现有 CUDA-like contract 回归结果

### Build

```
[ 50%] Building CXX object tests_cuda_like/CMakeFiles/cuda_like_contract_test.dir/cuda_like_contract.cpp.o
[100%] Linking CXX executable ../bin/cuda_like_contract_test
[100%] Built target cuda_like_contract_test
```

### CTest

```
Test project /data/home/ryeqiu/Tutti/build/round2-session2
    Start 1: cuda_like_contract_test
1/1 Test #1: cuda_like_contract_test ..........   Passed    0.00 sec

100% tests passed, 0 tests failed out of 1
Total Test time (real) =  0.00 sec
```

## 7. CMake 禁用命令与文件边界检查结果

### git diff --check

```
git diff --check -- tutti/CMakeLists.txt
EXIT=0
```
无空白错误 ✓

### grep 禁用命令检查

```
grep -RInE 'add_definitions|(^|[^[:alnum:]_])include_directories[[:space:]]*\(' tests/public_api
GREP_EXIT=1
```
无输出（exit 1 = 无匹配） ✓

### 文件边界

本 session 实际触碰的文件：
- `tutti/CMakeLists.txt`（修改）
- `tests/public_api/CMakeLists.txt`（新增）
- `tests/public_api/public_api_usage_test.cpp`（新增）
- `chat/round2/result2.md`（新增，本文件）

未修改允许列表外的任何文件 ✓

### 安全限制

未执行 insmod/rmmod/modprobe/sudo/make insmod/make rmmod ✓
未启动 daemon/client ✓
未访问 /dev/nvme* ✓
未执行任何硬件 IO ✓

## 8. 最终结论

PASS

## 总指挥验收

验收结论：`PASS`。

独立核验结果：

- `tutti_api` 为无源码的 `INTERFACE` target，使用正确的 build/install include interface。
- `tutti_api` 只通过 `INTERFACE` link 继承 `tutti_cuda_like`，没有重复定义 profile 宏，也没有直接链接 CUDA target。
- `tutti_api` 已加入现有 `tutti_targets` export 集合。
- `tests/public_api/CMakeLists.txt` 的测试 target 只链接 `tutti_api`，未手工添加 include、compile definition 或 vendor library。
- 生成的真实编译命令包含 `-DTUTTI_USE_HOST=1` 和 `-I/data/home/ryeqiu/Tutti/tutti/include`，证明 usage requirements 已经传递。
- HOST cache 为 `TUTTI_ACCELERATOR=HOST`、`TUTTI_BUILD_HARDWARE_STACK=OFF`。
- `tutti_public_api_usage_test` 和 `cuda_like_contract_test` 二进制均存在；CTest 记录确认新测试已执行，worker 报告两项均通过。
- 新测试目录未使用 `add_definitions()` 或全局 `include_directories()`。
- `git diff --check`、新文件尾随空白和 EOF newline 检查均通过；IDE/linter 为 0 diagnostics。
- 本 session 的源码改动严格位于 `tutti/CMakeLists.txt` 与 `tests/public_api/`；未执行模块、daemon、block device 或硬件 IO 操作。

后续决定：T-005 完成，不需要返工。Session 3、4 可继续并行执行。
