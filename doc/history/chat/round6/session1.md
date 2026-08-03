# TASK T-020

你是一名资深 Linux 系统 C/C++ 工程师。你只负责一件事：清理 libnvm 中一处**已被源码级确证为死代码**的 ioctl 调用，修好那条无信息量的错误日志，并用真实 attach 回归验证噪声消失且功能无回退。你看不到任何其他上下文，本 prompt 已包含完整现状、根因、精确改动点与验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 执行时机（重要）

**本任务必须单独执行，不与任何其他 session 并发。** 它会启动 NVMe daemon 并绑定四个 PCI 设备（独占硬件），同时修改 libnvm 源码（并发编译会读到半成品）。

开始前确认：

```bash
ps -eo pid,etime,cmd | grep -E '[c]make|[c]test|[t]utti_daemon' | head
```

若有命中，报告 `BLOCKED` 并列出进程，不要开始。

# 背景：已确证的根因（不需要你重新诊断）

daemon 每次退出时用户态打印四条（每设备一次）：

```text
ioctl_queue_helper err is -1
```

上一轮已完成源码级定位，以下结论**可直接采信**：

## 调用链

`backends/local/nvme/libnvm/src/ctrl.cpp:222-232`：

```c
void nvm_ctrl_free(nvm_ctrl_t* ctrl)
{
    if (ctrl != NULL)
    {   
        nvm_queue_clear(ctrl);        // <-- 第 226 行，本任务要移除
        nvm_device_unbind(ctrl);
        struct controller* container = _nvm_container_of(ctrl, struct controller, handle);
        nvm_chrdev_remove(container->device->fd_control, &ctrl->pdev_addr);
        _nvm_ctrl_put(container);
    }
}
```

`nvm_queue_clear` 最终落到 `ioctl_queue_helper(ctrl, 0, NVM_CLEAR_IOQ_NUM)`（`src/linux/device.cpp:373`）。注意它在 `nvm_device_unbind` 与关闭 fd **之前**执行，故失败与 unbind/fd 无关。

## 内核侧根因

内核模块 `tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017/pci.c` 的 ioctl dispatch **没有** `case NVM_CLEAR_IOQ_NUM` 分支。请求落到 `default:`；而 `NVM_IOCTL_TYPE` 为 `0x80`（`backends/local/nvme/libnvm/include/ioctl.h:14`）≠ `'N'`（0x4e），故走非 `'N'` 支路返回 `-EINVAL`，并打印 `pr_notice("snvme: unknown /dev/ssnvme ioctl ...")`。

**即：除用户态 4 条日志外，dmesg 里同样有 4 条内核噪声。本任务要同时消除两侧。**

## 为什么可安全删除

1. **内核从未实现该 handler**，任何内核版本组合下都只会失败，不可能有副作用。
2. **真正的清理由后续两步完成**：`nvm_device_unbind()` 解绑 PCI，`_nvm_ctrl_put()` 关闭 fd，内核 `snvm_dev_release` 级联清理该 fd 上所有 group 与 DMA map。队列清理不依赖此 ioctl。
3. **`ioctl_queue_helper` 全部三个入口都指向未实现的 ioctl，且无一在 daemon 功能路径上**：`NVM_CLEAR_IOQ_NUM`（`device.cpp:373`，本次断开）、`NVM_SET_IOQ_NUM`（`:335`，daemon 不调用）、`NVM_SET_SHARE_REG`（`:376`，daemon 不调用）。daemon 的 `kernel_ioq_cap` 走**另一条已实现**的 `NVM_SET_KERNEL_IOQ_CAP`（内核 `pci.c:6068` 有 handler），不受影响。

**删除该调用不影响任何功能，这是本任务成立的前提。**

## 那条日志为什么没用

`backends/local/nvme/libnvm/src/linux/device.cpp:326-329`：

```c
    if (err < 0){
        printf("ioctl_queue_helper err is %d\n",err);
        return errno;
    }
```

打印的是 `err`（`ioctl()` 返回值，失败时**恒为 -1**），真正的错误码在 `errno` 里却被 `return` 而从未打印。所以它永远只输出 `-1`。

# 关键前提：libnvm 有两份副本，当前字节完全相同

```text
backends/local/nvme/libnvm/src/ctrl.cpp
tutti/device_manager/nvme/libnvm/src/ctrl.cpp
        -> md5 均为 97420e98963de1949498b2499422d5f3

backends/local/nvme/libnvm/src/linux/device.cpp
tutti/device_manager/nvme/libnvm/src/linux/device.cpp
        -> md5 均为 3bb87df4e82fc11e9cab5bce5bace960
```

行号在两份中一致（`ctrl.cpp:226`、`device.cpp:327`）。

**哪份被编译：** 根 `CMakeLists.txt:177-180` 只 glob `backends/local/nvme/libnvm/`。`tutti/device_manager/nvme/libnvm/CMakeLists.txt:29` 定义同名 target，但从仓库根 configure 时 `TUTTI_BUILD_HARDWARE_STACK` 被强制 OFF，那份不参与根构建。

**要求：两份必须同步修改，改完后 md5 仍须两两相等。只改一份会造成副本漂移，是明确的失败。**

（两份副本本身是技术债，应择机收敛。那是独立任务，**本任务不要做**，也不要删除任何一份。）

# 精确改动（2 处 × 2 份 = 4 个编辑点）

## 改动 A：断开死调用

在**两份** `src/ctrl.cpp` 的 `nvm_ctrl_free()` 中删除 `nvm_queue_clear(ctrl);`，原位留注释说明三点：内核未实现该 ioctl、清理实际由 unbind + fd close 完成、若将来内核实现 handler 需恢复调用。

其余三行调用顺序与内容**完全不变**。

该函数上方文档注释块第 202 行现写：

```c
 *   nvm_queue_clear      -- legacy admin queue scrub
```

已与实际行为不符，请更新该行使之如实。**不要重写整个注释块**，只改这一处。

## 改动 B：让日志真正可用

在**两份** `src/linux/device.cpp` 的 `ioctl_queue_helper()` 中把那行 `printf` 改为打印真正有用的信息：**至少包含 `errno` 数值、`strerror(errno)` 文本、本次请求的 ioctl type**。

要求：

- 用 `nvm_error(...)` 而非 `printf` —— 该宏在同文件第 284 行已被使用，无需新增 include；
- 若使用 `strerror`，确认所需 header 已 include；**若没有**可新增，但须在结果中说明新增了什么、为什么；
- **必须先把 `errno` 存入局部变量再使用**（日志宏内部可能调用其他库函数而改写 `errno`），并按该局部变量 `return`；
- 函数签名、返回值语义与 `switch` 结构不变。

**不要修改** `default: return EINVAL;`（第 322-323 行）。它返回正数 `EINVAL`、失败分支返回 `errno`（也是正数）、成功返回 0，三种约定确实混在一起，是既有瑕疵，但改它会改变调用方可见语义，**超出本任务范围**。如认为该改，记录建议但不要动。

## 改动纪律

这是生产驱动代码，必须外科手术式改动：

- 只改上述两处，不重排、不重构、不「顺手改进」；
- **不要**改 `ioctl_queue_helper` 三个 `case` 分支的内部逻辑；
- **不要**改 `nvm_queue_clear` / `nvm_queue_setup` / `nvm_queue_set` / `nvm_queue_share` 的定义（保留它们，只是不再被 `nvm_ctrl_free` 调用）；
- **不要**改任何内核模块源码；
- **不要**改 `include/ioctl.h` 或任何 ioctl 号定义；
- 保持文件现有缩进与注释风格；
- 注意 `ctrl.cpp:225` 行尾有**既有**尾随空白。**不要修正它**（范围外），也不要新增任何尾随空白。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/backends/local/nvme/libnvm/src/ctrl.cpp`
- `/data/home/ryeqiu/Tutti/backends/local/nvme/libnvm/src/linux/device.cpp`
- `/data/home/ryeqiu/Tutti/tutti/device_manager/nvme/libnvm/src/ctrl.cpp`
- `/data/home/ryeqiu/Tutti/tutti/device_manager/nvme/libnvm/src/linux/device.cpp`
- `/data/home/ryeqiu/Tutti/chat/round6/result1.md`

构建产物只能写入既有的 `/data/home/ryeqiu/Tutti/build/`（重编现有目录，不新建）。attach 回归的运行产物由 harness 自行写入 `tests/service_client/.work/`，是其既定行为，允许。

禁止修改或创建任何其他文件。尤其禁止：修改内核模块源码或重编内核模块；修改 `tests/service_client/**` 任何文件（harness 与 generator 都不许动）；修改任何 `CMakeLists.txt`；修改 `tutti/include/**`、`tutti/spi/**`、`tutti/bindings/**`；修改 `.gitignore`；修改 `chat/**` 中除 `chat/round6/result1.md` 外的文件；删除任何一份 libnvm 副本。

禁止提交 Git commit。

# 安全限制

**绝对禁止** `insmod` / `rmmod` / `modprobe`。`snvme`、`snvme_core`、`phoenixfs` 当前已加载，本任务结束时状态必须与开始时**完全一致**。

**允许**的 `sudo` 仅限：运行 `tests/service_client/run_attach_smoke.sh --execute`（内部需特权 bind PCI 与启动 daemon）、读取 `dmesg`。

**禁止**：手工 `bind`/`unbind` PCI 设备、`mount`/`umount`、写 `/sys` 或 `/proc`、对 `/dev/nvme*` 或 `/dev/ssnvme*` 做读写 IO、执行 block IO 数据面测试。

attach 回归本身**不做** block IO（harness 走 `--skip-io` 路径），这是预期的。

若 daemon 启动失败或设备状态异常，**不要**手工修复绑定状态。停下、记录真实状态、报告 `BLOCKED`。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 改动前基线

```bash
md5sum backends/local/nvme/libnvm/src/ctrl.cpp \
       tutti/device_manager/nvme/libnvm/src/ctrl.cpp \
       backends/local/nvme/libnvm/src/linux/device.cpp \
       tutti/device_manager/nvme/libnvm/src/linux/device.cpp
grep -E '^(snvme|snvme_core|phoenixfs) ' /proc/modules
```

记录四个 md5 与模块基线。

## 2. 两份副本同步性

```bash
md5sum backends/local/nvme/libnvm/src/ctrl.cpp tutti/device_manager/nvme/libnvm/src/ctrl.cpp
md5sum backends/local/nvme/libnvm/src/linux/device.cpp tutti/device_manager/nvme/libnvm/src/linux/device.cpp
```

每一对必须相等。**不相等即失败**，修正后再继续。

## 3. 完整 diff

```bash
git diff -- backends/local/nvme/libnvm/src/ctrl.cpp \
            backends/local/nvme/libnvm/src/linux/device.cpp \
            tutti/device_manager/nvme/libnvm/src/ctrl.cpp \
            tutti/device_manager/nvme/libnvm/src/linux/device.cpp | cat
```

完整 diff 记入结果，确认改动量与预期一致。

## 4. 重编

```bash
cmake --build build -j8 2>&1 | tail -30
ls -l build/lib/libnvm.so build/bin/tutti_daemon build/bin/nvmeservice_client
```

要求**零新增编译告警或错误**。与本改动相关的告警必须修掉；无关的既有告警如实记录并说明为何无关。用 mtime 确认二进制真的更新了。

若某条构建命令超过 10 分钟无进展，中止并记录，不要无限等待。

## 5. dry-run 仍通过

```bash
tests/service_client/run_attach_smoke.sh
```

必须 `Dry-run result: PASS`、`exit=0`、零副作用（不新建时间戳日志目录、不起 daemon）。

## 6. 内核噪声基线（在步骤 7 之前先做）

```bash
sudo dmesg | grep -c 'unknown /dev/ssnvme ioctl'
```

记录该数值作为基线。若 `dmesg` 缓冲区可能回卷导致基线不可靠，改用 `dmesg -T` 按时间窗口过滤，并说明你采用的方法。

## 7. attach execute 回归（核心验证）

```bash
tests/service_client/run_attach_smoke.sh --execute
```

要求全部满足：

- 整体 PASS；
- 四组 attach 全部成功，每组 client 日志含完整五步（`cudaSetDevice` → `nvm_ctrl_attach_client` → `nvm_create_group` → `nvm_destroy_group` → `nvm_ctrl_free_client`）；
- `granted_queues` 与配置中的 `default_per_client` 相符；
- daemon 经 SIGTERM 干净退出（日志含 `Shutting down...` 与 `exited cleanly`）；
- daemon 退出后各 GPU 目录的 `ssnvme*` symlink 全部被清理。

## 8. 用户态噪声消失（核心验证）

```bash
RUN=$(ls -1d tests/service_client/.work/logs/*/ | tail -1)
echo "run dir: $RUN"
grep -rn 'ioctl_queue_helper' "$RUN" && echo '!!! STILL PRESENT' || echo 'user-space noise: GONE'
```

必须报告 `GONE`。

## 9. 内核侧噪声消失（核心验证）

```bash
sudo dmesg | grep -c 'unknown /dev/ssnvme ioctl'
```

必须与步骤 6 基线**相等**（本次 run 零新增内核噪声）。两个数值都记入结果。

## 10. 功能无回退的正面证据

```bash
RUN=$(ls -1d tests/service_client/.work/logs/*/ | tail -1)
grep -nE 'Connect rejected|Connect RPC failed|Disconnect RPC failed|Disconnect rejected|lease revoked by daemon for allocation|Write IO|Read IO|mapped SQ/CQ|nvm_add_user_queue' "$RUN"/client_device_*.log \
  && echo '!!! MARKER FOUND' || echo 'negative markers: ABSENT (correct)'
```

必须 `ABSENT`。前四项缺席证明 attach/detach 无回退，后四项缺席证明未意外触发 block IO 或 user queue。

## 11. 环境状态复原

```bash
pgrep -af tutti_daemon || echo '(no daemon running)'
ls /dev/ssnvme* 2>/dev/null || echo '(no /dev/ssnvme*)'
grep -E '^(snvme|snvme_core|phoenixfs) ' /proc/modules
```

要求：无残留 daemon；三个模块的 live 状态与步骤 1 基线**完全一致**（名称、大小、引用计数、地址均须一致）。

## 12. Hygiene

```bash
git diff --check -- backends/local/nvme/libnvm/src/ctrl.cpp \
                    backends/local/nvme/libnvm/src/linux/device.cpp \
                    tutti/device_manager/nvme/libnvm/src/ctrl.cpp \
                    tutti/device_manager/nvme/libnvm/src/linux/device.cpp
git status --short --untracked-files=all | head -20
```

对四个改动文件检查 EOF newline；确认未新增尾随空白（`ctrl.cpp:225` 的既有空白除外）；确认只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. 死调用已从**两份** `ctrl.cpp` 移除，原位有说明注释，其余三行调用不变；
2. 上方文档注释块的相关描述已更新；
3. **两份** `device.cpp` 的日志已改为打印 `errno` 数值 + `strerror` 文本 + ioctl type，使用 `nvm_error`，且 `errno` 先存局部变量；
4. 两对文件 md5 各自相等（副本未漂移）；
5. 重编零新增告警，三个二进制 mtime 已更新；
6. dry-run PASS 且零副作用；
7. attach execute 回归整体 PASS，四组 attach 五步完整，daemon 干净退出，symlink 清理；
8. 用户态 `ioctl_queue_helper` 日志在本次 run 中零出现；
9. 内核 `unknown /dev/ssnvme ioctl` 计数无新增；
10. 全部禁止标记缺席（功能无回退）；
11. 模块状态与基线完全一致，无残留 daemon；
12. 未修改允许列表外文件，未执行任何模块加载/卸载操作；
13. 空白与 EOF newline 检查通过。

如果 attach 回归失败，**不要**为了让它通过而扩大修改范围（尤其不要去改 harness 或内核模块）。回滚很简单（把两行改回去），如需回滚请记录回滚后的验证结果并报告 `BLOCKED`。

如果**只有**内核噪声计数不符预期（用户态已消失、功能无回退），不要伪造 PASS。如实记录两次计数，报告 `PASS WITH ANOMALY` 并给出你对差异来源的分析（例如是否有其他进程也在调用该 ioctl）。

# 结果落盘要求

把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round6/result1.md`

至少包含：

1. 并发与残留 daemon 检查结果
2. 改动前四个 md5 + 模块基线
3. 四个编辑点的具体改动说明（每处改了什么、为什么）
4. 改动后两对 md5 相等的证明
5. 完整 `git diff`
6. 重编输出（含告警情况）与三个二进制的 mtime
7. dry-run 结果
8. 内核噪声基线计数（步骤 6）
9. attach execute 完整结果（四组 attach 的五步证据、granted_queues、daemon 退出、symlink 清理）
10. 用户态噪声扫描结果
11. 内核噪声计数对照（前后两个数值）
12. 禁止标记扫描结果
13. 环境复原核验（daemon、设备节点、三个模块）
14. 文件边界与空白检查
15. 若新增了任何 header include，说明原因
16. 最终 `PASS` / `PASS WITH ANOMALY` / `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 命令失败就写真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round6/result1.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
