# Tutti 硬件测试环境要点

本文件记录 Tutti 项目**硬件相关测试**的环境事实、正确用法与已知陷阱。

**目的**：这些事实分散在多个源文件里，且我（AI 助手）已经在此犯过多次错误。任何涉及 NVMe / FIEMAP / 挂载 / daemon 的任务，**先读本文件**，不要凭 `df -T .` 之类的表面观察下结论。

最后更新：2026-07-30

---

## 1. 硬件边界（不可逾越）

### 允许使用的设备

**只有这四块 NVMe 可用于 Tutti 测试：**

```text
0000:08:00.0    0000:4b:00.0    0000:57:00.0    0000:63:00.0
```

它们的**正常静息态是 UNBOUND**（无驱动绑定）。不运行 Tutti 栈时看到 UNBOUND 是**预期状态，不是故障**。

> 历史错误：Round 3 Session 1 的 worker 把 UNBOUND 误判为「之前调试 SIGKILL 导致的环境异常」，并因此写出一条永远无法满足的 dry-run 门禁。Round 4 Session 2 已修正。

### 严禁触碰的设备

| 设备 | 内容 | 说明 |
| --- | --- | --- |
| `nvme4` ~ `nvme7`<br>（`0000:d2/df/86/c5:00.0`） | 组成 `/dev/md0` (raid0, xfs)，挂在 `/mnt/nvme4` | **生产数据**，绝对不要碰 |
| `/dev/sda1` | ext4，挂 `/` | 系统盘 |
| `/dev/sda2` | vfat，挂 `/boot/efi` | |
| `/dev/sda3` | ext4，挂 `/usr/local` | |
| `/dev/sda4` | xfs，挂 `/data` | **代码仓库所在盘**，见下方警告 |

### ⚠️ `/data` 不是测试目标

`/data/home/ryeqiu/Tutti` 是**代码仓库**位置，底层是 `/dev/sda4`（系统 SATA 盘，xfs）。

**绝对不要在 `/data` 上做 FIEMAP 测试。** 在它上面采到的 `fe_physical` 是 `sda4` 分区内的偏移，与四块目标 NVMe 毫无关系，测出来的数据是**废的**。

> 历史错误：我曾执行 `df -T .` 看到 `xfs`，就误以为「本机是 xfs 环境」，并据此推翻了 `nvme_storage` 刻意 hardcode ext4 的设计决策。这是把「代码所在的盘」当成了「数据要落的盘」。

---

## 2. 两条互斥的驱动栈

同一 PCI BDF **只能绑定一个驱动**。选哪条栈决定 `fe_physical` 的语义基准。

| | 内核 `nvme` | `snvme`（Tutti 生产栈） |
| --- | --- | --- |
| 块设备 | `/dev/nvme<N>n1` | `/dev/snvme<m>n<K>` |
| 起栈方式 | `echo -n <BDF> \| sudo tee /sys/bus/pci/drivers/nvme/bind` | `tutti_daemon` 或 `LocalNvmeDirectRegistry::Open()` |
| 需要 CUDA | 否 | 是（`prime_cuda` → `cudaSetDevice`） |
| `fe_physical` 语义 | 内核 nvme 的 LBA 空间 | **snvme 的 LBA 空间** |
| 与生产 DataPath 一致 | ❌ | ✅ |

**结论：凡涉及 extent / LBA 偏移的测试，必须走 snvme 栈。** 因为将来 DataPath 提交 IO 走的是 snvme 的用户队列，extent 必须在 snvme 块设备上采集。走内核 nvme 采到的偏移与生产路径只是「大概率相同」，不是契约保证。

### 内核 nvme 驱动是内建的

`/sys/bus/pci/drivers/nvme` 存在，但 `nvme` **不出现在** `/proc/modules`。因此**不需要** `modprobe nvme`。

### 已加载的 Tutti 内核模块

```text
snvme        (Live, O)
snvme_core   (Live, O, refcount 1 by snvme)
phoenixfs    (Live)
```

`/dev/snvm_control` 存在。

**任何任务结束时，这三个模块的状态必须与开始时完全一致**（名称、大小、引用计数、地址）。禁止 `insmod` / `rmmod` / `modprobe`。

---

## 3. 块设备命名陷阱

**不要硬编码 `snvme0n1`。**

`nvme_storage/src/host_fs_backed_nvme_storage.cpp:88-97` 的注释说明：`snvme<m>n<K>` 中

- `<m>` = chrdev minor（对应 `/dev/ssnvme<m>`）
- `<K>` = 内核的 namespace **实例**计数器，**不是** NVMe NSID

`<K>` 通常是 1，但**每次重绑同一控制器都会递增** —— daemon 重启后可能变成 `snvme0n2`。

**正确做法**：运行时查 `/sys/block`（只列活的 gendisk，也能避开 udev 残留的过期 `/dev` 节点）：

```bash
ls -l /sys/block/ | grep snvme
```

生产代码 `blk_path_from_chrdev()` 就是这么做的，并会拒绝分区（`...p1`）。

---

## 4. 挂载流程：谁做什么

**关键事实：`tutti_daemon` 不做 mkfs / mount。**

已 grep 确认 `backends/local/NVMeService/src/` 与 `include/` 中零 `mkfs` / `::mount(` / `umount` 命中。它只做：

1. libnvm B3 bring-up：chrdev_create + cap + bind + probe → 创建 `/dev/ssnvme<N>`（字符设备）与 `/dev/snvme<m>n<K>`（块设备）
2. `install_gpu_symlinks()`（`nvmeservice_state.cu:199`）：建 `<gpus[].mount_path>/ssnvme<N>` → `<nvmes[].mount_path>/GPU<id>` 符号链接
3. 提供 gRPC 让 client attach

**mkfs / mount 在 `HostFsBackedNvmeStorage::bootstrap()` 里**（`nvme_storage/src/host_fs_backed_nvme_storage.cpp`）：

- `mkfs_if_needed_locked()` (`:181`)：`mkfs.ext4 -F -q <blk>`，仅当无文件系统时；受 `Config::auto_mkfs` 控制
- `mount_if_needed_locked()` (`:220`)：`mount(blk, mount_path, "ext4", 0, nullptr)`；已挂载则复用并置 `we_mounted=false`
- `umount_locked()`：仅当 `we_mounted` 为真才 umount

### ⚠️ daemon 与 nvme_storage 是两条独立且互斥的栈

两者都会 bind 同一个 BDF。**不能**「daemon 挂好、nvme_storage 来读」。要么用 daemon（然后手工 mkfs+mount），要么用 `LocalNvmeDirectRegistry` + `bootstrap()`（自己全包）。

### ⚠️ snvme 挂载必须有驻留进程

块设备由持有 controller 的进程维持。进程退出 → `nvm_ctrl_free` → unbind → 块设备消失 → 挂载点悬空。

**因此 `nvme_storage_smoke` 等冒烟程序不能用作 bringup 工具** —— 它们第 [11]/[12] 步 `shutdown()`（umount）+ `reg.Close()`（unbind）会把环境拆掉。已 grep 确认四个冒烟程序（`smoke` / `bulk` / `gpu` / `e2e_stress`）**都没有** hold / keep-alive 模式。

---

## 5. 手工挂载命令（已实测验证 2026-07-30）

### 实测确认的环境事实

```text
块设备      /dev/snvme0n1  (0000:08:00.0)   5.8T
            /dev/snvme1n1  (0000:4b:00.0)   5.8T
挂载点      /mnt/nvme1  <- /dev/snvme0n1  ext4  rw,relatime
容量        6200798752 1K-blocks, 可用 5888221164 (5.5 TiB)
fs 参数     1562805846 个 4k block, 195354624 inodes
测试目录    /mnt/nvme1/GPU0/resolver_test  (chmod 777)
```

本次实测块设备名恰好是 `snvme0n1` / `snvme1n1`（`K=1`），但**仍不可硬编码**——见第 3 节，重绑会递增。

### 挂载

**终端 1** —— 起 daemon 并保持前台：

```bash
cd /data/home/ryeqiu/Tutti
sudo ./build/bin/tutti_daemon --config sys_config.yaml
```

预期输出 `tutti_daemon listening on 127.0.0.1:50051` 与 `Owned devices:`。

按当前 `sys_config.yaml`，接管 `0000:08:00.0`（device_id=0）与 `0000:4b:00.0`（device_id=1）。

**终端 2** —— mkfs + mount：

```bash
cd /data/home/ryeqiu/Tutti

# 必须查实际名字，不要假设 snvme0n1（见第 3 节）
ls -l /sys/block/ | grep snvme
lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT | grep -i snvme

# 用上面看到的真实名字替换 <BLK>
sudo mkfs.ext4 -F -E lazy_itable_init=1,lazy_journal_init=1 /dev/<BLK>

sudo mkdir -p /mnt/nvme1
sudo mount -t ext4 /dev/<BLK> /mnt/nvme1
sudo mkdir -p /mnt/nvme1/GPU0/resolver_test
sudo chmod 777 /mnt/nvme1/GPU0/resolver_test

findmnt /mnt/nvme1
df -T /mnt/nvme1
```

mkfs 在 5.8T 盘上约需十几秒（`lazy_itable_init=1,lazy_journal_init=1` 已大幅加速；无这两个选项会慢得多）。

### 清理（顺序不能反）

```bash
sudo umount /mnt/nvme1
# 然后在终端 1 按 Ctrl-C，或：sudo pkill -TERM tutti_daemon
```

**必须先 umount 再停 daemon。** daemon 退出会 unbind，块设备消失，挂载点悬空。

### 复原核验

```bash
for b in 0000:08:00.0 0000:4b:00.0 0000:57:00.0 0000:63:00.0; do
  printf '%s driver=' "$b"
  [ -e "/sys/bus/pci/devices/$b/driver" ] \
    && basename "$(readlink -f /sys/bus/pci/devices/$b/driver)" || echo '(UNBOUND)'
done
grep -E '^(snvme|snvme_core|phoenixfs) ' /proc/modules
pgrep -af tutti_daemon || echo '(no daemon)'
findmnt /mnt/nvme4   # 生产 RAID 必须完好
```

四块设备须全部回到 UNBOUND；三个模块状态须与基线一致。

---

## 6. `sys_config.yaml` 的 mount_path 语义

**两个 `mount_path` 含义完全不同，别混。**

```yaml
gpus:
  - id: 0
    mount_path: "/mnt/gpu0"        # 每 GPU 的【视图目录】，放符号链接

nvmes:
  - pci_addr: "0000:08:00.0"
    mount_path: "/mnt/nvme1"       # snvme 块设备的【真实挂载点】
    namespace_id: 1
    kernel_ioq_cap: 32
    allowed_gpus: [0]
  - pci_addr: "0000:4b:00.0"
    mount_path: "/mnt/nvme2"
    # ...
```

daemon 建立的关系：

```text
<gpus[].mount_path>/ssnvme<N>  ->  <nvmes[].mount_path>/GPU<id>
例：/mnt/gpu0/ssnvme0          ->  /mnt/nvme1/GPU0
```

GPU 进程通过 `/mnt/gpu0/ssnvme0/` 看到「自己的」存储，在那里建文件，然后 FIEMAP 解析。

其他字段：

- `namespace_id` —— 因为 `NVM_GET_DEV_INFO` 目前不返回它，所以写在 YAML 里
- `kernel_ioq_cap` —— `NVM_SET_KERNEL_IOQ_CAP` 的 pre-bind 提示（该 ioctl 内核侧**已实现**，`pci.c:6068`）
- `allowed_gpus` —— ACL，也决定为哪些 GPU 装符号链接
- `queue_pool.default_per_client: 16`, `max_per_client: 32`
- `lease.heartbeat_interval_sec: 10`, `timeout_sec: 30`

> 注意：`tests/service_client/run_attach_smoke.sh` 刻意把 `mount_path` 指向普通工作目录（`.work/logs/<ts>/mount_work/`），因为它**只测 attach 生命周期，不建文件系统**。不要因此以为 `mount_path` 从来不是真挂载点。

---

## 7. FIEMAP 要点

### `UNWRITTEN`：生产代码刻意接受（这是既定行为，搬运时原样保留）

**ext4 上 `fallocate()` 出来的块默认就是 `FIEMAP_EXTENT_UNWRITTEN`** —— 这正是 fallocate 的语义（分配但未初始化）。

`fiemap_helper.cpp:111-119` **刻意**把 `UNWRITTEN` 排除在拒绝掩码之外，理由（原文摘要）：fallocate 出来的 extent 在首次写入前都标 UNWRITTEN，但物理 LBA 范围已分配且稳定；把它当致命错误会导致每个新 fallocate 的文件都被拒，与需求相反。

生产 `create_file` 的用法正是「只 `fallocate` 就采集」，与此一致。

**搬运原则：原样保留这个行为，不要加严。** 新 resolver 的测试应覆盖「只 fallocate 不写」的文件并期望成功。

（`fiemap_helper.h:22-25` 的头注释与 `.cpp` 实现不一致，以实现为准。记录备查，不改。）

### 拒绝掩码（照抄，7 个 flag）

```c
FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED |
FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED |
FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_DATA_TAIL
```

`FIEMAP_EXTENT_UNWRITTEN` **不在**其中（见上）。`FIEMAP_EXTENT_LAST` 是正常终止标记，不是错误。

### `fiemap_helper.cpp` 的其他行为（搬运时照抄）

- 用 `fstat().st_blksize` 取 fs block size；校验 `fs_block_size % nvme_block_size == 0`
- 调 ioctl 前先 `fsync(fd)`；`fm_flags = FIEMAP_FLAG_SYNC`
- 单轮缓冲 `kFiemapMaxExtentsPerCall = 256`
- 总量上限 `kNvmeFileHeaderMaxExtents = 124`（`nvme_file_header.h`）
- 对 `fe_physical` 与 `fe_length` 双双做 `% nvme_block_size` 对齐检查
- 多轮游标 `fe_logical + fe_length`，见 `LAST` 结束；某轮返回数少于请求数且无 `LAST` 时 `break`
- `block_size == 0` 拒绝（`:51-54`）
- 0 extent 报错（`:178`）

### ⚠️ 搬运时必须做的机械补齐：`logical_offset`

旧返回类型 `LbaExtent`（`lba_extent.h`）只有两个字段，**以 NVMe block 为单位**：

```cpp
struct LbaExtent { uint64_t start_lba; uint64_t length_blocks; };
```

**`fe_logical` 被丢弃了** —— 它在 `fiemap_helper.cpp` 里只用于错误消息（`:133`）与循环游标（`:166`），从不进入返回值。生产代码靠「extent 紧密相邻」的隐式假设，在设备侧顺序 walk 得出逻辑位置（`nvme_file_device_handle.h:105-106`）。

而 binding 的 `Extent` 需要三个字段且是**字节**语义：

```cpp
struct Extent { uint64_t logical_offset; uint64_t device_offset; uint64_t length; };
```

**搬运时直接从 FIEMAP 原始字段取：**

```text
logical_offset = ex.fe_logical      // 字节，内核给的权威值
device_offset  = ex.fe_physical     // 字节，无需 block 往返换算
length         = ex.fe_length       // 字节
```

**不要用「累加前一个 extent 的 length」推算 `logical_offset`。** 那等于先假设无空洞、再让 `validate()` 去校验有无空洞 —— 校验会永远通过，中间有洞的文件被静默接受。直接用 `fe_logical` 既简单又正确。

这是搬运的必要动作（新结构需要的字段），不是行为改动。

### 多轮调用

单次 `FS_IOC_FIEMAP` 返回的 extent 数受缓冲区大小限制。若 `fm_mapped_extents == fm_extent_count` 且最后一个 extent **没有** `LAST` 标记，必须以「上次最后一个 extent 的结束逻辑偏移」为新 `fm_start` 再次调用。

两条必须的防御：

1. 设总 extent 上限，超过报 `RESOURCE_EXHAUSTED`
2. **若某轮返回 0 个 extent 但未见 `LAST`，必须跳出报错** —— 否则死循环

### 分区偏移

`fe_physical` 是相对于**承载该文件系统的块设备**的字节偏移。

- 若 fs 建在**整个命名空间**上（无分区表）→ `fe_physical` **就是**命名空间字节偏移，无需换算 ✅
- 若 fs 建在分区上 → 需加分区起始偏移（读 `/sys/block/*/start`）

**推荐做法：在整命名空间上 mkfs，不建分区表。** 这消除整类换算错误。

### 其他常量与工具

- `kNvmeBlockSize = 4096`（`host_fs_backed_nvme_storage.cpp:33`，注释称 `matches all NVMe deployments`）
- `filefrag -v <path>` 可用，**独立**读取 extent 映射 —— 是交叉验证 FIEMAP 采集正确性的好工具（注意它默认以 fs block 为单位，需换算为字节）
- `mkfs.ext4` / `fallocate` / `filefrag` 均已安装

### `.refs/` 硬链接机制

生产路径 `create_file` 在 `fsync` + `read_extents` 后，会 `linkat` 一份硬链接到 `<mount>/.tutti/.refs/<name>.bin`。

**目的**：即使外部 `rm` 了原路径，inode 也不被释放，从而保证 LBA extent 对仍在读的 GPU kernel 持续有效。

**这与「持有 fd」的保证强度不同**，设计 owner lease 时应对比：

- 持有 fd：阻止 inode 回收，但**不阻止**同一文件被写入、打洞或 truncate
- 硬链接：同样阻止 inode 回收；同样不阻止内容被改写

两者都不能防止「文件被改写导致 extent 失效」。

### 生产路径已打通

`nvme_storage_gpu_smoke.cu` 已验证：`create_file(1 MiB)` → `nf->extents` 非空（FIEMAP 已采集）→ `write_blocking` → `read_blocking` 校验。

**因此新 resolver 的增量价值是「转译成 SPI 契约」（`ResolvedTarget` + binding payload），不是「证明 FIEMAP 能用」。**

---

## 8. libnvm 两份副本

```text
backends/local/nvme/libnvm/            <- 根 CMakeLists.txt 编译这份
tutti/device_manager/nvme/libnvm/      <- 从仓库根 configure 时不参与构建
```

当前两份**字节完全相同**：

- `src/ctrl.cpp` md5 `97420e98963de1949498b2499422d5f3`
- `src/linux/device.cpp` md5 `3bb87df4e82fc11e9cab5bce5bace960`

行号也一致。**修改必须同步两份**，改完 md5 仍须两两相等。

根 `CMakeLists.txt:177-180` 只 glob `backends/local/nvme/libnvm/`。`tutti/device_manager/nvme/libnvm/CMakeLists.txt:29` 定义同名 target，但从仓库根 configure 时 `TUTTI_BUILD_HARDWARE_STACK` 被强制 OFF（根 `CMakeLists.txt:344`）。

（两份副本是技术债，应择机收敛，但那是独立任务。）

### 已知死代码

`ioctl_queue_helper` 的**全部三个入口**都指向内核**未实现**的 ioctl：

| 入口 | 位置 | daemon 是否使用 |
| --- | --- | --- |
| `NVM_CLEAR_IOQ_NUM` | `device.cpp:373` | 经 `nvm_ctrl_free` → `nvm_queue_clear`（`ctrl.cpp:226`） |
| `NVM_SET_IOQ_NUM` | `device.cpp:335` | 否 |
| `NVM_SET_SHARE_REG` | `device.cpp:376` | 否 |

内核 `pci.c` 无 `case NVM_CLEAR_IOQ_NUM`。因 `NVM_IOCTL_TYPE = 0x80`（`ioctl.h:14`）≠ `'N'`（0x4e），落到非-`'N'` 支路返回 `-EINVAL` 并打 `pr_notice("snvme: unknown /dev/ssnvme ioctl ...")`。

**症状**：daemon 退出时用户态每设备一条 `ioctl_queue_helper err is -1`，dmesg 里同样有 4 条内核噪声。

日志本身几乎无信息量：`device.cpp:327` 打印的是 `err`（`ioctl()` 返回值，失败恒为 -1），真正的 `errno` 被 `return` 却从未打印。

`kernel_ioq_cap` 走的是**另一条已实现**的 `NVM_SET_KERNEL_IOQ_CAP`（内核 `pci.c:6068`），**不受影响**。

---

## 9. 安全检查清单

任何硬件任务开始前：

```bash
ps -eo pid,etime,cmd | grep -E '[c]make|[c]test|[t]utti_daemon' | head
grep -E '^(snvme|snvme_core|phoenixfs) ' /proc/modules
for b in 0000:08:00.0 0000:4b:00.0 0000:57:00.0 0000:63:00.0; do
  printf '%s driver=' "$b"
  [ -e "/sys/bus/pci/devices/$b/driver" ] \
    && basename "$(readlink -f /sys/bus/pci/devices/$b/driver)" || echo '(UNBOUND)'
done
lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT
df -h /data | tail -1     # 使用率已 84%，注意剩余空间
```

绝对禁止：

- `insmod` / `rmmod` / `modprobe`（模块状态必须守恒）
- 触碰 `0000:4b/57/63:00.0` 之外的任何非授权设备
- 触碰 `nvme4`~`nvme7` / `/dev/md0` / `/mnt/nvme4`（**生产数据**）
- 对 `/dev/sda*` 做任何 mkfs / mount / umount
- `umount --force` / `--lazy`
- kill 非本任务启动的进程
- 在 `/data` 上做 FIEMAP 测试

磁盘空间：`/data` 使用率 84%（可用约 68 GiB）。测试数据控制在 1-2 GiB 内并确保清理。

无论任务成功失败，**清理步骤都必须执行**；若清理失败，必须在结果开头显著位置写明「设备未复原，需人工处理」。

---

## 10. 工作定位（重要，避免越界）

**新层的工作是「分层抽象 + 代码搬运」，不是设计评审。**

- `nvme_storage/**` 等既有代码是**已在生产跑通的参考实现**。职责是把逻辑搬到新 SPI 层并跑通。
- **不要评判既有代码的对错**，不要因为觉得某策略「不够安全」就改它。原样搬运其行为。
- **不要修改既有代码。** 发现问题一两句话记录即可，不要长篇论证该怎么改。
- 唯一允许的偏离是**机械性补齐**：新结构需要的字段，若旧返回类型没带出来，就从原始数据里一并取出（如 `fe_logical`）。这是搬运的必要动作。

判断标准：改动是为了「让旧逻辑适配新接口」→ 做；为了「让旧逻辑变得更好」→ 不做。

---

## 11. 我犯过的错误（避免重复）

| 错误 | 真相 |
| --- | --- |
| 用 `df -T .` 得出「本机是 xfs」，据此说 binding 里的 `ext4` 是历史命名 | `/data` 是代码仓库所在的系统 SATA 盘。`nvme_storage` **刻意** hardcode ext4（`host_fs_backed_nvme_storage.h:46` 明确声明），因为它自己 `mkfs.ext4` 那四块盘 |
| 以为 `mount_path` 从来不是真挂载点 | `nvmes[].mount_path` 是 snvme 块设备的**真实挂载点**。attach smoke 用普通目录只是因为它不测文件系统 |
| 打算复用 `nvme_storage_smoke` 做 bringup | 它跑完 `shutdown()` + `Close()` 会 umount + unbind。四个冒烟程序都无 hold 模式 |
| 以为 daemon 会挂载文件系统 | daemon 只 bind + 建符号链接。mkfs/mount 在 `HostFsBackedNvmeStorage::bootstrap()` |
| 把 UNBOUND 当故障（Round 3 遗留） | UNBOUND 是四块目标设备的**正常静息态** |
| 假设块设备名是 `snvme0n1` | `<K>` 每次重绑递增，必须查 `/sys/block` |
| 主张改掉既有代码的 `UNWRITTEN` 接受策略 | **越界**。那是既定行为，搬运时原样保留。见第 10 节 |
