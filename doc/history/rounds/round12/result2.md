# Round 12 Session 2 Result: CUDA-like profile 契约完善与唯一 profile 证明

## 概述

补齐 `cuda_like_contract_test` 覆盖到 Roadmap Phase 6 点名范围（allocation / pointer / stream+event / copy / context），新增 10 个测试函数（共 20 个）。HOST shim 补齐 `cudaDeviceSynchronize`/`cudaStreamQuery`/`cudaGetDeviceProperties`。以 configure + compile_commands.json 证据链证明恰好一个 profile 生效、未选中 SDK 零参与。

## 改动文件清单

| 文件 | 改动 |
|------|------|
| `tutti/include/tutti/gpu_vendor/host.h` | 新增 `cudaDeviceSynchronize`、`cudaStreamQuery`、`cudaDeviceProp` + `cudaGetDeviceProperties` |
| `tests/cuda_like/cuda_like_contract.cpp` | 新增 10 个测试函数（11-20），覆盖 pointer attrs / device sync / stream query / D2D copy / async memset / host register / last error / create with flags / device properties / profile macro |
| `chat/round12/result2.md` | 本文件 |

## 契约覆盖审计表

Roadmap Phase 6 点名：「allocation, pointer, stream/event, copy/context」

| 点名项 | API | 测试函数 | HOST shim | CUDA | 状态 |
|--------|-----|---------|-----------|------|------|
| **allocation** | cudaMalloc / cudaFree | test_malloc_free (3) | ✅ | ✅ | 已有 |
| | cudaMallocHost / cudaFreeHost | test_malloc_host_free_host (4) | ✅ | ✅ | 已有 |
| | cudaHostRegister / Unregister | test_host_register (16) | ✅ (NotSupported) | ✅ | **新增** |
| **pointer** | cudaPointerGetAttributes | test_pointer_attributes (11) | ✅ | ✅ | **新增** |
| **stream/event** | cudaStreamCreate/Destroy/Sync | test_stream (5) | ✅ | ✅ | 已有 |
| | cudaStreamQuery | test_stream_query (13) | ✅ **新增** | ✅ | **新增** |
| | cudaEventCreate/Record/Sync/Query/Destroy | test_event (6) | ✅ | ✅ | 已有 |
| | cudaStreamWaitEvent | test_event (6) | ✅ | ✅ | 已有 |
| | cudaStreamCreateWithFlags | test_create_with_flags (18) | ✅ | ✅ | **新增** |
| | cudaEventCreateWithFlags | test_create_with_flags (18) | ✅ | ✅ | **新增** |
| **copy** | cudaMemcpy (sync, H2D/D2H) | test_memset (8), test_null_errors (9) | ✅ | ✅ | 已有 |
| | cudaMemcpyAsync (H2D/D2H) | test_async_round_trip (7) | ✅ | ✅ | 已有 |
| | cudaMemcpy D2D | test_d2d_copy (14) | ✅ | ✅ | **新增** |
| | cudaMemsetAsync | test_memset_async (15) | ✅ | ✅ | **新增** |
| **context** | cudaGetDeviceCount | test_device_count (1) | ✅ | ✅ | 已有 |
| | cudaSetDevice / cudaGetDevice | test_set_get_device (2) | ✅ | ✅ | 已有 |
| | cudaDeviceSynchronize | test_device_synchronize (12) | ✅ **新增** | ✅ | **新增** |
| | cudaGetDeviceProperties | test_device_properties (19) | ✅ **新增** | ✅ | **新增** |
| | cudaGetLastError / PeekAtLastError | test_last_error (17) | ✅ | ✅ | **新增** |
| | cudaGetErrorString | test_error_string (10) | ✅ | ✅ | 已有 |
| **profile** | exactly-one TUTTI_USE_<PROFILE> | test_profile_macro (20) | ✅ | ✅ | **新增** |

### 缺口修复前 vs 修复后

| 维度 | 修复前 | 修复后 |
|------|--------|--------|
| 测试函数数 | 10 | 20 |
| HOST shim API 数 | 22 | 25 (+cudaDeviceSynchronize, +cudaStreamQuery, +cudaGetDeviceProperties) |
| pointer 属性查询 | ❌ 未测 | ✅ test 11 |
| device context sync | ❌ 未测 | ✅ test 12 |
| stream query | ❌ 未测, shim 缺 | ✅ test 13, shim 补 |
| D2D copy | ❌ 未测 | ✅ test 14 |
| async memset | ❌ 未测 | ✅ test 15 |
| host register | ❌ 未测 | ✅ test 16 |
| last error | ❌ 未测 | ✅ test 17 |
| create with flags | ❌ 未测 | ✅ test 18 |
| device properties | ❌ 未测, shim 缺 | ✅ test 19, shim 补 |
| profile macro | ❌ 未测 | ✅ test 20 |

## 唯一 profile 证明

### 1. Configure 期：非法值失败

```
$ cmake -S tutti -B /tmp/tutti-invalid -DTUTTI_ACCELERATOR=BOGUS 2>&1 | grep error
CMake Error at CMakeLists.txt:19 (message):
-- Configuring incomplete, errors occurred!
```

CMakeLists.txt 第 18-22 行：
```cmake
if(NOT TUTTI_ACCELERATOR MATCHES "^(CUDA|HOST)$")
    message(FATAL_ERROR
        "TUTTI_ACCELERATOR='${TUTTI_ACCELERATOR}' is not supported. "
        "Supported values: CUDA, HOST")
endif()
```

### 2. 编译期：cuda_like.h #error 守卫

```cpp
#if defined(TUTTI_USE_CUDA) && defined(TUTTI_USE_HOST)
#error "Both TUTTI_USE_CUDA and TUTTI_USE_HOST are defined; exactly one must be set"
#endif
#if !defined(TUTTI_USE_CUDA) && !defined(TUTTI_USE_HOST)
#error "Neither TUTTI_USE_CUDA nor TUTTI_USE_HOST is defined; exactly one must be set"
#endif
```

### 3. 测试期：test_profile_macro (20) static_assert

```cpp
#if defined(TUTTI_USE_CUDA) && defined(TUTTI_USE_HOST)
    #error "Both TUTTI_USE_CUDA and TUTTI_USE_HOST defined — contract violation"
#endif
#if !defined(TUTTI_USE_CUDA) && !defined(TUTTI_USE_HOST)
    #error "Neither TUTTI_USE_CUDA nor TUTTI_USE_HOST defined — contract violation"
#endif
```

### 4. compile_commands.json 证据

| Profile | TUTTI_USE_CUDA 出现次数 | TUTTI_USE_HOST 出现次数 |
|---------|----------------------|----------------------|
| HOST | 0 | 1 (per target, via `-DTUTTI_USE_HOST=1`) |
| CUDA | 49 | 0 |

**结论**：恰好一个 profile 宏被定义。

## 未选中 SDK 零参与证明

### HOST profile（TUTTI_ACCELERATOR=HOST）

```
$ grep -c 'TUTTI_USE_CUDA' /tmp/tutti-r12s2-host/compile_commands.json
0

$ grep -oP '\-I[^ ]*' /tmp/tutti-r12s2-host/compile_commands.json | grep -i cuda
(none — no CUDA SDK include paths)

$ grep -c 'cudart\|cuda_driver\|CUDA::' /tmp/tutti-r12s2-host/compile_commands.json
0
```

compile_commands.json 中唯一的 "cuda" 出现来自测试文件名 `cuda_like_contract.cpp` 和目录名 `tests_cuda_like` — 这是测试本身的名称（测试 CUDA-like API 抽象），不是 CUDA SDK 引用。

CUDA profile 的 `find_package(CUDAToolkit)` 在 HOST profile 下不执行（`CUDA.cmake` 未被 include）。

### CUDA profile（TUTTI_ACCELERATOR=CUDA）

```
$ grep -c 'gpu_vendor/host' tutti/build-profile-cuda/compile_commands.json
0

$ grep -c 'TUTTI_USE_HOST' tutti/build-profile-cuda/compile_commands.json
0
```

HOST shim 头文件 `tutti/include/tutti/gpu_vendor/host.h` 在 CUDA profile 的编译命令中不出现。`cuda_like.h` 在 `TUTTI_USE_CUDA` 下直接 include `<cuda.h>` + `<cuda_runtime.h>`，不 include HOST shim。

## 测试结果

### HOST profile

```
$ cmake -S tutti -B /tmp/tutti-r12s2-host -DBUILD_TESTING=ON -DTUTTI_ACCELERATOR=HOST
$ cmake --build /tmp/tutti-r12s2-host
$ cd /tmp/tutti-r12s2-host && ctest

100% tests passed, 0 tests failed out of 12
Total Test time = 0.03 sec
```

cuda_like_contract_test 输出：
```
cudaGetErrorString(cudaSuccess) = "success"
cudaGetErrorString(cudaErrorInvalidValue) = "invalid value"
  dev_ptr attrs: type=1 device=0
  host_ptr attrs: type=1 device=0
  cudaHostRegister result: 2 (not supported)
  device: "Tutti HOST shim" major=0 minor=0 SMs=0
  active profile: HOST (TUTTI_USE_HOST=1)
RESULT: all checks passed
```

### CUDA profile

```
$ cmake -S tutti -B tutti/build-profile-cuda -DTUTTI_ACCELERATOR=CUDA
$ cmake --build .
$ ctest -E 'hardware|RealHw|Ipc'

100% tests passed, 0 tests failed out of 112
Total Test time = 17.44 sec
```

cuda_like_contract_test 输出：
```
cudaGetErrorString(cudaSuccess) = "no error"
cudaGetErrorString(cudaErrorInvalidValue) = "invalid argument"
  dev_ptr attrs: type=2 device=0
  host_ptr attrs: type=1 device=0
  cudaHostRegister result: 0 (no error)
  device: "NVIDIA H20" major=9 minor=0 SMs=78
  active profile: CUDA (TUTTI_USE_CUDA=1)
RESULT: all checks passed
```

### Profile 差异验证

| 断言 | HOST | CUDA |
|------|------|------|
| cudaPointerGetAttributes(dev_ptr).type | cudaMemoryTypeHost (1) | cudaMemoryTypeDevice (2) |
| cudaHostRegister(malloc'd ptr) | cudaErrorNotSupported (2) | cudaSuccess (0) |
| device name | "Tutti HOST shim" | "NVIDIA H20" |
| TUTTI_USE_HOST | 1 | (undefined) |
| TUTTI_USE_CUDA | (undefined) | 1 |

## Profile 扩展指南

新增一个 accelerator profile（如 MACA）需要：

### 1. 创建 shim 头文件

`tutti/include/tutti/gpu_vendor/maca.h` — header-only inline 实现，提供与 `host.h` 相同的 API 面（cudaError_t、cudaStream_t、cudaMalloc/Free、cudaMemcpy/Async、cudaStream/Event 全套、cudaPointerGetAttributes、cudaDeviceSynchronize 等）。

### 2. 创建 CMake profile 模块

`tutti/cmake/accelerators/MACA.cmake`：
```cmake
function(tutti_configure_cuda_like target_name)
    find_package(MACA REQUIRED)  # 或对应 SDK
    target_compile_definitions(${target_name} INTERFACE TUTTI_USE_MACA=1)
    target_include_directories(${target_name} INTERFACE
        "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(${target_name} INTERFACE MACA::runtime)
endfunction()
```

### 3. 更新 cuda_like.h selector

在 `tutti/include/tutti/cuda_like.h` 的条件链中添加：
```cpp
#elif defined(TUTTI_USE_MACA)
#include <tutti/gpu_vendor/maca.h>
```

### 4. 更新 CMakeLists.txt profile 验证

```cmake
if(NOT TUTTI_ACCELERATOR MATCHES "^(CUDA|HOST|MACA)$")
    message(FATAL_ERROR "...")
endif()
```

### 5. 注册 profile（一行）

在 `tutti/CMakeLists.txt` 中，`include(cmake/accelerators/${TUTTI_ACCELERATOR}.cmake)` 已自动按变量展开，无需修改。但 `project()` 语言和 CUDA arch 设置需要条件分支。

### 6. 运行契约测试

`cuda_like_contract_test` 自动适配新 profile（通过 `cuda_like.h` selector）。`test_profile_macro` 的 `#if defined(TUTTI_USE_MACA)` 分支需要添加（当前预留 `#error` 占位）。

**总计**：2 个新文件 + 3 处既有文件修改 + 1 行 profile 注册。

## 总指挥验收（2026-08-02）

**PASS。** 独立核验：

- 契约覆盖审计表与代码一致（20 个测试函数，HOST shim +3 API）；`gpu_vendor/host.h` 的扩展属 prompt 允许的「契约缺口+测试先行」例外，诊断 0。
- 唯一 profile：`TUTTI_ACCELERATOR=BOGUS` configure 明确失败（复验）；HOST build 的 compile_commands 中 `TUTTI_USE_CUDA` 0 次、CUDA include 路径 0 条（复验一致）。
- profile 差异断言（pointer type、hostRegister 行为、device name、profile 宏）HOST/CUDA 两侧输出与报告一致。
- 复跑：HOST `13/13`、CUDA `133/133`；硬件基线 735/0 + 115/0 无影响。
- 扩展指南（2 新文件 + 3 处修改 + 1 行注册）与实际 CMake 结构核对一致。
