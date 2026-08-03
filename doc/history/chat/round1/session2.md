# TASK T-002

你是一名资深 CMake/CUDA 基础设施工程师，只负责建立 Tutti 第一阶段的 NVIDIA-first CUDA-like 构建 profile。你看不到任何其他上下文，本 prompt 已包含完整接口契约。

# 项目位置

`/data/home/ryeqiu/Tutti`

当前主要重构子工程：

`/data/home/ryeqiu/Tutti/tutti`

# 任务目标

实现最小 Phase 0 构建基线：

1. 默认面向 NVIDIA CUDA。
2. 提供 `TUTTI_ACCELERATOR=CUDA|HOST`。
3. CUDA profile 直接使用 NVIDIA CUDA headers/runtime。
4. HOST profile 不查找 CUDA、gRPC、yaml-cpp、libnvm 或任何硬件依赖。
5. 提供统一 `tutti/cuda_like.h`。
6. 新测试全部位于根目录 `/data/home/ryeqiu/Tutti/tests`。
7. 保持当前 CUDA production stack 默认可配置，不重构业务源码。
8. 不修改内核模块。

MACA/MUSA 本任务只在 selector 中预留编译分支和错误提示，不实现实际 vendor shim。

# 你只能修改或创建这些文件

- `/data/home/ryeqiu/Tutti/tutti/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tutti/build.sh`
- `/data/home/ryeqiu/Tutti/tutti/tests/io_engine/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tutti/cmake/accelerators/CUDA.cmake`
- `/data/home/ryeqiu/Tutti/tutti/cmake/accelerators/HOST.cmake`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/cuda_like.h`
- `/data/home/ryeqiu/Tutti/tutti/include/tutti/gpu_vendor/host.h`
- `/data/home/ryeqiu/Tutti/tests/cuda_like/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/cuda_like/cuda_like_contract.cpp`

禁止修改任何其他文件。

尤其禁止修改：

- `Roadmap.md`
- `README.md`
- `TUTTI_TARGET_ARCHITECTURE.md`
- `TUTTI_REFACTOR_TAKEOVER.md`
- `tutti/accel/CMakeLists.txt`
- `tutti/device_manager/CMakeLists.txt`
- `tutti/backends/**`
- `tutti/io_engine/CMakeLists.txt`
- 任意 kernel/libnvm/NVMeService 源码
- `doc/history/README.md`

禁止提交 Git commit。

# 冻结的构建接口

## Cache variable

```cmake
TUTTI_ACCELERATOR
```

类型：`STRING`

默认值：

```text
CUDA
```

本任务仅接受：

```text
CUDA
HOST
```

输入必须转成大写。未知值必须 `FATAL_ERROR`，错误信息中列出支持值。

## 冻结 target 名称

```cmake
tutti_cuda_like
```

类型：

```cmake
INTERFACE library
```

行为：

### CUDA profile

- 定义 `TUTTI_USE_CUDA=1`
- 传播 `/data/home/ryeqiu/Tutti/tutti/include`
- link：
  - `CUDA::cudart`
  - `CUDA::cuda_driver`

### HOST profile

- 定义 `TUTTI_USE_HOST=1`
- 传播 `/data/home/ryeqiu/Tutti/tutti/include`
- 不查找、不链接任何 CUDA 组件

## Profile module 固定函数

两个 profile 文件都必须定义：

```cmake
function(tutti_configure_cuda_like target_name)
```

CUDA 版本配置给定 target 的 CUDA definition/link requirements。

HOST 版本只配置 `TUTTI_USE_HOST=1`。

根 `tutti/CMakeLists.txt` 使用：

```cmake
include(cmake/accelerators/${TUTTI_ACCELERATOR}.cmake)
tutti_configure_cuda_like(tutti_cuda_like)
```

禁止更改函数名和 target 名。

## Hardware stack gate

定义：

```cmake
TUTTI_BUILD_HARDWARE_STACK
```

规则：

- CUDA profile：默认 `ON`
- HOST profile：强制 `OFF`
- HOST profile 如果用户显式请求 `ON`，必须报清晰错误或强制 OFF 并打印明确状态消息
- 只有 hardware stack ON 时才允许：
  - 查找 CUDAToolkit
  - 查找 gRPC/yaml-cpp
  - `add_subdirectory(accel)`
  - `add_subdirectory(device_manager)`
  - `add_subdirectory(backends)`
  - `add_subdirectory(io_engine)`
  - 添加现有硬件相关 tests

HOST profile 只能构建：

- `tutti_types`
- `tutti_cuda_like`
- 根目录 `tests/cuda_like`
- 后续纯 CPU contract targets；本任务不额外接入旧 tests

## `project()` 要求

不得继续无条件：

```cmake
project(Tutti LANGUAGES C CXX CUDA)
```

改为：

- CUDA profile：启用 `C CXX CUDA`
- HOST profile：只启用 `C CXX`

`TUTTI_ACCELERATOR` 必须在 `project()` 前完成 cache 定义和合法性检查。

## Usage requirements

`tutti_types` 必须：

```cmake
target_link_libraries(tutti_types INTERFACE tutti_cuda_like)
```

这样当前公共类型消费者能拿到 profile definition/include。

安装要求：

- `tutti_cuda_like` 加入现有 `tutti_targets` export
- 安装 `tutti/include/tutti/` 到 `include/tutti/`

# 冻结的 `cuda_like.h` 契约

文件：

`/data/home/ryeqiu/Tutti/tutti/include/tutti/cuda_like.h`

必须：

```cpp
#pragma once
```

必须在预处理期检查：

- `TUTTI_USE_CUDA` 与 `TUTTI_USE_HOST` 恰好定义一个
- 两个都定义：`#error`
- 两个都没定义：`#error`

CUDA 分支：

```cpp
#include <cuda.h>
#include <cuda_runtime.h>
```

HOST 分支：

```cpp
#include <tutti/gpu_vendor/host.h>
```

预留但本任务不实现：

```cpp
#elif defined(TUTTI_USE_MACA)
#error "TUTTI_USE_MACA profile is declared but its shim is not implemented"
#elif defined(TUTTI_USE_MUSA)
#error "TUTTI_USE_MUSA profile is declared but its shim is not implemented"
```

不得在 selector 中加入 NVMe、libnvm、PRP 或 framework 类型。

# HOST shim 最小 contract

文件：

`/data/home/ryeqiu/Tutti/tutti/include/tutti/gpu_vendor/host.h`

用途仅为 hardware-free contract tests，不是生产 fallback。

必须提供全局 CUDA 风格名称，至少包括：

## 类型和常量

```cpp
cudaError_t
cudaStream_t
cudaEvent_t
cudaMemcpyKind
cudaPointerAttributes

cudaSuccess
cudaErrorInvalidValue
cudaErrorNotSupported
cudaErrorNotReady

cudaMemcpyHostToHost
cudaMemcpyHostToDevice
cudaMemcpyDeviceToHost
cudaMemcpyDeviceToDevice
cudaMemcpyDefault

cudaStreamDefault
cudaStreamNonBlocking
cudaEventDefault
cudaEventDisableTiming
```

## 函数

```cpp
cudaGetDeviceCount
cudaSetDevice
cudaGetDevice
cudaMalloc
cudaFree
cudaMallocHost
cudaFreeHost
cudaHostRegister
cudaHostUnregister
cudaPointerGetAttributes
cudaStreamCreate
cudaStreamCreateWithFlags
cudaStreamDestroy
cudaStreamSynchronize
cudaEventCreate
cudaEventCreateWithFlags
cudaEventDestroy
cudaEventRecord
cudaEventSynchronize
cudaEventQuery
cudaStreamWaitEvent
cudaMemcpy
cudaMemcpyAsync
cudaMemset
cudaMemsetAsync
cudaGetErrorString
cudaGetLastError
cudaPeekAtLastError
```

HOST 语义：

- allocation 使用 `std::malloc/std::free`
- copy 使用 `std::memcpy`
- memset 使用 `std::memset`
- stream/event 可以是轻量 heap token
- 所有操作同步完成
- 非法空参数返回 `cudaErrorInvalidValue`
- 不支持但合法的能力返回 `cudaErrorNotSupported`
- 禁止静默返回成功掩盖非法参数
- header-only `inline`
- 不新增第三方依赖

# Contract test

文件：

`/data/home/ryeqiu/Tutti/tests/cuda_like/cuda_like_contract.cpp`

必须只 include：

```cpp
#include <tutti/cuda_like.h>
```

以及标准库头。

必须覆盖：

1. `cudaGetDeviceCount`
2. set/get device
3. `cudaMalloc` / `cudaFree`
4. `cudaMallocHost` / `cudaFreeHost`
5. stream create/destroy
6. event create/record/wait/query/destroy
7. async H2D、D2H round trip
8. memset
9. 非法空参数错误
10. `cudaGetErrorString`

测试不能使用 GTest。

如果 CUDA profile 下没有 GPU：

- 打印明确 `SKIP: no CUDA device`
- 返回 0

如果有 GPU，必须实际验证 round trip。

HOST profile 必须完整运行，不得 skip。

Target 名称固定：

```cmake
cuda_like_contract_test
```

必须注册：

```cmake
add_test(NAME cuda_like_contract_test COMMAND cuda_like_contract_test)
```

根 `tutti/CMakeLists.txt` 在 `BUILD_TESTING=ON` 时以独立 binary dir 添加：

```cmake
add_subdirectory(
  "${CMAKE_CURRENT_SOURCE_DIR}/../tests/cuda_like"
  "${CMAKE_CURRENT_BINARY_DIR}/tests_cuda_like"
)
```

# 修复现有测试配置阻断

当前：

`tutti/tests/io_engine/CMakeLists.txt`

无条件引用已经删除的：

```text
tutti/io_engine/tests/layer4_smoke_test.cpp
```

只能做最小修复：

- 仅当该源文件存在时才创建 `layer4_smoke_test`
- 不恢复该测试源
- 不修改其他测试语义

# build.sh

保持现有参数兼容，并增加环境变量：

```text
TUTTI_ACCELERATOR
```

默认 CUDA。

示例：

```bash
TUTTI_ACCELERATOR=HOST ./tutti/build.sh -j 8
TUTTI_ACCELERATOR=CUDA ./tutti/build.sh -j 8
```

脚本必须把该值传给 CMake。

禁止在脚本中运行 `insmod/rmmod`、sudo 或硬件 IO。

# CMake 风格

- 不使用仓库级 `add_definitions`
- 新 profile 定义必须 target-scoped
- 新 include 必须 target-scoped
- 不新增全局编译 flag
- 不删除现有 production target
- 不重构现有业务 target
- 最小外科手术式修改

# 验收标准

## HOST profile

必须执行并通过：

```bash
cd /data/home/ryeqiu/Tutti
rm -rf tutti/build-profile-host
cmake -S tutti -B tutti/build-profile-host \
  -DTUTTI_ACCELERATOR=HOST \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build tutti/build-profile-host --target cuda_like_contract_test -j8
ctest --test-dir tutti/build-profile-host \
  --output-on-failure \
  -R '^cuda_like_contract_test$'
```

配置日志中不得出现：

```text
Found CUDA
CUDAToolkit
gRPC
yaml-cpp
libnvm
```

## CUDA profile

必须至少执行：

```bash
cd /data/home/ryeqiu/Tutti
rm -rf tutti/build-profile-cuda
cmake -S tutti -B tutti/build-profile-cuda \
  -DTUTTI_ACCELERATOR=CUDA \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build tutti/build-profile-cuda --target cuda_like_contract_test -j8
ctest --test-dir tutti/build-profile-cuda \
  --output-on-failure \
  -R '^cuda_like_contract_test$'
```

不要因为当前其他 legacy target 的编译问题扩大改动范围。

## 额外检查

```bash
cd /data/home/ryeqiu/Tutti
git diff --check -- \
  tutti/CMakeLists.txt \
  tutti/build.sh \
  tutti/tests/io_engine/CMakeLists.txt \
  tutti/cmake/accelerators \
  tutti/include/tutti \
  tests/cuda_like
grep -RInE 'add_definitions|include_directories\\(' \
  tutti/cmake/accelerators tests/cuda_like
```

新文件中第二个 grep 应无结果，`target_include_directories` 不算违规。

# 输出要求

只返回一个 Markdown 代码块，包含：

1. 修改/新增文件列表
2. 每个文件的关键实现摘要
3. HOST profile 完整 configure/build/ctest 结果
4. CUDA profile 完整 configure/build/ctest 结果
5. `git diff --check` 结果
6. 如某项失败，写 `BLOCKED`、完整错误和你确认的最小原因

不要解释、不要寒暄、不要提交 Git commit、不要输出或修改其他文件。
