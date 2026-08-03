# TASK T-004

你是一名资深 CMake 构建系统工程师。你只负责修复 Tutti 顶层构建把“重构子工程硬件栈”和“根目录历史硬件栈”同时加入后产生的全局 target 重名。你看不到任何其他上下文，本 prompt 已包含完整背景、边界和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 已确认的故障

现有构建目录执行：

```bash
cmake --build /data/home/ryeqiu/Tutti/build \
  --target nvmeservice_client_example -j8
```

会触发 CMake 重新配置，并因下列 target 重复而失败：

```text
libnvm
nvmeservice
nvmeservice_daemon_example
nvmeservice_client_example
tutti_device_manager
```

直接原因已经确认：

1. 顶层 `/data/home/ryeqiu/Tutti/CMakeLists.txt` 先 `add_subdirectory(tutti)`。
2. `tutti/` 的 CUDA profile 默认打开 `TUTTI_BUILD_HARDWARE_STACK`，从 `tutti/device_manager/**` 声明一套 `libnvm`、`nvmeservice`、examples 和 `tutti_device_manager`。
3. 顶层随后又从历史根目录树声明同名 production targets。
4. CMake target 名处于全局命名空间，因此配置失败。

# 冻结的架构决定

当前迁移阶段必须遵循：

- 以仓库根目录配置时，历史根目录 hardware stack 仍是唯一 production target owner。
- `tutti/` 被根目录通过 `add_subdirectory(tutti)` 纳入时，只提供 Phase 0 的 `tutti_cuda_like`、`tutti_types` 等非重复基础 target，不再递归声明第二套 hardware stack。
- 单独执行 `cmake -S tutti ... -DTUTTI_ACCELERATOR=CUDA` 时，CUDA profile 仍保持 `TUTTI_BUILD_HARDWARE_STACK=ON` 的现有默认行为。
- 不重命名任何现有 target。
- 不通过在每个 `add_library`/`add_executable` 周围堆叠 `if(NOT TARGET ...)` 来掩盖冲突。
- 不删除任一源码树。
- 不改变 HOST/CUDA profile 的独立构建契约。

最小预期修复是在顶层根 `CMakeLists.txt` 调用 `add_subdirectory(tutti)` 前，明确把嵌套 `tutti/` 的 `TUTTI_BUILD_HARDWARE_STACK` 设为 `OFF`。该约束只存在于“从仓库根目录配置”的路径，不得修改 `tutti/` 独立配置时的默认值。

必须考虑当前 `/data/home/ryeqiu/Tutti/build/CMakeCache.txt` 可能已经缓存了 `TUTTI_BUILD_HARDWARE_STACK=ON`。修复必须让顶层重新配置时可靠关闭嵌套 hardware stack，不能要求用户手工删除 cache 才生效。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/chat/round2/result1.md`

其中 `chat/round2/result1.md` 只用于保存本 session 的完整原始执行结果，不得写入源码或额外设计文档。

禁止修改或创建任何其他源码、文档、测试或配置文件。尤其禁止修改：

- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tutti/**`
- `/data/home/ryeqiu/Tutti/tests/**`
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round2/result1.md` 外的任何文件
- 任意 kernel module、libnvm、NVMeService 源码

现有 `/data/home/ryeqiu/Tutti/build/` 是被 `.gitignore` 忽略的构建目录，可以由 CMake 更新构建产物和 cache；不得创建新的仓库内 build 目录。

禁止提交 Git commit。

# 实现约束

1. 只做解决 target 重名所需的最小改动。
2. 在顶层 `add_subdirectory(tutti)` 附近增加简短注释，说明：
   - 根目录拥有当前 production hardware targets；
   - 嵌套 `tutti/` 仅提供 Phase 0 contract targets；
   - 独立 `cmake -S tutti` 不受影响。
3. 必须覆盖已有 cache 中为 `ON` 的情况。
4. 不新增新的用户可见 option。
5. 不改 target 名、输出名、依赖关系或安装规则。
6. 不“顺手”整理顶层 CMake。

# 安全限制

绝对禁止执行：

```text
insmod
rmmod
modprobe
sudo make insmod
sudo make rmmod
make insmod
make rmmod
```

也禁止：

- 启动 daemon
- 访问或写入 `/dev/nvme*`
- mount/umount/格式化/分区
- 运行任何硬件 IO 测试
- 修改当前内核模块状态
- 使用 `sudo`

本任务只配置和编译用户态 target。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 重新配置现有根构建目录

```bash
cmake -S /data/home/ryeqiu/Tutti \
  -B /data/home/ryeqiu/Tutti/build
```

要求：

- 配置成功。
- 不再出现任何 `cannot create target ... because another target with the same name already exists`。
- 配置日志能确认嵌套 `tutti/` hardware stack 未被加入。
- 根目录历史 production targets 仍存在。

如果配置失败于与重复 target 无关的既有环境问题，禁止扩大修改范围；记录 `BLOCKED` 和完整首个错误。

## 2. 构建历史 client target

```bash
cmake --build /data/home/ryeqiu/Tutti/build \
  --target nvmeservice_client_example -j8
```

要求：

- target 构建成功。
- 真实输出文件是：

```text
/data/home/ryeqiu/Tutti/build/bin/nvmeservice_client
```

注意：`nvmeservice_client_example` 是 CMake target 名，不是二进制文件名；`OUTPUT_NAME` 为 `nvmeservice_client`。

## 3. 构建 daemon target

```bash
cmake --build /data/home/ryeqiu/Tutti/build \
  --target tutti_daemon -j8
```

要求真实输出仍为：

```text
/data/home/ryeqiu/Tutti/build/bin/tutti_daemon
```

只编译，不启动。

## 4. 最小静态检查

```bash
git diff --check -- CMakeLists.txt
git diff -- CMakeLists.txt
```

确认改动只位于顶层 `CMakeLists.txt`，且每一行都直接用于解决嵌套 hardware stack 重复声明。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. 顶层 CMake 配置不再出现五组 duplicate-target 错误。
2. `nvmeservice_client_example` 构建成功。
3. `/data/home/ryeqiu/Tutti/build/bin/nvmeservice_client` 存在且可执行。
4. `tutti_daemon` 构建成功，输出仍存在且可执行。
5. 未修改允许列表外的任何文件。
6. 未执行任何模块、daemon 或 block I/O 操作。
7. `git diff --check -- CMakeLists.txt` 通过。

如果用户态编译暴露新的、与 target 重名无关的错误，报告 `BLOCKED`，给出首个真实错误和最小归因；禁止为了让构建全绿而修改其他文件。

# 结果落盘要求

完成任务和验收后，必须把本 session 的完整原始结果直接写入：

`/data/home/ryeqiu/Tutti/chat/round2/result1.md`

该文件至少包含：

1. 修改文件列表
2. 最小修复说明
3. 根目录 configure 命令及完整结果摘要
4. duplicate-target 检查结果
5. `nvmeservice_client_example` build 结果与真实二进制路径
6. `tutti_daemon` build 结果与真实二进制路径
7. `git diff --check` 结果
8. 最终 `PASS` 或 `BLOCKED`

规则：

- 这是 worker 原始结果，必须由你自己写入，禁止要求用户复制终端输出。
- 如果命令失败，写入真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写“总指挥验收”内容；总指挥会在你结束后追加到同一文件末尾。
- 写完后执行 `git diff --check -- CMakeLists.txt chat/round2/result1.md`。
- 最终聊天回复只需给出状态和结果文件路径，例如：`PASS — 结果已写入 chat/round2/result1.md`。

不要解释、不要寒暄、不要提交 Git commit、不要修改其他文件。
