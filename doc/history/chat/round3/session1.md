# TASK T-008

你是一名资深系统测试工程师。你只负责修正并实际执行 Tutti 历史 `NVMeService` 的 client-mode 非破坏性 attach smoke。你看不到任何其他上下文，本 prompt 已包含全部硬件、程序接口、正确语义和安全边界。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 本轮背景

前一轮 `T-003` harness 有两个已确认的技术错误，本任务必须修正：

1. **mount target 要求写反了。**
   当前 raw NVMe `/dev/nvmeXn1` **不应**作为普通文件系统挂载。`nvmes[].mount_path` 只是 daemon 在其下创建 `GPU<n>` 目录和 GPU-view symlink 的工作基目录，不是该 NVMe block device 的 `findmnt` 结果。
2. **client 二进制名找错了。**
   `nvmeservice_client_example` 是 CMake target 名，真实可执行文件是：

   `/data/home/ryeqiu/Tutti/build/bin/nvmeservice_client`

T-004 已修复根构建 duplicate-target；当前上述 daemon 与 client 二进制均已构建成功。你现在需要在保留非破坏性约束的前提下，让四组 device/GPU attach 生命周期真正跑起来。

# 测试目标

对 GPU 0-3 与 NVMe device_id 0-3 做：

- daemon 启动；
- `ListDevices`；
- `Connect`；
- `nvm_ctrl_attach_client`；
- `nvm_create_group` / `nvm_destroy_group`；
- client heartbeat 至少维持到可验证的周期；
- `Disconnect`；
- daemon 干净退出。

所有 client 必须带 `--skip-io`；禁止 block read/write。

# 已确认的真实程序语义

## 1. `mount_path` 不是 raw NVMe 的 mount target

配置结构：

- `gpus[].mount_path`：每个 GPU 的 view 目录；
- `nvmes[].mount_path`：daemon 创建 `GPU<n>` 子目录的基目录。

运行时关系：

```text
<nvmes[].mount_path>/GPU<gpu_id>        # daemon 创建的目录
<gpus[].mount_path>/ssnvmeN             # daemon 创建的 symlink
```

client 的 `Connect` 返回 `mount_path=<gpus[].mount_path>/ssnvmeN`。

因此：

- 禁止用 `findmnt --source /dev/nvmeXn1` 的结果生成配置；
- 禁止要求 raw NVMe 必须已挂载；
- 应当相反：确认 `/dev/nvmeXn1` **没有普通 mount target**；
- 也应检查 `/sys/class/block/nvmeXn1/holders` 为空，避免明显被其他 block consumer 占用。

## 2. `--skip-io` 实际验证范围

client 的 `--skip-io` 路径实际执行：

```text
cudaSetDevice
nvm_ctrl_attach_client
nvm_create_group
nvm_destroy_group
nvm_ctrl_free_client
```

它不会：

- 创建实际 user queue pair；
- map SQ/CQ 或 data buffer；
- 执行 NVMe read/write；
- 证明业务数据面 IO 正确。

因此结果中只能声称“非 IO attach/group 生命周期通过”，不能声称真实 IO path 通过。

## 3. Heartbeat 验证边界

client 在 `Connect` 后启动 heartbeat thread；heartbeat loop 会立即建立一次 bidi stream、写入 allocation id、等待 daemon echo。daemon 对未知 allocation 会返回 `LEASE_REVOKED` notice，client 会打印：

```text
lease revoked by daemon for allocation ...
```

本 harness 应把该 notice 视为 heartbeat 失败证据。不要依赖单纯看到配置字符串 `heartbeat` 就声称 RPC 成功。

为了至少覆盖一次实际 heartbeat tick，建议生成 lease：

```yaml
lease:
  heartbeat_interval_sec: 1
  timeout_sec: 5
```

每个 attach client 使用 `--hold 2`。

## 4. daemon 的真实副作用

daemon 启动会作为 owner 执行 libnvm B3：chrdev create、可选 kernel IOQ cap、bind、probe；正常退出会释放 owner 状态。

因此这不是“零副作用”测试，但允许在本机已安装历史模块的部署状态下执行。**绝对禁止**加载、卸载、重签或修改内核模块。只能通过现有模块和现有 daemon/client 执行。

# 已知硬件与映射

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

# 固定二进制

Daemon：

```text
/data/home/ryeqiu/Tutti/build/bin/tutti_daemon
```

Client：

```text
/data/home/ryeqiu/Tutti/build/bin/nvmeservice_client
```

调用：

```bash
tutti_daemon --config <yaml>

nvmeservice_client \
  --endpoint 127.0.0.1:50051 \
  --device <id> \
  --cuda <id> \
  --count 2 \
  --hold 2 \
  --skip-io
```

Daemon 成功日志应包含：

```text
tutti_daemon listening on 127.0.0.1:50051
Owned devices:
```

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tests/service_client/generate_attach_config.py`
- `/data/home/ryeqiu/Tutti/tests/service_client/run_attach_smoke.sh`
- `/data/home/ryeqiu/Tutti/tests/service_client/README.md`
- `/data/home/ryeqiu/Tutti/tests/service_client/RESULTS.md`
- `/data/home/ryeqiu/Tutti/chat/round3/result1.md`

其中 `chat/round3/result1.md` 保存本 session 的完整原始执行结果。

运行时日志、时间戳目录、临时 YAML、临时 mount-work 目录只能写到：

`/data/home/ryeqiu/Tutti/tests/service_client/.work/`

这些运行时产物不是源码交付物，不要求加入 Git；不得写到仓库根目录或 `/mnt`。

禁止修改或创建任何其他文件。尤其禁止修改：

- 根或 `tutti/` 的任何 CMake/source
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round3/result1.md` 外的任何文件
- libnvm、NVMeService、CUDA、kernel module 源码
- 现有 daemon/client 二进制

禁止提交 Git commit。

# 配置生成器要求

固定接口：

```bash
python3 generate_attach_config.py --output <path>
```

行为：

1. 检查四个 PCI sysfs path 存在；
2. 检查四个 `/dev/nvmeXn1` 存在；
3. 使用只读命令确认每个 `/dev/nvmeXn1` **没有**普通 mount target；
4. 使用只读检查确认 `/sys/class/block/nvmeXn1/holders` 不存在 open consumer；
5. 解析 `--output`，取：

```text
<run_dir> = dirname(output)
```

6. 创建：

```text
<run_dir>/mount_work/gpus/gpu0
<run_dir>/mount_work/gpus/gpu1
<run_dir>/mount_work/gpus/gpu2
<run_dir>/mount_work/gpus/gpu3
<run_dir>/mount_work/nvmes/nvme0
<run_dir>/mount_work/nvmes/nvme1
<run_dir>/mount_work/nvmes/nvme2
<run_dir>/mount_work/nvmes/nvme3
```

这些目录是普通测试工作目录，不是 filesystem mount target。生成 YAML：

```yaml
grpc:
  endpoint: "127.0.0.1:50051"

gpus:
  - id: 0
    mount_path: "<run_dir>/mount_work/gpus/gpu0"
  # ... gpu1-3

nvmes:
  - pci_addr: "0000:08:00.0"
    mount_path: "<run_dir>/mount_work/nvmes/nvme0"
    namespace_id: 1
    kernel_ioq_cap: 32
    allowed_gpus: [0]
  # ...其余固定映射

queue_pool:
  default_per_client: 2
  max_per_client: 4

lease:
  heartbeat_interval_sec: 1
  timeout_sec: 5
```

只用 Python 标准库，不使用 PyYAML。若任一 raw NVMe 已挂载、holder 非空、PCI/block 缺失或输出目录不合法，明确失败，禁止猜测路径。

# Harness 要求

固定接口：

```bash
./run_attach_smoke.sh            # dry-run，完全无副作用
./run_attach_smoke.sh --execute  # 真正运行
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
CLIENT_BIN=/data/home/ryeqiu/Tutti/build/bin/nvmeservice_client
ENDPOINT=127.0.0.1:50051
SUDO="sudo -n"
```

## dry-run 必须检查

- 四个 GPU 可经 `nvidia-smi -i <id>` 查询；
- 四个 PCI BDF 存在；
- 四个 block devices 存在；
- 四个 block devices 没有普通 mount target；
- 四个 block device holders 为空；
- daemon/client binary 可执行；
- `127.0.0.1:50051` 未被占用；
- `sudo -n true` 可用；
- 打印完整计划命令；
- 不启动 daemon，不创建 `.work/`，不生成 YAML。

## `--execute` 必须

1. 创建 `.work/logs/<timestamp>/`；
2. 调用 generator 生成 `attach_config.yaml` 和 `mount_work/`；
3. 用 `sudo -n` 启动 daemon；
4. 最多 30 秒等待 `tutti_daemon listening on 127.0.0.1:50051` 与 `Owned devices:`；
5. 执行一次：

```bash
CLIENT_BIN --endpoint ... --list-only --skip-io
```

6. 依次执行四组：

```bash
sudo -n CLIENT_BIN --endpoint ... --device <id> --cuda <id> --count 2 --hold 2 --skip-io
```

7. 每条 client 命令使用 `timeout 90s`；
8. SIGTERM daemon，等待最多 20 秒；SIGKILL 只能是最后兜底，不得作为第一选择；
9. 无论成功失败，trap 路径都必须清理 daemon 进程；
10. daemon 退出日志必须验证 clean exit 字样；
11. 保存完整 daemon/client/harness 日志。

## client 输出验证

`ListDevices` log 必须包含：

```text
ListDevices
device_id=0
device_id=1
device_id=2
device_id=3
```

每组 attach log 至少验证：

```text
ListDevices
allocation_id
device_id     : <id>
cuda_device   : <id>
granted_queues: 2
snvme_dev
mount_path
mount->
nvm_ctrl_attach_client
nvm_create_group
nvm_destroy_group (skip-io path)
nvm_ctrl_free_client (skip-io path)
Holding session for 2s
Disconnect
Done.
```

再验证：

- 正确 `device_id`；
- 正确 `cuda_device`；
- `mount_path` 位于该次 `<run_dir>/mount_work/gpus/gpu<id>/` 下；
- 日志中**没有**：
  - `Write IO`
  - `Read IO`
  - `Write+Read+verify`
  - `mapped SQ/CQ`
  - `nvm_add_user_queue`
  - `lease revoked by daemon for allocation`
  - `Connect rejected`
  - `Connect RPC failed`
  - `Disconnect RPC failed`
  - `Disconnect rejected`

每组运行结束后，检查对应 symlink `<gpu_work>/ssnvmeN` 存在且指向 `<nvme_work>/GPU<id>`；daemon 全部停止后，记录 symlink/subdir 是否被 daemon 清理。

# 绝对禁止

- `insmod` / `rmmod` / `modprobe`
- 签名、安装或替换内核模块
- mount / umount / 格式化 / 分区
- block read/write smoke
- 省略 `--skip-io`
- 写 `/dev/nvme*n1`
- 修改 `/etc`、`/sys`、`/dev`
- 杀已有 daemon 或抢占已占用 endpoint
- 使用交互式 sudo
- 修改 `sys_config.yaml`

如果 daemon 启动失败、设备被占用、模块缺失或 raw NVMe 状态不安全，必须报告 `BLOCKED`，不得伪造 PASS。

# 自检与验收

至少执行：

```bash
cd /data/home/ryeqiu/Tutti
python3 -m py_compile tests/service_client/generate_attach_config.py
bash -n tests/service_client/run_attach_smoke.sh
tests/service_client/run_attach_smoke.sh
tests/service_client/run_attach_smoke.sh --execute
```

`dry-run` 必须 `PASS`；`--execute` 目标是 `PASS`。随后执行：

```bash
git diff --check -- tests/service_client
```

注意 `git diff --check` 不会覆盖未跟踪新文件；对四个正式测试文件和 `chat/round3/result1.md` 自行做尾随空白与 EOF newline 检查。

`RESULTS.md` 必须记录：

```text
Date
Git HEAD
Daemon binary
Client binary
Kernel/module state observed（只读命令）
Generated device mapping
Raw NVMe unmounted/holders check
Dry-run result
ListDevices result
device/gpu 0-3 各自 connect / attach / create_group / destroy_group / heartbeat / disconnect
Daemon cleanup
Overall PASS / FAIL / BLOCKED
```

如执行中暴露真实 daemon/client 缺陷，记录完整日志路径和最小复现，不要为了通过而修改源码。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. 生成器正确拒绝 raw NVMe 已挂载或 holders 非空的状态；
2. dry-run 通过且完全无副作用；
3. `--execute` 中 daemon 成功监听；
4. `ListDevices` 返回四个设备；
5. 四组 `Connect + attach + create_group + destroy_group + disconnect` 全部成功；
6. 四组均有至少一个 heartbeat window，且没有 `LEASE_REVOKED` 或 RPC 失败；
7. 没有任何 block IO、user queue 或 data buffer map marker；
8. daemon 经 SIGTERM 干净退出；
9. 运行日志完整保存在 `.work/logs/<timestamp>/`；
10. `RESULTS.md` 如实记录结果；
11. 未修改允许列表外文件，未改变内核模块状态；
12. 空白检查通过。

若任一条件不满足，写 `BLOCKED` 或 `FAIL`，记录真实原因。

# 结果落盘要求

完成任务和验收后，必须把完整原始结果直接写入：

`/data/home/ryeqiu/Tutti/chat/round3/result1.md`

至少包含：

1. 修改文件列表
2. 对旧 harness 两项错误前提的修正说明
3. dry-run 完整结果摘要
4. `--execute` 真实结果
5. 四组 device/GPU attach 逐项结果
6. heartbeat 验证结果与限制
7. daemon clean exit 结果
8. 最新运行日志目录路径
9. `RESULTS.md` 完整内容
10. 空白检查结果
11. 最终 `PASS`、`FAIL` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 不得预留或编写“总指挥验收”内容；总指挥会在结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round3/result1.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
