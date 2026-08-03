# TASK T-013

你是一名资深系统测试工程师。你只负责修正 Tutti attach smoke harness 中一个**结构性设计错误**：dry-run 的 block-device 门禁永远无法满足。你看不到任何其他上下文，本 prompt 已包含完整硬件事实、根因分析、正确解法和安全边界。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 已确认的 BUG（必须修复）

`/data/home/ryeqiu/Tutti/tests/service_client/run_attach_smoke.sh` 的 dry-run 分支要求四个 block device 必须存在，缺失即 `add_error`（约第 197 行）。

## 根因：这四个 NVMe 的静息状态就是 UNBOUND

实测四个目标 PCI 设备当前驱动绑定状态：

```text
0000:08:00.0 driver=(UNBOUND)
0000:4b:00.0 driver=(UNBOUND)
0000:57:00.0 driver=(UNBOUND)
0000:63:00.0 driver=(UNBOUND)
```

对照组：内核托管的 `nvme4..7` 绑在 `0000:d2/df/86/c5:00.0`，与这四个设备无关。

关键事实链：

1. 这四个设备被 Tutti 独占管理，**平时不绑内核 nvme 驱动**；
2. `tutti_daemon` 启动时以 owner 身份执行 libnvm B3（chrdev create → bind → probe），此时才创建 `/dev/ssnvmeN`；
3. daemon 退出即释放 owner 状态，设备回到 UNBOUND；
4. 因此 **daemon 未运行 ⇒ 无 block device ⇒ 这是稳态，不是故障**。

而 dry-run 的定义正是「daemon 未运行时的前置检查」。所以「dry-run 要求 block device 存在」是一条**永远无法在正常状态下满足**的条件。它不会偶尔失败，而是永远失败。

## 现有代码自相矛盾

同一个函数里，execute 分支的注释已经写对了（约第 191-195 行）：

```text
# In execute mode, block devices may not exist yet because the
# snvme driver only creates them while the daemon is running.
# The daemon's B3 process (bind+probe) will create them.
```

但 dry-run 分支仍然 `add_error`。两处逻辑互相矛盾。

## 上一轮的错误归因（不要重复）

上一轮 harness 把 dry-run 的 BLOCKED 归因为「之前调试用 SIGKILL 导致 PCI 解绑」的偶发环境问题。**这个归因是错的**，已被上述实测推翻。本任务必须在文档中纠正该误解，避免后续再被当成环境故障排查。

# 正确解法

## 1. dry-run 的 block-device 检查改为条件式

语义从「必须存在」改为「**若存在**则必须未挂载且 holders 为空」：

- block device **不存在** → 打印 INFO，明确说明「daemon 未运行时这是预期状态（设备由 daemon 在运行期 bind 创建）」，**不计入错误**；
- block device **存在** → 仍然必须验证：无普通 mount target、holders 为空。任一不满足则是**真实错误**，必须 `add_error`。

第二条不能放松。raw NVMe 被挂载或被其他 block consumer 占用，是真正危险的状态，必须继续拦截。

## 2. dry-run 改为检查真正的静息前置条件

dry-run 应当检查那些在 daemon 未运行时**就应当成立**的条件：

- 四个 GPU 可经 `nvidia-smi -i <id>` 查询；
- 四个 PCI BDF 的 sysfs path 存在；
- 四个 PCI 设备当前驱动绑定状态**可读并报告**（绑定到什么驱动，或 UNBOUND）。这是信息性输出，不是错误判据 —— 除非绑定到内核 `nvme` 驱动且同时已挂载，那种情况应告警；
- daemon binary 可执行；
- client binary 可执行；
- `127.0.0.1:50051` 未被占用；
- `sudo -n true` 可用；
- 打印完整计划命令。

dry-run 必须保持**零副作用**：不启动 daemon、不创建 `.work/`、不生成 YAML、不 bind/unbind 任何设备、不修改 `/sys`。

## 3. 修复后 dry-run 必须在当前静息状态下 PASS

这是本任务的核心验收点。

## 4. 退出码语义明确化

当前 dry-run BLOCKED 时 `exit 1`。修复后需区分：

- 全部前置条件满足 → `exit 0`，打印 `Dry-run result: PASS`；
- 存在真实错误（binary 缺失、endpoint 被占、GPU 不可查询、已挂载的 raw NVMe 等）→ 非零退出，打印 `Dry-run result: BLOCKED` 或 `FAIL`，并逐条列出原因。

在 README 中写明退出码约定，便于 CI 区分「环境不满足」与「harness 逻辑错误」。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tests/service_client/run_attach_smoke.sh`
- `/data/home/ryeqiu/Tutti/tests/service_client/README.md`
- `/data/home/ryeqiu/Tutti/tests/service_client/RESULTS.md`
- `/data/home/ryeqiu/Tutti/tests/service_client/.gitignore`（新建）
- `/data/home/ryeqiu/Tutti/chat/round4/result2.md`

其中 `chat/round4/result2.md` 保存本 session 的完整原始执行结果。

运行时日志、时间戳目录、临时 YAML、临时 mount-work 目录只能写到：

`/data/home/ryeqiu/Tutti/tests/service_client/.work/`

禁止修改或创建任何其他文件。尤其禁止修改：

- `/data/home/ryeqiu/Tutti/tests/service_client/generate_attach_config.py`（**本轮不改**，它的 mount/holder 检查已经是正确的）
- `/data/home/ryeqiu/Tutti/.gitignore`（仓库根的，**不要动**；只允许新建 `tests/service_client/.gitignore`）
- 根或 `tutti/` 的任何 CMake/source
- `/data/home/ryeqiu/Tutti/tutti/**`
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round4/result2.md` 外的任何文件
- libnvm、NVMeService、CUDA、kernel module 源码
- 现有 daemon/client 二进制

禁止提交 Git commit。

# 附带修复（必做）

## 1. 新建 `tests/service_client/.gitignore`

已确认问题：运行产物只有 `*.log` 被仓库根 `.gitignore` 的全局规则意外命中，而 `attach_config.yaml` **未被忽略**，已经在 untracked 列表里堆积了多份（每次 run 一份）。

新建 `/data/home/ryeqiu/Tutti/tests/service_client/.gitignore`，至少忽略：

```text
.work/
__pycache__/
```

理由：`.work/` 是每轮 run 的运行产物目录，不是源码交付物。`__pycache__/` 是 Python 字节码缓存，已经连续两轮被人工清理，应当机器化忽略。

## 2. harness 调用 generator 时避免生成字节码

`run_attach_smoke.sh` 调用 generator 处改用：

```bash
python3 -B <generator> --output ...
```

`-B` 抑制 `.pyc` 写入。这与上一条是双重保险，不要只做一个。

# 已知硬件与映射（供 harness 使用，不要改动映射本身）

```text
GPU IDs: 0, 1, 2, 3

device_id=0  PCI=0000:08:00.0  block=/dev/nvme0n1  ns=1  allowed GPU=0
device_id=1  PCI=0000:4b:00.0  block=/dev/nvme1n1  ns=1  allowed GPU=1
device_id=2  PCI=0000:57:00.0  block=/dev/nvme2n1  ns=1  allowed GPU=2
device_id=3  PCI=0000:63:00.0  block=/dev/nvme3n1  ns=1  allowed GPU=3
```

注意 harness 已有 `resolve_block_device()` 会同时尝试 `nvmeXn1` 与 `snvmeXn1` 两种命名（snvme 驱动 re-bind 后为后者）。保留该逻辑。

gRPC endpoint：`127.0.0.1:50051`

固定二进制：

```text
/data/home/ryeqiu/Tutti/build/bin/tutti_daemon
/data/home/ryeqiu/Tutti/build/bin/nvmeservice_client
```

可覆盖变量：`DAEMON_BIN`、`CLIENT_BIN`、`SUDO`、`ENDPOINT`。

# execute 模式：保持现有行为，不要改坏

execute 模式上一轮已验证 **PASS**，四组 device/GPU attach 全部成功。本任务**不是**重做 execute 模式，而是修 dry-run。

因此：

- execute 模式的既有逻辑（generator 调用、daemon 启动与 30s 等待、`--list-only`、四组 `--device N --cuda N --count 2 --hold 2 --skip-io`、`timeout 90s`、SIGTERM 取真实 PID、clean exit 验证、symlink 检查、trap 清理）**必须原样保留**；
- 你可以重跑一次 execute 模式确认没有回归，但**不要**为了「改进」而重构它。

如果你重跑 execute 模式，所有 client 必须带 `--skip-io`；禁止 block read/write。

# 绝对禁止

- `insmod` / `rmmod` / `modprobe`
- 签名、安装或替换内核模块
- 手工 bind / unbind PCI 设备（写 `/sys/bus/pci/drivers/*/bind` 或 `unbind`）
- mount / umount / 格式化 / 分区
- block read/write smoke
- 省略 `--skip-io`
- 写 `/dev/nvme*n1`
- 修改 `/etc`、`/sys`、`/dev`
- 杀已有 daemon 或抢占已占用 endpoint
- 使用交互式 sudo
- 修改 `sys_config.yaml`

如果 daemon 启动失败、设备被占用、模块缺失或 raw NVMe 状态不安全，必须报告 `BLOCKED`，不得伪造 PASS。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 语法自检

```bash
bash -n tests/service_client/run_attach_smoke.sh
```

## 2. 修复前先记录现状（证明 BUG 真实存在）

```bash
tests/service_client/run_attach_smoke.sh; echo "exit=$?"
```

记录修复前的真实 BLOCKED 输出与退出码。

## 3. 修复后 dry-run 必须 PASS

```bash
tests/service_client/run_attach_smoke.sh; echo "exit=$?"
```

要求 `Dry-run result: PASS` 且 `exit=0`。

同时验证零副作用：运行前后对比

```bash
ls tests/service_client/.work/logs/ 2>/dev/null | wc -l
```

dry-run 前后该数量必须相同（dry-run 不得新建时间戳目录）。

## 4. 验证危险状态仍被拦截（关键回归）

必须证明放松后**没有**把真实危险状态也放过。用只读方式构造验证，例如临时把某个 `BLOCK_DEVICES` 项指向一个**确实已挂载**的 block device（可用当前系统上任意已挂载设备，通过环境变量或临时改脚本内数组的方式，**不要真的去挂载任何东西**），确认 dry-run 报错并非零退出。

在结果中说明你用什么方式验证，以及验证是否覆盖了「存在且已挂载 → 必须报错」这条路径。如果无法在不产生副作用的前提下验证，明确记录该限制，不要伪造。

## 5. execute 模式回归（可选但推荐）

```bash
tests/service_client/run_attach_smoke.sh --execute; echo "exit=$?"
```

若执行，要求仍为 PASS，且四组 attach 与 daemon clean exit 均无回归。若不执行，在结果中说明未做 execute 回归。

## 6. 忽略规则生效验证

```bash
git check-ignore -v tests/service_client/.work/logs
git status --short --untracked-files=all -- tests/service_client | cat
```

要求 `.work/` 被忽略；`git status` 中不再出现 `attach_config.yaml` 或 `.pyc`。

## 7. Hygiene

```bash
git diff --check -- tests/service_client
```

注意 `git diff --check` 不覆盖未跟踪新文件；对所有改动/新增文件自行做尾随空白与 EOF newline 检查。确认本 session 只触碰允许列表。

# `RESULTS.md` 要求

更新 `RESULTS.md`，除保留既有记录结构外，必须新增/更正：

- 明确记载「四个目标 NVMe 的稳态是 UNBOUND，由 daemon 在运行期 bind 创建 block device」这一事实；
- 明确纠正上一轮「dry-run BLOCKED 是 SIGKILL 导致的偶发环境问题」的错误归因；
- 记录修复后的 dry-run PASS 结果与退出码约定。

# `README.md` 要求

必须写明：

- 四个目标 NVMe 的静息态是 UNBOUND，这是**正常**的，不是故障；
- block device 只在 daemon 运行期存在；
- dry-run 与 execute 各自检查什么、退出码含义；
- `.work/` 是运行产物目录，已被忽略，可安全删除。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. 修复前的 dry-run 阻塞已被真实记录；
2. dry-run 的 block-device 检查改为条件式，缺失为 INFO、存在则仍校验 mount/holders；
3. 修复后 dry-run 在当前静息状态下 `PASS` 且 `exit=0`；
4. dry-run 仍然零副作用（无新时间戳目录、未起 daemon、未生成 YAML）；
5. 「存在且已挂载」路径仍被拦截（或明确记录无法安全验证的限制）；
6. 退出码语义已实现并在 README 记录；
7. 新建 `tests/service_client/.gitignore` 且 `.work/`、`__pycache__/` 被忽略；
8. generator 调用改用 `python3 -B`；
9. `generate_attach_config.py` 零改动；
10. `RESULTS.md` 纠正了 UNBOUND 归因错误；
11. execute 模式无回归（或明确说明未做回归）；
12. 未修改允许列表外文件，未改变内核模块状态，未手工 bind/unbind；
13. 空白检查通过。

# 结果落盘要求

完成任务和验收后，必须把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round4/result2.md`

至少包含：

1. 修改/新增文件列表
2. 修复前 dry-run 的真实输出与退出码
3. dry-run 门禁的具体改法说明（改了哪几处判据）
4. 修复后 dry-run 完整输出与退出码
5. 零副作用验证结果（时间戳目录数量前后对比）
6. 「存在且已挂载」拦截路径的验证方式与结果（或限制说明）
7. `.gitignore` 与 `python3 -B` 的验证结果
8. `generate_attach_config.py` 未改动的证明
9. execute 模式回归结果（若执行）
10. 文件边界与空白检查结果
11. 最终 `PASS`、`FAIL` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 如果命令失败，写入真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round4/result2.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
