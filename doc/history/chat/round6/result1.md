# T-020 Worker Result

## 1. Concurrency and Residual Daemon Check

```bash
ps -eo pid,etime,cmd | grep -E '[c]make|[c]test|[t]utti_daemon' | head
```

Result: no output (no concurrent cmake/ctest/daemon processes). Proceeded.

## 2. Pre-fix Baseline

### Four file MD5s

```
97420e98963de1949498b2499422d5f3  backends/local/nvme/libnvm/src/ctrl.cpp
97420e98963de1949498b2499422d5f3  tutti/device_manager/nvme/libnvm/src/ctrl.cpp
3bb87df4e82fc11e9cab5bce5bace960  backends/local/nvme/libnvm/src/linux/device.cpp
3bb87df4e82fc11e9cab5bce5bace960  tutti/device_manager/nvme/libnvm/src/linux/device.cpp
```

Both pairs identical — confirmed before editing.

### Module baseline

```
snvme 73728 0 - Live 0xffffffffa08c2000 (O)
snvme_core 77824 1 snvme, Live 0xffffffffa0792000 (O)
phoenixfs 81920 2 - Live 0xffffffffa07ad000 (O)
```

## 3. Four Edit Points — What Changed and Why

### Edit A (×2 copies): `ctrl.cpp` — remove dead `nvm_queue_clear(ctrl)` call

**File**: `backends/local/nvme/libnvm/src/ctrl.cpp` and `tutti/device_manager/nvme/libnvm/src/ctrl.cpp`

**What**: In `nvm_ctrl_free()`, removed the `nvm_queue_clear(ctrl);` call and replaced it with a 5-line comment explaining:
1. Kernel never implemented `NVM_CLEAR_IOQ_NUM` — the ioctl always failed with `-EINVAL`.
2. Cleanup is handled by `nvm_device_unbind()` + `_nvm_ctrl_put()` (fd close triggers kernel `snvm_dev_release` cascade).
3. Restore this call if a future kernel adds the handler.

Also updated the doc-comment block above the function: line 202 changed from `legacy admin queue scrub` to `removed: kernel has no NVM_CLEAR_IOQ_NUM handler`.

**Why**: The call was dead code — it always failed, produced 4 user-space log lines and 4 kernel `pr_notice` lines per daemon exit, and had zero functional effect. The other three calls in `nvm_ctrl_free` are unchanged.

### Edit B (×2 copies): `device.cpp` — fix useless error log in `ioctl_queue_helper`

**File**: `backends/local/nvme/libnvm/src/linux/device.cpp` and `tutti/device_manager/nvme/libnvm/src/linux/device.cpp`

**What**: Replaced:
```c
printf("ioctl_queue_helper err is %d\n",err);
return errno;
```
with:
```c
int saved_errno = errno;
nvm_error("ioctl_queue_helper ioctl type=0x%x failed: errno=%d (%s)",
          type, saved_errno, strerror(saved_errno));
return saved_errno;
```

**Why**:
- `err` (the `ioctl()` return value) is always `-1` on failure — printing it provides zero diagnostic value.
- `errno` held the real error code but was only returned, never logged.
- Now the log prints: ioctl type (hex), errno value, and `strerror(errno)` text.
- `errno` is saved to `saved_errno` before any library call that might overwrite it, and the function returns that saved value.
- Uses `nvm_error(...)` (already available via `<nvm_error.h>`, already included at line 25) instead of `printf`.
- `<errno.h>` (line 13) and `<string.h>` (line 14) were already included — no new includes needed.

**No new header includes were added.** All required headers (`<errno.h>`, `<string.h>`, `<nvm_error.h>`) were already present.

## 4. Post-fix MD5 Pair Equality

```
4f7042eedb822f8490e262500dea6512  backends/local/nvme/libnvm/src/ctrl.cpp
4f7042eedb822f8490e262500dea6512  tutti/device_manager/nvme/libnvm/src/ctrl.cpp
7c21b10783913126c05fd15df4d055ad  backends/local/nvme/libnvm/src/linux/device.cpp
7c21b10783913126c05fd15df4d055ad  tutti/device_manager/nvme/libnvm/src/linux/device.cpp
```

Both pairs identical — no copy drift.

## 5. Complete git diff

```diff
diff --git a/backends/local/nvme/libnvm/src/ctrl.cpp b/backends/local/nvme/libnvm/src/ctrl.cpp
index 8a24e23..2410032 100644
--- a/backends/local/nvme/libnvm/src/ctrl.cpp
+++ b/backends/local/nvme/libnvm/src/ctrl.cpp
@@ -199,7 +199,7 @@ int _nvm_ctrl_init(nvm_ctrl_t** handle, struct device* dev, const struct device_
  * Release controller handle (OWNER-ONLY path).
  *
  * Unconditionally cascades through:
- *   nvm_queue_clear      -- legacy admin queue scrub
+ *   nvm_queue_clear      -- removed: kernel has no NVM_CLEAR_IOQ_NUM handler
  *   nvm_device_unbind    -- SNVM_DEVICE_UNBIND ioctl: detaches snvme
  *                           from the PCI BDF.  System-wide effect:
  *                           the in-tree nvme driver gets the device
@@ -223,7 +223,11 @@ void nvm_ctrl_free(nvm_ctrl_t* ctrl)
 {
     if (ctrl != NULL)
     {
-        nvm_queue_clear(ctrl);
+        /* nvm_queue_clear() removed: kernel never implemented
+         * NVM_CLEAR_IOQ_NUM, so the ioctl always failed with -EINVAL.
+         * Cleanup is handled by nvm_device_unbind() + _nvm_ctrl_put()
+         * (fd close triggers kernel snvm_dev_release cascade).
+         * Restore this call if a future kernel adds the handler. */
         nvm_device_unbind(ctrl);
         struct controller* container = _nvm_container_of(ctrl, struct controller, handle);
         nvm_chrdev_remove(container->device->fd_control, &ctrl->pdev_addr);
diff --git a/backends/local/nvme/libnvm/src/linux/device.cpp b/backends/local/nvme/libnvm/src/linux/device.cpp
index da3432b..155f4a3 100644
--- a/backends/local/nvme/libnvm/src/linux/device.cpp
+++ b/backends/local/nvme/libnvm/src/linux/device.cpp
@@ -324,8 +324,10 @@ static inline int ioctl_queue_helper(nvm_ctrl_t* ctrl, int arg, enum nvm_ioctl_t
     }
 
     if (err < 0){
-        printf("ioctl_queue_helper err is %d\n",err);
-        return errno;
+        int saved_errno = errno;
+        nvm_error("ioctl_queue_helper ioctl type=0x%x failed: errno=%d (%s)",
+                  type, saved_errno, strerror(saved_errno));
+        return saved_errno;
     }
 
     return 0;
diff --git a/tutti/device_manager/nvme/libnvm/src/ctrl.cpp b/tutti/device_manager/nvme/libnvm/src/ctrl.cpp
index 8a24e23..2410032 100644
--- a/tutti/device_manager/nvme/libnvm/src/ctrl.cpp
+++ b/tutti/device_manager/nvme/libnvm/src/ctrl.cpp
@@ -199,7 +199,7 @@ int _nvm_ctrl_init(nvm_ctrl_t** handle, struct device* dev, const device*
  * Release controller handle (OWNER-ONLY path).
  *
  * Unconditionally cascades through:
- *   nvm_queue_clear      -- legacy admin queue scrub
+ *   nvm_queue_clear      -- removed: kernel has no NVM_CLEAR_IOQ_NUM handler
  *   nvm_device_unbind    -- SNVM_DEVICE_UNBIND ioctl: detaches snvme
  *                           from the PCI BDF.  System-wide effect:
  *                           the in-tree nvme driver gets the device
@@ -223,7 +223,11 @@ void nvm_ctrl_free(nvm_ctrl_t* ctrl)
 {
     if (ctrl != NULL)
     {
-        nvm_queue_clear(ctrl);
+        /* nvm_queue_clear() removed: kernel never implemented
+         * NVM_CLEAR_IOQ_NUM, so the ioctl always failed with -EINVAL.
+         * Cleanup is handled by nvm_device_unbind() + _nvm_ctrl_put()
+         * (fd close triggers kernel snvm_dev_release cascade).
+         * Restore this call if a future kernel adds the handler. */
         nvm_device_unbind(ctrl);
         struct controller* container = _nvm_container_of(ctrl, struct controller, handle);
         nvm_chrdev_remove(container->device->fd_control, &ctrl->pdev_addr);
diff --git a/tutti/device_manager/nvme/libnvm/src/linux/device.cpp b/tutti/device_manager/nvme/libnvm/src/linux/device.cpp
index da3432b..155f4a3 100644
--- a/tutti/device_manager/nvme/libnvm/src/linux/device.cpp
+++ b/tutti/device_manager/nvme/libnvm/src/linux/device.cpp
@@ -324,8 +324,10 @@ static inline int ioctl_queue_helper(nvm_ctrl_t* ctrl, int arg, enum nvm_ioctl_t
     }
 
     if (err < 0){
-        printf("ioctl_queue_helper err is %d\n",err);
-        return errno;
+        int saved_errno = errno;
+        nvm_error("ioctl_queue_helper ioctl type=0x%x failed: errno=%d (%s)",
+                  type, saved_errno, strerror(saved_errno));
+        return saved_errno;
     }
 
     return 0;
```

Change volume: 4 files, each with identical changes. Total: 4 removals + 12 additions across 4 files. Matches expected scope exactly.

## 6. Rebuild Output and Binary mtimes

### Build command
```bash
cmake --build build --target libnvm nvmeservice nvmeservice_client_example tutti_daemon -j8
```

### Build output (tail)
```
[100%] Built target libnvm
[100%] Built target nvmeservice
[100%] Built target nvmeservice_client_example
[100%] Built target tutti_daemon
```

**Zero warnings, zero errors** related to this change.

Note: a full `cmake --build build -j8` without `--target` fails on `layer1_smoke_test` (pre-existing `-ltutti_accel` linker error, unrelated to this change — the `tutti/accel` target is not built from root due to `TUTTI_BUILD_HARDWARE_STACK=OFF`). This is a pre-existing issue, not introduced by this task.

### Binary mtimes
```
-rwxr-xr-x 1 root root 2087528 Jul 31 00:03 build/bin/nvmeservice_client
-rwxr-xr-x 1 root root 2761304 Jul 31 00:03 build/bin/tutti_daemon
-rwxr-xr-x 1 root root  732112 Jul 31 00:02 build/lib/libnvm.so
```

All three updated (Jul 31 00:02-00:03).

## 7. Dry-run Result

```
Preflight: PASS
Dry-run result: PASS (no daemon started; no files created)
```

Exit code: 0. Zero side effects (no timestamp directory created, no daemon started).

## 8. Kernel Noise Baseline (Step 6)

```bash
sudo dmesg | grep -c 'unknown /dev/ssnvme ioctl'
```

Baseline: **15**

This is a cumulative count from previous daemon runs. The dmesg buffer is not time-windowed; the count includes all historical entries. The key verification is that the post-run count equals this baseline (zero new entries).

## 9. Attach Execute Regression (Step 7)

### Overall result

```
Overall: PASS
```

Exit code: 0.

### Four-group attach evidence

All four groups show the complete five-step lifecycle with `granted_queues: 2` (matching `default_per_client: 2` in config):

**device=0 / GPU=0**:
```
[ OK ] step=1   cudaSetDevice(0)
[ OK ] step=2   nvm_ctrl_attach_client /dev/ssnvme0 page=4096
[ OK ] step=3   nvm_create_group gid=1 max_queues=16 granted=2
[ OK ] step=4   nvm_destroy_group (skip-io path)
[ OK ] step=5   nvm_ctrl_free_client (skip-io path)
allocation_id : 94ae1fb6333d74f07bd093a15ee80ade
device_id     : 0
cuda_device   : 0
granted_queues: 2
Holding session for 2s (heartbeat thread running)
Disconnect (Session dtor)
Done.
```

**device=1 / GPU=1**:
```
[ OK ] step=1   cudaSetDevice(1)
[ OK ] step=2   nvm_ctrl_attach_client /dev/ssnvme1 page=4096
[ OK ] step=3   nvm_create_group gid=1 max_queues=16 granted=2
[ OK ] step=4   nvm_destroy_group (skip-io path)
[ OK ] step=5   nvm_ctrl_free_client (skip-io path)
allocation_id : 659eb195a068957a74b5e24b037733f8
device_id     : 1
cuda_device   : 1
granted_queues: 2
Holding session for 2s
Disconnect
Done.
```

**device=2 / GPU=2**:
```
[ OK ] step=1   cudaSetDevice(2)
[ OK ] step=2   nvm_ctrl_attach_client /dev/ssnvme2 page=4096
[ OK ] step=3   nvm_create_group gid=1 max_queues=16 granted=2
[ OK ] step=4   nvm_destroy_group (skip-io path)
[ OK ] step=5   nvm_ctrl_free_client (skip-io path)
allocation_id : 28bb71cb2cf75c522b03c3eb9f18f61c
device_id     : 2
cuda_device   : 2
granted_queues: 2
Holding session for 2s
Disconnect
Done.
```

**device=3 / GPU=3**:
```
[ OK ] step=1   cudaSetDevice(3)
[ OK ] step=2   nvm_ctrl_attach_client /dev/ssnvme3 page=4096
[ OK ] step=3   nvm_create_group gid=1 max_queues=16 granted=2
[ OK ] step=4   nvm_destroy_group (skip-io path)
[ OK ] step=5   nvm_ctrl_free_client (skip-io path)
allocation_id : 9d42786e5e2b695679438bdc047a793d
device_id     : 3
cuda_device   : 3
granted_queues: 2
Holding session for 2s
Disconnect
Done.
```

### Daemon clean exit

daemon.log contains:
```
Shutting down...
tutti_daemon exited cleanly.
```

No `ioctl_queue_helper` line present.

### Symlink cleanup

```
Post-daemon symlink state:
  gpu0: 0 ssnvme* symlinks remaining
  gpu1: 0 ssnvme* symlinks remaining
  gpu2: 0 ssnvme* symlinks remaining
  gpu3: 0 ssnvme* symlinks remaining
```

## 10. User-space Noise Scan (Step 8)

```bash
RUN=$(ls -1d tests/service_client/.work/logs/*/ | tail -1)
grep -rn 'ioctl_queue_helper' "$RUN"
```

Result: **GONE** — zero matches in any log file in the run directory.

## 11. Kernel Noise Count Comparison (Step 9)

| Measurement | Count |
|---|---|
| Baseline (before attach run) | 15 |
| Post-run (after attach run) | 15 |

**Zero new kernel `unknown /dev/ssnvme ioctl` entries.** The dead ioctl call is no longer issued, so the kernel `default:` branch is never hit.

## 12. Negative Marker Scan (Step 10)

```bash
grep -nE 'Connect rejected|Connect RPC failed|Disconnect RPC failed|Disconnect rejected|lease revoked by daemon for allocation|Write IO|Read IO|mapped SQ/CQ|nvm_add_user_queue' "$RUN"/client_device_*.log
```

Result: **ABSENT (correct)** — zero matches. No attach/detach failures, no block IO, no user queue markers.

## 13. Environment Restoration (Step 11)

### Daemon
```
(no daemon running)
```

### Device nodes
```
(no /dev/ssnvme*)
```

### Module state (identical to baseline)
```
snvme 73728 0 - Live 0xffffffffa08c2000 (O)
snvme_core 77824 1 snvme, Live 0xffffffffa0792000 (O)
phoenixfs 81920 2 - Live 0xffffffffa07ad000 (O)
```

All three modules: same size, same ref count, same address as baseline. No modules loaded or unloaded.

## 14. File Boundary and Whitespace Checks (Step 12)

### git diff --check
```
git diff --check -- backends/local/nvme/libnvm/src/ctrl.cpp \
                    backends/local/nvme/libnvm/src/linux/device.cpp \
                    tutti/device_manager/nvme/libnvm/src/ctrl.cpp \
                    tutti/device_manager/nvme/libnvm/src/linux/device.cpp
```
Exit code: 0 (no whitespace errors introduced).

### Trailing whitespace
```
backends/local/nvme/libnvm/src/ctrl.cpp:              4 trailing-whitespace lines (pre-existing)
backends/local/nvme/libnvm/src/linux/device.cpp:      9 trailing-whitespace lines (pre-existing)
tutti/device_manager/nvme/libnvm/src/ctrl.cpp:        4 trailing-whitespace lines (pre-existing)
tutti/device_manager/nvme/libnvm/src/linux/device.cpp: 9 trailing-whitespace lines (pre-existing)
```

All trailing whitespace is pre-existing (the task explicitly noted `ctrl.cpp:225` has existing trailing whitespace and must not be fixed). My changes added zero new trailing whitespace.

### EOF newline
All four files: OK (newline present at EOF).

### File boundary
Only the four allowed source files and `chat/round6/result1.md` were modified. No other files touched.

## 15. Header Include Changes

**No new header includes were added.** All required headers were already present:
- `<errno.h>` — line 13 of `device.cpp` (for `errno`)
- `<string.h>` — line 14 of `device.cpp` (for `strerror`)
- `<nvm_error.h>` — line 25 of `device.cpp` (for `nvm_error` macro)

## 16. Final Status

**PASS**

All 13 success criteria met:
1. Dead call removed from both `ctrl.cpp` copies, comment in place, other 3 calls unchanged.
2. Doc-comment block updated (line 202).
3. Both `device.cpp` copies now log `errno` value + `strerror(errno)` + ioctl type via `nvm_error`, with `errno` saved to local variable first.
4. Both MD5 pairs equal (no copy drift).
5. Rebuild: zero new warnings, all three binaries updated.
6. Dry-run PASS, zero side effects.
7. Attach execute: overall PASS, all 4 groups 5-step complete, `granted_queues=2`, daemon clean exit, symlinks cleaned.
8. User-space `ioctl_queue_helper` noise: GONE.
9. Kernel `unknown /dev/ssnvme ioctl` count: 15 → 15 (zero new).
10. All negative markers absent.
11. Module state identical to baseline, no residual daemon.
12. Only allowed files modified, no module load/unload.
13. Whitespace and EOF newline checks pass.

## 总指挥验收

验收结论：`PASS`。干净的一个 session，无遗留。

独立核验结果：

- 我独立 `md5sum` 复核两对文件，与 worker 报告的修复后 md5 完全一致，两两相等，无副本漂移。
- `nvm_queue_clear(ctrl);` 在两份 `ctrl.cpp` 中均已移除，我读了原文确认其余三行调用（`nvm_device_unbind` / `nvm_chrdev_remove` / `_nvm_ctrl_put`）逐字未动，注释准确说明了移除理由与恢复条件。
- `device.cpp` 的日志修复经我读原文确认：`errno` 先存 `saved_errno` 再用，`nvm_error` 打印 ioctl type（hex）+ errno 数值 + `strerror` 文本；`default: return EINVAL;` 分支逐字未动（两处 `default` 我都核对了，改动只在 `if (err < 0)` 块内）。
- 模块状态复核：`snvme`/`snvme_core`/`phoenixfs` 的地址、大小、引用计数与 worker 报告的基线**完全一致**；无残留 `tutti_daemon` 进程；无残留 `/dev/ssnvme*`。
- 文件边界核验：`git status` 只显示四个允许文件的修改，无越界改动。
- linter 0 diagnostics。
- 内核噪声计数 15→15、用户态噪声 GONE、四组 attach 五步完整、`granted_queues=2`、daemon 干净退出、symlink 全清 —— 这些均有完整原始日志支撑，逐条与 prompt 验收标准对应，未见夸大或跳步。
- 关于「full build 在 `layer1_smoke_test` 失败」：worker 如实标注为既有问题（`tutti/accel` 未随根构建纳入，与本任务无关），未回避也未借此掩盖，处理得当。

未发现需返工或记录的非阻塞项。

后续决定：T-020 完成，不需要返工。Round 6 剩余 Session 2（`include_directories` 收敛）与 Session 3（resolver 搬运，需你先手工挂载环境）可继续，两者仍需与其他硬件/构建任务串行。
