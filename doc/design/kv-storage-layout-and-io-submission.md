# KV 落盘布局与 IO 提交设计

> **状态**：当前实现设计收束（2026-10-04）
>
> 本文说明 GPU 直连 NVMe 的 KV 读写为什么采用现在的磁盘数据布局（固定段文件 +
> 32 KiB 槽位前缀 + 按文件组预建）和现在的 IO 提交方式（融合 kernel + 固定 worker
> 池 + 可选 green context），每一条都给出实测依据、被否决的方案和剩余的优化空间。
> 这两件事对端到端性能的影响远大于软件栈本身：同一套代码，只改请求落到的 LBA，
> 8 卡读带宽从 19 GiB/s 变成 26.6 GiB/s。

相关文档：[`tutti-runtime-assembly.md`](tutti-runtime-assembly.md)（Resource /
Resolver / DataPath 装配）、[`multi-accelerator-runtime.md`](multi-accelerator-runtime.md)
（多卡多盘与 daemon）、[`../vllm-connector/Target_arch.md`](../vllm-connector/Target_arch.md) §8
（connector 视角的 key 与对象布局）。

测试环境（下文所有数字均出自此环境）：8 × NVIDIA H20（78 SM），4 × Intel D7-P5520
7.68 TB（SSDPF2KX076T1，ext4，每盘约 6.5–7.0 GB/s 读），HY3-FP8，vLLM TP8。
KV 几何：每 chunk 每层每 rank 一个 128 KiB 段（生产读粒度 32 KiB / 64 KiB），
80 层，每 chunk 10 MiB payload。

---

## 1. 结论速览

| 问题 | 设计 | 依据 |
|---|---|---|
| 文件层读带宽低于裸块设备 | 请求起点 16 KiB 对齐：槽位前缀 32 KiB | 4 盘 fio：4 KiB 前缀 21.1 → 32 KiB 前缀 26.4 GiB/s；8 卡探针 19.3 → 26.6 GiB/s |
| 每 chunk 一个文件：建文件慢、extent 多、票据多 | 固定大段文件：每盘每 rank 一个文件放 2048 个槽位 | 每盘一个票据；ext4 extent 可控；回收不改文件 |
| 前向线程上建文件阻塞推理 | 预建以"文件组"为单位，冷池在 open 时建首组，之后后台建，写路径永不建文件 | 旧方案请求中途扩容 64 槽位 2.85 s；冷池 53 s 内写入全被拒绝 |
| IO 线程数随批大小膨胀、占满 SM | 每次 launch 固定 `min(条目数, 256)` 个 worker，block 固定 32 线程 | 256 与 2048 worker 带宽相同（单卡 12.4、8 卡 26.6 GiB/s） |
| 每层读 kernel 等 GEMM 让出 SM（层间空隙 2.4 ms） | 可选 green context（`TUTTI_GREEN_CTX_IO_SMS`） | 空隙 2.4 ms → 19 µs，读带宽 26.4 GiB/s；但 cutlass 需改 grid（§5.4） |

端到端（HY3-FP8 TP8，4 盘，64K token，复用 95%，热池稳态，无 nsys）：

| 配置 | B（命中 95%） |
|---|---:|
| HBM 前缀缓存（不走存储，下限） | 1.618 s |
| 旧布局（每 chunk 一文件，4 KiB 头） | 2.047 s |
| **段文件 + 32 KiB 前缀** | **1.685 s** |

冷池（每轮新 prompt，B 还要写入新增的 5%）：旧布局 2.33 s，段文件 1.95–2.04 s；
开服时若不等首个段文件就绪，B 约 10 s（零命中）。

---

## 2. 磁盘数据布局

### 2.1 根因：瓶颈是 LBA 规律，不是软件栈

现象：GPU 直连读，文件层约 19 GiB/s，同样的盘按裸块设备读 26+ GiB/s。逐项排除过
提交路径、队列数、单队列深度、doorbell 聚合、kernel 线程数，均无关。

决定性实验是"只改请求地址，不改其他任何东西"：

- **fio 3.19 回放**（libaio，`direct=1`，用 iolog 回放生产读的地址序列）：4 盘合计
  4 KiB 前缀 21.13 / 32 KiB 前缀 26.41 / 完全随机 26.59 GiB/s。CPU 路径复现同样排序，
  证明与 GPU 直连无关。
- **GPU 探针**（`nvme_segment_bw_probe --addr-stride-kb/--addr-base-kb`，只改读地址，
  不重建文件）：8 卡 4 KiB 前缀 19.27 → 32 KiB 前缀 26.62 GiB/s。
- **写入方式对照**：顺序写（相位 0 / 相位 4K）与随机顺序写，惩罚始终跟随**读取**时的
  LBA，与数据是怎么写进去的无关，排除 NAND die 撞车。

规律（单盘）：

1. 请求起点必须 16 KiB 对齐。
2. 同一层并发请求的高位地址（bit17 起，盘内带哈希）不能集中。

推测机理：D7-P5520 报告 NOIOB = 32 × 4 KiB = 128 KiB（`chunk_sectors=256`），盘内按
128 KiB LBA 条带划分并行单元（高位地址参与哈希），处理粒度 16 KiB。起点不对齐的
32/64 KiB 请求会跨两个处理单元；所有槽位步长相同、前缀 4 KiB 时，同层请求的地址
低位完全一样，高位落进少数并行单元。

> 32 KiB 前缀只在 HY3 TP8（payload 10 MiB）几何上验证过。换模型、TP 或段大小后槽位
> 步长会变，需要用探针在现有文件上复核（`--addr-stride-kb <前缀+payload>
> --addr-base-kb <前缀>`，单盘约 10 s）。

### 2.2 布局：固定段文件

```text
一个 chunk = 一个对象 = 一个槽位（slot）
device   = slot % N                          （N = 本 rank 的挂载点数）
file_id  = (slot / N) / segment_file_slots
path     = <mount>/r<rank>/segments/<file_id>.seg
槽位起点 = ((slot / N) % segment_file_slots) × slot_bytes
slot_bytes = segment_header_bytes + num_layers × segment_bytes
layer L  = 槽位起点 + segment_header_bytes + L × segment_bytes
```

默认 `segment_file_slots = 2048`、`segment_header_bytes = 32 KiB`（对象头占前 4 KiB），
HY3 TP8 每文件约 20 GiB。代码：`csrc/storage_objects/slot_placement_policy.{h,cpp}`
（`FixedSegmentFilePlacement`，唯一实现），默认值集中在
`tutti/storage/tutti_nvme/object_layout.py`。

设计要点：

- **不做条带**。一个 chunk 的全部层落在同一块盘的一段连续 LBA 上，单个 IO 永不跨盘，
  只在 MDTS 和 extent 边界拆分。多盘并发来自相邻 chunk 在盘间轮转——一个长 prompt
  的一层读，自然分散到所有盘。64 KiB 细粒度条带曾实测把 4 盘塌陷到 2.35 GiB/s。
- **大文件而不是每 chunk 一个文件**。每 chunk 一个文件意味着每次分配都要建文件、
  每个文件一个票据（resolve + FIEMAP + peer-memory 注册），回收要删改文件。段文件
  每盘每 rank 只有一个票据，槽位回收只就地清零槽位前缀，文件与 extent 不变，票据
  永不失效；槽位是否被复用由对象层的 slot generation 判定。
- **rank 子目录不可省**。8 个 rank 共享一组盘、各自从槽位 0 编号，缺了 `r<rank>`
  会互相覆盖。
- **几何进命名空间指纹**（`segment-files-rotating-v4-slots:<n>-header:<bytes>`）。
  换几何即换池；盘上 `.ready` 记录的是别的几何时 `open()` 直接报错，不会把旧文件
  当半截文件续写。调度侧只读视图必须用与 worker 相同的几何，否则指纹对不上、冷启动
  恢复静默失败（曾经出现过：非默认前缀在调度侧被丢掉）。
- **检查点在 `kv_root`，段文件在挂载点**。换 `kv_root` 得到新索引，已有段文件被收编；
  恢复只来自检查点 + 对象头交叉校验。

### 2.3 段文件的物理要求

- **真实写零**。resolver 拒绝 `UNWRITTEN` / `DELALLOC` extent（DMA 不能打到文件系统
  未提交的块），所以不能用 `fallocate` 或稀疏文件。
- **extent 数 ≤ 124**（resolver 的上限）。ext4 FIEMAP 会把物理连续区间按 128 MiB
  拆开返回，resolver 把逻辑、物理都相邻的片段合并后再计数，否则 20 GiB 文件必然超限。
- **同一挂载点串行创建**（`<mount>/.tutti_segment_precreate.lock` flock）。8 个 rank
  并发建 20 GiB 文件实测碎成 2000 多个 extent。
- **`.ready` 标记**：写零、fsync、FIEMAP 校验通过后写 `<file>.ready`（file_bytes、
  st_dev、st_ino）。标记绑定 inode，同名同大小的替换文件不会被误认作就绪。

### 2.4 预建：按文件组，写路径永不建文件

演进过程中踩过的坑决定了现在的形态：

| 曾经的做法 | 问题 |
|---|---|
| 写路径按需建槽位文件 | 建文件 = 实写零 + fsync，64 槽位 2.85 s、单槽约 44 ms，直接阻塞前向线程 |
| 后台按水位补槽位，水位设小了 | 请求中途扩容，32K 请求波动 4.9–7.7 s |
| 预建一路补到容量上限 | 8 rank × 8 TiB 超过物理盘，写到 ENOSPC，留下半截文件 |
| 段文件冷池直接开服 | 首个 20 GiB 文件要建约 53 s，期间写入全部被拒、零命中 |

现在的规则（`csrc/storage_objects/object_store_core.cpp`、`space_allocator.cpp`）：

1. 预建单位是**文件组** = 每盘一个段文件 = `segment_file_slots × N` 个槽位
   （HY3 TP8 4 盘：8192 槽）。分配器只交出已发布组内的槽位
   （`SpaceAllocator::set_ready_limit`），未发布的槽位不存在。
2. `open()` 先收编盘上 `.ready` 匹配的组；冷池时**同步建出首组**（每 rank 约 50 s，
   8 rank 并行），开服即可写。服务启动多等约 1 分钟，换来首轮即命中。
3. 写路径越过已发布组时，与容量耗尽同一契约：**非阻塞拒绝**，调用方裁剪写批。
4. 后台单线程 `precreate_step(headroom)`，headroom 取半组：最后一组用过一半
   （每 rank 4096 槽 ≈ 40 GiB KV）时建下一组。不在组刚开始用就建，因为建组是重写入
   （每盘 8 rank × 20 GiB），会与在线 KV 读写争带宽。最后一组截到容量；失败不发布、
   5 s 后重试；可用空间低于 `min(32 GiB, 5%)` 时暂停。
5. 容量只是上限，盘上占用跟随分配前沿。

---

## 3. IO 提交

### 3.1 一层一次 launch，融合发往所有盘

每个 rank 每层只 launch 一次 `fused_submit_kernel_pool`：设备表每个打开的目标一行，
kernel 按条目的 `dev_idx` 把命令发到对应盘的队列，同一次 launch 覆盖全部 N 块盘。
主机侧 feeder 线程逐层提交、每层记一个完成事件，计算流在 attention 前对该层事件
`wait_event`——纯设备侧依赖，主机不轮询。

### 3.2 固定 worker 池

旧形态是"每个条目一个线程"或"2048 个 worker × 每 block 16 线程 = 128 个 block"。
现在每次 launch 的 worker 数固定为 `min(条目数, 256)`（`TUTTI_POOL_WORKERS` 覆盖，
0 = 旧的每条目一线程 kernel），block 固定 32 线程（`kSubmitBlockThreads`，一个
warp，不可配置）。worker 从原子游标领取条目，发出命令后轮询自己的 CQ，完成后领下一条。

为什么是 256（单卡，4 盘，每层 486 个 64 KiB 请求）：

| worker | 32 | 64 | 128 | **256** | 512 | 1024 | 2048 |
|---|---:|---:|---:|---:|---:|---:|---:|
| GiB/s | 5.0 | 8.2 | 11.2 | **12.4** | 12.1 | 12.3 | 12.4 |

8 卡同时读：256 与 2048 worker 都是 26.6 GiB/s。worker 只决定提交与轮询的并行度，
256 足以让每盘保持足够深的队列；再多只是占更多 SM 槽位。

为什么 block 是 32：GPU 没有"默认 block 大小"，32 是 warp 宽度。旧的 16 线程 block
半个 warp 空转；旧注释里"block 线程数不超过每盘队列数"的约束早已不成立——队列支持
多线程并发提交（`nvm_parallel_queue` 原子 cid），实际每盘只拿到 8 个队列也照常工作。

其他细节：

- **队列映射**用全局线程号取模（`QueueAcquireHelper::acquire_queue`）。旧公式
  `blockDim × 32 + threadIdx` 没用 `blockIdx`，所有 block 的第 t 号线程挤在同一个
  队列上。
- **每盘队列数不影响带宽**：4 / 8 / 16 / 32 个队列，单卡 12.0 / 11.9 / 12.4 /
  12.5 GiB/s。
- **launch 前没有 memset**。原来每层有两个 `cudaMemsetAsync`：状态缓冲填 0xFF（防止
  未执行的条目被误判成功）和任务游标清零。前者多余——每个条目恰被领取一次、
  `submit_*_one` 每条返回路径都写状态，kernel 中途崩溃时完成事件本身报错；后者改成
  最后一个退出的 worker 自己把游标清零（arena 初始化时清零一次）。去掉它们代码更
  干净，但**不改变带宽**：memset 本身也是 kernel，它和读 kernel 等的是同一样东西（§4）。

---

## 4. 与模型计算争 SM

### 4.1 现象

nsys（8 卡，64K，复用 95%）里，同一张卡上相邻两层的读 kernel 之间有 2–4 ms 空隙，
每张卡每个 B 请求约 79 个空隙、合计约 190 ms，占读窗口约 20%。读 kernel 在跑时 8 卡
合计 24–25 GiB/s，按首尾跨度算只有 20–23 GiB/s，差额就是这些空隙。

不是 feeder：下一层的读 kernel 平均比上一层结束**早约 335 ms** 就已经 launch
（632 个空隙里 631 个如此），feeder 每层主机开销约 2.5 ms，完全和 kernel 重叠。

### 4.2 根因

空隙开始时，同一张卡的计算流上总在跑 vLLM 的 FP8 GEMM
（`cutlass_3x_gemm_sm90_fp8`，三轮 8 卡 1406 个大空隙里 1404 个）：

- grid = 78 个 CTA = H20 的 78 个 SM，持久化调度，一个 SM 一个 CTA；
- 每 CTA 384 线程 × 168 寄存器 = 64512 个寄存器，一个 SM 共 65536 个；
  外加 221.5 KiB 共享内存。

所以 GEMM 在跑时**任何一个 SM 都放不下别的 block**，再小的 kernel（1 线程、8 个
寄存器）也只能等 GEMM 的 CTA 陆续退出。单独实验证实：GEMM 寄存器少时 IO kernel
2 ms 内开始，寄存器占满时要等整个 GEMM 结束。

反过来，IO kernel 只要驻留，就会让这个 GEMM 多一波（20 个 200 µs 的整卡 GEMM 串联）：

| 同时驻留的 IO kernel | GEMM 串耗时 |
|---|---:|
| 无 | 1.00× |
| 1 block × 256 线程 | 1.99× |
| 8 block × 32 线程 | 1.99× |
| 128 block × 16 线程（旧形态） | 3.84× |

结论：线程数和 block 形状只能把干扰从 3.8× 降到 2×，降不到 1×。按需 launch 的 IO
kernel 与一个占满整卡的 GEMM 同时出现时，只有两种结果——IO 等 GEMM（层间空隙），
或 GEMM 因为 IO 占着 SM 多跑一波。

### 4.3 试过但无效的

| 方案 | 结果 |
|---|---|
| 去掉 launch 前两个 memset | B 2.118 → 2.157 s，空隙 2.4 → 2.0 ms，噪声内 |
| 读流设高优先级 | 无效：调度器不能抢占已驻留的 CTA，而读 kernel 本来就在第一批 SM 空出时拿到资源 |
| 减少 worker / block 数 | 带宽不降，但空隙不变（同一个 GEMM 照样占满 SM） |

### 4.4 green context（可选）

`tutti/integration/vllm/green_context.py`：设置 `TUTTI_GREEN_CTX_IO_SMS=N`（sm90 上
N 为 8 的倍数）后，worker 在注册 KV cache 时（前向线程、模型第一个 kernel 之前）把
当前流换成一个 `78 − N` 个 SM 的 green context 流，模型计算从此只能用这 `78 − N`
个 SM；IO kernel 仍在主 context 的普通流上**按需 launch**，落到留出来的 N 个 SM 上。
默认关闭。

实测（N = 8）：

| 指标 | 关闭 | 开启 |
|---|---:|---:|
| 读 kernel 间空隙 p50 | 2.5–2.8 ms | **19 µs** |
| 8 卡读带宽（首尾跨度） | 19.7–21.1 GiB/s | **26.4 GiB/s** |
| GPU0 计算忙碌时间（A / B） | 约 9.7 / 1.3 s | 约 13.1 / 2.0 s |
| 端到端 B / A | 2.143 / 10.61 s | 2.109 / 13.21 s |

IO 侧问题解决了，但计算慢了约 25%。原因：cutlass 按设备的 78 个 SM 决定持久化 grid
（`KernelHardwareInfo::query_device_multiprocessor_count`，查的是设备属性，不是
green context），78 个 CTA 在 70 个 SM 上要两波。下一步是让 vLLM 的
`cutlass_gemm_caller.cuh` 在 green context 下把 `hw_info.sm_count` 设为实际 SM 数，
使 GEMM 一波跑完——预期计算只按 70/78 慢约 11%，同时保留 26.4 GiB/s。这需要改 vLLM
源码，尚未实施。

为什么不用常驻 IO kernel：常驻 kernel 永久占着 SM，等价于 green context 的 SM 划分
却失去了按需释放的能力，并要求重写提交模型；IO kernel 保持按需 launch。

---

## 5. 单卡带宽上限

单卡读 4 盘只有 12.4 GiB/s，8 卡同时读才到 26.6 GiB/s。限制既不是队列数也不是
worker 数（§3.2 两张表），而是**逐层同步 + 每层读量小**：

| 每批条目（每盘数据量） | 128（2 MiB） | 486（7.6 MiB，生产一层） | 972 | 1944 | 3888 |
|---|---:|---:|---:|---:|---:|
| 单卡 GiB/s | 5.2 | 12.4 | 17.4 | 21.2 | 23.3 |

kernel 内逐条计时（486 条，256 worker）：kernel 内部 2.2 ms，按盘速一层本该约 1.2 ms。
多出的约 1 ms 是开头的队列填充和结尾的尾延迟——50% 命令 919 µs 完成、90% 1415 µs、
最后一条 2201 µs，四块盘结束时间相差 0.5 ms，整层要等最慢的那块。把 worker 加到
486（所有命令第 16 µs 全部发出）不起作用：盘里排队更深，单条延迟从 575 µs 翻到
980 µs，总时间不变。

多卡时各卡层边界错开，互相填满对方的尾巴：

| 同时读的 GPU | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| 4 盘合计 GiB/s | 13.1 | 19.0 | 24.7 | 26.6 |

TP8 不受影响。TP1/TP2 部署时这是主要瓶颈，对策是同一张卡同时保持 2 层在飞（两个
读流交替提交、每层各自完成事件），而不是合并成大批——vLLM 按层等数据，合并会推迟
第一层可用时间。

---

## 6. 复现

```bash
# 端到端（离线驱动，8 卡 4 盘；--nsys 输出到 /mnt/nvme4/tutti-profile/reports）
bash scripts/vllm/run/bench-8gpu-striped.sh [--nsys] \
    --tokens 65536 --reuse-pct 95 --rounds 3 --tag <tag> --pool-tag <pool>
# 开 green context
TUTTI_GREEN_CTX_IO_SMS=8 bash scripts/vllm/run/bench-8gpu-striped.sh ...

# 单卡段文件带宽（4 盘，生产一层 = 243 chunk × 2 个 64 KiB 请求）
TUTTI_POOL_WORKERS=256 build/bin/tutti_nvme_segment_bw_probe \
    --config examples/nvme_bw_probe/tutti_nvme_bw_probe_striped.yaml \
    --directory /mnt/nvme0 --directory /mnt/nvme1 --directory /mnt/nvme2 --directory /mnt/nvme3 \
    --chunks 243 --layers 80 --io-kb 64 --requests-per-chunk 2 \
    --slots-per-file 2048 --header-kb 32 --rounds 2

# 8 卡同时读：每卡一份配置（accel_id = 卡号，queues_per_controller = 8），先逐卡
# --prepare-only 建文件，再 8 个进程带 --reuse-only --barrier <prefix> 并发启动，
# 全部就绪后 touch <prefix>.go，吞吐相加。

# kernel 内逐条 IO 计时汇总（setup / exec / reclaim）
TUTTI_IO_TIMING=1 build/bin/tutti_nvme_segment_bw_probe ...

# 地址规律复核（只改读地址，不重建文件）
build/bin/tutti_nvme_segment_bw_probe ... --addr-stride-kb <前缀+payload KiB> --addr-base-kb <前缀 KiB>
```

nsys 分析口径：读/写 kernel 用 `tutti.striped_nvme.io_kernel|op=read|write` NVTX 区间
关联 kernel 的 launch 线程；`correlationId` 在进程间会重复，关联时必须带上进程号。

## 7. 待办

1. vLLM cutlass 在 green context 下按实际 SM 数设置 grid（§4.4），之后再评估默认开启。
2. TP1/TP2：同卡 2 层在飞（§5）。
3. 32 KiB 前缀在其他几何上的复核；32 KiB 读粒度的对齐规律单测（目前探针与 fio
   用的是 64 KiB）。
4. 运行期建下一文件组时与在线 KV 争带宽，目前只有契约测试覆盖，未在线上触发过。
