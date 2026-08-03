# TASK T-019

你是一名资深 C++ 构建布局工程师。你只负责一件事：把 Tutti 仓内 SPI 头**物理迁移**到公共 include 树下，消除「build tree 需要两个 include root」的不对称，并同步更新所有受影响的构建定义。你看不到任何其他上下文，本 prompt 已包含完整现状、迁移理由、精确步骤和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 执行时机（重要）

**本任务必须在同一轮其他 session 全部结束后单独执行。** 它会移动文件，若与其他 worker 并发会导致对方编译中断。如果你被并发启动，请立即报告 `BLOCKED` 并说明原因，不要开始移动。

开始前请确认无其他构建进程在跑：

```bash
ps -eo pid,etime,cmd | grep -E '[c]make|[c]test' | head
```

# 现状与迁移理由

## 当前布局

```text
tutti/spi/data_path.h                    <- 仓内 SPI
tutti/spi/storage_target_resolver.h      <- 仓内 SPI
tutti/include/tutti/status.h             <- 公共头
tutti/include/tutti/io_types.h           <- 公共头
tutti/include/tutti/memory_types.h       <- 公共头
tutti/include/tutti/storage_runtime.h    <- 公共头
tutti/include/tutti/cuda_like.h          <- 公共头
tutti/include/tutti/gpu_vendor/host.h    <- 公共头
```

SPI 头的逻辑 include 路径是 `<tutti/spi/data_path.h>`。要让它解析，include root 必须是**仓库根**（`tutti/` 的上一级）。而公共头的 root 是 `tutti/include`。

## 由此产生的三个问题

### 问题 1：build tree 与 install tree 不对称

上一轮建立的 `tutti_spi` target 当前是：

```cmake
add_library(tutti_spi INTERFACE)
target_include_directories(tutti_spi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>   # 仓库根
    $<INSTALL_INTERFACE:include>                        # 单一根
)
target_link_libraries(tutti_spi INTERFACE tutti_api)
```

build tree 需要**两个**根（`tutti/include` 供公共头 + 仓库根供 SPI 头），install tree 只需**一个**（`include`）。安装时通过

```cmake
install(DIRECTORY spi/ DESTINATION include/tutti/spi FILES_MATCHING PATTERN "*.h")
```

把 SPI 头搬到 `include/tutti/spi/`，即**安装布局已经是本任务要迁移到的目标形状**。也就是说源码树与安装树的物理布局现在是不一致的，靠 install 规则临时弥合。

### 问题 2：仓库根作为 include root 暴露面过大

`-I<repo root>` 会让所有 SPI 消费者同时能 `#include <backends/...>`、`#include <io_engine/...>`、`#include <examples/...>`、`#include <coordinator/...>` 等。这与「SPI 是受控边界」的意图相悖。

### 问题 3：contract test 硬编码仓库根绝对路径

以下两个测试的 CMake 目前都硬编码了仓库根，正是为了解析 `<tutti/spi/...>`：

```text
tests/data_path_contract/CMakeLists.txt
tests/storage_target_resolver_contract/CMakeLists.txt
```

## 迁移目标

```text
tutti/include/tutti/spi/data_path.h
tutti/include/tutti/spi/storage_target_resolver.h
```

迁移后：

- 逻辑 include 路径 `<tutti/spi/...>` **完全不变**（这是关键 —— 所有 `#include` 语句都不需要改）；
- 单一 include root `tutti/include` 同时服务公共头与 SPI 头；
- build tree 与 install tree 布局一致；
- 仓库根不再需要出现在任何 include 路径中；
- `tutti/spi/` 目录被移除。

# 精确迁移步骤

## 步骤 1：移动文件

**注意：这两个 SPI 头在 Git 中是 untracked（`??`）状态**，因为它们由前几轮 worker 新建且尚未 commit。因此：

- `git mv` 对它们**不适用**，请用普通 `mv`；
- 移动后**无法**用 `git diff` 证明内容未变。你必须用 checksum 证明：

```bash
# 移动前
md5sum tutti/spi/data_path.h tutti/spi/storage_target_resolver.h

mkdir -p tutti/include/tutti/spi
mv tutti/spi/data_path.h              tutti/include/tutti/spi/data_path.h
mv tutti/spi/storage_target_resolver.h tutti/include/tutti/spi/storage_target_resolver.h
rmdir tutti/spi

# 移动后
md5sum tutti/include/tutti/spi/data_path.h tutti/include/tutti/spi/storage_target_resolver.h
```

两组 md5 必须**逐一对应相等**。把两组输出都记入结果文件。

**禁止修改这两个头文件的任何内容。** 本任务是纯位置迁移。如果你认为它们内部有问题，记录但不要改。

`rmdir tutti/spi` 必须成功（说明目录已空）。若失败说明还有其他文件，停下来报告 `BLOCKED` 并列出残留文件。

## 步骤 2：更新 `tutti_spi` 的 include interface

把 `BUILD_INTERFACE` 从仓库根改为公共 include 根：

```cmake
target_include_directories(tutti_spi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
```

同时更新该 target 上方的注释块，使其如实描述新布局（不要留下「repository include root」这类过时描述）。

**关于 `tutti_spi` 是否仍有必要保留：** 迁移后它的 include root 与 `tutti_api` 相同，看似冗余。**请保留它**，理由：

- 它是「我消费仓内 SPI」这一语义的显式声明点，与「我只用公共 API」有意义地区分；
- 它已在 `tutti_targets` export 集合中，移除会破坏已安装 package 的 target 名；
- 目标架构的 target 清单中 `tutti_spi` 是一个独立条目。

在结果中说明你保留它的理由，以及迁移后它与 `tutti_api` 的实际差异（如果只剩语义差异，如实写出来）。

## 步骤 3：更新 SPI 头的安装规则

原规则：

```cmake
install(DIRECTORY spi/ DESTINATION include/tutti/spi FILES_MATCHING PATTERN "*.h")
```

迁移后 SPI 头已位于 `include/tutti/spi/`，而现有规则

```cmake
install(DIRECTORY include/tutti/ DESTINATION include/tutti FILES_MATCHING PATTERN "*.h")
```

**已经会递归安装它们**。请：

- 删除现在多余的 `install(DIRECTORY spi/ ...)` 规则；
- 验证安装后 SPI 头仍出现在 `include/tutti/spi/` 下（见验收步骤）；
- 确认没有产生重复安装或路径错位。

## 步骤 4：更新两个 contract test 的 include root

以下两个文件目前硬编码仓库根，迁移后该根不再必要：

```text
tests/data_path_contract/CMakeLists.txt
tests/storage_target_resolver_contract/CMakeLists.txt
```

把它们的 include root 收敛为只需要的那些。迁移后 `<tutti/spi/...>` 与 `<tutti/status.h>` 都从 `tutti/include` 解析，因此仓库根可以移除。

**保持这两个测试的其他内容不变**：target 名、CTest 名、编译选项、源文件列表都不要动。只改 include 路径。

注意 `tests/data_path_contract/data_path_contract_test.cpp` 会 include `<tutti/memory_types.h>`、`<tutti/spi/storage_target_resolver.h>`、`<tutti/spi/data_path.h>` 三个头 —— 迁移后它们**都**在 `tutti/include` 下，所以单个 root 就够。

## 步骤 5：不要动的文件

- `tests/spi_consumer/CMakeLists.txt`：它只 `target_link_libraries(... tutti_spi)`，不含任何 include 路径，迁移后自动跟随新的 `BUILD_INTERFACE`。**不要改它。** 但要验证它仍然构建通过。
- `tests/binding_contract/CMakeLists.txt`（若存在）：它由另一 worker 建立，已刻意同时提供仓库根与 `tutti/include` 两个 root，因此对本次迁移不敏感。**不要改它。** 但要验证它仍然构建通过（若该目录存在）。
- 所有 `.cpp` / `.h` 源文件中的 `#include` 语句：逻辑路径不变，**一律不需要改**。如果你发现某处必须改，说明它用了相对路径 include，请记录该位置但**不要**修改（那属于另一 worker 的文件）。

# 你只能修改、创建或移动

- `/data/home/ryeqiu/Tutti/tutti/spi/data_path.h` → 移动到 `/data/home/ryeqiu/Tutti/tutti/include/tutti/spi/data_path.h`
- `/data/home/ryeqiu/Tutti/tutti/spi/storage_target_resolver.h` → 移动到 `/data/home/ryeqiu/Tutti/tutti/include/tutti/spi/storage_target_resolver.h`
- `/data/home/ryeqiu/Tutti/tutti/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/data_path_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/storage_target_resolver_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/chat/round5/result4.md`

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round5-session4/`

禁止修改或创建任何其他文件。尤其禁止：

- **修改两个 SPI 头的内容**（只许移动位置）
- 修改任何 `.cpp` / `.cu` 测试源文件
- 修改 `tests/spi_consumer/**`、`tests/binding_contract/**`、`tests/storage_runtime_contract/**`
- 修改 `tutti/include/tutti/` 下的公共头（`status.h`、`io_types.h`、`memory_types.h`、`storage_runtime.h`、`cuda_like.h`、`gpu_vendor/**`）
- 修改根 `/data/home/ryeqiu/Tutti/CMakeLists.txt`
- 修改 `/data/home/ryeqiu/Tutti/.gitignore`
- 修改 `/data/home/ryeqiu/Tutti/tutti/bindings/**`
- 修改 `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round5/result4.md` 外的任何文件

禁止提交 Git commit。

## 对 `tutti/CMakeLists.txt` 的改动纪律

这是共享文件，必须外科手术式改动：

- 只改 `tutti_spi` 的 `target_include_directories`、其上方注释、以及删除多余的 `install(DIRECTORY spi/ ...)`；
- 若需要为 SPI 头新增/调整 install 规则，仅限该处；
- **不要**重排、重构或「顺手改进」既有内容；
- **不要**改动 `tutti_api`、`tutti_cuda_like`、`tutti_types` 的定义；
- **不要**改动 `TUTTI_ACCELERATOR` / `TUTTI_BUILD_HARDWARE_STACK` 逻辑；
- **不要**改动既有 `add_subdirectory` 顺序或 `BUILD_TESTING` 分支；
- 保持文件现有注释风格。

# 安全限制

绝对禁止 `sudo` / `insmod` / `rmmod` / `modprobe`；禁止启动 daemon/client、访问 `/dev/nvme*`、执行 CUDA 调用或任何硬件 IO。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 迁移完整性

```bash
test ! -d tutti/spi && echo 'old dir removed: PASS'
ls -l tutti/include/tutti/spi/
```

两个头必须存在于新位置，`tutti/spi/` 必须已消失。md5 前后对照必须相等（见步骤 1）。

## 2. 单一 include root 足够（核心验证）

用**只有** `tutti/include` 一个 root 编译一个探测程序：

```bash
printf '%s\n' '#include <tutti/memory_types.h>' '#include <tutti/spi/storage_target_resolver.h>' '#include <tutti/spi/data_path.h>' '#include <tutti/storage_runtime.h>' 'int main(){ return 0; }' \
  | /opt/rh/gcc-toolset-13/root/usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -DTUTTI_USE_HOST=1 \
    -I/data/home/ryeqiu/Tutti/tutti/include -x c++ -fsyntax-only -
```

必须通过。这证明单根已足够。

## 3. 仓库根不再必要（否定验证）

再用**不含**仓库根、也**不含** `tutti/` 目录本身的 flag 编译同一探测（即只给 `tutti/include`），已在步骤 2 完成。额外确认仓库根已从 `tutti_spi` 中移除：

```bash
grep -n -A4 'add_library(tutti_spi' tutti/CMakeLists.txt
```

`BUILD_INTERFACE` 中不应再出现 `/..`。

## 4. HOST profile 全量 configure / build / ctest

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round5-session4

cmake -S /data/home/ryeqiu/Tutti/tutti \
  -B /data/home/ryeqiu/Tutti/build/round5-session4 \
  -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build /data/home/ryeqiu/Tutti/build/round5-session4 -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round5-session4 --output-on-failure
```

要求全部测试通过。特别确认 `tutti_spi_consumer_test` 在**未修改**其 CMake 的前提下仍然通过 —— 这证明 `tutti_spi` 的 usage requirements 正确跟随了迁移。

## 5. 两个独立 contract test 仍通过

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round5-session4-dpc
cmake -S tests/data_path_contract -B build/round5-session4-dpc -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round5-session4-dpc --target tutti_data_path_contract_test -j8
ctest --test-dir build/round5-session4-dpc --output-on-failure -R '^tutti_data_path_contract_test$'

rm -rf /data/home/ryeqiu/Tutti/build/round5-session4-strc
cmake -S tests/storage_target_resolver_contract -B build/round5-session4-strc -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round5-session4-strc --target tutti_storage_target_resolver_contract_test -j8
ctest --test-dir build/round5-session4-strc --output-on-failure -R '^tutti_storage_target_resolver_contract_test$'
```

两者均须 1/1 PASS。

（这两个额外 build 目录也在 ignored `build/` 下，允许创建。）

## 6. 未被本任务改动的相邻测试仍通过

若 `tests/binding_contract/` 存在，用其自带 CMake 验证它未被迁移破坏：

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round5-session4-bind
cmake -S tests/binding_contract -B build/round5-session4-bind -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round5-session4-bind --target tutti_binding_contract_test -j8
ctest --test-dir build/round5-session4-bind --output-on-failure -R '^tutti_binding_contract_test$'
```

若该目录不存在，记录「未创建，跳过」。若存在且失败，**不要修改它**，记录失败详情并在结果中标为需总指挥裁决。

## 7. Install 冒烟与布局一致性

```bash
cmake --install /data/home/ryeqiu/Tutti/build/round5-session4 \
  --prefix /data/home/ryeqiu/Tutti/build/round5-session4/_install

find /data/home/ryeqiu/Tutti/build/round5-session4/_install/include -type f | sort
```

要求：

- SPI 头出现在 `include/tutti/spi/` 下；
- 无重复安装、无路径错位；
- 用安装树做一次相对路径编译验证：

```bash
cd /data/home/ryeqiu/Tutti/build/round5-session4/_install
printf '%s\n' '#include <tutti/spi/data_path.h>' '#include <tutti/spi/storage_target_resolver.h>' 'int main(){ return 0; }' \
  | /opt/rh/gcc-toolset-13/root/usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -DTUTTI_USE_HOST=1 -Iinclude -x c++ -fsyntax-only -
```

必须通过。

若 `cmake --install` 因既有无关 target 报错或耗时过长，记录真实情况并说明是否属本任务范围；不得为此去改其他 target。**若某条命令超过 3 分钟无响应，中止它并记录，不要无限等待。**

## 8. SPI 头内容零改动

```bash
# 与步骤 1 记录的迁移前 md5 对照
md5sum tutti/include/tutti/spi/data_path.h tutti/include/tutti/spi/storage_target_resolver.h
```

必须与迁移前完全一致。

## 9. 无残留旧路径引用

```bash
grep -rn 'tutti/spi/' --include=CMakeLists.txt . | grep -v 'include/tutti/spi' | head
grep -rn '"\.\./\.\./spi/\|"\.\./spi/' --include=*.h --include=*.cpp . | head
```

第一条应只剩指向新位置或逻辑 include 路径的引用；第二条（相对路径 include）应无命中。逐条说明每处命中。

## 10. Hygiene

```bash
git diff --check -- tutti/CMakeLists.txt tests/data_path_contract/CMakeLists.txt tests/storage_target_resolver_contract/CMakeLists.txt
git diff -- tutti/CMakeLists.txt | cat
```

对所有改动文件额外检查尾随空白与 EOF newline。确认只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. 两个 SPI 头已位于 `tutti/include/tutti/spi/`，`tutti/spi/` 已移除；
2. 迁移前后 md5 逐一相等（内容零改动）；
3. 单一 include root `tutti/include` 足以解析公共头与 SPI 头；
4. `tutti_spi` 的 `BUILD_INTERFACE` 不再包含仓库根；
5. `tutti_spi` 被保留，并说明其与 `tutti_api` 迁移后的实际差异；
6. 多余的 `install(DIRECTORY spi/ ...)` 已删除，SPI 头仍正确安装到 `include/tutti/spi/`；
7. 安装树下单根相对路径编译通过；
8. HOST 全量 configure/build/ctest 通过；
9. `tutti_spi_consumer_test` 在其 CMake **未被修改**的前提下通过；
10. 两个独立 contract test 通过；
11. `tests/binding_contract`（若存在）通过或已如实记录失败；
12. 无残留旧路径 / 相对路径 include；
13. `tutti/CMakeLists.txt` 改动为最小外科手术式，未重构既有内容；
14. 未修改允许列表外文件；
15. 未执行任何模块、daemon 或 IO 操作；
16. 空白检查通过。

如果迁移过程中发现某个未预料的依赖断裂（例如某个文件用相对路径 include SPI 头），**停下来**，在结果中记录具体位置与影响，报告 `BLOCKED`，不要擅自扩大修改范围。回滚方式很简单（把两个头移回 `tutti/spi/`），如需回滚请记录回滚后的验证结果。

# 结果落盘要求

把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round5/result4.md`

至少包含：

1. 并发检查结果（确认无其他构建进程）
2. 修改/移动文件列表
3. 迁移前后 md5 对照（两组完整输出）
4. `tutti_spi` 改动前后的 CMake 代码对照
5. 保留 `tutti_spi` 的理由及其与 `tutti_api` 迁移后的实际差异
6. install 规则改动说明
7. 单根编译验证结果（步骤 2）
8. HOST 全量 configure/build/ctest 结果（含各测试名与结果）
9. `tutti_spi_consumer_test` 未改 CMake 仍通过的证据
10. 两个独立 contract test 结果
11. `tests/binding_contract` 结果或「未创建，跳过」
12. install 冒烟 + 安装树单根编译结果 + 完整安装文件清单
13. SPI 头内容零改动的 md5 证明
14. 无残留旧路径引用的 grep 结果（逐条说明）
15. `tutti/CMakeLists.txt` 完整 diff
16. 文件边界与空白检查结果
17. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 命令失败就写真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round5/result4.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
