# TASK T-005

你是一名资深 CMake/C++ 公共接口工程师。你只负责为 Tutti 新目标架构建立最小 `tutti_api` 公共 usage-requirements target，并用 HOST profile 证明消费者无需手工添加 include、profile 宏或 vendor 依赖。你看不到任何其他上下文，本 prompt 已包含完整背景、边界和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

主要重构子工程：

`/data/home/ryeqiu/Tutti/tutti`

# 已完成基线

前一轮已经建立：

- `TUTTI_ACCELERATOR=CUDA|HOST`
- `tutti_cuda_like`：`INTERFACE` target
- `tutti/cuda_like.h`
- CUDA profile：传播 `TUTTI_USE_CUDA=1`，链接 CUDA runtime/driver
- HOST profile：传播 `TUTTI_USE_HOST=1`，不查找 CUDA、gRPC、yaml-cpp、libnvm
- `tests/cuda_like/cuda_like_contract_test`

当前 `tutti/CMakeLists.txt` 中已有 `tutti_cuda_like` 和 `tutti_types`，但尚无目标架构要求的公共 target：

```text
tutti_api
```

# 任务目标

新增最小 `tutti_api` target，使今后的公共 API consumer 只链接 `tutti_api` 即可获得：

1. `/data/home/ryeqiu/Tutti/tutti/include` 下的公共头路径；
2. 当前 profile 的 `TUTTI_USE_<PROFILE>` definition；
3. 当前 profile 的 CUDA-like 编译/链接 usage requirements；
4. 正确的 build/install include interface。

`tutti_api` 本轮只承载公共头和 usage requirements，不包含源码，不实现 `StorageRuntime`、`DataPath`、Resolver 或任何数据面。

# 冻结的 target 契约

Target 名称固定：

```cmake
tutti_api
```

类型固定：

```cmake
INTERFACE library
```

必须满足：

```text
tutti_api
  INTERFACE include: tutti/include（build tree）
  INTERFACE include: include（install tree）
  INTERFACE link: tutti_cuda_like
```

关键约束：

- `tutti_api` 不得再次定义 `TUTTI_USE_CUDA` 或 `TUTTI_USE_HOST`；必须通过链接 `tutti_cuda_like` 继承。
- `tutti_api` 不得直接链接 CUDA target；vendor 依赖只由 `tutti_cuda_like` 传播。
- 不得传播旧 `backends/`、`io_engine/`、`device_manager/`、libnvm、NVMeService 或内核头路径。
- 不得加入源码或变成 `STATIC`/`SHARED` library。
- 必须加入现有 `tutti_targets` install export 集合。
- 不删除或重命名 `tutti_cuda_like`、`tutti_types` 或现有测试 target。
- 不建立 package config、version config 或稳定 binary ABI；这些不属于本任务。

# 测试目标

新增根目录测试：

```text
/data/home/ryeqiu/Tutti/tests/public_api/
```

测试 target 名固定：

```cmake
tutti_public_api_usage_test
```

CTest 名固定：

```cmake
tutti_public_api_usage_test
```

测试必须：

1. 只链接 `tutti_api`；
2. 不调用 `target_include_directories()`；
3. 不调用 `target_compile_definitions()`；
4. 不直接链接 `tutti_cuda_like` 或 CUDA target；
5. 源码只 include：

```cpp
#include <tutti/cuda_like.h>
```

以及标准库头；
6. 在 HOST profile 下编译期确认只定义 `TUTTI_USE_HOST`，未定义 `TUTTI_USE_CUDA`；
7. 至少调用一个通过统一头提供的 CUDA-like API，例如 `cudaGetDeviceCount()`，确认 usage requirements 真正传递；
8. 不使用 GTest 或第三方测试库；
9. 成功打印清晰 PASS 并返回 0。

`tutti/CMakeLists.txt` 在 `BUILD_TESTING=ON` 时必须以独立 binary dir 添加该测试目录，不能破坏已有 `tests/cuda_like`。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/public_api/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/public_api/public_api_usage_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round2/result2.md`

其中 `chat/round2/result2.md` 只用于保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round2-session2/`

该目录已位于仓库既有 ignored `build/` 下，不计入源码文件允许列表。

禁止修改或创建任何其他文件。尤其禁止修改：

- `/data/home/ryeqiu/Tutti/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tutti/cmake/accelerators/**`
- `/data/home/ryeqiu/Tutti/tutti/include/**`
- `/data/home/ryeqiu/Tutti/tests/cuda_like/**`
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round2/result2.md` 外的任何文件
- 任意 accelerator、hardware stack、libnvm、NVMeService 或 kernel module 源码

禁止提交 Git commit。

# 实现要求

1. 先读取当前 `tutti/CMakeLists.txt`，在现有 `tutti_cuda_like` 附近做最小改动。
2. 使用 generator expressions 区分 build/install include：

```cmake
$<BUILD_INTERFACE:...>
$<INSTALL_INTERFACE:...>
```

3. 使用 target-scoped CMake 命令。
4. 不使用：

```cmake
add_definitions(...)
include_directories(...)
link_directories(...)
```

5. 不整理、重排或重写 `tutti/CMakeLists.txt` 的无关内容。
6. 不“顺手”修复其他 legacy CMake 问题。
7. 如果发现现有 `tutti_cuda_like` install interface 有独立缺口，只记录，不得越过允许文件去修改 profile module。

# 安全限制

绝对禁止执行：

```text
insmod
rmmod
modprobe
sudo
make insmod
make rmmod
```

也禁止：

- 启动 daemon/client
- 访问或写入 `/dev/nvme*`
- mount/umount/格式化/分区
- 运行任何硬件 IO
- 修改内核模块状态

本任务只能运行 hardware-free HOST profile 的 configure/build/test。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 清理本 session 专用 build 目录

只允许清理：

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round2-session2
```

禁止删除其他 build 目录。

## 2. HOST configure

```bash
cmake -S /data/home/ryeqiu/Tutti/tutti \
  -B /data/home/ryeqiu/Tutti/build/round2-session2 \
  -DTUTTI_ACCELERATOR=HOST \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

要求：

- configure 成功；
- `TUTTI_BUILD_HARDWARE_STACK=OFF`；
- 不查找或启用 CUDA、gRPC、yaml-cpp、libnvm；
- `tutti_api` 和 `tutti_public_api_usage_test` target 存在；
- 已有 `cuda_like_contract_test` 仍存在。

## 3. 构建并运行新测试

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round2-session2 \
  --target tutti_public_api_usage_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round2-session2 \
  --output-on-failure \
  -R '^tutti_public_api_usage_test$'
```

要求：1/1 PASS。

## 4. 回归现有 CUDA-like contract

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round2-session2 \
  --target cuda_like_contract_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round2-session2 \
  --output-on-failure \
  -R '^cuda_like_contract_test$'
```

要求：1/1 PASS。

## 5. CMake 和文件边界检查

```bash
git diff --check -- tutti/CMakeLists.txt

grep -RInE 'add_definitions|(^|[^[:alnum:]_])include_directories[[:space:]]*\(' \
  tests/public_api
```

第二条命令应无输出；`target_include_directories` 不算违规，但本测试 target 本身也不应需要它。

再确认源码改动只在允许列表中。不要把其他 worker 已存在的未提交文件误算成你的改动；记录本 session 实际触碰的文件即可。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. `tutti_api` 是 `INTERFACE` target。
2. `tutti_api` 通过 `INTERFACE` link 继承 `tutti_cuda_like`。
3. build/install include interface 正确。
4. `tutti_api` 加入现有 `tutti_targets` export。
5. 新测试仅链接 `tutti_api`，没有手工 include/profile definition。
6. HOST configure 不引入任何 GPU SDK 或 hardware stack。
7. 新测试和已有 `cuda_like_contract_test` 均通过。
8. 未修改允许列表外的源码文件。
9. 未执行任何模块、daemon 或 IO 操作。
10. 空白检查通过。

如失败，写 `BLOCKED` 并记录首个真实错误；禁止扩大修改范围。

# 结果落盘要求

完成任务和验收后，必须把本 session 的完整原始结果直接写入：

`/data/home/ryeqiu/Tutti/chat/round2/result2.md`

至少包含：

1. 修改/新增文件列表
2. `tutti_api` 实现摘要
3. 测试如何证明 usage requirements 传递
4. HOST configure 完整结果摘要
5. 新测试 build/ctest 结果
6. 现有 CUDA-like contract 回归结果
7. CMake 禁用命令与文件边界检查结果
8. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 不得预留或编写“总指挥验收”内容；总指挥会在你结束后追加到文件末尾。
- 新文件尾随空白必须自行检查。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round2/result2.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
