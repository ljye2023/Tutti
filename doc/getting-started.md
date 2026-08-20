# Tutti 入门指南 / Getting Started with Tutti

> 本文是面向**第一次上手 Tutti** 的完整、可照做的入门文档，覆盖：需要什么机器、
> 如何装依赖、如何编译、如何运行示例、如何用 `nsys` 看预期结果，以及常见坑。
> 所有命令默认在项目根目录执行；文中出现的 PCI BDF、挂载路径、CUDA 版本等
> **均为示例**，必须先在本机确认后再照抄。
>
> This is a complete, copy-paste-friendly onboarding guide for first-time Tutti
> users: required hardware, dependency setup, build, running the example,
> profiling with `nsys`, and common pitfalls. All commands assume the project
> root as CWD; PCI BDFs, mount paths, CUDA versions, etc. are **examples** —
> verify them on your own host first.

---

## 0. 一分钟看懂 Tutti / What Tutti is in one minute

Tutti 是一个 **GPU 为中心、SSD 支撑的 KV-cache 对象存储**：CPU 只负责发射 I/O
内核（`open/register/submit/wait`），GPU 内核自己读描述符、写 SQ、敲 doorbell、
轮询完成，直接驱动 NVMe。上层应用只面对一个稳定公共 API `StorageRuntime`。

Tutti is a **GPU-centric, SSD-backed KV-cache object store**: the CPU only
launches I/O kernels (`open/register/submit/wait`), while GPU kernels read
descriptors, write SQEs, ring doorbells, and poll completions to drive NVMe
directly. Applications only ever touch one stable public API, `StorageRuntime`.

```text
Application (uri / offset / size)
    │  open / register / submit / wait      ← CPU 每批只出现 O(1) 次 / CPU appears O(1) per batch
    ▼
StorageRuntime — 稳定公共 API / stable public API
    ▼
DataPaths — local_nvme / striped_local_nvme
    ▼
libnvm + tutti_daemon → snvme 内核模块 / snvme kernel module
    ▼
NVMe SSDs ◄──────► GPU HBM
```

---

## 1. 需要什么机器 / Hardware & topology requirements

### 1.1 硬性要求 / Hard requirements

| 要求 / Requirement | 说明 / Notes |
| --- | --- |
| NVIDIA GPU | CUDA 已验证；Metax 已有社区验证；MUSA/MACA 构建 profile 就绪 |
| NVMe SSD | 建议 1–8 块，striped 模式要求块数为 2 的幂（2/4/8） |
| Linux 内核 / kernel | 必须匹配仓库内的 `snvme-<tag>` 基线：`5.15.0` / `6.8.0` / `5.4.241-tlinux4` |

### 1.2 硬件拓扑 / Topology

GPU 与 NVMe 之间的 **拓扑距离** 直接决定性能：距离越近越好（`0` 同 PCIe switch >
`1` 同 NUMA 节点 > `2` 跨 NUMA）。Tutti 通过 `allowed_accel_ids` ACL 控制哪块 GPU
能访问哪块 NVMe。

Run the topology script to discover GPU↔NVMe affinity（用脚本探测 GPU↔NVMe 亲和性）：

```bash
sudo bash scripts/pci_topology_check.sh
# 结果打印矩阵，并写入 /mnt/sys_GPU_NVMe_topology.json
```

### 1.3 本文参考机器 / The reference machine for this guide

| 项 / Item | 值 / Value |
| --- | --- |
| OS | TencentOS Server 3.2（el8.2） |
| 内核 / kernel | `5.4.241-1-tlinux4-0017.7`（匹配 `snvme-5.4.241-1-tlinux4-0017`） |
| GPU | 8× NVIDIA H20（sm_90，96 GB HBM，驱动 580.105.08） |
| NVMe | 8× Samsung PM9A1（5.8 T，`/dev/nvme0n1..7`） |
| 示例默认 4 盘 / example default | `0000:08:00.0` `0000:4b:00.0` `0000:57:00.0` `0000:63:00.0` |

---

## 2. 安装依赖 / Install dependencies

### 2.1 一键准备编译依赖 / One-shot build deps

```bash
bash scripts/prepare_env.sh            # 可选 -j N 控制并行度
```

该脚本会：安装编译器工具链 + `protobuf`/`gRPC`/`uuid`/`yaml-cpp`/`libunwind`；
系统没有 gRPC 时自动回退 vcpkg（本机就是这种情况）；最后生成：
`CMakePresets.json` 与 `build_dependencies.tsv`（二者都是机器生成、已 gitignore）。

The script installs the toolchain + `protobuf`/`gRPC`/`uuid`/`yaml-cpp`/`libunwind`,
falls back to vcpkg when the system lacks gRPC (the case on this host), and finally
generates `CMakePresets.json` + `build_dependencies.tsv` (machine-generated, gitignored).

### 2.2 CUDA toolkit（`nvcc`）/ CUDA toolkit (`nvcc`)

需要与 GPU 驱动匹配的 CUDA toolkit（`nvcc`）。标准安装位置为 `/usr/local/cuda`
（通常是 `/usr/local/cuda-<ver>` 的符号链接）：

Install a CUDA toolkit (`nvcc`) matching your GPU driver. The standard location is
`/usr/local/cuda` (usually a symlink to `/usr/local/cuda-<ver>`):

```bash
nvcc --version            # 应打印版本；否则 CUDA toolkit 未安装 / should print a version
ls -ld /usr/local/cuda    # 标准位置 / standard location
```

CMake 会自动在标准位置找到 CUDA；只有装在非标准位置时才需额外指定
`CUDAToolkit_ROOT` / `CMAKE_CUDA_COMPILER`（见 `third_pkgs/tencent_os.md`）。

CMake finds CUDA in the standard location automatically; only a non-standard
install needs `CUDAToolkit_ROOT` / `CMAKE_CUDA_COMPILER` (see `third_pkgs/tencent_os.md`).

H20 是 sm_90，仓库默认 `CMAKE_CUDA_ARCHITECTURES=90` 已正确，通常无需改动。

H20 is sm_90; the default `CMAKE_CUDA_ARCHITECTURES=90` matches.

### 2.3 nsys（Nsight Systems，可选）/ nsys (Nsight Systems, optional)

> **`nsys` 不是必选项**：它只是 profiling 工具，仅用于第 6 节的性能分析；不装
> 也能正常编译、运行示例。**`nsys` is optional** — profiling only (§6); build and
> run work fine without it.

本机当前 **未安装 `nsys`**。如需 profiling，随 CUDA toolkit 的 `nsight-systems`
包安装，或从 NVIDIA 官网单独安装，装完确认：

Not installed on this host. To profile, install via the CUDA toolkit's
`nsight-systems` package or NVIDIA's standalone installer, then verify:

```bash
nsys --version
```

---

## 3. 编译 / Build

### 3.1 可用 preset / Available presets

| Preset | 用途 / Purpose |
| --- | --- |
| `host` | 无 GPU 的 API/SPI 契约测试 / hardware-free contract tests |
| `cuda` | CUDA 用户态栈（含示例），内核模块关闭 |
| `cuda-module` | CUDA 栈 + `snvme` 内核模块目标 |

### 3.2 完整编译（NVIDIA，含内核模块）/ Full build (NVIDIA, incl. kernel module)

一条 `cuda-module` preset 编译出「用户态示例 + snvme 内核模块 + daemon」全部产物：

One `cuda-module` preset builds everything — example + snvme kernel module + daemon:

```bash
cmake --preset cuda-module --fresh -DTUTTI_BUILD_HARDWARE_TESTS=ON
cmake --build --preset cuda-module \
  --target tutti_layerwise_kv_overlap modules tutti_daemon --parallel 8
```

产物 / outputs:

| 产物 / Artifact | 路径 / Path |
| --- | --- |
| KV-cache 示例 / example | `build/cuda-module/bin/tutti_layerwise_kv_overlap` |
| 内核模块 / kernel module | `build/cuda-module/module/snvme.ko` + `snvme-core.ko` |
| daemon | `build/cuda-module/bin/tutti_daemon` |

> 以下参数都有默认值，**无需显式指定**：`SNVME_KERNEL_VERSION`（自动匹配
> `uname -r`）、`TUTTI_P2P_BACKEND`（CUDA profile 默认 `nvidia`）、`nv-p2p.h`
> （已随仓库 vendoring，CMake 自动定位）。

> **产物需送签**：编译出的 `snvme.ko` / `snvme-core.ko` 不能直接 `insmod`，需送
> 公司签名系统后再安装加载（见 `third_pkgs/tencent_os.md`）。

### 3.3 只编译用户态（不含内核模块）/ User-space only (no kernel module)

不需要内核模块时（只跑用户态示例），用 `cuda` preset：

```bash
cmake --preset cuda --fresh -DTUTTI_BUILD_HARDWARE_TESTS=ON
cmake --build --preset cuda --target tutti_layerwise_kv_overlap --parallel 8
# 产物 / output: build/cuda/bin/tutti_layerwise_kv_overlap
```

---

## 4. 严格顺序的启动 / Strict bring-up order

块设备 `/dev/snvme*` 只在 daemon 启动后才创建，顺序不可颠倒：
**内核模块 → `tutti_daemon` → 挂载**。

The block devices exist only after daemon bring-up; the order is fixed:
**kernel module → `tutti_daemon` → mount**.

### 4.1 加载内核模块 / Load the kernel module

一条命令安装并加载（内部等价于 `insmod snvme-core.ko && insmod snvme.ko`）：

Install and load with one command (equivalent to
`insmod snvme-core.ko && insmod snvme.ko`):

```bash
cmake --build --preset cuda-module --target insmod
lsmod | grep snvme                                    # 应看到 snvme + snvme_core
ls -l /dev/snvm_control                               # 应存在
```

> 队列深度无需配置：snvme 自动取控制器最大值（`NVMe CAP.MQES + 1`，
> 数据中心 SSD 通常为 1024）。

### 4.2 准备本机 YAML / Prepare a host-local YAML

```bash
mkdir -p config/local
cp config/local_nvme_config.yaml config/local/tutti_daemon.yaml
# 编辑：只保留本机要用的 GPU/NVMe，改对 pci_addr、backing_mount_path、allowed_accel_ids
```

填写 `pci_addr` 前，先用 §1.2 的拓扑脚本确认本机有哪些 NVMe SSD 及其 PCI 地址：

Run §1.2's topology script first to discover your NVMe SSDs and their PCI addresses:

```bash
sudo bash scripts/pci_topology_check.sh
```

> **⚠️ 数据安全（务必先读）/ Data safety warning**
>
> daemon 会把配置里的 NVMe 从内核 `nvme` 驱动**解绑**，再**绑定到 `snvme`** 接管，
> 并挂载 ext4。这一步会**破坏盘上原有数据**：
> - 这些盘必须是没有重要数据的**裸盘/空盘**；
> - 已组成 **RAID（mdadm）或 LVM** 的，必须先删除阵列/卷，**退回裸盘**；
> - 切勿把系统盘、业务盘写进 `pci_addr`。
>
> The daemon unbinds the configured NVMe from the in-tree `nvme` driver, binds it
> to `snvme`, and mounts ext4 — **this destroys existing data**. Use throwaway bare
> disks only; **RAID (mdadm) / LVM must be torn down back to bare disks** first;
> never list the OS or data disks.

**队列数量上限 / queue-count ceiling**：配置 `kernel_ioq_cap` 和 `queue_pool` 时，
不能超过 NVMe 设备支持的最大 IO 队列数。用 `lspci -vvv -s <BDF>` 看
`MSI-X: Enable+ Count=` 的值，通常就是该设备支持的最大 IO 队列数：

Check the controller's max IO queues via `lspci -vvv -s <BDF>` (the
`MSI-X: Enable+ Count=` field):

```bash
lspci -vvv -s 0000:08:00.0 | grep -i 'msi-x'
# 例 / e.g.:  MSI-X: Enable+ Count=65 Masked-
# Count 通常 = 最大 IO 队列数 + 1（含 admin queue）
```

约束关系：`kernel_ioq_cap`（内核占用的队列）+ 每 client 的队列数，**总和不能超过
该上限**。即 `kernel_ioq_cap + max_per_client ≤ 最大 IO 队列数`。超出会导致
`NVM_ADD_USER_QUEUE` 分配失败。

Rule of thumb: `kernel_ioq_cap + max_per_client ≤ max IO queues`; exceeding it makes
`NVM_ADD_USER_QUEUE` fail.

### 4.3 启动 daemon / Start the daemon

```bash
sudo env TUTTI_VERBOSE=1 \
  ./build/cuda-module/bin/tutti_daemon \
  --config config/local/tutti_daemon.yaml &
```

成功标志（不应出现 `auto-mount ... failed` / `not mounted` 警告）：
`tutti_daemon listening on 127.0.0.1:50051` + `mount_manager: mounted ...`。

### 4.4 确认挂载 / Verify mounts

```bash
lsblk | grep snvme            # 4 个 /dev/snvmeNn1 挂到 /mnt/nvme0-3
readlink -f /mnt/gpu0/ssnvme0 # 应解析到 /mnt/nvme0/ACCEL0
```

### 4.5 关闭与清理 / Shutdown & cleanup

```bash
sudo kill -TERM <tutti_daemon_pid>      # 优雅停止（自动卸载）
# 需要重载模块时：
sudo bash scripts/unbind.sh             # 解绑所有 snvme 控制器（幂等）
cmake --build --preset cuda-module --target rmmod insmod
```

---

## 5. 运行示例 / Run the example

示例已迁移到 **TuttiRuntime**：runtime 由 Tutti YAML 组装，设备/队列/挂载等
部署事实全部来自 daemon；用户只传 daemon 发布的 view 目录（`--directory`），
不再需要 `--nvme ssnvme_path,pci_bdf,...` 之类的设备细节。

The example now uses **TuttiRuntime**: the runtime is assembled from a Tutti YAML
and all deployment facts come from the daemon; the user only passes
daemon-published view directories (`--directory`) — no PCI BDF / chrdev / mount
details.

### 5.1 striped 模式（4 盘）/ Striped mode (4 drives)

`--directory` 顺序须与默认 YAML（`examples/layerwise_kv_overlap/tutti_layerwise_striped.yaml`）
的 `device_ids` 一致：

```bash
sudo ./build/cuda-module/bin/tutti_layerwise_kv_overlap --striped \
  --directory /mnt/gpu0/ssnvme0 \
  --directory /mnt/gpu0/ssnvme1 \
  --directory /mnt/gpu0/ssnvme2 \
  --directory /mnt/gpu0/ssnvme3
```

### 5.2 单盘模式 / Single-drive mode

```bash
sudo ./build/cuda-module/bin/tutti_layerwise_kv_overlap --single \
  --config examples/layerwise_kv_overlap/tutti_layerwise_local.yaml \
  --directory /mnt/gpu0/ssnvme0
```

### 5.3 常用参数 / Common flags

| 参数 / Flag | 默认 / Default | 说明 / Notes |
| --- | --- | --- |
| `--directory` | **必填** | daemon 发布的 view 目录，可重复；striped 要求 2 的幂个，顺序与 YAML `device_ids` 一致 |
| `--config` | `tutti_layerwise_striped.yaml` | Tutti YAML 路径（字段含义见文件内注释；改设备/队列不用重编译） |
| `--layers` | 80 | 层数 |
| `--ctx-tokens` | 131072 | 上下文 token 数 |
| `--hit-pct` | 90 | prefix hit 百分比 |
| `--tensor-kb` | 512 | 每 K/V tensor 大小（KiB），须与 YAML `stripe_unit` 一致 |
| `--compute-us` | 0（自动校准） | 每次 compute 模拟延迟 |
| `--striped` / `--single` | striped 默认 | 多盘 / 单盘 |
| `--no-verify` | — | 跳过逐字节校验 |

### 5.4 YAML 关键字段速查 / YAML tuning quick reference

调整设备、队列、性能参数改 YAML 即可（`--config` 指向改后文件），无需重编译。
完整逐字段注释见 `examples/layerwise_kv_overlap/tutti_layerwise_striped.yaml`。

Edit the YAML to change devices / queues / tuning — no rebuild needed. Full
per-field comments live in the example YAML.

| 字段 | 单位 | 说明 |
| --- | --- | --- |
| `runtime.accel_id` | 个 | CUDA device index；须在 daemon `allowed_accel_ids` 内 |
| `allocation.device_ids` | 个 | **daemon YAML 的 `device_id`，不是 /dev 名**；顺序 = 条带顺序，须与 `--directory` 一致 |
| `allocation.queues_per_controller` | 队列对（QP） | 每盘申请的用户队列数；≤ daemon `queue_pool.max_per_client` |
| `datapaths[].threads_per_block` | 个（线程） | ≤ 实际获批队列数（违反启动即拒） |
| `handle_cache_capacity` / `prp_cache_capacity` / `handle_cache_l2_capacity` | **槽位**（entries，非字节） | L1/L1/L2 缓存容量；`0` = 关闭（L2 为 0 时自动取 4×L1） |
| `max_batch_entries` | 个（条目） | **单批**并行下发的 IO 条目数——吞吐靠它吃满（本负载单批 920 条） |
| `max_in_flight_operations` | 个（批次） | **并发 `submit()` 批次数**，不限制批内请求数（见下） |
| `backends[].config.stripe_unit` | 字节 | 条带单元；须 = 工作负载 tensor 大小（`--tensor-kb` × 1024） |

**批次的两层结构 / batch two-level structure**：一次 `submit()` = 1 个 op =
1 次 fused kernel 发射，批内请求（≤ `max_batch_entries`）并行下发。吞吐主要
来自单批内的并行；`max_in_flight_operations` 只限制同时在途的批次数。理论
最大在途 IO 条目 = `max_in_flight_operations × max_batch_entries`。本负载每条
stream 提交一批即 wait，read+write 双 stream 并发 ≤ 2 批，故 4 已是双倍余量。

### 5.5 预期输出 / Expected output

```text
[ OK ] cudaSetDevice(0) via TuttiRuntime (tutti_layerwise_striped.yaml)
[ OK ] StorageRuntime created (StripedDataPath, N=4)
[ OK ] Phase A (striped): 512 targets x 4 shards (40.0 GB) in X.XXs
[ OK ] Phase B: 460 hit + 52 miss chunks, 1024 tensors registered
[ OK ] Phase C: opened 512 targets (striped)
[INFO] rq1 L9   read 482.3MB/21.2ms=22.8GB/s write 54.5MB/38.4ms=1.4GB/s
...
[ OK ] SIM TOTAL: 2 req wall=9.4s | READ 77.18GB=23.0GB/s | WRITE 8.72GB=1.4GB/s | overlap 40%
[ OK ] Phase H: verified 26 samples, all correct
=== layerwise_kv_overlap: PASSED ===
```

**参考带宽**（H20 + PM9A1 实测）：4 盘 striped 读 ~23 GB/s，单盘读 ~6.4 GB/s。
Reference: ~23 GB/s READ on 4-drive striped, ~6.4 GB/s single-drive (measured).

---

## 6. 用 nsys 看预期结果（可选）/ Profile with nsys (optional)

> 本节需要 `nsys`（第 2.3 节，非必装）；不装可跳过，不影响示例运行。
> This section needs `nsys` (§2.3, optional); skip if not installed.

示例代码在流水线（Phase G）前后调用了 `cudaProfilerStart()/Stop()`，`nsys` 会在
该区间捕获 GPU kernel 时间线。The example brackets the pipeline (Phase G) with
`cudaProfilerStart()/Stop()`, so `nsys` captures the GPU timeline there.

### 6.1 采集 / Capture

```bash
sudo nsys profile -o kv_overlap \
  --trace=cuda,nvtx,osrt \
  ./build/cuda-module/bin/tutti_layerwise_kv_overlap --striped \
    --directory /mnt/gpu0/ssnvme0 --directory /mnt/gpu0/ssnvme1 \
  --directory /mnt/gpu0/ssnvme2 --directory /mnt/gpu0/ssnvme3
```

> 需要 root（示例要访问 `/dev/ssnvme*` 与挂载点），`nsys` 与目标都要在 root 下跑。
> 也可精简参数减少开销：`nsys profile --trace=cuda -o kv_overlap <cmd>`。

### 6.2 看时间线 / Inspect the timeline

```bash
nsys-ui kv_overlap.nsys-rep          # 图形界面 / GUI
# 或命令行汇总 / or CLI summary:
nsys stats kv_overlap.nsys-rep
```

### 6.3 预期看到什么 / What to expect

| 现象 / Signal | 含义 / Meaning |
| --- | --- |
| 大量 `*nvme*`/`fused_submit` 类 kernel | GPU 直接提交/轮询 NVMe I/O，CPU 不在数据路径上 |
| `sgemm`（compute）与 read/write kernel **在时间线上重叠** | 3-stream `read∥compute∥write` 流水线生效 |
| 每个 layer/方向只有 **1 次 kernel launch** | `submit` 每批一次边界跨越（O(1) per batch） |
| 结束处无 `[FAIL]`、`PASSED` | 端到端正确 |

如果 compute 与 I/O **没有重叠**（串行），检查 daemon 队列配置是否生效。

---

## 7. 常见坑 / Common pitfalls

| 现象 / Symptom | 原因与处理 / Cause & fix |
| --- | --- |
| `prepare_env.sh` 卡在 `yaml-cpp-devel` 报错退出 | EL8 上该包被 `modular filtering` 过滤装不上；已修复：脚本里改为 `optional`，由 vcpkg 提供 |
| 链接报 `undefined reference to std::filesystem::...` | GCC 8 的 `std::filesystem` 需单独 `-lstdc++fs`；已修复：根 `CMakeLists.txt` 检测 GCC <9 时统一链接 `stdc++fs` |
| `insmod snvme.ko` 报 `Key was rejected by service` | 本机强制签名；`.ko` 未送签。送签后 `modprobe`（见 `third_pkgs/tencent_os.md`） |
| `rmmod snvme` 报 "in use" | 有挂载的 `/dev/snvme*n*` 或打开中的 `/dev/snvme*`；先 umount / 关进程（`lsof /dev/snvm*` 排查），再走 §4.5 重载 |
| `NVM_ADD_USER_QUEUE` 失败 | 控制器 IOQ 总量不足；下调 `SNVME_TEST_KERNEL_IOQ_CAP`（smoke 测试） |
| 打开文件过多 | 示例会自动抬 `RLIMIT_NOFILE`；硬上限不足会报所需最小值 |
| 多盘 block size 不一致 | daemon 会在挂载前拒绝启动；统一 namespace logical block size（应为 4 KiB） |

---

## 8. 脚本现状 / Script status（截至本机核对）

| 脚本 / Script | 状态 / Status | 说明 / Notes |
| --- | --- | --- |
| `prepare_env.sh` | ✅ 当前 | 一键依赖 + 生成 presets |
| `pci_topology_check.sh` | ✅ 当前 | 探测 GPU↔NVMe 拓扑 |
| `bind_nvme_device.sh` / `unbind.sh` | ✅ 当前 | 驱动绑定 / 解绑 |
| `umount_nvme_layer_and_reset.sh` | ⚠️ 待确认 | 依赖旧 `/mnt/nvme_layer` 布局 |
| `kv_cache_e2e_sweep.sh` / `kv_cache_read_only_sweep.sh` | ❌ 过时 | 引用的 `kv_cache_e2e_stress` 目标已不存在 |
| `raid0_create.sh` / `raid0_delete.sh` | ❌ 过时 | `mdadm` RAID0 已被 `striped_local_nvme` 取代 |

> 后续迭代可清理/改写这些过时脚本，或在本节标注替代方案。

---

## 9. 更多资料 / Further reading

- TencentOS 环境适配手册（强制签名 / 根目录满 / vendored toolkit 与 nv-p2p.h）：[`third_pkgs/tencent_os.md`](../third_pkgs/tencent_os.md)
- 系统架构：[doc/architecture/system-architecture.md](architecture/system-architecture.md)
- snvme 设计依据（为什么需要定制驱动）：[doc/architecture/snvme-design-rationale.md](architecture/snvme-design-rationale.md)
- 关键设计：[doc/architecture/key-designs.md](architecture/key-designs.md)
- 构建与 SNVMe 测试：[doc/build_and_test.md](build_and_test.md)
- daemon 部署：[doc/tutti_daemon.md](tutti_daemon.md)
- GPU 移植：[doc/gpu-porting-guide.md](gpu-porting-guide.md)
