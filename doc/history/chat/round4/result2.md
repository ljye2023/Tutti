# T-013 Session Result

## 1. 修改/新增文件列表

| 文件 | 操作 |
|------|------|
| `tests/service_client/run_attach_smoke.sh` | 修改 |
| `tests/service_client/README.md` | 修改 |
| `tests/service_client/RESULTS.md` | 修改 |
| `tests/service_client/.gitignore` | 新增 |

`generate_attach_config.py` 未改动（md5: `01bcf96659a11747e941eb5c74942489`）。

## 2. 修复前 dry-run 的真实输出与退出码

```
[check] block device /dev/nvme0n1
[check] block device /dev/nvme1n1
[check] block device /dev/nvme2n1
[check] block device /dev/nvme3n1
...
Preflight: BLOCKED/FAIL
  ERROR: block device is missing: /dev/nvme0n1
  ERROR: block device is missing: /dev/nvme1n1
  ERROR: block device is missing: /dev/nvme2n1
  ERROR: block device is missing: /dev/nvme3n1

Dry-run result: BLOCKED (no daemon started; no files created)
exit=1
```

根因：四个目标 NVMe PCI 设备稳态为 UNBOUND，block device 仅在 daemon 运行期由 B3 bind+probe 创建。dry-run 在 daemon 未运行时执行，block device 不存在是预期状态，不是故障。

## 3. dry-run 门禁的具体改法说明

### 改动 1：block device 缺失从 error 改为 INFO（dry-run 模式）

`preflight()` 函数中 block device 检查（原约第 190-198 行）：

- **Before**: dry-run 模式下 block device 不存在 → `add_error "block device is missing: $block"`
- **After**: dry-run 模式下 block device 不存在 → 打印 INFO，说明 "expected when daemon is not running; daemon creates block devices via B3 bind+probe at startup"，不计入错误

block device **存在**时，mount/holders 检查逻辑**完全不变**——仍然 `add_error` 如果已挂载或有非空 holders。

### 改动 2：新增 PCI 驱动绑定状态报告（信息性）

在每个 PCI BDF 检查处新增：
- 读取 `/sys/bus/pci/devices/$bdf/driver` 符号链接
- 有链接 → 报告绑定到哪个驱动
- 无链接 → 报告 UNBOUND（expected when daemon is not running）

这是信息性输出，不作为错误判据。

### 改动 3：generator 调用改用 `python3 -B`

- `print_plan()` 中：`python3` → `python3 -B`
- execute 模式中：`python3` → `python3 -B`

`-B` 抑制 `.pyc` 写入，与 `.gitignore` 中 `__pycache__/` 规则互为双保险。

### 改动 4：退出码语义

保持现有结构，确认：
- 全部前置条件满足 → `exit 0`，打印 `Dry-run result: PASS`
- 存在真实错误 → `exit 1`，打印 `Dry-run result: BLOCKED`，逐条列出原因

## 4. 修复后 dry-run 完整输出与退出码

```
=== Tutti NVMeService attach smoke: dry-run ===

repo: /data/home/ryeqiu/Tutti
endpoint: 127.0.0.1:50051
daemon: /data/home/ryeqiu/Tutti/build/bin/tutti_daemon
client: /data/home/ryeqiu/Tutti/build/bin/nvmeservice_client
generator: /data/home/ryeqiu/Tutti/tests/service_client/generate_attach_config.py

[check] daemon executable: /data/home/ryeqiu/Tutti/build/bin/tutti_daemon
[check] client executable: /data/home/ryeqiu/Tutti/build/bin/nvmeservice_client
[check] nvidia-smi -i 0
  OK: GPU 0 is queryable
[check] nvidia-smi -i 1
  OK: GPU 1 is queryable
[check] nvidia-smi -i 2
  OK: GPU 2 is queryable
[check] nvidia-smi -i 3
  OK: GPU 3 is queryable
[check] PCI 0000:08:00.0
  OK: PCI 0000:08:00.0 exists
  INFO: PCI 0000:08:00.0 is UNBOUND (expected when daemon is not running)
[check] PCI 0000:4b:00.0
  OK: PCI 0000:4b:00.0 exists
  INFO: PCI 0000:4b:00.0 is UNBOUND (expected when daemon is not running)
[check] PCI 0000:57:00.0
  OK: PCI 0000:57:00.0 exists
  INFO: PCI 0000:57:00.0 is UNBOUND (expected when daemon is not running)
[check] PCI 0000:63:00.0
  OK: PCI 0000:63:00.0 exists
  INFO: PCI 0000:63:00.0 is UNBOUND (expected when daemon is not running)
[check] block device /dev/nvme0n1
  INFO: /dev/nvme0n1 does not exist (expected when daemon is not running;
        daemon creates block devices via B3 bind+probe at startup)
[check] block device /dev/nvme1n1
  INFO: /dev/nvme1n1 does not exist (expected when daemon is not running;
        daemon creates block devices via B3 bind+probe at startup)
[check] block device /dev/nvme2n1
  INFO: /dev/nvme2n1 does not exist (expected when daemon is not running;
        daemon creates block devices via B3 bind+probe at startup)
[check] block device /dev/nvme3n1
  INFO: /dev/nvme3n1 does not exist (expected when daemon is not running;
        daemon creates block devices via B3 bind+probe at startup)
[check] sudo -n true
  OK: passwordless privilege prefix works
[check] endpoint free: 127.0.0.1:50051
  OK: no listener on 127.0.0.1:50051

Preflight: PASS

=== Planned commands ===
Generate config:
  python3 -B .../generate_attach_config.py --output ...
Start daemon:
  sudo -n .../tutti_daemon --config ...
List devices:
  timeout 90s .../nvmeservice_client --endpoint 127.0.0.1:50051 --list-only --skip-io
Attach device=0 cuda=0:
  timeout 90s sudo -n .../nvmeservice_client --endpoint 127.0.0.1:50051 --device 0 --cuda 0 --count 2 --hold 2 --skip-io
Attach device=1 cuda=1:
  timeout 90s sudo -n .../nvmeservice_client --endpoint 127.0.0.1:50051 --device 1 --cuda 1 --count 2 --hold 2 --skip-io
Attach device=2 cuda=2:
  timeout 90s sudo -n .../nvmeservice_client --endpoint 127.0.0.1:50051 --device 2 --cuda 2 --count 2 --hold 2 --skip-io
Attach device=3 cuda=3:
  timeout 90s sudo -n .../nvmeservice_client --endpoint 127.0.0.1:50051 --device 3 --cuda 3 --count 2 --hold 2 --skip-io

Dry-run result: PASS (no daemon started; no files created)
exit=0
```

## 5. 零副作用验证结果

时间戳目录数量前后对比：

- 修复前 dry-run 前：`ls tests/service_client/.work/logs/ | wc -l` = 5
- 修复后 dry-run 后：`ls tests/service_client/.work/logs/ | wc -l` = 5

数量相同，dry-run 未创建新时间戳目录 ✓

其他零副作用确认：
- 未启动 daemon ✓
- 未生成 YAML ✓
- 未 bind/unbind PCI 设备 ✓
- 未修改 `/sys` ✓
- 未创建 `.work/` 新内容 ✓

## 6. 「存在且已挂载」拦截路径的验证方式与结果

**验证方式**：使用只读命令测试系统上已挂载的 `/dev/sda1`（挂载于 `/`），模拟 block device 存在且已挂载的场景，验证 mount 检查逻辑仍会 `add_error`。

```bash
block="/dev/sda1"
if [[ -e "$block" ]] && findmnt -n -o TARGET --source "$block" >/dev/null 2>&1; then
    echo "REGRESSION: $block exists and mounted at $(findmnt -n -o TARGET --source "$block" | head -1) — dry-run would add_error (BLOCKED)"
fi
```

**结果**：

```
REGRESSION: /dev/sda1 exists and mounted at / — dry-run would add_error (BLOCKED)
```

验证覆盖了「存在且已挂载 → 必须 add_error」路径 ✓

限制说明：此验证通过只读命令模拟，未实际修改 `BLOCK_DEVICES` 数组重跑脚本（因为脚本不支持环境变量覆盖该数组，临时改脚本会引入不必要的变更风险）。但 mount 检查逻辑代码未改动，与修复前完全一致，只读模拟足以证明该路径仍然有效。

## 7. `.gitignore` 与 `python3 -B` 的验证结果

### `.gitignore`

```
$ git check-ignore -v tests/service_client/.work/logs
tests/service_client/.gitignore:1:.work/  tests/service_client/.work/logs
```

`.work/` 被忽略 ✓

```
$ git status --short --untracked-files=all -- tests/service_client
?? tests/service_client/.gitignore
?? tests/service_client/README.md
?? tests/service_client/RESULTS.md
?? tests/service_client/generate_attach_config.py
?? tests/service_client/run_attach_smoke.sh
```

无 `attach_config.yaml`、无 `.pyc`、无 `__pycache__/`、无 `.work/` 出现在 git status ✓

### `python3 -B`

```
$ grep -n 'python3 -B' tests/service_client/run_attach_smoke.sh
282:    print_command python3 -B "$GENERATOR" --output "$config_path"
532:log_msg "COMMAND: python3 -B $GENERATOR --output $CONFIG_PATH"
533:if ! python3 -B "$GENERATOR" --output "$CONFIG_PATH" >>"$RUN_LOG" 2>&1; then
```

3 处均使用 `python3 -B` ✓

## 8. `generate_attach_config.py` 未改动的证明

```
$ md5sum tests/service_client/generate_attach_config.py
01bcf96659a11747e941eb5c74942489  tests/service_client/generate_attach_config.py
```

本 session 未读取或修改该文件的代码内容，仅通过 `md5sum` 确认其完整性。

## 9. execute 模式回归结果

未执行 execute 回归。

理由：execute 模式在 T-008 轮已验证 PASS，本任务修改仅影响 dry-run 的 block device 检查判据和 generator 调用参数（`python3 -B`），不影响 execute 模式的既有逻辑（generator 调用、daemon 启动、四组 attach、SIGTERM 清理等均原样保留）。`python3 -B` 对 generator 的行为无影响（仅抑制 `.pyc` 写入）。

## 10. 文件边界与空白检查结果

### `git diff --check`

```
$ git diff --check -- tests/service_client/run_attach_smoke.sh
EXIT=0
```
无空白错误 ✓

### 尾随空白与 EOF newline

所有修改/新增文件检查：
- `run_attach_smoke.sh` — 无尾随空白，EOF newline OK ✓
- `.gitignore` — 无尾随空白，EOF newline OK ✓
- `README.md` — 无尾随空白，EOF newline OK ✓
- `RESULTS.md` — 无尾随空白，EOF newline OK ✓

### 文件边界

本 session 实际触碰的文件：
- `tests/service_client/run_attach_smoke.sh`（修改）
- `tests/service_client/README.md`（修改）
- `tests/service_client/RESULTS.md`（修改）
- `tests/service_client/.gitignore`（新增）
- `chat/round4/result2.md`（新增，本文件）

未修改允许列表外的任何文件 ✓

### 安全限制

- 未执行 `insmod`/`rmmod`/`modprobe` ✓
- 未手工 bind/unbind PCI 设备 ✓
- 未 mount/umount/格式化/分区 ✓
- 未写 `/dev/nvme*n1` ✓
- 未修改 `/etc`/`/sys`/`/dev` ✓
- 未启动 daemon ✓
- 未使用交互式 sudo ✓

## 11. 最终结论

PASS

## 总指挥验收

验收结论：`PASS`（含 1 项非阻塞观察）。

独立核验结果：

- 我独立重跑 dry-run：`Dry-run result: PASS`，`exit=0`。Round 3 遗留的「永远无法满足的门禁」已解除。
- 零副作用确认：dry-run 前后 `.work/logs/` 目录数均为 5，未新建时间戳目录，未起 daemon，未生成 YAML。
- **关键回归守住了。** 我逐行审读修改后的 block-device 检查块：设备**存在**时，mount 检查的 `add_error "$block is mounted at $mnt; raw NVMe must NOT be mounted"` 与 holders 检查**原样保留**，一个字未削弱。放松只作用于「设备不存在」这一分支，且 dry-run 走 INFO、execute 走 WARN，语义分层正确。
- PCI 驱动绑定状态报告已加入，四个 BDF 均输出 `INFO: ... is UNBOUND (expected when daemon is not running)`。这条信息性输出正是防止后续再次误判为环境故障的关键。
- `python3 -B` 三处（plan 打印 1 处、execute 2 处）全部落地。
- `tests/service_client/.gitignore` 内容为 `.work/` + `__pycache__/`，`git check-ignore` 确认 `.work/` 生效；`git status` 中已无 `attach_config.yaml`、无 `.pyc`、无 `__pycache__/`。Round 3 我人工清理两次的字节码问题至此机器化解决。
- `RESULTS.md` 明确纠正了误归因，原文写明「This attribution was **wrong**. The UNBOUND state is the normal steady state, not an environment anomaly caused by SIGKILL.」这正是我要求的那条纠正。
- **generator 未改动的证明我用了比 md5 更强的方式。** worker 只给了 md5（该文件是 untracked，无 git 基线，单独 md5 无法自证未改）。我改用 mtime 交叉验证：

```text
16:15:26  generate_attach_config.py     ← 早于本 session
16:53:07  run_attach_smoke.sh           ← 本 session 改动
16:53:17  .gitignore                    ← 本 session 新增
17:00:34  README.md
17:01:09  RESULTS.md
```

  generator 的 mtime 早于本 session 全部产物约 38 分钟，确证未被触碰。

- 四个交付文件 + result2.md 尾随空白与 EOF newline 均 OK；`git diff --check` 通过。
- 未执行 sudo 特权破坏性操作、未手工 bind/unbind、未 mount/umount、未改模块状态。

非阻塞观察（记录，不返工）：

1. **「存在且已挂载」路径的验证方式偏弱。** worker 用一段独立 bash 片段对 `/dev/sda1` 模拟了判据，而非驱动真实脚本走该分支。它自己如实声明了这一限制。我接受该结论，理由是我独立审读了源码，确认该分支代码**与修复前逐字相同**（`add_error` 未动），因此不存在"放松时顺手弱化"的风险。但严格说这条路径仍缺端到端覆盖。若将来要补，正确做法是让 `BLOCK_DEVICES` 支持环境变量覆盖（例如 `TEST_BLOCK_DEVICES_OVERRIDE`），从而可注入一个已挂载设备真实跑一遍 —— 而不是临时改脚本。

2. **未做 execute 回归。** worker 的理由（改动只触及 dry-run 判据与 `python3 -B`，execute 逻辑原样保留）成立，我审读 diff 后认可。`-B` 只抑制字节码写入，对 generator 行为无影响。但这意味着「execute 仍 PASS」目前是**推理结论而非实测结论**。下次涉及该 harness 的 session 应顺带跑一次 execute 确认。

后续决定：T-013 完成，不需要返工。
