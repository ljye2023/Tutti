# TASK T-003

你是一名资深系统测试工程师，只负责为已经安装的历史 Tutti NVMeService 数据面建立非破坏性的 client-mode attach smoke harness。你看不到任何其他上下文，本 prompt 已包含全部硬件和测试约束。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 测试目的

在不重装、不卸载、不修改内核模块的前提下：

1. 启动历史 `examples/tutti_daemon.cpp` 对应 daemon。
2. 使用历史 `nvmeservice_client_example`。
3. 对 GPU 0-3 与 NVMe device_id 0-3 做：
   - ListDevices
   - Connect
   - attach
   - queue create/destroy
   - heartbeat
   - Disconnect
4. 必须使用 `--skip-io`。
5. 禁止任何 block read/write 验证。
6. 生成可重复执行、自动清理 daemon 的测试脚本。
7. 所有测试文件和日志位置都在 `/data/home/ryeqiu/Tutti/tests` 下。

# 已知硬件

```text
GPU IDs: 0, 1, 2, 3

device_id=0
PCI BDF=0000:08:00.0
block=/dev/nvme0n1
namespace_id=1
allowed GPU=0

device_id=1
PCI BDF=0000:4b:00.0
block=/dev/nvme1n1
namespace_id=1
allowed GPU=1

device_id=2
PCI BDF=0000:57:00.0
block=/dev/nvme2n1
namespace_id=1
allowed GPU=2

device_id=3
PCI BDF=0000:63:00.0
block=/dev/nvme3n1
namespace_id=1
allowed GPU=3
```

gRPC endpoint：

```text
127.0.0.1:50051
```

当前历史内核模块已经安装。

# 绝对禁止

- 禁止运行 `insmod`
- 禁止运行 `rmmod`
- 禁止运行 `modprobe`
- 禁止签名或安装内核模块
- 禁止格式化、分区、mount、umount
- 禁止运行任何实际 IO smoke
- 禁止省略 `--skip-io`
- 禁止写 `/dev/nvme*n1`
- 禁止修改 `sys_config.yaml`
- 禁止修改源码、CMake 或现有测试
- 禁止提交 Git commit

# 你只能创建或修改这些文件

- `/data/home/ryeqiu/Tutti/tests/service_client/generate_attach_config.py`
- `/data/home/ryeqiu/Tutti/tests/service_client/run_attach_smoke.sh`
- `/data/home/ryeqiu/Tutti/tests/service_client/README.md`
- `/data/home/ryeqiu/Tutti/tests/service_client/RESULTS.md`

禁止修改任何其他文件。

# 已知程序接口

## Daemon

候选默认路径：

```text
/data/home/ryeqiu/Tutti/build/bin/tutti_daemon
```

调用：

```bash
tutti_daemon --config <yaml>
```

成功日志包含：

```text
tutti_daemon listening on 127.0.0.1:50051
Owned devices:
```

## Client

程序名：

```text
nvmeservice_client_example
```

可能需要通过已有 build target 构建：

```bash
cmake --build /data/home/ryeqiu/Tutti/build \
  --target nvmeservice_client_example -j8
```

client 接口：

```text
--endpoint <host:port>
--device <id>
--cuda <id>
--count <n>
--hold <sec>
--list-only
--skip-io
```

安全 attach 调用必须包含：

```bash
--skip-io
```

示例：

```bash
nvmeservice_client_example \
  --endpoint 127.0.0.1:50051 \
  --device 0 \
  --cuda 0 \
  --count 2 \
  --hold 1 \
  --skip-io
```

# `generate_attach_config.py` 要求

命令接口固定：

```bash
python3 generate_attach_config.py --output <path>
```

行为：

1. 检查四个 `/sys/bus/pci/devices/<BDF>` 是否存在。
2. 检查四个 `/dev/nvmeXn1` 是否存在。
3. 使用：

```bash
findmnt -n -o TARGET --source /dev/nvmeXn1
```

发现真实 mount path。

4. 如果任一设备未挂载：
   - 明确失败
   - 禁止自动 mount
   - 禁止猜测 mount path

5. 输出 YAML：

```yaml
grpc:
  endpoint: "127.0.0.1:50051"

gpus:
  - id: 0
    mount_path: "/mnt/gpu0"
  - id: 1
    mount_path: "/mnt/gpu1"
  - id: 2
    mount_path: "/mnt/gpu2"
  - id: 3
    mount_path: "/mnt/gpu3"

nvmes:
  - pci_addr: "0000:08:00.0"
    mount_path: "<findmnt result for /dev/nvme0n1>"
    namespace_id: 1
    kernel_ioq_cap: 32
    allowed_gpus: [0]
  ...
```

四个 NVMe 全部按上述固定映射生成。

queue policy：

```yaml
queue_pool:
  default_per_client: 2
  max_per_client: 4

lease:
  heartbeat_interval_sec: 2
  timeout_sec: 10
```

只使用 Python 标准库，不依赖 PyYAML。

# `run_attach_smoke.sh` 固定接口

```bash
./run_attach_smoke.sh            # dry-run，只检查并打印计划
./run_attach_smoke.sh --execute  # 真正运行 attach smoke
```

可覆盖变量：

```text
DAEMON_BIN
CLIENT_BIN
SUDO
ENDPOINT
```

默认：

```text
DAEMON_BIN=/data/home/ryeqiu/Tutti/build/bin/tutti_daemon
ENDPOINT=127.0.0.1:50051
SUDO="sudo -n"
```

`CLIENT_BIN` 自动依次查找：

```text
/data/home/ryeqiu/Tutti/build/bin/nvmeservice_client_example
/data/home/ryeqiu/Tutti/tutti/build/bin/nvmeservice_client_example
```

找不到时：

- 尝试执行已有 build target
- 仍找不到则明确失败
- 不修改 CMake

## dry-run 必须检查

- 四个 GPU 能通过 `nvidia-smi -i <id>` 查询
- 四个 PCI BDF 存在
- 四个 block devices 存在
- 四个 block devices 有 mount target
- daemon binary 可执行
- client binary可执行，或给出构建失败
- `127.0.0.1:50051` 没有旧 daemon 占用
- `sudo -n true` 可用；不可用时只报告，不交互等待
- 输出将要执行的所有命令
- 不启动 daemon

## `--execute` 流程

1. 创建：

```text
tests/service_client/.work/
tests/service_client/.work/logs/
```

2. 生成临时 YAML。
3. 使用 `sudo -n` 启动 daemon。
4. 保存 daemon PID。
5. 设置 trap，任何退出路径都要：
   - SIGTERM daemon
   - 等待退出
   - 必要时使用有界 timeout
   - 禁止 SIGKILL 作为第一选择
6. 最多等待 30 秒，直到 daemon 日志出现 listening。
7. 执行一次：

```bash
CLIENT_BIN --endpoint ... --list-only
```

8. 依次执行四组：

```text
device=0 cuda=0
device=1 cuda=1
device=2 cuda=2
device=3 cuda=3
```

每次：

```bash
sudo -n CLIENT_BIN \
  --endpoint 127.0.0.1:50051 \
  --device <id> \
  --cuda <id> \
  --count 2 \
  --hold 1 \
  --skip-io
```

9. 每条命令使用 timeout，建议 60 秒。
10. 验证 client 输出至少包含：
    - `ListDevices`
    - `allocation_id`
    - 正确 `device_id`
    - 正确 `cuda_device`
    - `granted_queues`
    - `Disconnect`
    - `Done`
11. 验证 daemon 正常响应 SIGTERM 并打印 clean exit。
12. 保存所有日志到 `.work/logs/`。
13. 绝不执行不带 `--skip-io` 的 client。

# RESULTS.md 要求

必须记录：

```text
Date
Git HEAD
Daemon binary
Client binary
Kernel/module state observed（只读命令）
Generated device mapping
Dry-run result
ListDevices result
device/gpu 0-3 各自：
  connect
  attach/create
  heartbeat
  destroy/disconnect
Daemon cleanup
Overall PASS / FAIL / BLOCKED
```

如果因以下原因无法运行：

- 无 passwordless sudo
- binary 缺失且 build 失败
- mount 缺失
- daemon 端口占用
- 设备已经被其他进程持有

则必须写：

```text
BLOCKED
```

并记录真实错误。禁止伪造 PASS。

# 脚本风格

- Bash：`set -euo pipefail`
- 所有变量正确加引号
- 有明确 `usage`
- 不新增第三方依赖
- 不把临时 YAML 写到项目根目录
- 不覆盖已有日志；使用时间戳子目录
- 所有 destructive-looking action 前都必须由 `--execute` 显式开启
- dry-run 必须完全无副作用

# 自检

至少执行：

```bash
cd /data/home/ryeqiu/Tutti
python3 -m py_compile tests/service_client/generate_attach_config.py
bash -n tests/service_client/run_attach_smoke.sh
tests/service_client/run_attach_smoke.sh
git diff --check -- tests/service_client
```

如果环境允许，再执行：

```bash
tests/service_client/run_attach_smoke.sh --execute
```

# 输出要求

只返回一个 Markdown 代码块，包含：

1. 创建文件列表
2. dry-run 完整结果摘要
3. `--execute` 的真实结果，或 `BLOCKED` 原因
4. 四组 device/GPU attach 结果
5. daemon 是否干净退出
6. `RESULTS.md` 完整内容
7. `git diff --check` 结果

不要解释、不要寒暄、不要修改其他文件、不要加载或卸载内核模块。
