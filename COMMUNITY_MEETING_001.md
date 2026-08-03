
### Tutti 是什么

Tutti 是一个 **GPU launch IO 的 GPU 存储系统运行时**。核心I/O路径：由 GPU kernel 自己发起存储 IO——GPU 线程直接构造 NVMe SQE、敲 doorbell、轮询 CQ，数据在 GPU 显存和 NVMe 之间直传，全程不经过 CPU 和 host 内存（当前设计，以后可以扩展方寸范围）。这和 cuFile / GDS 有本质区别：cuFile 是 CPU 侧调用 API 触发 DMA，IO 的发起方和控制方都是 CPU；Tutti 是 GPU 侧 launch IO，CPU 只做前期的资源准备（打开文件、注册内存、分配队列），真正的 IO 提交和完成都在 GPU stream 上发生，和计算 kernel 一样交给 GPU scheduler 调度，不同 stream 上的 IO 和 compute 可以并发。

Tutti 的另一个核心是 **通过 GeminiFS 的思想与现有存储系统兼容**。GeminiFS 是这个项目的前身，它的做法是：用 FIEMAP ioctl 拿到 ext4/xfs 上普通文件的物理 extent 分布，把文件逻辑偏移映射到底层 NVMe 的 LBA，然后让 GPU 直接对这些 LBA 发 NVMe 命令。这样不需要专门的文件系统或裸盘——应用看到的是普通文件路径，Tutti 在背后解析成物理地址，GPU 直接读写。这个机制现在保留在 `resolvers/local_file/` 里（`resolver.h` 调 `FS_IOC_FIEMAP`），产出 `Ext4LocalNvmePayload` 交给 `LocalNvmeDataPath`。



### 要解决什么问题

旧 Tutti（仓库根目录代码）把多个维度的特殊逻辑混在一起：CUDA 内存管理、NVIDIA GPU page pin / P2P DMA、libnvm 队列和 PRP/SGL、snvme 内核 ABI、FIEMAP 文件元数据、上层 KV-cache API。这导致"换 GPU 厂商""换传输后端""换 namespace/文件系统""换 kernel 版本"彼此耦合——动一个维度要改全局。

重构的目标是把这些维度拆成正交的扩展边界，让每类变化停留在正确位置，新增功能时只加新 package，不修改公共 API。


### 重构前：单树耦合

旧代码在仓库根目录下（`coordinator/`、`memory/`、`io_engine/`、`nvme_storage/`、`block_storage/`、`device_manager/`、`adapters/`），`Coordinator` 是唯一组装者，按固定顺序拉起全栈：

```
应用代码 / KvCacheIoAdapter
  │
  ▼
Coordinator (coordinator/include/coordinator.h)
  │  按序 bring-up:
  ├──► device_manager   (NVMe 控制器发现 / 队列租约)
  ├──► nvme_storage     (FIEMAP → NvmeFileDeviceHandle)
  ├──► block_storage    (GpuFile / shard 管理)
  ├──► memory           (cudaMalloc + nvm_dma_map + PRP cache)
  └──► io_engine        (host batch build → GPU kernel → CQ poll)
```

这套结构能用，但换任何一个维度都要动全局。`MemoryRegion` 同时携带 host pointer、device pointer 和 DMA bus 地址（`memory/include/memory_region.h`）；`NvmeBatchEntry` 把 PRP 指针和 shard 表塞进跨层传递的 POD（`io_engine/include/local_nvme/nvme_batch.h`）；`StorageTarget` 把 NVMe extent、LBA、RDMA rkey 全放一个 struct。更根本的是，CUDA allocation、NVIDIA P2P pin、libnvm queue、snvme ABI 混在同一编译单元里——换 GPU 厂商、换存储后端、换 kernel 版本，这三件事彼此耦合，没法独立做。（详见 `MAIN_IO_PATH.md` 第 1-4 节、`TUTTI_REFACTOR_TAKEOVER.md` 第 2.1 节。）

### 核心设计思想

**1. 稳定公共 noun，隔离私有 verb。** 公共层只保留稳定的名词——`MemoryHandle`（已注册内存）、`TargetHandle`（已打开存储目标）、`IoHandle`（一批异步 IO 的生命周期）、`IoRequest`（IO 意图：direction + memory offset + target offset + length）。PRP、SQE、CQ、queue、doorbell、descriptor cache 这些动词留在具体 DataPath 内部。

**2. namespace 和 transport 正交分离。** 文件系统/对象/DFS 等 namespace 层只负责把"名字/对象"解析成 target，NVMe/GDS/RDMA 等 transport 层只消费 target 并移动字节。两者通过 Binding 传递 pair-private payload，不通过公共 union 堆字段。

**3. NVIDIA-first CUDA-like API。** 应用统一用 CUDA 风格的 API（`cudaMalloc`/`cudaStream_t`/`cudaEventRecord` 等），构建时由 `TUTTI_ACCELERATOR` 选一种 profile。NVIDIA 直接用 CUDA headers，兼容厂商（沐曦 MACA、摩尔线程 MUSA 等）通过编译期 shim 映射。"CUDA-like 可编译"只证明 runtime API 有源码映射，不等于 P2P/GPUDirect/device kernel 能用——后者要由 profile 和具体 DataPath 单独验证。

**4. 能力不支持必须显式失败。** 缺驱动不静默切 mock；direct path 没实现不返回成功；alignment/memory kind/execution domain 不支持时不继续尝试；kernel/controller/stream 错误不丢弃后报成功。


### 完整架构与关键组件

下图展示 Tutti 从应用到硬件的完整数据通路。CUDA-like API 不是一个和应用层平行的组件，而是整个栈的编译期底座——应用用它分配内存和创建 stream，Runtime 的接口里带 `cudaStream_t` 类型，DataPath 用它在 stream 上 launch kernel。所有人站在它上面。

```
┌─────────────────────────────────────────────────────────────────────┐
│  应用层                                                              │
│                                                                      │
│  C++ 应用          VllmAdapter         LmCacheAdapter               │
│  (直接调 API)      (KV block layout    (tensor lifecycle             │
│                     → IoRequest)         → IoRequest)                │
└──────┬──────────────────┬───────────────────┬───────────────────────┘
       │                  │                   │
       │  storage API     │                   │
       │  (open/register/ │                   │
       │   submit/wait)   │                   │
       ▼                  ▼                   ▼
┌──────────────────────────────────────────────────────────────────┐
│ StorageRuntime                                                    │
│                                                                   │
│ · 分配/注册内存 → MemoryHandle                                   │
│ · 打开目标 → TargetHandle                                        │
│ · 提交 IO → IoHandle                                             │
│ · 按 {DataPath, target} 分组、背压                               │
│ · 错误聚合、query/wait、shutdown drain                           │
│                                                                   │
│  HostSubmitContext 里带 cudaStream_t, 透传给 DataPath            │
└──────┬──────────────────────────────────────────────────────────┘
       │ 1. open(uri)
       ▼
┌──────────────────┐    ┌──────────────────┐
│ StorageTarget    │    │ Binding          │
│ Resolver         │───►│                  │
│ · 解析 URI       │    │ pair-private     │
│ · 读 FIEMAP      │    │ payload 契约     │
│   拿物理 extent  │    │                  │
│ · 验证完整性     │    │ Ext4LocalNvme-   │
│ · 持有 fd lease  │    │   Payload:       │
│ · 产出           │    │   NamespaceID    │
│   ResolvedTarget │    │   Extent[]       │
└──────────────────┘    └────────┬─────────┘
       │ ResolvedTarget    │
       ▼                   ▼
┌──────────────────────────────────────────────────────────────────┐
│ DataPath (具体数据移动实现, 对 Runtime 是 透明的)                  │
│                                                                   │
│ LocalNvmeDataPath:                                                │
│ · open: payload → DeviceTargetHandle (GPU 显存里的 extent 表)    │
│ · register_memory: nvm_dma_map_data_device() → ioaddrs[]        │
│ · submit: MDTS fan-out → PRP → IO kernel 排入 caller stream     │
│ · GPU kernel: resolve_lba → SQE + doorbell → poll CQ            │
│ · progress/query/release                                         │
│                                                                   │
│ 私有: NvmeQueueGroup / PRP pool / metadata arena / snvme.ko /   │
│       libnvm —— Runtime 看不到这些                               │
└──────────────┬──────────────────────────────┬───────────────────┘
               │                              │
               ▼                              ▼
        ┌──────────────┐           ┌──────────────────┐
        │ NVMe SSD     │           │ GPU              │
        └──────────────┘           └──────────────────┘

═══════════════════════════════════════════════════════════════════════
  tutti/cuda_like.h — 编译期 profile selector (底座层, 全栈共用)

  提供 cudaMalloc / cudaFree / cudaStream_t / cudaEvent_t /
        cudaMemcpyAsync / __global__ / __device__ / <<<>>>
  给以上所有层使用:

    应用:  cudaMalloc 分配 GPU 内存, cudaStreamCreate 创建 stream
    Runtime: HostSubmitContext 接口带 cudaStream_t 类型
    DataPath: 用 __global__/<<<>>> 在 caller stream 上 launch IO kernel

  构建时由 TUTTI_ACCELERATOR 选一种, 全局生效:
    CUDA → 直接 include CUDA headers
    MACA → gpu_vendor/maca.h (#define cudaMalloc mcMalloc ...)
    HOST → 测试用 stub
═══════════════════════════════════════════════════════════════════════
```

各组件具体干什么：

**StorageRuntime** — 应用唯一的存储入口。应用通过它分配和注册 GPU 内存（拿到 `MemoryHandle`），打开文件或存储对象（拿到 `TargetHandle`），提交 IO 批次（拿到 `IoHandle`），然后查询或等待完成。Runtime 负责验证请求边界、按目标分组、背压控制、错误聚合和资源生命周期管理。应用创建的 `cudaStream_t` 通过 `HostSubmitContext` 传进来，Runtime 透传给 DataPath——Runtime 自己不 launch kernel，也不理解 PRP、SQE、FIEMAP 这些底层细节。

**CUDA-like API / profile** — 不是独立组件，是编译期底座。`tutti/cuda_like.h` 根据 `TUTTI_ACCELERATOR` 选一种 profile：NVIDIA 直接 include CUDA headers；沐曦 MACA 通过 `gpu_vendor/maca.h` 的 `#define` 映射到 `mcMalloc`/`mcStream_t` 等。它提供 `cudaMalloc`/`cudaFree`/`cudaStream_t`/`cudaEventRecord`/`cudaMemcpyAsync` 和 device 侧的 `__global__`/`__device__`/`<<<>>>` 语法给全栈使用——应用用它分配内存和创建 stream，Runtime 的接口里带 `cudaStream_t` 类型，DataPath 用它在 stream 上 launch IO kernel。

**StorageTargetResolver** — 把 URI（如 `file:///data/kv_cache/layer_0.bin`）解析成物理存储目标。ext4 resolver 的做法是打开文件、调 `FS_IOC_FIEMAP` 拿物理 extent 分布、验证 extent 完整性（无 hole、无 overlap、完整覆盖文件范围）、持有 fd lease。产出的是一个 type-erased 的 `ResolvedTarget`，里面带着物理映射信息但不含任何传输私有类型。

**Binding** — Resolver 和 DataPath 之间的 pair-private 契约。比如 `ext4_local_nvme` binding 定义了 `Ext4LocalNvmePayload`（包含 NVMe namespace identity + extent 列表 + 文件大小），resolver 产出它，DataPath 消费它。payload type id 和 API version 只在 binding header 里定义一次，两边都调 binding helper，物理上不会不一致。Runtime 不看 payload 内容，只检查 type/version 兼容性。

**DataPath** — 具体的数据移动实现。`LocalNvmeDataPath` 是第一个完整实现：它从 `ResolvedTarget` 取出 extent payload，构建 GPU 显存里的 `DeviceTargetHandle`；调 `nvm_dma_map_data_device()` 把 GPU buffer 注册到 NVMe controller 拿到 DMA 地址；submit 时按 MDTS 拆请求、构造 PRP、分配 descriptor lease，把 IO kernel 排入 caller 的 CUDA stream。GPU kernel 内部自己做 LBA 解析、SQE 构造、doorbell 敲击、CQ 轮询。queue group、PRP pool、metadata cache 这些都是 DataPath 私有资源，Runtime 看不到。

**FrameworkAdapter** — 把上层框架的概念翻译成通用 `IoRequest`。比如 vLLM 的 `VllmAdapter` 把 KV cache 的 layer/block 布局、request 生命周期、block id 映射成 `MemoryHandle` + offset 和 `TargetHandle` + offset，然后调 `StorageRuntime::submit()`。framework 的类型（tensor shape、scheduler metadata、Python class）不进入 core runtime。

---

### 重构后：按扩展边界组织

核心思路：让每类变化停留在正确位置，不再通过修改公共 union/enum 来加功能：

```
Framework layout 变化          → FrameworkAdapter
URI / filesystem 变化           → StorageTargetResolver
GPU 厂商 / device memory 变化   → TUTTI_ACCELERATOR profile + cuda_like shim
IO 数据移动方式变化             → DataPath
Batch 生命周期 / 错误 / 背压     → StorageRuntime
PRP pool / metadata cache / kernel 优化 → LocalNvmeDataPath 私有实现
Linux kernel / peer-memory API   → snvme compat + peer_memory_ops
```

上层应用看到的公共 noun（`IoRequest`、handles）不变。（`TUTTI_TARGET_ARCHITECTURE.md` 第 23 节有完整边界总结。）


---

## 硬件抽象层的边界划分

硬件抽象分三个正交边界，各自独立演进。

### 边界一：snvme 内核模块

```
userspace LocalNvmeDataPath
          │ stable, versioned ioctl UAPI
          ▼
include/uapi/tutti_snvme.h       # 唯一共享定义
          │
┌─────────▼────────────────────────────────────┐
│ snvme common                                  │
│ queue-group、mapping lifecycle、ioctl validation│
├───────────────────────┬──────────────────────┤
│ kernel compat ops     │ peer-memory ops      │
│ 5.4 / 5.15 / 6.1...   │ dma-buf / NVIDIA P2P / ...│
└───────────────────────┴──────────────────────┘
```

仓库里只能有一份被构建的 snvme 源码；UAPI 头由内核和 libnvm 共同 include，不手工复制 struct；kernel 版本差异集中到 compat 层；GPU pin/map 通过 `peer_memory_ops` 隔离，`map.c` 的 common 流程不直接写死 `nvidia_p2p_*`。模块由部署系统预装，普通测试不自动 `insmod/rmmod`。

### 边界二：上层运行时库（用户态公共 API）

公共层只保留稳定 noun，不放任何硬件私有类型。头文件都在 `tutti/include/tutti/` 下：

```
tutti/include/tutti/
├── storage_runtime.h      # StorageRuntime: open/close/register/submit/query/wait
├── io_types.h             # IoRequest: direction + MemoryHandle + offset + TargetHandle + offset + length
├── memory_types.h         # MemoryHandle / MemoryView / MemoryKind / MemoryOwnership
├── status.h               # Status / Result<T> / StatusCode
├── cuda_like.h            # profile selector: CUDA direct | MACA/MUSA shim | HOST
├── gpu_vendor/            # 厂商 shim header
└── spi/                   # 仓内 source-level 扩展接口
    ├── data_path.h        # DataPath SPI
    └── storage_target_resolver.h  # Resolver SPI
```

`IoRequest` 只有 6 个字段——direction、memory handle + offset、target handle + offset、length（`io_types.h:76-87`）。没有 PRP、LBA、extent、fd、framework block id、stream。accelerator 类型（`cudaStream_t`）只能通过 `tutti/cuda_like.h` 拿到。

CUDA-like profile 的机制是：构建时 `TUTTI_ACCELERATOR=CUDA|HOST|MACA|MUSA|...` 恰好选一种，CMake 只生成一个 target-scoped `TUTTI_USE_<PROFILE>` 定义。NVIDIA 直接 include CUDA headers；兼容厂商通过 shim 映射；没选中的 SDK 不参与 configure/include/link。目前 `cuda_like.h` 里 CUDA 和 HOST profile 已实现，MACA/MUSA 是 `#error` 占位等厂商 shim。

跨厂商编译的方案参考了Mooncake 的做法（`third_pkgs/Mooncake/mooncake-transfer-engine/`）。Mooncake 的 `src/` 下没有任何 `.cu` 文件，host 侧全是 `.cpp`，通过 `include/cuda_alike.h` + `include/gpu_vendor/maca.h` 做纯 `#define` 映射（`#define cudaMalloc mcMalloc`、`#define cudaStream_t mcStream_t` 等），CMake 只设 include path 和 link library，不换编译器。

### 边界三：IO kernel（DataPath 私有）

IO kernel、PRP、SGL、SQE、CQ polling、descriptor cache 都是具体 DataPath 的私有实现，不进公共 API。当前 `data_paths/local_nvme/` 下的结构：

```
data_paths/local_nvme/
├── io/                        # target/memory/submit/completion
│   ├── nvme_queue_group.*     # SQ/CQ ring + doorbell
│   ├── device_target.*        # GPU-resident target handle
│   ├── submit_one.*           # GPU kernel: SQE + doorbell + CQ poll
│   └── prp_builder.h          # PRP SINGLE/DUAL/LIST 构造
├── local_nvme_data_path.*     # DataPath SPI 实现
├── metadata/                  # (规划) MetadataArena / TieredHandleCache / PrpPageCache
├── control/                   # (规划) resource grant
└── interop/cuda_like/         # (规划) host_launch / device_api / profile_overrides
```

当前 `data_paths/local_nvme/io/` 下的 `.cu` 文件（`submit_one.cu`、`device_target.cu`、`nvme_queue_group.cu`）直接用了 `__global__`/`__device__`/`<<<>>>` 和 `cudaMalloc`/`cudaMemcpy` 等 CUDA runtime API，还有 `#include <cuda_runtime.h>` 和 libnvm device header 依赖。

Device 侧 Mooncake 的做法是：定义一组公共 device 函数接口（`mc_ld_acquire`、`mc_st_release` 等），每个厂商在自己的 `.cuh` 里实现（`include/transport/device/cuda/cuda_ops.cuh` 用 PTX inline asm，`include/transport/device/maca/maca_ops.cuh` 用 `__threadfence_system` + volatile）。选择器文件 `device_ops.cuh` 是唯一有 `#ifdef` 的地方，kernel 本身零 `#ifdef`。注释明确写道："MACA's cu-bridge compiler accepts CUDA-like intrinsics, but does not reliably compile the PTX acquire/release/barrier instructions"——也就是说沐曦的编译器能认 `__global__`/`__device__`/`<<<>>>`，真正需要厂商替代的是 PTX inline assembly 这类底层原语。

Tutti 的跨厂商方案沿用同样的思路：

- Host 侧：`tutti/cuda_like.h` + `gpu_vendor/maca.h` 做 `#define` 映射，覆盖 `cudaMalloc`/`cudaStream_t`/`cudaEventRecord` 等 runtime API。和 Mooncake 一样不换编译器。
- Device 侧：`__global__`/`__device__`/`<<<>>>` 这些 CUDA 语法沐曦的 cu-bridge 编译器能直接编译，不需要整份 `.cu` 替换。真正需要厂商特定实现的是 PTX inline assembly、GPU atomics、memory ordering intrinsics 等底层原语——放在 `interop/cuda_like/profile_overrides/<vendor>/` 下，用 Mooncake 的"公共接口 + 厂商实现 + 单一选择器"模式。
- 需要审计的是 libnvm 的 device header（`ctrl.h`、`queue.h`、`nvm_parallel_queue.h`）里有没有 PTX 或 CUDA 专有 intrinsics，如果有，同样需要厂商替代。

当前代码还没做这个抽象——`submit_one.cuh` 直接写了 `__device__`/`__global__`，`#include <cuda_runtime.h>` 也是直接 include。接入沐曦时需要：加 `gpu_vendor/maca.h` shim、审计 libnvm device header 的厂商兼容性、对 PTX/intrinsics 部分提供厂商替代实现。这是沐曦可以认领的范围。

---

## Tutti runtime 与后端存储的解耦

### DataPath SPI

DataPath 是 runtime 和后端存储之间的完整数据面 SPI，覆盖 target 生命周期、memory registration、submit/progress/query/release：

```cpp
// tutti/include/tutti/spi/data_path.h
class DataPath {
public:
    virtual const DataPathCapabilities& capabilities() const = 0;
    virtual Status initialize(const DataPathConfig&, ResourceProvider&) = 0;
    virtual Status shutdown(std::uint64_t timeout_ns) = 0;

    // target 生命周期
    virtual Result<DataPathTarget> open(const ResolvedTarget&) = 0;
    virtual Status close(DataPathTarget) = 0;
    virtual Result<RegistrationDomainKey> registration_domain(DataPathTarget) const = 0;

    // memory registration（DataPath 负责 DMA mapping，不放 HAL）
    virtual Result<DataPathMemory> register_memory(const DataPathMemoryView&, const RegistrationDomainKey&) = 0;
    virtual Status unregister_memory(DataPathMemory) = 0;

    // submit / progress / query / release
    virtual SubmitOutcome submit(const DataPathRequest*, std::size_t, const HostSubmitContext&) = 0;
    virtual Result<ProgressResult> progress(ProgressBudget) = 0;
    virtual Result<DataPathSnapshot> query(DataPathOp) const = 0;
    virtual Status release(DataPathOp) = 0;
};
```

DMA registration 放在 DataPath 而不是硬件抽象层，是因为映射结果同时取决于 accelerator memory、storage controller、IOMMU domain 和传输实现。放 HAL 会让 HAL 依赖 libnvm/RDMA/cuFile；放单个 `MemoryRegion::backend_private` 又没法表示同一块 buffer 对多个 DataPath 的映射（`TUTTI_TARGET_ARCHITECTURE.md` 第 9.3-9.4 节）。

### StorageTargetResolver + DataPath + Binding：两个正交的扩展轴

Resolver 和 DataPath 是两个独立变化的扩展轴。**Resolver 回答"目标在哪、物理地址是什么"**，**DataPath 回答"怎么把字节搬过去"**。同一个 DataPath 可以接不同 Resolver，同一个 Resolver 也可以对接不同 DataPath：

核心目的：

1. 隔离实现细节：Runtime 和公共 API 不需要知道 FIEMAP、物理 offset、NVMe LBA 等。
2. 类型/版本安全：binding 统一定义 payload 的 type id、版本和推荐 DataPath；DataPath 消费前会校验，避免 resolver 与 DataPath 静默错配。
3. 资源生命周期正确：ResolvedTarget 同时持有 payload 和文件描述符 lease，确保物理映射在使用期间有效。
4. 限制耦合范围：binding 是 resolver 与 DataPath 之间的 pair-private 契约，不是公共插件 API，也不负责实际 IO。


| 场景 | Resolver 做什么 | DataPath 做什么 | Binding |
|---|---|---|---|
| ext4 文件 + 本地 NVMe | FIEMAP 拿 extent → LBA | libnvm + GPU kernel 敲 doorbell | `ext4_local_nvme` |
| 裸盘 + 本地 NVMe | URI 参数直接给 namespace + LBA range | 同上（搬运机制一样） | `raw_local_nvme`（新建） |
| DFS + RDMA | 问 DFS metadata server 拿 chunk 分布 + rkey | RDMA verbs 读写远端内存 | `dfs_rdma`（新建） |
| DFS + 本地 NVMe（chunk 在本机） | DFS metadata 说在本地 → FIEMAP | libnvm + GPU kernel | `dfs_local_nvme`（新建） |

比如裸盘场景：URI 是 `nvme:///dev/nvme0n1?ns=1&offset=0&size=1G`，Resolver 从 URI 参数直接拿 namespace ID + LBA range，不需要 FIEMAP。但 DataPath 还是 `LocalNvmeDataPath`——因为搬运机制相同，都是 GPU kernel 敲 NVMe doorbell。区别只在 Resolver 怎么拿到物理地址。

再比如分布式文件系统 + RDMA：Resolver 问 DFS metadata server 拿到 chunk 分布（哪些 chunk 在哪个节点、rkey 是什么），DataPath 用 RDMA verbs 读写远端内存。这里 Resolver 和 DataPath 都和本地 NVMe 场景不同。

### Binding 是什么

Binding 是 **一个 Resolver 和一个 DataPath 之间的 pair-private payload 契约**——就是"Resolver 产出什么数据结构，DataPath 怎么取出来用"。它存在的理由是：Runtime 需要把 Resolver 的产出传给 DataPath，但 Runtime 自己不能理解内容（不能知道 extent、LBA、rkey），否则每加一个后端就要改公共 API。


具体来说，Binding 是一个 header 文件，定义三样东西：

1. **Payload 类型**——Resolver 产出、DataPath 消费的数据结构。比如 `Ext4LocalNvmePayload` 包含 namespace identity + extent 列表 + 文件大小。
2. **Type ID 字符串 + API version**——让 Runtime 做 type-erased 传递（`shared_ptr<void>`），DataPath 取回时校验类型和版本匹配。
3. **两个 helper 函数**——`make_resolved_target()`（Resolver 侧打包 payload + lease）和 `view_payload()`（DataPath 侧检查 type/version 后取回 payload 指针）。

当前的例子是 `bindings/ext4_local_nvme/binding.h`：

```cpp
// payload 类型: Resolver 产出, DataPath 消费
class Ext4LocalNvmePayload {
    NamespaceIdentity ns;      // PCI addr + namespace ID + block size
    vector<Extent> extents;    // logical_offset → device_offset + length
    uint64_t file_size;
};

// 唯一身份标识 (type id + version 只在这里定义一次)
constexpr string_view kPayloadTypeId = "ext4-local-nvme-payload-v1";
constexpr uint32_t    kPayloadApiVersion = 1;

// Resolver 侧: 把 payload + fd lease 打包成 type-erased ResolvedTarget
Result<ResolvedTarget> make_resolved_target(..., payload, lease);

// DataPath 侧: 检查 type/version 匹配后取回 payload 指针
Result<const Ext4LocalNvmePayload*> view_payload(const ResolvedTarget&);
```

Resolver 产出 `ResolvedTarget`（type-erased，Runtime 只看 type id 和 version），DataPath 调 `view_payload()` 取回具体 payload。Runtime 全程不看 payload 内容。如果要做裸盘，新写一个 `bindings/raw_local_nvme/binding.h` 定义 `RawNvmePayload`（含 namespace + start LBA + size），配合 `RawNvmeResolver`。`LocalNvmeDataPath::open()` 根据 payload type id 走不同分支——ext4 payload 走 extent 表构建，raw payload 走直接 LBA range。

一次 `open()` 的完整流程：

```
应用调 rt->open("file:///data/kv_cache/layer_0.bin")
  │
  │  URI scheme "file" → Runtime 选 LocalFileResolver
  ▼
LocalFileResolver::resolve()
  │  open(fd) → FS_IOC_FIEMAP → extents → validate
  │  调 binding::make_resolved_target() 打包:
  │    payload = Ext4LocalNvmePayload{ns, extents, file_size}
  │    lease  = FileDescriptorLease{fd}
  │    type_id = "ext4-local-nvme-payload-v1"
  │  产出 type-erased ResolvedTarget
  ▼
Runtime 收到 ResolvedTarget, 看 recommended_data_path_key = "local-nvme-ext4"
  │  匹配到 LocalNvmeDataPath 实例
  │  调 DataPath::open(ResolvedTarget)
  ▼
LocalNvmeDataPath::open()
  │  调 binding::view_payload(target) → 取回 Ext4LocalNvmePayload*
  │  检查 type_id + version 匹配 (不匹配返回 UNSUPPORTED)
  │  从 payload 取 extents → 转 LbaExtent
  │  构建 GPU-resident DeviceTargetHandle (extent 表放 GPU 显存)
  ▼
Runtime 拿到 DataPathTarget, 存入 TargetRegistry
  │  返回 opaque TargetHandle 给应用
```

### 怎么增加新的 IO 路径

加新后端时，先想清楚要加的是 Resolver、DataPath、还是两个都要加：

**只加 Resolver（搬运机制不变，只是换一种拿地址的方式）：**
- 裸盘：写 `resolvers/raw_nvme/` + `bindings/raw_local_nvme/`，DataPath 复用 `LocalNvmeDataPath`
- 新文件系统（xfs/btrfs 等）：写 `resolvers/xfs/` + `bindings/xfs_local_nvme/`，FIEMAP 是 VFS 级 ioctl，ext4 resolver 其实已经能工作

**只加 DataPath（目标地址解析方式不变，换搬运机制）：**
- GDS：写 `data_paths/gds/`，Resolver 可以复用 `LocalFileResolver`（还是 FIEMAP 拿 extent），但 binding 要新建 `bindings/ext4_gds/`（payload 不同——GDS 需要 cuFile handle 而不是 NVMe LBA）
- NVLink 跨 GPU 传输：写 `data_paths/nvlink/`，利用 NVLink P2P 直接搬数据

**Resolver 和 DataPath 都加：**
- DFS + RDMA：写 `resolvers/dfs/` + `data_paths/rdma/` + `bindings/dfs_rdma/`
- NVMe-oF：写 `resolvers/nvme_of/` + `data_paths/nvme_of/` + `bindings/nvme_of/`

每个新 DataPath 需要提供：`DataPathCapabilities` 声明（支持的 profile/memory kind/execution domain/alignment/limits）、完整的 open/register/submit/progress/query/release 实现、registration domain 定义、contract tests（含 hardware-free negative tests）、feature ON/OFF CI。（`TUTTI_TARGET_ARCHITECTURE.md` 第 16 节有完整的扩展场景验算。）

### 多 NVMe 设备组合（striping）

旧 Tutti 通过 `block_storage` 层的 GpuFile sharding 把多个 NVMe 设备组合成一个更大的逻辑空间：数据按 `tensor_size` 粒度 round-robin 交错分布到 N 个 shard（最多 4 个，每个对应一个 NVMe 控制器）。GPU kernel 里每个 thread 调 `gpu_file_resolve(tensor_size, num_shards, byte_offset)` 算出落在哪个 shard，再取对应 shard 的 handle 发 NVMe 命令（`block_storage/include/gpu_file_resolve.h:105-124`）。多个 shard 天然并行——各有独立的 SQ/CQ 队列。

在新架构里，striping 是 **DataPath 的私有实现细节**，不是 Runtime 的事。Runtime 看到的是一个 `TargetHandle`，`IoRequest.target_offset` 是逻辑偏移，DataPath 内部做 shard 解析。各层的分工：

- **Resolver**：产出包含多个 shard 信息的 payload。比如一个 `StripedExt4NvmePayload` 包含 N 个 `{namespace, extent_list}`（每个 shard 一份）+ striping 参数（`tensor_size`, `num_shards`）。Resolver 打开 N 个文件、对每个做 FIEMAP，打包成一个 `ResolvedTarget`。
- **Binding**：定义 `StripedExt4NvmePayload` 类型 + type id + version。
- **DataPath**：`LocalNvmeDataPath::open()` 从 payload 取出 N 个 shard 的信息，为每个 shard 构建 `DeviceTargetHandle`（extent 表 + queue 指针），再把 N 个 shard handle 组成 `StripedDeviceTargetHandle` 放到 GPU 显存。GPU kernel 保留 `gpu_file_resolve()` 逻辑——先算 shard_idx 和 shard_local_offset，再取对应 shard 的 handle 发 NVMe 命令：

```
IoRequest { target_offset = 5 * tensor_size }
  │
  │  DataPath 内部 (GPU kernel):
  ▼
gpu_file_resolve(tensor_size, num_shards, target_offset)
  → shard_idx = 2, shard_byte_off = 1 * tensor_size
  │
  ▼
StripedDeviceTargetHandle.shards[2]  → NvmeFileDeviceHandle
  → resolve_lba(shard_byte_off) → starting_lba
  → SQE + doorbell (shard 2 的 queue)
  → poll CQ
```

- **Runtime**：一个 `TargetHandle`，一次 `submit()`，一个 `IoHandle`。不知道有 N 个 shard、不碰 `gpu_file_resolve`、不碰多个 queue。
- **应用**：一个文件路径，逻辑偏移从 0 开始连续。不需要知道数据分布在几个 NVMe 上。

`gpu_file_resolve()` 本身是 `constexpr` + `__host__ __device__`，可以直接从旧代码搬到 `data_paths/local_nvme/io/` 下。每个 shard 有自己的 `NvmeQueueGroup`（已经是独立 RAII owner，建 N 个就行）。这属于 `Roadmap.md` Phase 4 之后的工作，v0.1 不恢复 striping，但架构上已经留好位置——不需要改公共 API，只在 `data_paths/local_nvme/` 内部扩展。

---

## 上层应用的集成方式

应用和 Framework Adapter 只面对 `StorageRuntime` 的稳定接口，不直接 include 厂商 shim、libnvm、PRP、LBA 或 FIEMAP extent：

```cpp
#include <tutti/storage_runtime.h>
#include <tutti/cuda_like.h>

// 1. 创建 Runtime（注入 Resolver + DataPath）
auto rt = StorageRuntime::create(config).value();

// 2. 注册内存（一次注册，多 DataPath 复用）
auto mem = rt->register_memory({ptr, size, MemoryKind::DEVICE, device_id}).value();

// 3. 打开存储目标（URI → Resolver → DataPath）
auto target = rt->open("file:///data/kv_cache/layer_0.bin", {}).value();

// 4. 提交 IO（host 发起，device 执行）
IoRequest req{
    IoDirection::READ, mem, 0, target, layer_offset, tensor_size};
HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, device_id, stream};
auto outcome = rt->submit(&req, 1, ctx);

// 5. 观察完成
auto result = rt->wait(outcome.io.value(), timeout_ms);
```

FrameworkAdapter 负责把 framework 的生命周期和 tensor/KV/block layout 翻译成 `MemoryHandle`、`TargetHandle`、`IoRequest`。以 vLLM 为例：scheduler 产出 metadata → VllmAdapter 锁定 `KVConnectorBase_V1` 版本 → 每个 worker/TP rank 拥有自己的 `StorageRuntime` → 整块 KV allocation 注册一次 → block/layer id 变成 `MemoryHandle` + offset → submit `IoRequest[]` → `IoHandle` 映射回 request id → IO 终态后才释放 framework KV block。

Core Runtime 不包含 framework callback、Python class、scheduler metadata 或 tensor shape。vLLM 对外包装按协议可以叫 `TuttiKVConnector`，内部类还是 `VllmAdapter`。（`TUTTI_TARGET_ARCHITECTURE.md` 第 15 节。）

---

## Roadmap 讨论

当前版本 `v0.1`，Roadmap 分 8 个 Phase（`Roadmap.md`），下面按议程子项过。

### 硬件适配

#### 内核版本扩展

snvme 当前基于 Linux 5.15 baseline（`tutti/device_manager/nvme/kernel_modules/snvme-5.15.0-public/`）。目标是把 kernel-version 差异集中到 compat ops 层，至少两个受支持 baseline 进 compile-only CI，UAPI 用固定宽度类型 + ABI version/capability handshake。如果沐曦的部署环境用不同 kernel baseline，可以贡献 compat 适配和 compile-only CI 矩阵。

#### 多厂商加速卡

Tutti 采用 NVIDIA-first CUDA-like API 模型（`TUTTI_TARGET_ARCHITECTURE.md` 第 10 节）。NVIDIA 是参考实现，`TUTTI_ACCELERATOR=CUDA` 直接 include CUDA headers。沐曦对应 `MACA` profile，通过 `tutti/gpu_vendor/maca.h` shim 把 CUDA 风格调用映射到 `mc*` API。AMD 对应 ROCm/HIP，Ascend 需要评估是否满足 CUDA-like 最小 contract。

接入路径（第 10.5 节）：

1. 加 `cmake/accelerators/MACA.cmake` + `include/tutti/gpu_vendor/maca.h` shim
2. 过 CUDA-like contract tests（allocation/pointer/stream/event/copy）
3. 优先用同一份 `LocalNvmeDataPath/interop/cuda_like/` 编译；shim 不够时才加 `profile_overrides/maca`
4. 加对应 peer-memory/driver capability
5. 真实硬件 IO 测试通过后才声明 local-NVMe 支持

要强调的一点："CUDA-like 可编译"不等于 DataPath 可用。device compiler、kernel launch、atomics、P2P、GPUDirect peer-memory、doorbell MMIO 这些能力必须由 profile 和具体 DataPath 单独验证，不是 shim 过了就完事。


