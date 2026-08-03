# TASK T-021

你是一名资深 CMake 构建工程师。你只负责一件事：审查并尽可能收敛 `tutti/CMakeLists.txt` 中一处全局 `include_directories()`，使 SPI/公共 API 消费者不再被动获得 `tutti/` 全目录的 include 暴露面。**这是一个以「审查 + 证据」为主、「改动」为次的任务** —— 如果证据表明无法安全移除，如实报告并给出精确的阻碍清单，那也是合格交付。

你看不到任何其他上下文，本 prompt 已包含完整现状、已核实的事实和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 执行时机

本任务纯构建系统与源码分析，不碰硬件。

**但它不可与任何执行 CMake 构建的任务并发**，原因有两条：

1. 它会修改 `tutti/CMakeLists.txt`，任何正在 configure 该工程的进程会读到半成品；
2. 它会修改 `tests/binding_contract/CMakeLists.txt`（仅注释），而其他任务可能正在构建该测试作为回归验证。

开始前确认无并发构建：

```bash
ps -eo pid,etime,cmd | grep -E '[c]make|[c]test' | head
```

若有命中，报告 `BLOCKED` 并列出进程，不要开始。

# 背景：问题是什么

上一轮把两个 SPI 头从 `tutti/spi/` 物理迁移到 `tutti/include/tutti/spi/`，目的之一是消除「仓库根作为 include root」造成的过大暴露面。迁移后 `tutti_spi` 与 `tutti_api` 的 include interface 都收敛为单一根：

```cmake
target_include_directories(tutti_spi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
```

**但暴露面只缩小了一半，没有闭合。** 原因是 `tutti/CMakeLists.txt:91-93` 有一个**既有的全局**指令：

```cmake
# Make tutti/ headers available to all targets
include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
```

它把 `tutti/` 注入该工程内**每一个** target（`include_directories` 是目录级全局，对当前目录及其后续所有 `add_subdirectory` 生效）。而 `tutti/` 下有：

```text
tutti/accel/          tutti/backends/       tutti/bindings/
tutti/block_storage/  tutti/coordinator/    tutti/device_manager/
tutti/io_engine/      tutti/raw_device/     tutti/abstraction/  tutti/types/
```

因此 tutti 工程内的 SPI 消费者依然能 `#include <backends/...>`、`#include <io_engine/...>` 等，「SPI 是受控边界」的意图未真正落地。

**已核实的重要事实（缩小暴露面的证据基础）：**

外部经 `tutti_spi` / `tutti_api` 消费的路径**已经是干净的**。根 `tests/` 下的独立 contract test（`data_path_contract`、`storage_target_resolver_contract`、`binding_contract`、`spi_consumer`、`storage_runtime_contract` 等）都有自己的顶层 `CMakeLists.txt`、独立 configure，**不经过** `tutti/CMakeLists.txt`，因此不受该全局影响。受影响的**只有** tutti 工程内部的 target。

# 已核实的事实（不需要你重新验证，但需要你据此展开）

## 事实 1：内部 target 已自带该路径

`tutti/` 下各层的 `CMakeLists.txt` **已经**在自己的 `target_include_directories` 里显式声明了工程根。例如：

`tutti/accel/CMakeLists.txt:74-80`：

```cmake
target_include_directories(${LAYER_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/common>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/cuda>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>          # <-- 工程根
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ...
```

`tutti/io_engine/CMakeLists.txt:64-71` 同样含 `$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>`。

**这意味着全局指令很可能是冗余的** —— 至少对这些已正确声明的 target 而言。你的首要任务就是查清「哪些 target 真的只靠全局指令才能编译」。

**注意一个陷阱：** `${PROJECT_SOURCE_DIR}` 的值取决于最近的 `project()` 调用。从仓库根 configure 时它是仓库根；独立 `cmake -S tutti` 时它是 `tutti/`。两种情形下同一行的含义不同，你必须区分清楚，不要混为一谈。

## 事实 2：依赖该路径的 include 前缀分布

统计 `tutti/` 下源码中形如 `#include <前缀/...>` 的出现次数（前缀为 `tutti/` 的子目录名）：

```text
  37  backends
  27  accel
  18  io_engine
   6  block_storage
   4  device_manager
   4  coordinator
```

这些 include 需要「`tutti/` 作为 root」才能解析。它们全部位于硬件栈内部。

## 事实 3：硬件栈的门控状态

`tutti/CMakeLists.txt:166` 起有 `if(TUTTI_BUILD_HARDWARE_STACK)` 分支，内含 `add_subdirectory(accel)`、`device_manager`、`backends`、`io_engine`（`block_storage`/`coordinator`/`raw_device` 目前被注释掉）。

- 从**仓库根** configure 时：根 `CMakeLists.txt:344` 强制 `set(TUTTI_BUILD_HARDWARE_STACK OFF ... FORCE)`，因此 tutti 工程只贡献 Phase 0 契约 target（`tutti_cuda_like` / `tutti_api` / `tutti_spi` / `tutti_types`），**硬件栈子目录不参与构建**。
- 独立 `cmake -S tutti` 时：`tutti/cmake/accelerators/CUDA.cmake:9` 默认 `TUTTI_BUILD_HARDWARE_STACK=ON`；HOST profile 强制 OFF。

**这给了你一个关键的验证维度：** HOST profile 与「从仓库根 configure」两种情形下硬件栈不参与构建，移除全局指令的风险极低；而独立 CUDA profile 会构建硬件栈，风险集中在那里。

## 事实 4：CUDA profile 的历史构建产物存在

`tutti/build-profile-cuda/` 与 `tutti/build-profile-host/` 目录存在，说明两种 profile 此前都曾被 configure 过。你可以参考它们判断历史可行性，但**不要**在这两个目录里构建（见文件边界）。

# 你的任务

分三步，**每步都必须产出证据**。

## 步骤 1：确定移除是否安全（审查为主）

回答这个核心问题：**如果删除 `tutti/CMakeLists.txt:91-93` 的全局 `include_directories`，哪些 target 会编译失败？**

方法建议（可自行调整，但必须给出可复核的证据）：

- 枚举 tutti 工程内定义的所有 target 及其 `target_include_directories` 声明，判断谁已自带工程根、谁没有；
- 对没有自带的 target，找出它们实际依赖哪些跨目录 include；
- 区分「PUBLIC/INTERFACE 传递而来」与「仅靠全局指令」两种来源 —— 这是判断的关键，因为经 `target_link_libraries` 传递的 PUBLIC include 目录不受全局指令删除影响；
- 注意 `tutti/` 内还有 20 个 `CMakeLists.txt`（含各层 `tests/` 子目录），测试可执行文件也可能依赖该全局。

产出一份清单：**「已自带工程根的 target」/「靠传递获得的 target」/「仅靠全局指令的 target」**，逐个给出文件与行号依据。

## 步骤 2：实施最安全的收敛方案

根据步骤 1 的结论，选择并实施**其中一个**方案，在结果中说明为何选它：

**方案 A（首选，若步骤 1 显示无 target 仅靠全局指令）：** 直接删除 `include_directories(...)` 块，保留一条注释说明「工程根 include 由各 target 自行通过 `target_include_directories` 声明」。

**方案 B（若少数 target 依赖它）：** 删除全局指令，并为那些 target 在其**各自的** `CMakeLists.txt` 中补上 `target_include_directories(... PRIVATE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>)`。注意：这会让你修改硬件栈层的 CMakeLists，**必须**在结果中逐个列出改了哪些文件哪些行，且只加 include 目录、不动任何其他内容。

**方案 C（若步骤 1 显示大范围依赖且无法逐个补齐）：** 不删除，改为**收窄作用域** —— 把全局指令移到 `if(TUTTI_BUILD_HARDWARE_STACK)` 分支**内部**，使其只对硬件栈生效，Phase 0 契约 target（`tutti_cuda_like`/`tutti_api`/`tutti_spi`/`tutti_types`）不再被注入。这已经解决了本任务的核心诉求（SPI 消费者不被污染），是完全可接受的交付。

**方案 D（若连方案 C 都不可行）：** 不改动，产出完整的阻碍清单与建议路径，报告 `BLOCKED (analysis complete)`。这也是合格交付 —— **前提是你的分析证据充分**。

**严禁**为了让构建通过而：把 include 目录塞进不相关的 target、修改任何 `#include` 语句、把头文件搬来搬去、或用 `link_libraries` 等其他全局指令替换一个全局指令。

## 步骤 3：更正两处过时注释

上一轮的 SPI 头迁移使以下两处注释与事实不符（它们描述的是迁移前的布局）：

- `tests/spi_consumer/CMakeLists.txt:5`：`#   - repository include root   -> <tutti/spi/...> resolves`
  实际现在是公共 include 根（`tutti/include`），不是仓库根。

- `tests/binding_contract/CMakeLists.txt:19`：`#   - /data/home/ryeqiu/Tutti                 : current layout (tutti/spi/*.h)`
  实际 `tutti/spi/` 已不存在，SPI 头在 `tutti/include/tutti/spi/`。

**只改注释文字，不改任何 CMake 指令。** 这两个测试当前都通过，改完后必须仍然通过。

顺带说明：`tests/binding_contract/CMakeLists.txt` 提供了两个 include root，其中指向仓库根的那个现在已不解析任何 SPI 头（属冗余但无害 —— 正是这个「布局无关」设计让它免于被上一轮迁移波及）。**保留它**，只在注释中如实说明其现状即可。若你认为该精简，记录建议但不要删。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/spi_consumer/CMakeLists.txt`（**仅注释文字**）
- `/data/home/ryeqiu/Tutti/tests/binding_contract/CMakeLists.txt`（**仅注释文字**）
- `/data/home/ryeqiu/Tutti/chat/round6/result2.md`
- 若采用方案 B：`tutti/` 下相关层的 `CMakeLists.txt`（**仅添加 include 目录**，必须在结果中逐个列出）

构建产物只能写入：

`/data/home/ryeqiu/Tutti/build/round6-session2*`（可建多个后缀目录用于不同 profile）

禁止修改或创建任何其他文件。尤其禁止：

- 修改任何 `.c` / `.cpp` / `.cu` / `.h` 源文件（**包括任何 `#include` 语句**）
- 修改根 `/data/home/ryeqiu/Tutti/CMakeLists.txt`
- 修改 `tutti/include/**`（含 `tutti/include/tutti/spi/**`）
- 修改 `tutti/bindings/**`
- 修改 `tutti/cmake/accelerators/**`
- 修改 `tests/` 下除上述两个 CMakeLists 注释外的任何文件
- 移动或删除任何头文件
- 在 `tutti/build-profile-cuda/` 或 `tutti/build-profile-host/` 中构建
- 修改 `.gitignore`
- 修改 `chat/**` 中除 `chat/round6/result2.md` 外的任何文件

禁止提交 Git commit。

## 对 `tutti/CMakeLists.txt` 的改动纪律

这是共享文件，且工作树中已有未提交的既有改动（前几轮的 profile 门控重构 + `tutti_spi` target）。你必须：

- 只改全局 `include_directories` 那一处（删除或移动），以及必要的注释；
- **不要**改动 `tutti_api` / `tutti_cuda_like` / `tutti_spi` / `tutti_types` 的定义；
- **不要**改动 `TUTTI_ACCELERATOR` / `TUTTI_BUILD_HARDWARE_STACK` 的逻辑或默认值；
- **不要**改动既有 `add_subdirectory` 顺序、`BUILD_TESTING` 分支或任何 `install()` 规则；
- **不要**重排、重构或「顺手改进」既有内容；
- 在结果中明确区分「本任务的改动」与「工作树中既有的未提交改动」（后者不属于你，不要为它辩护也不要修改它）。

# 安全限制

绝对禁止 `sudo` / `insmod` / `rmmod` / `modprobe`；禁止启动 daemon/client、访问 `/dev/nvme*`、执行 CUDA kernel 或任何硬件 IO。

**允许** CUDA profile 的 configure/build（那只是编译，不执行）。但若 CUDA 构建因缺少 SDK、缺少依赖或耗时过长而无法完成，如实记录并说明，不要为此修改任何 target 或依赖声明。**若某条命令超过 10 分钟无进展，中止并记录。**

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. HOST profile 全量（必须通过）

```bash
rm -rf build/round6-session2-host
cmake -S tutti -B build/round6-session2-host \
  -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round6-session2-host -j8
ctest --test-dir build/round6-session2-host --output-on-failure
```

全部测试必须通过（预期 3 个：`cuda_like_contract_test`、`tutti_public_api_usage_test`、`tutti_spi_consumer_test`）。

## 2. 从仓库根 configure（必须通过 configure 阶段）

```bash
rm -rf build/round6-session2-root
cmake -S . -B build/round6-session2-root -DCMAKE_BUILD_TYPE=RelWithDebInfo 2>&1 | tail -20
```

configure 必须成功。**完整构建整个仓库可能非常耗时**，因此：只需 configure 成功，然后构建 tutti 工程贡献的契约 target 即可（例如 `cmake --build build/round6-session2-root --target tutti_spi_consumer_test` 若该 target 存在于根构建中；若不存在，说明原因并跳过）。不要为了跑完整构建而无限等待。

## 3. 独立 CUDA profile configure（尽力而为）

```bash
rm -rf build/round6-session2-cuda
cmake -S tutti -B build/round6-session2-cuda \
  -DTUTTI_ACCELERATOR=CUDA -DCMAKE_BUILD_TYPE=RelWithDebInfo 2>&1 | tail -25
```

这是硬件栈参与构建的情形，是本任务风险最集中处。要求：

- configure 必须成功；
- 尝试构建硬件栈中至少一个库 target（例如 accel 或 io_engine 层），验证跨目录 include 仍能解析；
- 若因环境原因（CUDA SDK / gRPC / yaml-cpp 缺失等）无法 configure 或构建，**如实记录完整错误**并说明这是环境限制而非你的改动所致 —— 判断依据：在**改动前**同样命令是否也失败。若改动前也失败，必须给出改动前的对照输出。

**这一条是本任务最重要的证据。** 如果你无法验证 CUDA profile，必须明确声明「移除的安全性在硬件栈上未经实测」，不得含糊过去。

## 4. 收敛效果的正面验证

证明 Phase 0 契约 target 不再获得 `tutti/` 注入。从 `compile_commands.json` 抽取 `spi_consumer` target 的实际 `-I` flag：

```bash
grep -o '\-I[^ "]*' build/round6-session2-host/compile_commands.json | sort -u
```

改动前该 target 会同时得到 `-I<...>/tutti` 与 `-I<...>/tutti/include`。改动后（方案 A/B/C 任一成功时）`-I<...>/tutti` 应消失。

**把改动前与改动后的 flag 对照都记入结果。** 改动前的数据需要你在动手前先采集（或从既有 `build/round5-session4/compile_commary_commands.json` 读取，若存在）。

## 5. 暴露面收敛的否定验证

改动后，用 `spi_consumer` target 实际获得的 include flag 编译一个探测程序，确认它**无法**再解析硬件栈头：

```bash
printf '%s\n' '#include <io_engine/...>' 'int main(){return 0;}' | ...
```

（把 `<io_engine/...>` 换成一个真实存在的硬件栈头路径，请自行从 `tutti/io_engine/include/` 下选一个。）

预期：**编译失败**（`No such file or directory`）。这证明暴露面真的关闭了，而不只是 flag 列表变短。

若方案 C（收窄作用域）被采用，该验证同样适用于 Phase 0 target。

## 6. 两处注释更正后测试仍通过

```bash
rm -rf build/round6-session2-spi build/round6-session2-bind
cmake -S tests/spi_consumer -B build/round6-session2-spi -DCMAKE_BUILD_TYPE=RelWithDebInfo 2>&1 | tail -5
cmake --build build/round6-session2-spi -j8
ctest --test-dir build/round6-session2-spi --output-on-failure

cmake -S tests/binding_contract -B build/round6-session2-bind -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round6-session2-bind -j8
ctest --test-dir build/round6-session2-bind --output-on-failure
```

（若 `tests/spi_consumer` 没有可独立 configure 的顶层 CMakeLists，说明原因并改用步骤 1 的结果作为其通过证据。）

两者均须通过。

## 7. 相邻 contract test 未被破坏

```bash
for t in data_path_contract storage_target_resolver_contract storage_runtime_contract; do
  rm -rf build/round6-session2-$t
  cmake -S tests/$t -B build/round6-session2-$t -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null 2>&1
  cmake --build build/round6-session2-$t -j8 >/dev/null 2>&1
  echo "--- $t ---"
  ctest --test-dir build/round6-session2-$t 2>&1 | grep -E 'tests passed|tests failed'
done
```

全部必须通过。这些测试不经过 `tutti/CMakeLists.txt`，理论上不受影响 —— 若有任何一个失败，说明你的改动影响范围超出预期，必须查清原因。

## 8. Hygiene

```bash
git diff -- tutti/CMakeLists.txt | cat
git diff --check -- tutti/CMakeLists.txt
git status --short --untracked-files=all | head -20
```

对所有改动文件检查尾随空白与 EOF newline；确认只触碰允许列表。

# 成功标准

报告 `PASS` 需满足：

1. 步骤 1 的 target 依赖清单完整，逐项有文件行号依据；
2. 已实施方案 A、B 或 C 之一，并说明选择理由；
3. HOST profile 全量 configure/build/ctest 通过；
4. 从仓库根 configure 成功；
5. CUDA profile configure 成功且至少一个硬件栈 target 构建通过 —— **或** 明确记录环境限制并给出改动前的同样失败作为对照；
6. `compile_commands.json` 证明 Phase 0 契约 target 不再获得 `tutti/` 注入（前后 flag 对照）；
7. 否定验证证明硬件栈头已无法从 SPI 消费者解析；
8. 两处注释已更正，且相关测试仍通过；
9. 三个相邻 contract test 未被破坏；
10. `tutti/CMakeLists.txt` 改动为最小外科手术式，未重构既有内容，且明确区分了本任务改动与工作树既有改动；
11. 未修改任何源文件的 `#include`，未移动任何头文件；
12. 未修改允许列表外文件；
13. 未执行 sudo / 模块操作 / daemon / 硬件 IO；
14. 空白与 EOF newline 检查通过。

报告 `BLOCKED (analysis complete)` 是**合格交付**，条件是：步骤 1 的分析证据充分（清单完整、有行号依据）、给出了精确的阻碍原因、并说明为何方案 A/B/C 都不可行。此时不要留下任何半成品改动 —— 若已试改请回滚，并记录回滚后步骤 1/3/6/7 仍通过。

**不得为了拿到 PASS 而削弱验证**。特别是：若 CUDA profile 未能实测，必须显式声明该风险，不得以「HOST 通过即可」蒙混。

# 结果落盘要求

把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round6/result2.md`

至少包含：

1. 并发检查结果
2. 步骤 1 的完整 target 依赖清单（三类分组 + 文件行号依据）
3. 选定方案（A/B/C/D）及选择理由
4. `tutti/CMakeLists.txt` 的精确改动内容，以及「本任务改动」与「工作树既有改动」的区分说明
5. 若采用方案 B：逐个列出补充了 include 目录的文件与行号
6. HOST profile 全量结果（含各测试名与结果）
7. 从仓库根 configure 结果
8. CUDA profile configure/build 结果，或环境限制的完整记录 + 改动前对照
9. `compile_commands.json` 的 `-I` flag 前后对照
10. 暴露面否定验证结果（所选硬件栈头路径 + 编译失败输出）
11. 两处注释更正的前后文字，及相关测试结果
12. 三个相邻 contract test 结果
13. 完整 `git diff`
14. 文件边界与空白检查结果
15. 未解决的遗留与后续建议（例如若采用方案 C，说明彻底关闭还需要什么）
16. 最终 `PASS` / `BLOCKED (analysis complete)`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 命令失败就写真实错误，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round6/result2.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
