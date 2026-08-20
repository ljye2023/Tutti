# snvme 设计依据：为什么需要一个定制的 NVMe 驱动

> Design rationale：本文回答一个常见问题——**"snvme 能否用现有内核基础设施替代？"**
> 结论：不能。下面给出逐项对照与根本原因，并给出唯一现实的替代路径（上游化）。

## 1. snvme 做的三件不可替代的事

1. **在内核 nvme 驱动继续持有控制器的前提下，创建额外的用户态 I/O 队列对**
   （通过被独占的 Admin Queue 下发 `Create I/O CQ/SQ`），并把 SQ/CQ DMA 内存、
   **doorbell BAR 的 mmap** 交给用户态；
2. **GPU P2P 映射**（`peer_memory/` 后端）：pin HBM、建立 NVMe 控制器可见的
   DMA 映射，SSD DMA 直达 HBM；
3. **poll 模式完成**：GPU 线程自己写 SQE、敲 doorbell、轮询 CQ phase bit——
   CPU 完全退出数据路径。

## 2. 现有内核基础设施逐项对照

| 设施 | 它给了什么 | 为什么不满足 |
| --- | --- | --- |
| GDS（nvidia-fs + cuFile） | 不 fork nvme、支持 ext4、DMA 直达 HBM | 每个 IO 仍由 **CPU 走 block layer 提交**；GPU kernel 无法自主提交（正是 Tutti 论文对比并胜出的基线）；且 nvidia-fs 本身也是内核外模块 |
| io_uring（含 NVMe passthrough，5.19+） | 用户态提交原始 NVMe 命令、绕过部分 block layer | 提交仍是 CPU（`io_uring_enter` / SQPOLL 内核线程），**doorbell 仍由内核驱动写**；优化的是"每 IO 一次 syscall"，不是"CPU 退出数据路径" |
| AF_XDP | 用户态 ring + NIC 直接 DMA 用户内存 | 网络专用；NVMe 无对应物 |
| VFIO 整卡直通 | 用户态 mmap BAR（能摸到 doorbell） | 前提是 unbind 内核 nvme 驱动 → 丢块设备、丢 ext4/FIEMAP；用户态须重写完整 NVMe 栈 |
| NVMe 字符设备 passthrough | 原始命令提交 | 每命令一次 CPU syscall；不暴露 doorbell |

## 3. 根本阻塞点（不随内核版本变化）

1. **Doorbell 所有权**：NVMe 的 doorbell 是 BAR0 里的 MMIO 寄存器，只有 bind
   到设备的那一个驱动能 `ioremap`。主线 nvme 驱动从不把 doorbell 页映射给
   用户态——这是 NVMe 类设备"OS 所有"的设计立场，不是待实现的功能。
2. **Admin Queue 独占**：创建额外 I/O 队列必须通过 Admin Queue，而它被内核
   驱动私有其 admin tag 与队列结构，无任何导出接口。**外部 companion 模块
   （gdrdrv / nvidia_peermem 风格）结构上不可能替用户创建队列**——这就是
   snvme 必须是 fork 而不是伴随模块的原因。

> 旁证：做同类系统（GPU 自主发起存储 IO）的学术工作——GeminiFS（FAST'25）、
> BaM（GPU-initiated storage）——全部实现了自定义内核模块。这个空白是行业性的。

## 4. RDMA 世界的参照：用户态直敲 doorbell 是什么

RDMA（InfiniBand/RoCE）的 verbs 数据路径以 Mellanox/NVIDIA ConnectX 为例：

```text
─── 控制面（慢路径，内核参与）────────────────────────────
ibv_create_qp / ibv_reg_mr …  →  uverbs ioctl
  内核驱动：创建 QP、pin SQ/RQ/CQ 内存、建 DMA 映射、
  并把 doorbell 所在的 BAR 页 mmap 给用户进程

─── 数据面（快路径，0 syscall）──────────────────────────
① 用户直接写 WQE 到自己进程内的 SQ ring（pinned，NIC 可 DMA 读）
② 用户对 mmap 的 doorbell 地址做一次普通内存写（一次 store）
③ NIC 从 SQ ring 取走 WQE，执行 DMA（含直接读写 HBM）
④ NIC 把 CQE DMA 写回用户进程内的 CQ ring
⑤ 用户轮询自己内存里的 CQE（ibv_poll_cq 是纯用户态函数）
```

Mellanox 的 **Blue Flame** 优化：doorbell 区域是 write-combining 内存，那次
store 会把 doorbell 值 + WQE 头部一并刷给网卡，省一次 DMA round trip。

**安全模型**：不是"用户随便写设备寄存器"。内核在 setup 阶段完成全部授权
（`ibv_reg_mr` pin 内存并登记网卡页表），NIC 只被允许 DMA 到预注册区域；
doorbell 只能投递到内核替你创建的、属于你的队列。越界操作在硬件层被限制。

NIC 世界能做到，是因为**设备类协议从第一天就为 user doorbell 设计，且内核
驱动官方支持**（uverbs ABI 是稳定接口）。NVMe 的 doorbell 硬件上同样只是
一段可写 MMIO——缺的不是硬件能力，而是主线驱动愿意提供的两样"setup 服务"：
替用户建队列 + 把 doorbell 映射给用户。

## 5. RDMA ↔ snvme 逐项对照

| RDMA 世界 | snvme 对应物 |
| --- | --- |
| `ibv_create_qp`（内核建队列，ring 放用户内存） | ioctl 创建额外 I/O SQ/CQ，ring pin 后映射给用户/GPU |
| uverbs mmap doorbell BAR 页 | `/dev/ssnvme<N>` mmap BAR0（doorbell 区域） |
| 用户写 WQE + 敲 doorbell，0 syscall | GPU 线程写 SQE + 敲 doorbell（一次 `cudaMemcpyAsync` 带整批） |
| 用户轮询 CQE（poll 模式） | GPU 线程轮询 CQ phase bit，完成随 CUDA stream |
| `ibv_reg_mr` pin 内存 | `register_memory`：DMA-map 一次，预建 PRP |
| `nvidia_peermem`（HBM 注册给 NIC） | `peer_memory/nvidia.c`（HBM pin 给 NVMe DMA） |

**一句话**：snvme 是把 RDMA 验证了二十年的"user doorbell + pinned ring +
poll completion"姿势移植到 NVMe。

## 6. 替代路径：上游化

摆脱 fork 的唯一实质路径是把该能力提案进 linux-nvme（"驱动管理下的
userspace queue pairs + doorbell mmap + poll completion"）。参考先例：
RDMA 的 uverbs ABI。snvme 的 fork 增量已按 chrdev / 用户队列 / peer_memory
三块收敛（见 `kernel_modules/PORTING.md`），可进一步整理为按功能拆分的
patch series，降低对每个内核基线的维护成本。
