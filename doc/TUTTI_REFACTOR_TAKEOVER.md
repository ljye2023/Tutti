# Tutti 重构理解、评估与接手方案

**评审范围：** `97cedd27d0ee801e62cd257c613c4695b0f90df4^..HEAD`（含起始提交；评审时 `HEAD=1e0b333d9f6fc1b4f70733a791d979acf4523a26`）

**评审日期：** 2026-07-30

**目标：** 将上层运行时与内核侧的硬件/传输特殊逻辑隔离到可替换后端，使 Tutti 能支持 local NVMe、GDS、RDMA、POSIX/其他存储实现。
**说明：** 本文以当前源码为准，`doc/layered/` 为作者最新文档基线；较早的 `doc/design/`、`doc/refactor/` 和组件内验证报告只用于理解设计演进。未加载/卸载内核模块，也未执行真实硬件 IO。

---

## 1. 结论先行

这个重构的**方向基本正确，但尚未形成可接管的稳定主干**。

作者已经完成了三个有价值的基础动作：

1. 建立 L0/L1 accelerator abstraction，将大部分公开 API 中的 CUDA 类型替换成不透明句柄。
2. 将 L2 Device Manager 改成 `IDeviceDriver` 插件模型，并用 mock 验证了 common 接口不依赖 NVMe/CUDA。
3. 将 local NVMe 的控制面、用户态队列库、服务和后端代码聚拢到 `tutti/` 子树；作者报告直接调用 L3 NVMe 后端时的 RAW GPU IO 已通过硬件验证。

但按“新增一个存储后端时无需修改通用层”这一最终验收标准，当前仍处于**早中期**，不适合给出可复核性不足的完成百分比。本次新增的大量行数来自两套完整 Linux NVMe driver 源码复制，不能视为同等比例的抽象进度。

当前最关键的事实是：

- **L1-L4 也没有形成可从公开 API 走通的完整链路。** L3 硬件测试手工调用 `nvm_dma_map_data_device()`；正式代码没有任何路径把 `MemoryRegion` 映射成 `backend_private/ioaddrs`。
- **L4 名义上是 backend-neutral，实际直接依赖 `nvme::IBatchSubmitter`、NVMe PRP descriptor 和 NVMe target handle。** 第二个后端无法只实现 `IBackend` 后接入。
- **L5/L6 在旧 `IBackendProvider` 上提前实现，L3 接口重做后整体失编译并被禁用。** 现在不能作为上层 API 使用。
- **内核模块只是搬运/复制，不是抽象。** 新树没有独立构建接线，父工程仍构建旧树；两棵树已经分叉。
- **当前数据路径有多项确定的数据正确性和生命周期问题。** 包括 DMA 页索引错误、混合 target 错路由、非块对齐 IO 越界、异步共享 scratch 覆盖、释放后缓存返回悬空 target handle 等。

因此不建议继续从 L5/L6 往上补功能。接手后的第一目标应是：**借鉴 Mooncake 的窄 runtime 模型，但定义 Tutti 自有的 `StorageRuntime` 契约，并打通正确、可测试的 file-backed local-NVMe 垂直切片。`raw_device` 不进入重构目标。**

---

## 2. 我对作者意图的理解

### 2.1 原始问题

旧 Tutti/GeminiFS 将多个不同维度的特殊逻辑混在一起：

- CUDA 内存、stream、event、kernel launch；
- NVIDIA GPU page pin / P2P DMA；
- libnvm、NVMe queue、PRP/SGL、doorbell/CQ polling；
- snvme 内核 ABI 和具体 Linux kernel baseline；
- FIEMAP、文件元数据、striping；
- 上层 KV-cache/file API。

这使“换 GPU vendor”“换传输后端”“换 namespace/文件系统”“换 kernel 版本”彼此耦合。

### 2.2 作者最终想建立的分层

当前 `doc/layered/architecture-overview.md:21-76` 给出的主线是：

```text
L6  Coordinator                 应用 façade / 依赖注入 / buffer registry
 ↓
L5  Storage Interfaces          block/file 与 raw device 两个并列入口
 ↓
L4  IO Engine                   request fan-out / batch / async / dispatch
 ↓
L3  Backends                    transport backend、target、descriptor、submit
 ↓
L2  Device Manager              物理设备发现、跨进程所有权、vdevice 切片
 ↓
L1  Accelerator HAL             allocation / registry / stream / event / IPC
 ↓
L0  Device-code macros          CUDA/ROCm 等编译期语法映射
```

另一个重要设计是把扩展拆成两个正交轴：

```text
namespace / metadata                         transport / data path
(filesystem、object、DFS resolver)           (NVMe、GDS、RDMA、POSIX)
            \                                  /
             \------ StorageTarget ----------/
```

也就是：namespace 层只把“名字/对象”解析成 target，backend 只消费 target 并移动字节。该思想见 `doc/design/storage-extensibility.md:56-85`，是本次设计中最值得保留的部分。

### 2.3 提交演进

| 阶段 | 提交 | 意图与结果 |
|---|---|---|
| 设计 | `97cedd2`, `17c719f` | 提出 L0-L6 clean-slate 分层和逐层实现计划。 |
| L0/L1 | `2556c4a` | 实现 CUDA HAL 和宏层；修复 64 KiB 对齐指针释放；当时 11 个 smoke tests 通过。 |
| 初版 L2 | `2467bac` | 把 libnvm、nvmeservice、snvme 和 NVMe device code 搬到 `tutti/device_manager/nvme/`；完成一次硬件验证。主要是目录隔离和复制。 |
| 初版 L3/L4/L5/L6 | `9fb833a`–`8179081` | 基于 `IBackendProvider` 快速向上实现全部层，并给出“完成/通过”报告。此时很多测试是类型、mock 或直接组件测试。 |
| L1 清理 | `4298337`, `7805980` | vendor-neutral 命名、GoogleTest 和 standalone build。 |
| L2 重做 | `17be531`–`d399b37` | 用 `IDeviceManager` + `IDeviceDriver` + `I{Physical,Virtual}Device` 替换 NVMe 耦合接口；daemon real path 完成，direct path仍为 stub；删除旧 L2 API。 |
| L3 重做 | `cc430c4` | 删除 `IBackendProvider`，改为很窄的 `IBackend`；NVMe 私有提交保留在具体类/`IBatchSubmitter`。这一步改善了 common header，但直接打断 L4-L6。 |
| 缺口显式化 | `8132b27` | 文档明确 CPU sync、SGL、错误回传、RDMA/GDS 未实现。 |
| L4 暂存迁移 | `530890d` | 引入 NVMe 私有 `IBatchSubmitter` 让 L4 重新编译；增加 `submit_one`/StripeManager，同时删除旧 smoke source，但新测试 CMake 仍引用它。 |
| 文档归并 | `1e0b333` | `doc/layered/` 成为最新事实源，诚实记录 L4-L6 失效状态。 |

整体演进说明作者已经意识到“目录搬迁不等于抽象”，并在 L2/L3 上进行了第二轮接口重做；但重做发生在 L5/L6 已经铺开之后，所以当前存在两代接口并存、上层整体断裂的问题。

### 2.4 对当前层次、目录和命名的评价

**结论：L0-L6 适合作为迁移时的职责清单，不适合作为最终代码结构。** 当前系统的真实关系是一个 DAG，不是七层线性栈；强行编号导致“先填满层数、后稳定接口”，也是 L5/L6 在 L3 重做后整体失效的直接原因。

| 当前区域 | 合理性判断 | 建议 |
|---|---|---|
| L0 `abstraction/` | 不应是独立层。一个 device qualifier 宏头只是 platform/device SDK 的实现细节；`types/` 目录甚至不存在。 | 合并到 `platform/`，公共值类型归 `include/tutti/`。 |
| L1 `accel/` | 现有 `IAccelerator` 同时做 allocation、registry、stream/event、kernel launch、IPC，职责过宽；`MemoryRegion::backend_private` 又塞入 data-path registration。 | 不保留目标虚接口；CUDA runtime 能力迁入 NVIDIA-first `cuda_like.h`，memory identity 归 Runtime，DMA registration 归 `DataPath`。 |
| L2 `device_manager/` | common 接口有价值，但目录/target 不内聚：通用接口、mock、NVMe driver、libnvm、gRPC daemon、CUDA queue allocator、内核模块被放进一个构建单元。 | 通用资源接口按需保留；NVMe resource/control 全部收进 `LocalNvmeDataPath` package。不要把所有 data path 都强迫成 vdevice/queue 模型。 |
| L3 `backends/` | “backend” 已被 Tutti、vLLM、Mooncake 多处过载；当前 `IBackend` 又只有 lifecycle/roster/metadata，不是数据面 SPI。 | 改称 `DataPath`，定义完整 target + memory registration + submit + progress/completion 契约。 |
| L4 `io_engine/` | Engine 概念合理，但 `TransferEngine` 又是 Mooncake 标志性名字；当前实现则是 NVMe executor，直接理解 PRP 和 `IBatchSubmitter`。 | 对外统一为 `StorageRuntime`；具体 descriptor/kernel/workspace 全留在 `DataPath` 内。 |
| L5 `block_storage/` | 把文件目录、LBA 分配、WAL、striping、backend handle 和 durability 一次性都做了，超出本轮重构目标。 | 本轮不恢复。只保留最小 target/file resolver seam，具体 file store/striping/WAL 等有需求再实现。 |
| L5 `raw_device/` | 两份实现、两套接口，且不是当前需求。 | 从目标架构和接手计划中移除；不要为方便测试把内部 LBA range 提升成公共 feature。 |
| L6 `coordinator/` | 名字含混；代码并不 bootstrap L2-L5，只是再次包装 registry、stream 和 submit。 | 取消独立层，职责并入公开 `StorageRuntime`。 |

当前目录也没有沿真实插件/部署边界组织：

- 一个 local-NVMe backend 被横切到 `device_manager/nvme/`、`backends/nvme/`、`io_engine/`，上层修改很容易跨三个所谓层。
- `libnvm`、`nvmeservice`、`snvme` 是同一 backend 的用户态 ABI、控制面服务和内核组件，却被称为通用 Device Manager 的内容。
- `raw_device` 同时存在于 `coordinator/` 和 `raw_device/`；多个 `GpuFile` 模型并存。
- 根目录旧实现和 `tutti/` 新实现定义同名 CMake target，部署事实源不唯一。

命名上也有明显认知成本：

- `abstraction`、`Coordinator`、`DeviceManager` 太泛，读名字无法知道对象管理什么。
- `IoEngine` 声称通用，实际是 NVMe PRP executor；应重写成通用 engine，而不是保留误导名字。
- `IBackend` 声称 backend，却不能提交 IO；`IBatchSubmitter` 才是实际数据面且被放在 `nvme::`。
- `StorageTarget` 看似通用，字段却是 NVMe LBA/extent + RDMA rkey 的闭集。
- `GpuFile` 把 accelerator vendor/位置写进存储对象名；上层应看到 `TargetHandle`/`ObjectHandle`。
- `VDevice` 对其他 backend 含义不稳定；在 local-NVMe 内部应直接命名为 `NvmeQueueSlice` 或 `NvmeResourceGrant`。

因此接手时不应继续维护“层号完整性”。建议采用一套符合 Tutti “统一存储运行时”定位、且不借用其他项目标志性术语的名字：**`StorageRuntime`（顶层）、NVIDIA-first CUDA-like API/profile（device runtime）、`DataPath`（数据移动实现）、`StorageTargetResolver`（名字到目标）、`FrameworkAdapter`（框架适配）**。vLLM 对外类因协议要求仍可叫 `TuttiKVConnector`，但 Tutti 内部不用 `Connector`。

---

## 3. 建议采用的最终目标架构

### 3.1 借鉴 Mooncake 模型，但采用 Tutti 自有命名

不建议继续把当前 L0-L6 原样补齐。Mooncake 值得借鉴的是“少量 runtime 动作 + opaque handle + operation completion”，不是照搬它的 `TransferEngine` 名字。Tutti 的产品定位是统一存储运行时，因此顶层命名为 **`StorageRuntime`**：

```text
VllmAdapter        LmCacheAdapter          C++ application
       \                  |                    /
        └─────────────────▼───────────────────┘
                     Tutti StorageRuntime
              open(uri) / close(TargetHandle)
              register_memory / unregister_memory
              submit(IoRequest[], HostSubmitContext) -> IoHandle
              query(IoHandle) / wait(IoHandle)
                              │
                 registry / grouping / ownership /
                 backpressure / completion / errors
                              │
                 ┌────────────▼────────────┐
                 │       DataPath          │
                 │ open/register/submit/    │
                 │ progress/query           │
                 └───────┬─────────┬───────┘
                         │         │
                 LocalNvmeDataPath future data path
                                   GDS/Mooncake/DFS...
```

建议的最小公共 noun：

```text
MemoryHandle     已注册本地内存；不暴露 ioaddrs/rkey
TargetHandle     已打开存储对象；不暴露 LBA/extent/fd
IoRequest        direction + memory(handle, offset)
                 + target(handle, offset) + length
IoHandle         一批 IO 的生命周期与 completion owner
HostSubmitContext    execution domain + accelerator/device；DEVICE_EXECUTION 必填 native stream
Status           结构化错误；不能只返回 bool
```

`StorageRuntime` 本身就是 assembly root 和应用 façade，不再额外设置 `Coordinator`/`Engine`。本轮只做 file/KV-cache 主线需要的方法，不提前加入 cancel、priority、policy、notification、failover。

`HostSubmitContext` 带 `{execution_domain, device_id, cudaStream_t}`。它只描述 host API：`DEVICE_EXECUTION` 必须提供来自 `tutti/cuda_like.h` 的 stream；`HOST_EXECUTION` 可以不提供 stream。`DataPath` 把 IO kernel/fence 排入指定 stream，并把 host-visible 状态收敛到 `IoHandle`。

这不是永久的 host-only 限制。未来允许 caller device kernel 直接发起 IO，但应放在 `DataPath/interop/<accelerator>/device_api` 编译期 sidecar：host Runtime 先准备 device-visible target/memory/queue context，device API 不调用 host registry、锁或 C++ virtual。此轮只保留该边界，不冻结 device request/completion ABI。

关键约束：

1. `IoRequest` 只表达 IO 意图，不包含 NVMe descriptor、CUDA pointer type、filesystem extent 或 framework block id。
2. PRP、SGL、RDMA WR、cuFile handle 和 GPU metadata workspace 只存在于具体 `DataPath` 内。
3. `StorageRuntime` 不调用 `prepare_descriptors()`；data path 自己决定 MDTS fan-out、descriptor、kernel 和 completion strategy。
4. `DataPath` 负责 memory registration，因为映射同时依赖 accelerator memory、controller/IOMMU domain 和传输实现。一个 `MemoryHandle` 可以在 runtime 内关联多份 data-path registration，不能只放一个 `void* backend_private`。
5. public target 是带 owner 的 opaque handle。不要让 `StorageTarget` 继续增长成 NVMe/RDMA 字段大集合。
6. GPU 上不做虚函数分派。host-initiated 路径由 Runtime 选 DataPath；未来 device-initiated 路径通过编译期选中的 DataPath/accelerator sidecar 静态分派。
7. `raw_device` 不进入 public API。本轮若底层单测需要固定 LBA fixture，只作为 `LocalNvmeDataPath` 私有测试工具。

### 3.2 Accelerator、DataPath、Resolver 和 Adapter 的边界

最终不采用 TENT `Platform`/`Accelerator` 虚接口，而采用 Mooncake `cuda_alike.h` 的源码兼容方式：NVIDIA profile 直接 include CUDA headers，调用方使用 `cudaMalloc/cudaStream_t/cudaEventRecord/...`；沐曦对应 `MACA` profile，通过 shim 将 CUDA 名称映射到 `mc*` API，MUSA 等同理。

用户通过 `TUTTI_ACCELERATOR=CUDA|MACA|MUSA|HOST|<vendor>` 选择 profile，CMake 只定义一个 target-scoped `TUTTI_USE_<VENDOR>` 并启用对应 toolchain/SDK。使用 `TUTTI_` 前缀避免子项目宏冲突；vendor 分支集中在 `tutti/cuda_like.h`、`gpu_vendor/` 和 DataPath interop，不能散落进 Runtime。CUDA-like 可编译只证明 runtime source compatibility，local-NVMe P2P、device kernel、peer-memory driver 仍需独立 capability 和硬件验证。

Tutti 应拆成：

- **CUDA-like profile/shim：** 提供 NVIDIA CUDA 风格的 allocation、pointer、stream/event、copy API；NVIDIA 直接调用，兼容厂商编译期映射。它不负责 NVMe/RDMA DMA mapping。
- **DataPath：** 打开 target、注册 data-path memory、提交 IO、推进并汇总 completion；local-NVMe 的 queue/PRP/kernel/libnvm 和 metadata workspace 全在这里，也能容纳未来 Mooncake/RDMA 数据移动实现。
- **StorageTargetResolver：** 可选的 name/URI → resolved target 解析。ext4 FIEMAP、定制磁盘布局或 DFS metadata client 可以实现该 seam；若 3FS 等系统同时拥有 metadata 和 data path，也可以将 resolver 与 data path 放在同一个 package。
- **FrameworkAdapter：** 把 vLLM/LMCache 的 block id、tensor layout、scheduler/worker 生命周期翻译为 `MemoryHandle`、`TargetHandle`、`IoRequest`。framework 类型不进入 core。

文件系统当前**不能**直接接入活动的 `tutti/` 重构树：它没有 resolver SPI，`StorageTarget` 固定携带 NVMe LBA/extent，也没有接入根目录旧实现已经存在的 FIEMAP producer（`nvme_storage/include/fiemap_helper.h`、`nvme_storage/src/fiemap_helper.cpp`）。最小 `StorageTargetResolver` 本轮只需要 `resolve(uri)`；`create/remove/list/WAL/striping` 都由真实需求再增加。

Target 生命周期必须只有一个 owner：`StorageRuntime::open(uri)` 按 URI scheme 选择静态注入的 resolver；resolver 返回 RAII `ResolvedTarget`（持有 fd/inode/extent lease 和 data-path key）；runtime 再调用对应 `DataPath::open(resolved)`，并在内部 target table 同时持有两者。public `TargetHandle` 只索引该 table。关闭顺序固定为 data-path handle → resolved target。这样 resolver 不提交 IO，data path 不解析 framework 名字，应用也看不到 fd/LBA/extent。

第一条 file 路径只迁移旧代码已经具备的语义：**Tutti 管理、预分配且在 handle 生命周期内禁止 truncate/hole-punch/reflink/COW 变化的普通文件**。不承诺任意既有文件都能用物理 extent 直写；不满足 extent 稳定性、底层设备可识别或 coherency 条件时应拒绝打开，而不是静默直通。

其他 GPU 厂商当前也**不能**仅凭 CUDA-like 编译通过就宣称端到端接入。目标采用单 build/profile、同 profile 多设备：NVIDIA `CUDA` 是参考；沐曦使用 `MACA` shim，将 CUDA 风格调用映射到 `mc*`；MUSA 等同理。`MemoryHandle` 记录 profile/device identity，一次 request 的 memory、stream 和 DataPath registration 必须兼容。

未来接入兼容厂商至少需要：

1. `TUTTI_ACCELERATOR=<vendor>` profile 与 `tutti/cuda_like.h` shim；
2. CUDA-like allocation/pointer/stream/event/copy contract tests；
3. 目标 `DataPath` 的 vendor interop/device code；
4. peer-memory/driver capability；
5. 真实硬件 IO 验证。

因此“CUDA-like API 可编译”不等于 local-NVMe 已支持该厂商。`DataPathCapabilities` 仍需声明 profile/memory pair、direct/staged、方向、alignment、批次/在飞数、scatter-gather 和 completion 模型。

### 3.3 建议目录结构

目录应沿部署和插件边界，而不是层号：

```text
tutti/
├── cmake/accelerators/         # CUDA/MACA/MUSA/HOST profiles
├── include/tutti/
│   ├── storage_runtime.h       # host 应用/control-plane 入口
│   ├── io_types.h              # Memory/Target/Io handles + requests
│   ├── status.h
│   ├── cuda_like.h             # NVIDIA direct / vendor shim selector
│   └── gpu_vendor/
├── runtime/
│   ├── storage_runtime.cpp
│   ├── memory_registry.*
│   ├── target_registry.*
│   └── data_path_registry.*
├── spi/                        # 仓内 source-level 扩展接口
│   ├── data_path.h
│   └── storage_target_resolver.h
├── cuda_like/
│   ├── host_test/
│   └── contract_tests/
├── data_paths/
│   ├── mock/
│   └── local_nvme/
│       ├── io/                 # target/memory/submit/completion
│       ├── metadata/           # handle/descriptor/PRP pools
│       ├── control/            # discovery/resource grant/daemon client
│       ├── interop/cuda_like/
│       │   ├── host_launch/    # 当前 host-initiated device execution
│       │   ├── device_api/     # 未来 device-initiated sidecar
│       │   └── profile_overrides/
│       ├── userspace/libnvm/
│       ├── service/nvmeservice/
│       └── kmod/snvme/         # 唯一内核事实源
├── target_resolvers/
│   └── ext4_fiemap/            # 迁移已有 file 解析语义
└── tests/

adapters/
├── vllm/
└── lmcache/
```

`DeviceManager` 不再作为所有 data path 必经的顶层层次。现有 common 接口中可复用的资源发现/配额思想，可以变成 `LocalNvmeDataPath/control/` 的私有 `ResourceProvider`；只有出现第二个实现共享同一种资源管理模型时，再提炼公共接口。

本轮只承诺**仓内 source-level SPI**：DataPath、Accelerator、Resolver 通过构造注入或静态 factory 注册，不做 `dlopen`、稳定 C ABI、运行时卸载。`include/tutti/` 是应用 public API；`spi/` 标记 experimental。等真实第三方实现者出现后，再决定安装 SPI header 和 binary ABI。

社区友好的最低标准不是“所有东西都插件化”，而是贡献局部化：新增 accelerator/profile、DataPath、Resolver/Binding、FrameworkAdapter 或 interop sidecar 时，主要改动落在一个新 package、tests 和 docs；feature OFF 时不引入 SDK；不修改 Runtime 公共 noun；capability 和 unsupported 行为明确。kernel geometry、cache policy 等只作为 owner 内 feature，不为每个优化新增全局接口。

### 3.4 Mooncake 和 vLLM Adapter 如何接

Mooncake 有两种价值，不能混为一谈：

1. **作为设计参考：** 借鉴其窄 public API、accelerator abstraction、capability、注册内存和 operation completion 模型。
2. **作为未来 DataPath：** 若需要远端 memory/file transfer，可实现 `MooncakeDataPath`，内部委托 Mooncake `TransferEngine`；Tutti core 不 include Mooncake transport 私有类型。本轮只确保 `DataPath` 能承载，不主动实现。

Tutti 内部不定义通用 `IConnector`。vLLM、LMCache 的 scheduler 生命周期不同，正确公共连接点是 `StorageRuntime`；具体 `VllmAdapter`/`LmCacheAdapter` 各自实现 framework 官方协议。只有 vLLM 对外包装类按协议命名为 `TuttiKVConnector`：

```text
vLLM KVConnector callbacks
  → VllmAdapter 解析 cache group / layer / block layout
  → 注册整块 KV allocation（避免每 tensor 重复注册）
  → block ids 合并为 memory/target slices
  → StorageRuntime::submit(IoRequest[])
  → IoHandle 映射回 finished request ids
```

当前 `adapters/kv_cache` 不具备这个边界：它链接旧 Coordinator，公开 `cudaStream_t`，构造 `NvmeBatchInputTensor`，并假设 `GpuFile` 与固定 K/V offset（`adapters/kv_cache/include/kv_cache_io_adapter.h:5-45,51-167`；实现 `:28-80,102-179`）。可保留的只有“KV layout → IO slices”和 contiguous block 合并思路；其余应按实际 framework layout 重写为 Adapter。

实现首个 vLLM Adapter 前必须选择并锁定一个明确的 `KVConnectorBase_V1` 版本。边界固定为：scheduler 只产生 versioned metadata；每个 worker/process 按 TP rank 拥有自己的 `StorageRuntime` 和 memory registrations；Python 通过只暴露 public runtime API 的薄 C ABI/pybind 层调用 C++；`IoHandle` 到 request id 的映射保留到终态，在此之前禁止 framework 回收相关 KV blocks；abort 和 worker shutdown 先 drain/观察终态，不把 framework 生命周期塞进 core。

### 3.5 不应照搬 Mooncake 的部分

Mooncake 的总体方向值得借鉴，但不应复制其所有实现：

- classic `cuda_alike.h` 主要是按宏切换 vendor header，不是完整抽象；真正值得参考的是 TENT `Platform` 和 device plugin。
- classic `BatchID` 将对象指针强转成 `uint64_t`（`transport/transport.h:97-110`），Tutti 应使用有代际校验的 opaque handle/RAII operation，避免悬空句柄。
- Mooncake `Request::source` 是裸指针，`target_offset` 在 memory segment 中常兼作远端地址；Tutti 应使用 `MemoryHandle + offset`、`TargetHandle + offset` 做边界和所有权校验。
- TENT `TransportType` 仍是 core 中的闭集 enum；Tutti `DataPathRegistry` 不应要求每新增 data path 都修改全局 enum。
- Mooncake 的 topology policy、priority、notification、failover 是分布式传输需求，不是本轮 storage refactor 的必要条件。

### 3.6 Memory pool 与 GPU IO 优化是否能被架构承载

**当前 `tutti/` 架构不能安全承载这些优化；上述 `StorageRuntime + Accelerator + DataPath` 架构补齐最小资源/进度契约后可以。** 关键不是增加新的公共“Memory Pool 层”或“Kernel 层”，而是把优化资源放回正确 owner。

#### 3.6.1 根目录旧 `memory/` 的迁移价值

旧 `memory/` 不是一个可整体保留的通用层，它混合了 runtime memory registry、CUDA allocation/stream ordering、NVMe DMA mapping、descriptor 和 PRP cache。但其中有值得迁移的成熟组件：

| 旧组件 | 价值 | 新归属 |
|---|---|---|
| `MemoryRegion`/lookup/allocation | 用户 allocation 身份、范围和生命周期 | `StorageRuntime::MemoryRegistry` + CUDA-like allocation；去掉 NVMe 字段。 |
| `GpuSlotPool<T>` | 固定容量 GPU metadata slots、批量 H2D、跨 stream reuse fence | 基于 CUDA-like API 的私有原语；由具体 DataPath metadata arena 持有。 |
| `HostSlotPool<T>` | pinned-host metadata backing/staging | CUDA accelerator 私有原语；使用策略由 DataPath 决定。 |
| `TieredHandleCache<T>` | GPU DRAM L1 + pinned-host L2，适合数百万 file handle 的成本控制 | 先迁入 `LocalNvmeDataPath/metadata/`；第二个使用者出现后再抽公共 policy。 |
| `PrpListPool`/`PrpPageCache` | PRP page 的 DMA-resident working set、LRU、批量 `prp2` patch | 严格属于 `LocalNvmeDataPath/metadata/prp/`。 |
| `IoSliceTable`/`AddressDescriptor` | 预计算 DMA descriptor，减少 IO-time CPU 开销 | 严格属于一次 local-NVMe memory registration。 |

旧实现已经认识到 GPU DRAM 很贵：`TieredHandleCache` 用较小 GPU L1 保存热元数据、较大 pinned-host L2 保存完整模板；`PrpPageCache` 也只把当前 batch 的 PRP pages 提升到 GPU。这种**分层元数据缓存**非常符合 Tutti 的 file/KV-cache 特点，应保留设计，而不是保留当前 `memory/` 模块边界。

但代码不能原样复用：`GpuSlotPool`/cache 直接暴露 CUDA stream，PRP cache 使用共享 staging，多个并发 stream/operation 的 workspace 生命周期没有被统一 owner 持有；当前新 `tutti/` 路径反而退化成一个共享 `d_descs_`。迁移时必须改成 lease 模型：

```text
IoHandle
  └── owns DataPathOp until terminal
        ├── strong refs: MemoryHandle + TargetHandle registrations
        ├── MetadataLease: descriptor/SQE/status slots
        ├── PrpLease: DMA-mapped PRP pages
        └── CompletionFence
```

pool 只能在 operation 到达 terminal 后回收 lease；池满返回结构化 `RESOURCE_EXHAUSTED/BUSY`，由 runtime backpressure，不允许覆盖共享 scratch。pool 容量、L1/L2 hit、promotion/eviction、staging bytes 和 peak in-flight 都必须可观测。

#### 3.6.2 通用 runtime 与 local-NVMe kernel 优化的边界

`StorageRuntime` 只负责：

- 按 `{DataPath, TargetHandle}` 分组和 batch packing；
- 有界 in-flight/backpressure；
- `IoHandle` 生命周期、query/wait、错误聚合；
- memory/target 引用在 operation 终态前保持有效；
- 调用 data path 的 bounded progress；
- 通用延迟、吞吐和资源指标。

`LocalNvmeDataPath` 私有负责：

- MDTS/extent fan-out、PRP/SGL、SQE/CID；
- queue/warp mapping、doorbell batching、CQ phase/status；
- descriptor/PRP/status workspace pools；
- CUDA kernel 选择和 launch geometry；
- GPU completion strategy、HOST_EXECUTION progress 和 host-visible status harvest。

因此“减少 GPU 资源消耗”不能抽象成 public kernel API。以下策略都应在 `LocalNvmeDataPath` 内部独立 benchmark，外部 `StorageRuntime` API 不变：

1. 现状 baseline：one-thread-per-IO + thread 内 busy-poll CQ；
2. warp/lane-group per QP：同一 warp 批量 reserve CID/SQ slot、合并 doorbell；
3. split submit/completion：短 submit kernel + 少量 queue-owner warp drain CQ；
4. host-visible status harvest，或用于 `DEVICE_EXECUTION` device-side CQ 推进的小型 persistent progress kernel；
5. descriptor 长期缓存、contiguous metadata H2D、CUDA Graph 仅在 profiling 证明有效后采用。

旧 kernel 的 queue hash `return (blockDim.x * 32 + threadIdx.x) % num_queues` 忽略 `blockIdx.x`（`nvme_storage/include/queue_acquire_helper.cuh:58-65`），不同 block 重复冲击相同 queue；新 kernel 虽改成 global thread id，仍是一线程一 descriptor并在内部无限 poll。两者都不适合作为最终低占用方案。

#### 3.6.3 真正异步 IO：当前 host launch，未来 device direct submit

当前 `submit_batch_async()` 只表示“CUDA kernel 已排队”，caller 没有可拥有的 operation，且多个 stream 共用 `d_descs_`。先把当前要实现的 host-initiated/device-executed 路径做正确：

1. host `StorageRuntime::submit()` 负责 validation、资源准备和 kernel launch；
2. `DEVICE_EXECUTION` 下，host 在返回前把 GPU IO kernel 和代表真实 storage 完成的 completion fence 排入 caller 指定 stream；
3. IO kernel 与 compute kernel 一起由 GPU scheduler 调度，不同 stream 可以并发；
4. 同 stream 通过天然顺序保证 `compute producer → IO → compute consumer`；
5. 跨 stream 由 caller/Adapter 使用 `record(event)` / `wait(event)` 建立 producer、IO 和 consumer 依赖；
6. caller 在 IO stream 上、submit 返回后记录的 event 只能在真实 IO 完成后 signal；
7. `IoHandle` 用于 host status/error/lifetime，不替代 GPU stream/event 顺序；
8. 每个 operation 独立持有 descriptor、PRP、status 和 event/fence lease，不能共享覆盖 scratch。

```text
CPU host: StorageRuntime::submit(..., stream) -> IoHandle
              │ enqueue, no stream synchronize
              ▼
GPU stream: prior compute → IO kernel / completion fence → later compute/event

DataPath::submit(...)   -> DataPathOp
DataPath::progress(...) -> bounded host status harvest
DataPath::query(op)     -> PENDING / COMPLETED / FAILED / TIMEOUT
```

对于 local-NVMe `DEVICE_EXECUTION`，device-side IO 必须自主推进；即使 caller 从不调用 `query()`/`wait()`，stream 也必须最终越过 completion fence。`progress()` 只负责 host 状态收割、错误和回收，不能成为 GPU IO 前进的前提。split completion、queue-owner warp 或 persistent kernel 等优化也必须保留 caller stream 上的等价 completion fence。

未来如果应用要求 GPU caller kernel 自己发起 IO，不应把 `StorageRuntime::submit()` 做成 device 函数，而是在 `LocalNvmeDataPath/interop/<accelerator>/device_api` 提供编译期 sidecar。host Runtime 先准备 device-visible target/memory/queue context；device API 静态分派、无 host virtual/registry。本轮不实现也不冻结它，只保证目录、resource owner 和 capability 不堵死。

架构验收不看是否写了多少 pool/kernel 类，而看：同/跨 stream 顺序、多 stream 并发、无需 host polling 的 device progress，以及 GPU active warps/SM、kernel residency、doorbell/atomic 次数、CQ scan 次数、metadata H2D bytes、pool hit/eviction、in-flight 深度、submit latency、IO p50/p99、IOPS/BW 和 compute overlap。

### 3.7 内核侧目标

当前 snvme 是完整 fork 的 Linux NVMe host driver。短期无法完全消除 kernel baseline 差异，但可以隔离：

```text
userspace LocalNvmeDataPath
          │ stable, versioned ioctl UAPI
          ▼
include/uapi/tutti_snvme.h       # 唯一共享定义，显式 ABI version/capabilities
          │
┌─────────▼────────────────────────────────────────────┐
│ snvme common                                         │
│ queue-group、mapping lifecycle、ioctl validation      │
├───────────────────────┬──────────────────────────────┤
│ kernel compat ops     │ peer-memory ops              │
│ 5.4 / 5.15 / 6.1...   │ dma-buf / NVIDIA P2P / ...   │
└───────────────────────┴──────────────────────────────┘
```

最低要求：

- 仓库中只能有一份被构建的 snvme 事实源；
- UAPI 头由内核和 libnvm 共同 include，不手工复制结构；
- kernel-version 差异集中在 compat 层或可重复生成的 patch series；
- GPU pin/map 通过 `peer_memory_ops` 隔离，`map.c` common 流程不直接写死 `nvidia_p2p_*`/Phoenix symbol；
- 模块缺失、ABI 不匹配、能力缺失必须在 `LocalNvmeDataPath` 初始化时返回结构化错误，不能降级成“看起来可用”的 mock 设备；
- 构建系统只负责 build/package，不由普通测试自动执行 `insmod/rmmod`。

---

## 4. 当前实现的真实结构与调用链

### 4.1 当前可构建意图

`tutti/CMakeLists.txt:124-135` 只启用：

```text
accel → device_manager → backends → io_engine
```

`block_storage`、`coordinator`、legacy `raw_device` 全部注释。`tutti/CMakeLists.txt:17-19` 的“只构建 L0-L3”注释也已经落后于实际启用的 L4。

### 4.2 当前 backend-private RAW 测试路径（仅用于判断现状）

以下路径是当前唯一被作者报告为硬件验证的数据面证据，不代表建议保留 `raw_device` 产品接口。直接调用 L3 时的路径是：

```text
测试/调用者
  ├─ 手工 cudaMalloc / cudaHostAlloc
  ├─ 手工 nvm_dma_map_data_*() 得到 ioaddrs
  ├─ NvmeBackend::prepare_descriptors(ioaddrs)
  ├─ NvmeBackend::acquire_target_handle(NVME_RAW, vdev)
  └─ launch_batch_gpu_stream()
       → submit_batch_kernel
       → resolve_lba
       → QueuePair SQE + doorbell
       → inline poll CQ
```

该路径在作者环境做过 RAW read 和 destructive write/read/verify，见 `doc/layered/layer3-backends.md:442-476`。这证明 **snvme/libnvm/queue/kernel 的局部数据面可工作**，但不证明公开 API 或 L4-L6 已端到端可用。

### 4.3 当前 L4 路径

`IoEngineImpl` 直接持有 `backends::nvme::IBatchSubmitter*`（`tutti/io_engine/src/io_engine_impl.h:8,17-27,51-55`），流程是：

```text
IoRequest[]
  → 按 backend metadata.max_io_size 做 MDTS fan-out
  → 构造 nvme::SubSliceInfo[]
  → backend.prepare_descriptors() 生成 NVMe PRP
  → 复制到共享 d_descs_
  → backend.launch_batch_gpu_stream()
  → stream sync/event
```

所以 L4 只是在 concrete `NvmeBackend` 前增加了可 mock 的 NVMe 私有接口，并没有形成可供 GDS/RDMA/POSIX 共用的数据面 SPI。

---

## 5. 进度评估

### 5.1 分层进度

| 区域 | 当前状态 | 对最终目标的判断 |
|---|---|---|
| L0 device macros | CUDA 可用；ROCm atomics 是占位，SYCL/CANN 不支持 | CUDA 代码可复用，但宏头应归入 platform/device SDK，不保留独立层。 |
| L1 accelerator HAL | CUDA allocation/stream/event/copy 基本可用；IPC import、external teardown、host pin 缺失 | 拆为 CUDA-like API + Runtime memory identity；DMA mapping/registration 归 `DataPath`。 |
| L2 common Device Manager | common + mock 较完整，但同一 target 混入 NVMe、CUDA、libnvm、daemon | 保留 grant/accounting 思想；不把整层作为所有 DataPath 的公共前提。 |
| L2 local NVMe daemon | gRPC/libnvm/vdevice queue 实现且做过硬件测试 | 可作为 `LocalNvmeDataPath` control plane 原型。 |
| L2 direct NVMe | 伪造设备，`d_qps == nullptr` | 不可用；在真实实现前明确返回 unsupported。 |
| L3 common backend | `IBackend` 只覆盖生命周期、vdevice roster、metadata | 不是可提交 IO 的 SPI；需重定义完整 `DataPath`。 |
| L3 NVMe | RAW 直接组件测试可用；FILE metadata、跨 extent、CPU path、SGL、错误回传缺失 | 只作为 `LocalNvmeDataPath` 私有原型；RAW 不成为公共 feature。 |
| L4 IO Engine | 能编译到 NVMe 私有 SPI；StripeManager 未接线；存在数据正确性 bug | 不能原样保留；通用职责归 `StorageRuntime`，kernel/workspace 归 DataPath。 |
| L5 storage interfaces | 源码存在，但引用已删除接口，未构建；block path 不调用 engine | 不恢复现实现；仅按 file/KV 需求实现最小 target resolver。 |
| L6 coordinator | 旧 SPI 上的 mock façade，未构建；async callback 未实现 | 取消独立层；职责并入 `StorageRuntime`。 |
| Kernel portability | 两个完整 baseline + 大量 port 文档 | 只有版本复制，没有 common/compat/peer-memory 抽象。 |
| 第二存储后端 | 只有 enum 和注释 | 0%；尚未证明抽象成立。 |

### 5.2 以验收门衡量

| 验收门 | 状态 |
|---|---|
| public storage headers 只经 `tutti/cuda_like.h` 使用 CUDA-like 类型，不含 libnvm/NVMe 私有类型 | **失败**：当前无统一 selector，且 L4 headers/实现直接绑定 CUDA 与 NVMe。 |
| 公开 API → memory registration → file target IO → completion 完整可用 | **失败**：正式 DMA registration 和 file target resolver 不存在，原 L5/L6 失编译。 |
| 第二个 DataPath 只新增实现、不改 StorageRuntime | **失败**：L4 必须改掉 `nvme::IBatchSubmitter` 和 NVMe descriptor。 |
| 单一、可独立构建的 kernel/user-space 事实源 | **失败**：新旧 libnvm/nvmeservice/snvme 并存。 |
| 默认 build + tests 可重现 | **失败**：见下一节的实际构建结果。 |
| 错误可从 NVMe command 传播到 API | **失败**：kernel per-command error 被丢弃，stream sync 结果也未返回。 |

---

## 6. 正确性审查

## 6.1 P0：接手前必须处理

### P0-1：当前没有可工作的生产 DMA registration 路径

证据：

- `IAccelerator` 没有 `dma_map/dma_unmap`：`tutti/accel/include/common/iaccel.h:29-100`。
- `register_device()` 只做 bookkeeping，并把 `backend_private` 置空：`tutti/accel/src/cuda/cuda_accelerator.cu:204-225`。
- L4 明确要求 `region->backend_private` 已经是 `uint64_t* ioaddrs`，为空直接失败：`tutti/io_engine/src/io_engine_impl.cpp:104-114,208-218,336-339`。
- 唯一真实映射调用在测试和 nvmeservice example 中；生产层没有调用 `nvm_dma_map_data_device/host`。

结果：作者验证的是“手工映射后直接调用 backend”，不是“通过 Tutti 注册 buffer 后 IO”。

修复方向：`DataPath` 增加 `register_memory/unregister_memory`，Runtime 保存 per-DataPath registration handle；不要继续滥用单个 `MemoryRegion::backend_private`。

### P0-2：Coordinator 无法注册 device-only buffer

- `CudaAccelerator::register_device()` 生成的 region 具有 `host_ptr == nullptr`：`cuda_accelerator.cu:211-225`。
- `BufferRegistry::add_region()` 拒绝任何 `host_ptr == nullptr` 的 region：`tutti/coordinator/src/buffer_registry.cpp:9-12`。
- `CoordinatorImpl::register_buffer(DEVICE)` 随后调用 `add_region()` 并回滚：`tutti/coordinator/src/coordinator_impl.cpp:93-113`。

因此 L6 即使修到可编译，GPU device memory 注册仍必然失败，与 Tutti 的核心用例冲突。

### P0-3：L4 的 DMA 页索引计算错误，会向错误内存页发 IO

- fan-out 将 `slice_index` 设成全局 sub-IO 序号：`io_engine_impl.cpp:116-128,220-232,347-356`。
- 每个 slice 单独调用 `prepare_descriptors(..., n=1)`：`io_engine_impl.cpp:139-146,243-250,361-366`。
- NVMe builder 把 `slice_index` 直接当 `ioaddrs[]` 页索引：`tutti/backends/nvme/src/nvme_command_builder.cpp:39-69`。

例如 page=4 KiB、MDTS=512 KiB 时，第二个 chunk 应从 `ioaddrs[128]` 开始，当前使用 `ioaddrs[1]`。不同 region 混合时还会把前面 region 的全局 slice 数当成新 region 的页索引。

此外 `IoRequest` 没有独立的 memory offset；`byte_offset` 同时只被当成 storage offset，无法表达“region 内从第 N 字节开始”。

### P0-4：混合 target batch 被静默写到第一个 target

每个 `IoRequest` 有自己的 `target_handle`（`tutti/io_engine/include/io_types.h:10-16`），但 `submit_batch` 和 async 只把 `requests[0].target_handle` 传给整个 kernel，且不验证全批相同：`io_engine_impl.cpp:158-169,261-272`。

这不是抽象问题，而是确定的数据路由错误。短期必须拒绝 mixed-target batch；长期由 generic scheduler 按 `{backend,target}` 分组。

### P0-5：非块对齐 IO 会读写错误 LBA并可能越过 buffer

L4 未检查 target offset、memory offset 和 length 的 backend alignment。device helper：

- 用右移把非对齐 `logical_offset` 向下取整到 LBA；
- 用向上取整把 `nbytes` 变成整块数。

见 `tutti/backends/nvme/device/nvme_device_helpers.cuh:89-109`。例如 1-byte 请求可能让 NVMe 实际传输 4 KiB，而 descriptor/buffer 只按 1 byte 计算页覆盖，造成错误数据或 DMA 越界。必须在 host submit 前按 backend capability 严格校验，或实现显式 read-modify-write/bounce 策略。

### P0-6：async 和并发 submit 共用一个 GPU descriptor scratch

`IoEngineImpl` 只有一个 `d_descs_`（`io_engine_impl.h:54`）。每次 sync/async submit 都向同一地址复制 descriptor；async context 只保存 host descriptor 和 event，不保存独立 device buffer：`io_engine_impl.cpp:253-285`。

第二次提交可在第一次 kernel 尚未读取完时覆盖 descriptor。同步调用若来自不同线程/stream 也有同样问题。析构时还会在 pending kernel 未同步的情况下释放 PRP pages、event 和 `d_descs_`：`io_engine_impl.cpp:55-73`。

这会导致错 IO、use-after-free 或 GPU fault。需要 per-inflight slot/arena，并定义 engine 的线程安全与 teardown drain 契约。

### P0-7：direct/fallback 路径暴露不可用 NVMe vdevice

- Direct driver 伪造一个设备并让 `d_qps` 保持 null：`tutti/device_manager/nvme/src/direct_nvme_device_driver.cpp:34-63,99-107`。
- 无 gRPC 或 `mock_mode` 时 daemon driver 也暴露 null-ctrl/null-queue 设备：`daemon_nvme_device_driver.cpp:177-218,309-342`。
- `NvmeBackend::initialize()` 不检查 `d_qps/ctrl`，反而用默认 block size/MDTS 后返回成功：`tutti/backends/nvme/src/nvme_backend.cpp:70-91`。
- GPU helper 随后直接解引用 queue array：`nvme_device_helpers.cuh:98-107`。

生产 backend 缺少依赖时必须返回 `UNSUPPORTED/NOT_READY`；mock 必须是显式测试 plugin，不能作为静默降级。

### P0-8：target handle cache 存在释放后悬空返回

`acquire_target_handle()` 先从 `target_handle_cache_` 返回缓存：`tutti/backends/nvme/src/nvme_target_handle.cpp:31-45`。`release_target_handle()` 只从 `target_handles_` 删除并 `cudaFree`，没有删除对应 cache entry：`:185-213`。后续 acquire 会直接返回已经释放的 device pointer。

`shutdown()` 同样未 clear `target_handle_cache_`（`tutti/backends/nvme/src/nvme_backend.cpp:99-119`），同一 backend 对象再次 initialize 时也可能返回旧指针。

cache key 也不完整：RAW key 缺 namespace、length、block geometry；`RawDeviceImpl` 创建 target 时未设置 `target_id`，默认全是 0（`tutti/coordinator/src/raw_device_impl.cpp:39-45`）。相同 start LBA 的不同 namespace 可错误复用同一 handle。

### P0-9：逻辑/硬件错误被报告为成功

- kernel 中 `submit_one_impl()` 的负错误码被直接丢弃：`tutti/backends/nvme/device/submit_batch_kernel.cu:56-67`。
- backend launch 返回 `void`，CUDA launch error 只打印日志：`:79-107`。
- `IAccelerator::synchronize_stream()` 返回 `void`，CUDA sync error 被忽略：`cuda_accelerator.cu:348-352`。
- L4 最终无条件返回 `true`：`io_engine_impl.cpp:162-177,378-387`。
- CPU sync stub 不发 IO却返回全成功：`tutti/backends/nvme/src/nvme_submission.cpp:78-97`。

在恢复任何上层 API 前，必须定义 request/batch completion 结构和错误传播语义。

### P0-10：PRP LIST 把 CUDA 虚拟地址当成 NVMe DMA 地址

PRP-list page 由 `cudaMalloc()` 分配（`tutti/backends/nvme/src/prp_page_cache.cpp:17-24,134-140`），但没有通过 `nvm_dma_map_data_device()` 映射到 controller 可见的 bus/I/O address。builder 随后把这个 CUDA device pointer 直接写入 `desc.prp2`：`tutti/backends/nvme/src/nvme_command_builder.cpp:64-79`。

NVMe PRP2 在 LIST 模式下必须是 PRP-list page 的 DMA 地址，而不是进程看到的 CUDA 虚拟地址。`nvm_dma_t::ioaddrs[]` 才是 libnvm 定义的 controller DMA 地址（`tutti/device_manager/nvme/libnvm/include/nvm_types.h:82-107`）。因此超过两页的请求可能失败或访问错误地址；现有真实 IO 报告只覆盖 DUAL，没有覆盖 LIST。

修复时 PRP-list pool 本身也必须注册到目标 controller，并同时保存 CPU/GPU virtual view 与 DMA address；`prp2` 写 DMA address，cleanup 释放对应 mapping。

### P0-11：NVMe SQE 未清零，保留字段可能携带随机值

`submit_one_impl()` 在栈上声明未初始化的 `nvm_cmd_t cmd`，随后只写部分 dword：`tutti/backends/nvme/device/nvme_device_helpers.cuh:104-109`。`nvm_cmd_rw_blks()` 还显式保留 `dword[12]` 的高 16 位：`tutti/device_manager/nvme/libnvm/include/nvm_cmd.h:79-88`；其余未覆盖的 reserved/control dword 同样保持未定义值。

发送 SQE 前必须零初始化整个 command，再设置 opcode、namespace、DPTR、SLBA/NLB 等字段。否则控制器可能收到随机 command flags/control 字段。

### P0-12：请求未校验 `MemoryRegion` 边界

L4 只检查 region、target、length 非空和 `backend_private` 存在，没有验证 memory offset/length 是否落在 `MemoryRegion::size` 内：`tutti/io_engine/src/io_engine_impl.cpp:104-129,208-233,325-367`。结合错误的页索引，builder 可越界读取 `ioaddrs[]`，随后构造越过已注册 buffer 的 DMA。

新请求契约必须有独立 `memory_offset`，并在任何 descriptor 构造前用无溢出的方式验证 `memory_offset <= size` 且 `length <= size - memory_offset`；还要校验 target 范围。

### P0-13：CQ polling 没有 timeout 或取消，故障时 GPU kernel 可永久挂起

`cq_poll()` 使用无界 `while (true)`，只有匹配 CID/phase 才返回：`tutti/device_manager/nvme/libnvm/include/nvm_parallel_queue.h:391-445`。控制器 reset、队列损坏、丢 completion 或错误命令都可能使 kernel 永久 spin，进而阻塞 stream synchronize、shutdown 和进程退出。

需要定义 timeout/watchdog/reset/cancel 策略；至少让提交返回明确的超时错误并阻止 teardown 释放仍被 GPU 使用的资源。

### P0-14：构建和事实源当前不可重现

静态问题：

- 父工程先定义旧 `libnvm`（根 `CMakeLists.txt:294-315`），再 `add_subdirectory(tutti)`（`:333-337`）；新树再次定义同名 target。gRPC 开启时 `nvmeservice` 也重复定义。CMake target namespace 全局，父工程无法同时配置两套实现。
- `tutti/tests/io_engine/CMakeLists.txt:51-53` 引用已删除的 `tutti/io_engine/tests/layer4_smoke_test.cpp`。
- L5/L6 引用不存在的 `backends/include/backend_provider.h` 并被禁用。

本次实际验证：

1. 执行项目自带 `./tutti/build.sh -j 8`，CMake generate 确定失败，原因是缺失 `layer4_smoke_test.cpp`。
2. 用 `BUILD_TESTING=OFF` 配置成功；当前主机 CUDA 13.0 下编译在 `tutti/device_manager/nvme/libnvm/src/queue.cpp:9` 的 `<cuda/atomic>` 缺失处停止。这个问题可能是 CCCL/toolkit packaging 或 target include wiring，需要在支持矩阵环境中复核，但当前 checkout 不能按自带脚本完成 clean build。

## 6.2 P1：架构和工程风险

### P1-1：`IBackend` 不是数据面 backend SPI

`IBackend` 只含 initialize/shutdown、vdevice roster 和 metadata（`tutti/backends/include/backend.h:58-102`）。真正 IO 接口在 NVMe 私有 `IBatchSubmitter`，且类型直接是 `nvme::SubSliceInfo/BufferDescriptor`（`tutti/backends/nvme/include/batch_submitter.h:14-65`）。

这对“先把单一 NVMe 路径 mockable”是合理的暂存手段，但不能作为最终抽象。新增 GDS/RDMA 时必须修改 L4 类型和实现，当前尚未证明 backend extensibility。

### P1-2：`StorageTarget` 是闭集、NVMe 中心的数据结构

`tutti/backends/include/storage_target.h:18-73` 把 NVMe namespace/block/extent、RAW LBA、RDMA rkey 全放在同一结构中，且没有 backend instance owner。每增加一种 target 都会修改 common header；target 生命周期由裸指针表达；同一 target 无法清晰绑定 backend/controller。

应由 `StorageTargetResolver` 产生 `ResolvedTarget`，再由目标 `DataPath` 创建 opaque target handle，而不是在 common struct 中持续增加硬件字段。

### P1-3：DMA mapping 被放错层且只容纳一份映射

旧设计文档想让 `IAccelerator::dma_map()` 隐藏 NVIDIA API，但映射结果实际取决于 accelerator、storage controller、IOMMU domain 和 DataPath。把它放 HAL 会让 HAL 依赖 libnvm/RDMA/cuFile；放单个 `MemoryRegion::backend_private` 又无法表示同一 buffer 对多个 DataPath/device 的映射。

应由 `DataPath` 注册通用 memory view，内部调用 accelerator interop 和 driver UAPI；Runtime 只保存 registration handle 和生命周期。

### P1-4：Device Manager common 与 local-NVMe deployment package 没有真正解耦

common interface 本身干净，这是优点；但 `tutti_device_manager` target 同时编译并 PUBLIC 暴露 common、mock、NVMe headers，并内置 libnvm/nvmeservice/CUDA（`tutti/device_manager/CMakeLists.txt:108-193`）。因此 common consumer 实际仍承担 local-NVMe 构建依赖。

建议拆成：

- `tutti_device_manager_core`；
- `tutti_device_driver_mock`；
- `tutti_local_nvme_control` plugin。

### P1-5：内核模块没有完成特殊硬件抽象

- `tutti/device_manager/nvme/kernel_modules/` 是在 `2467bac` 中复制的两套完整 kernel baseline。
- 新 `tutti/` CMake 只把它描述为 reference，完全没有 `module_root/module_ccflags/configure_file` 接线：`tutti/device_manager/CMakeLists.txt:234-242`。
- 父工程实际仍从 `backends/local/kernel_modules` 构建：根 `CMakeLists.txt:199-255`。
- 新 5.15 树在 `035626c` 增加了 28 行 Ubuntu compatibility 修复，旧树没有；所以父工程构建不到新修复。
- `map.c` common flow直接依赖 `nvidia_p2p_*`，并直接动态解析 Phoenix symbol：`tutti/device_manager/nvme/kernel_modules/snvme-5.15.0-public/map.c:18-103,560-648`。
- 两个 kernel baseline 是完整源码副本；新增版本仍需人工复制/port。

这只是“把 NVMe 特殊代码集中到目录”，不是“剥离出稳定边界”。

### P1-6：L1 HAL 的资源语义不完整

`ipc_import()` 成功打开 CUDA handle 后以 size=0 调 `register_external()`，后者拒绝并返回 null，导致 imported pointer 泄漏：`cuda_accelerator.cu:463-496`。`unregister()` 也不做 `cudaIpcCloseMemHandle/munmap/cudaHostUnregister`：`:260-282`。`register_host()` 不 pin memory，只记账。

这说明 `register` 一词的语义不稳定；接手时应明确区分：

- runtime tracking；
- accelerator pin/import；
- backend DMA registration。

### P1-7：测试通过声明不能等价为当前主线正确

初版 L5/L6 报告的通过发生在 `cc430c4` 删除 `IBackendProvider` 之前。当前 `doc/layered/` 已正确指出这些产物陈旧。另有以下测试盲点：

- fallback mock 暴露的 backend initialize 成功，但不能 IO；
- L3 real-HW 测试手工完成 DMA map，绕过 public registration；
- L4 只运行 `submit_one` mock，`submit_batch/async` 未运行；
- IPC 测试用 skip 掩盖确定失败；
- L5/L6 当前完全不在构建图内。

接手后需要按端到端能力设置测试门，而不是按类/接口数量统计完成度。

## 6.3 P2：清理项

- `tutti/CMakeLists.txt` 仍声明/安装不存在的 `types/`，顶部状态注释过时。
- `tutti/backends/CMakeLists.txt` 和 `tutti/backends/nvme/CMakeLists.txt` 大段注释仍描述已删除的 `IBackendProvider`。
- L4 `io_types.h::SubSliceInfo` 未使用；`launch_batch.h` 无定义/无调用；`LocalNvmeIoEngineConfig` 被忽略。
- L4 StripeManager 不在 `libtutti_io_engine` source list 中。
- 两个 `raw_device` 实现和多个 `GpuFile` 模型并存；`raw_device` 不在目标范围，应从活动构建/设计中移除而不是合并。
- 提交范围 `git diff --check` 有大量历史 trailing whitespace；不影响架构，但说明批量复制缺少基础 hygiene gate。

这些不应抢在 P0 前“顺手重构”，但应进入后续删除清单。

---

## 7. 合理性评价

### 7.1 应保留的设计

1. **namespace 与 transport 的概念分离。** 这有利于 file/object/DFS metadata 与 NVMe/GDS/RDMA data path 独立演进；但它是可选组合关系，不必强制每个系统拆成两层。
2. **GPU kernel 不做运行时虚分派。** host 选 DataPath，DataPath 启动自己的 vendor kernel，符合性能与工具链现实。
3. **设备资源授予与 steady-state IO 分离。** 当前 L2 的 grant/accounting 思想可复用，但应成为相关 DataPath 的控制面，而不是所有实现必经的全局层。
4. **mock 验证 common contract。** `MockDataPath` 应是显式测试替身，用来证明 Runtime 不依赖 CUDA/NVMe，不能作为生产 fallback。
5. **最新文档开始以源码事实为准。** `1e0b333` 明确记录 L4-L6 缺口，适合作为接手起点，而不是继续相信旧 verification report。
6. **借鉴 Mooncake 的窄接口，但使用 Tutti 术语。** `StorageRuntime + opaque handles + register/submit/query` 比当前多层 façade 更适合作为稳定连接点。

### 7.2 需要调整的设计

1. **层数不是目标，稳定契约才是目标。** 当前为填满 L0-L6 过早实现 L5/L6，接口一变整层报废。
2. **不要把“common header 干净”误认为 DataPath 已抽象。** 如果 `StorageRuntime` 仍理解 PRP/NVMe descriptor，边界没有完成。
3. **不要用全局 enum + 大 union 模拟无限扩展。** 这会让每个新 DataPath 修改 core API。
4. **不要让 test mock 作为缺依赖时的生产 fallback。** mock 必须显式选择。
5. **不要维护两套完整 kernel fork。** 先确定唯一事实源和 ABI，再做 compat；否则所有用户态抽象都会被部署层分叉抵消。
6. **不恢复 `raw_device`、WAL、striping 等非必要 feature。** 本轮只建立 file/KV-cache 主线所需的 target resolver、memory registration、submit、progress 和 completion；其他能力由实际需求驱动。

---

## 8. 接手后的建议顺序

## Phase 0：冻结与建立事实源

**动作：**

- 宣布 `doc/layered/` + 本文为接手基线；旧组件 verification report 仅作历史记录。
- 明确一个 build 入口：先以 standalone `tutti/` 为主，父工程旧树作为迁移参照；禁止两套同名 target 同时定义。
- 选定唯一 local-NVMe kernel/libnvm/nvmeservice 源；在迁移完成前不要继续两边修 bug。
- 修复默认 configure/build/test 阻断，但不恢复 L5/L6。
- 建立 `TUTTI_ACCELERATOR=HOST|CUDA|...` 单 profile 入口和 target-scoped `TUTTI_USE_<VENDOR>`；移除根 CMake 对 CUDA language/SDK 的无条件依赖。

**验收：** `HOST` profile 不需要 GPU SDK；当前 `CUDA` profile clean configure/build；feature OFF 不引入其依赖；hardware-free tests 全通过。

## Phase 1：冻结 StorageRuntime 最小契约

**动作：**

```text
StorageRuntime
  open / close
  register_memory / unregister_memory
  submit(IoRequest[], HostSubmitContext) -> IoHandle
  query / wait

CUDA-like API/profile
  cudaMalloc / cudaFree / cudaPointerGetAttributes
  cudaStream / cudaEvent / cudaMemcpy
  CUDA direct; MACA/MUSA compile-time shims

DataPath
  capabilities
  open(ResolvedTarget) / close
  register_memory / unregister_memory
  submit / progress / query

StorageTargetResolver
  resolve(uri) -> ResolvedTarget

IoRequest
  target_handle + target_offset
  memory_handle + memory_offset
  length + direction
```

- `HostSubmitContext.execution_domain` 只描述 host API 的 `HOST_EXECUTION` / `DEVICE_EXECUTION`，不把未来 device caller 混进同一个 enum。
- DataPath 内部负责 MDTS、extent、PRP/SGL、kernel、workspace 和 completion strategy。
- Runtime 只负责 host control plane、handle ownership、分组、backpressure、`IoHandle`、错误归一化和 bounded status harvest。
- 为 `DataPath/interop/<accelerator>/device_api` 保留未来 sidecar 位置，但本轮不冻结 device API。
- 明确线程安全、shutdown drain、timeout、pool exhaustion 和 registration lifetime；取消操作等真实 Adapter 提出需求后再扩展。
- 用 `MockDataPath` 和 `HOST` CUDA-like shim 做 contract tests，不加入产品功能。

**验收：** public storage header 只经 `tutti/cuda_like.h` 使用选中 profile 类型，不出现 PRP/libnvm/NVMe 类型；测试 DataPath 与 profile shim 不修改 Runtime storage noun/算法。

## Phase 2：打通最小 file-backed LocalNvmeDataPath

**动作：**

- 实现正式 data-path memory registration 和 RAII unmap。
- 实现 `Ext4FiemapTargetResolver`：只打开预配置、Tutti 管理且已预分配的文件，校验 extent 稳定性；不新增 public create/delete/list/WAL/striping API。
- 修复 P0-3 至 P0-13：memory offset/bounds、alignment、target grouping、per-inflight descriptors、handle ownership、PRP-list DMA mapping、SQE 初始化、completion/timeout。
- direct path 未实现前返回 unsupported；移除生产 fallback mock。
- backend-private 单测可用 synthetic extent fixture，但不形成 `raw_device` API。

**验收：** `StorageRuntime` 打开测试文件，注册 GPU buffer，完成 file offset write/read/verify，返回真实 validation/NVMe/timeout 错误；重复 lifecycle 无泄漏。内核模块加载和设备绑定仍由人工执行。

## Phase 3：收敛 LocalNvmeDataPath 与 kernel 边界

- 将 Device Manager 的 resource grant/accounting 移入 `LocalNvmeDataPath/control/`。
- 合并 libnvm/nvmeservice/snvme 新旧树，建立共享版本化 UAPI。
- kernel-version 差异集中到 compat，GPU pinning 集中到 peer-memory ops。
- local-NVMe 私有 header、CUDA kernel、libnvm 不从 public target 传播。

**验收：** 根工程和 standalone 工程引用同一源；至少两个支持 kernel baseline 做 compile-only CI；修改 UAPI/driver 不再同步复制。

## Phase 4：迁移 metadata pools 并实现真正 async IO

- 从旧 `memory/` 迁移 `GpuSlotPool`/`HostSlotPool` 的思想，建立 per-device、bounded `MetadataArena`。
- 迁移 `TieredHandleCache` 与 DMA-correct `PrpPageCache` 到 `LocalNvmeDataPath/metadata/`，修复共享 staging 与多 stream lifetime。
- `IoHandle` 强持有 descriptor/PRP/status/event lease、memory registration 和 target，终态后统一回收。
- `DEVICE_EXECUTION` 要求 CPU 在返回前将 IO kernel 和 completion fence 排入 caller stream，且不做 stream synchronize。
- 验证同 stream 的 compute→IO→compute 顺序，以及跨 stream 的 producer-event→IO-event→consumer 顺序。
- 实现 `DataPath::progress(budget)`；device-side IO 不依赖 host query/wait 才推进，再 benchmark warp-per-QP、doorbell batching、split submit/completion、host/persistent status harvest。
- 不把 kernel strategy 或 pool 类型加入 public API。

**验收：** 多 stream/多 thread submit 无 scratch/event 覆盖；IO 与 compute kernel 可跨 stream 并发；同/跨 stream event 顺序正确；不调用 query/wait 时 stream 仍越过真实 IO completion fence；query/wait 能返回真实终态；bounded pool 正确 backpressure；shutdown 能 drain；用 GPU warp/SM 占用、metadata H2D、doorbell/atomic/CQ scan、IOPS/BW、p99 和 compute overlap 比较策略。

## Phase 5：接入真实 FrameworkAdapter

1. 不恢复当前 `raw_device`、`block_storage`、`Coordinator`。
2. 选择一个真实消费方，实现 `VllmAdapter` 或 `LmCacheAdapter`；只有 vLLM 外部包装类按协议叫 `TuttiKVConnector`。
3. Adapter 负责 KV layout、block id、request lifecycle；只调用 `StorageRuntime`。
4. 注册整块 KV allocation，用 `memory_offset` 表示 block/layer slice。
5. `IoHandle` 映射回 finished request；IO 终态前禁止回收对应 KV block。

**验收：** Adapter 与 `LocalNvmeDataPath` 独立编译；替换 `MockDataPath` 时 Adapter 源码不变；core 不出现 vLLM/LMCache 类型。

## Phase 6：用测试替身证明社区扩展性

- `MockDataPath` 覆盖 open/register/submit/progress/completion/error；
- `HOST`/`CUDA` CUDA-like contract 覆盖 allocation、pointer、stream/event、copy/context；
- sample Resolver/Binding 证明 pair-private payload 不污染 Runtime；
- `HOST`/`CUDA` profile 证明只选一个 `TUTTI_USE_XXX`，未选 SDK 不参与构建；
- 扩展通过构造注入或静态 factory 注册，不修改 `StorageRuntime` 公共 noun/算法；
- feature ON/OFF 都有 CI；
- 不主动实现 raw、GDS、RDMA、Mooncake DataPath、新 filesystem、新 GPU vendor 或 device-initiated IO。

真实需求出现后，需求方可按 SPI/profile/sidecar 实现 `MooncakeDataPath`、其他 Resolver、Accelerator 或 device API。测试门通过前不能宣称社区扩展边界完成。

## Phase 7：迁移旧根目录并删除兼容树

只有功能、性能和硬件测试对齐后，才删除旧 `memory/`、`device_manager/`、`nvme_storage/`、`io_engine/`、`backends/local/`。删除前保留 API/正确性/性能对照，避免失去可回退基线。

---

## 9. 建议第一批实际任务

按风险和依赖排序：

1. 修默认 CMake/test target 与 target 重名，确定唯一 build/source ownership；增加 `TUTTI_ACCELERATOR=HOST|CUDA` 和 target-scoped `TUTTI_USE_XXX`。
2. 冻结 `StorageRuntime`、CUDA-like API/profile、`DataPath`、`StorageTargetResolver`、`IoRequest`、`IoHandle`、`HostSubmitContext` 最小 host 契约；只记录未来 device sidecar 不变量，不实现其 API。
3. 用 `MockDataPath`/`HOST` shim 写 contract tests：offset/bounds、分组、backpressure、错误、progress、operation lifetime。
4. 为 public registration → data-path DMA map 缺口写失败测试，再实现 data-path-owned registration。
5. 将 local-NVMe control/data/kernel/metadata 收进 `LocalNvmeDataPath`，修复页索引、PRP-list、SQE、scratch、completion、timeout 和 handle cache。
6. 迁移最小 `Ext4FiemapTargetResolver`，完成人工准备环境下的 file write/read/verify。
7. 再迁移 metadata pools 和 async/progress；以资源占用与 overlap benchmark 驱动 kernel 策略，而不是先写多套抽象。
8. 选择首个真实 `VllmAdapter`/`LmCacheAdapter`；删除或冻结 raw、旧 block-storage、Coordinator。

---

## 10. 接手判断

可以接手，但应把当前代码定义为：

> **“一个含有可复用 CUDA/local-NVMe 组件和部分资源管理思想的数据面原型”，而不是“已经完成 L0-L6 的新 Tutti”。**

建议保留并演进的是代码与思想，而不是原层号：

- CUDA allocation/copy/stream/event 调用，迁入 `tutti/cuda_like.h` 使用模型和 Runtime memory 管理；
- `GpuSlotPool`、`HostSlotPool` 的有界 metadata allocation 思想；
- `TieredHandleCache` 的 GPU L1 + pinned-host L2 成本模型；
- DMA-correct PRP pool/cache、NVMe queue/target/kernel，收进 `LocalNvmeDataPath` 私有实现；
- L2 resource grant/accounting 思想，收进 local-NVMe control plane；
- mock contract tests、事实文档和 namespace/data-path 分离原则。

建议重新设计或重接：

- 公开的 host `StorageRuntime`，以及未来 device-initiated sidecar 的边界；
- `DataPath`、`StorageTargetResolver` 的仓内 SPI；
- NVIDIA-first `cuda_like.h` 与 `TUTTI_ACCELERATOR` + target-scoped `TUTTI_USE_XXX` 编译 profile；
- data-path-owned registration、metadata arena、progress/completion；
- `VllmAdapter`/`LmCacheAdapter`；
- kernel source/UAPI/compat/peer-memory 组织；
- build、正确性与性能测试门。

最重要的接手原则是：**不追求补齐 L0-L6，也不在重构期实现 raw 或其他无需求 feature；先让 file/KV-cache 消费方只面对稳定的 host `StorageRuntime`，让 accelerator/DataPath/Resolver/Adapter 可按 profile 和 contract 局部扩展，并为未来 device kernel 直接发起 IO 留出 sidecar，而不提前冻结错误的通用 device API。**

对“是否社区友好”的最终判断：**目标架构可以，当前实现还不可以。** 达到可对外贡献的最低门槛是：单 `TUTTI_ACCELERATOR` profile 且未选 SDK 零依赖；一个完整 DataPath/Resolver contract kit；扩展 ON/OFF CI；唯一 build/source owner；示例扩展不修改 Runtime 公共 noun/算法。满足这些门槛后，社区成员才能主要通过新增 package/profile/tests/docs 增加 feature，而不是继续改 core 分支。
