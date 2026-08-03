# Tutti 目标架构

**状态：** 接手阶段目标设计，待接口评审后冻结

**目标版本：** 当前 `v0.1` 重构主线，不推进版本号

**日期：** 2026-07-30

**适用范围：** 用户态公共 API、仓内扩展 SPI、local-NVMe 用户态/内核边界、Framework Adapter

## 文档定位

本文描述 Tutti 重构完成后的**目标架构和接口契约**，不表示当前代码已经达到该状态。

相关文档的职责如下：

- `TUTTI_TARGET_ARCHITECTURE.md`：目标结构、稳定边界、资源所有权和验收标准；
- `TUTTI_REFACTOR_TAKEOVER.md`：当前实现、提交历史、已知问题和迁移顺序；
- `Roadmap.md`：当前版本定位和路线概览；
- `doc/layered/`：上一轮 L0-L6 设计和实现历史，不作为最终目录结构规范。

本文细化并修正 `Roadmap.md` 中较早的线性分层、`IBackendProvider`、descriptor union、public raw range、`IO Engine` 和全局 `DeviceManager` 等设计。**当前本文仍是待评审草案，因此不能与活动 `Roadmap.md` 同时被解释为两个规范事实源。**

冻结本文必须在同一变更中完成：

1. 将 `Roadmap.md` 的目标架构、交付物、目录和 raw 用例改为本文术语，或明确链接本文为唯一规范；
2. 将旧 L0-L6、`IBackendProvider` 和 raw 路线标记为历史；
3. 明确唯一 build/source ownership；
4. 才允许实现任务以本文作为验收基线。

冻结后，后续实现以本文定义的 `StorageRuntime`、NVIDIA-first CUDA-like API、`DataPath`、`StorageTargetResolver` 和 `FrameworkAdapter` 边界为准。文中的“必须”“禁止”是架构约束；“建议”是默认实现选择，可通过 RFC/评审修改。

---

## 1. 一句话目标

Tutti 是一个 **CPU/GPU companion unified storage runtime**：应用只面对一组稳定的内存、目标和异步 IO 句柄；文件解析、GPU 厂商能力、local NVMe、GDS、RDMA 或其他数据移动方式都位于可替换的内部边界之后。

目标调用链为：

```text
FrameworkAdapter / C++ Application
                 │
                 ▼
          Tutti StorageRuntime
   memory / target / IO lifecycle
                 │
       ┌─────────┴──────────────┐
       │                        │
StorageTargetResolver   CUDA-like API/profile
       │                NVIDIA / MACA / MUSA
       └─────────┬──────────────┘
                 ▼
             DataPath
                 │
        LocalNvmeDataPath
                 │
      libnvm / NVMeService / snvme
```

最重要的验收条件不是目录看起来“分层”，而是：

1. 应用和 Framework Adapter 可以使用 `tutti/cuda_like.h` 的 CUDA 风格 stream/event API，但不直接 include 厂商私有 shim、libnvm、PRP、LBA 或 FIEMAP extent；
2. `StorageRuntime` 不理解任何具体 storage descriptor 或 DataPath kernel；
3. 新增仓内 `DataPath`、CUDA-like profile/shim 或 `StorageTargetResolver` 时，不修改公共 storage 请求模型；
4. metadata pool、GPU kernel 和 completion strategy 可以独立优化，不改变公共 API；
5. file/KV-cache 路径能够从公开 API 走完内存注册、提交、完成和资源释放。

---

## 2. 目标与非目标

### 2.1 当前重构必须达到的目标

1. **稳定 host control plane 与可扩展提交面**
   - `StorageRuntime` 统一负责 host 侧内存 registration、target open、资源准备、host submission、query/wait 和 teardown；
   - 当前主线实现 host-initiated/host-executed 与 host-initiated/device-executed 两条路径；
   - 架构必须为未来 device-initiated/device-executed IO 保留位置，但本轮不冻结、不实现通用 device API；
   - host API 统一暴露 `tutti/cuda_like.h` 的 CUDA 风格类型；NVIDIA 直接使用 CUDA，兼容厂商由编译期 shim 映射；未来 device API 按 profile/DataPath 组合。

2. **完整数据面 SPI**
   - `DataPath` 同时覆盖 target、data-path memory registration、submit、progress 和 completion；
   - local NVMe 是第一个完整参考实现，而不是通用层中的特殊分支。

3. **一等内存子系统**
   - 区分 allocation、runtime registration、accelerator host-pin lease、data-path DMA registration；v0.1 不管理跨进程 import；
   - 支持 host、pinned host、device 和 managed memory 的身份与生命周期；
   - 支持同一 `MemoryHandle` 对不同 data path / mapping domain 的多份注册。

4. **真正异步、stream-ordered 的 device execution**
   - 当前实现路径由 host 调用 `submit()`，将 device IO kernel 和 completion fence 排入 caller 指定的 accelerator stream；
   - IO kernel 与计算 kernel 一起由 accelerator 调度，不同 stream 可以并发；
   - 同 stream 的先后顺序以及跨 stream 的 event wait/record 保证 compute 与 IO 依赖；
   - `submit()` 返回可拥有的 `IoHandle`，不 hard-sync caller stream；
   - operation 在终态前持有 target、memory registration、metadata 和 completion-event lease；
   - device-side IO 不能依赖 caller 反复 `query/wait` 才前进；host progress 只收割状态、处理错误和回收资源；
   - 未来 caller device kernel 可以经编译期 device sidecar 直接发起 IO，并复用同一套 host-prepared resources 和生命周期约束。

5. **优化隔离**
   - GPU DRAM metadata pool、pinned-host tier、PRP cache、descriptor cache、kernel launch geometry、CQ progress 都是 DataPath 私有策略；
   - 优化这些组件不改变 `StorageRuntime` 和 Framework Adapter。

6. **清晰部署边界**
   - local-NVMe 的 control、data path、libnvm、NVMeService、snvme 和 accelerator interop 形成一个部署单元；
   - 用户态与内核共享唯一、版本化 UAPI；
   - 内核模块由部署系统预先安装，普通测试不自动加载或卸载。

### 2.2 当前明确不做

本轮重构禁止为了“架构完整”提前实现以下能力：

- public `raw_device` API；
- `cancel()`；
- `dlopen` 动态插件、运行时卸载或稳定第三方 binary ABI；
- WAL、striping、通用 create/remove/list 文件服务；
- priority、failover、notification、自动重试 policy；
- CUDA 与 ROCm 在同一 build/profile 中混用；
- cooperative host/device submit；
- 本轮实现 device-initiated IO 或冻结通用 device-side public API；
- 在没有真实需求方时实现 GDS、RDMA、Mooncake DataPath 或新文件系统；
- 让 core 直接依赖 vLLM、LMCache 或 Mooncake 的对象模型。

local-NVMe 单元测试可以使用固定 LBA 或 synthetic extent fixture，但它们只能是 DataPath 私有测试设施，不能演化成产品功能。

---

## 3. 架构原则

### 3.1 按职责 DAG 组织，不再追求 L0-L6 线性层

Tutti 的真实依赖是一个 DAG：

- Runtime 同时依赖编译期 CUDA-like profile、resolver 和 data path SPI；
- resolver 与 data path 可以独立组合，也可以由同一存储系统 package 一起提供；
- local-NVMe control plane 只服务 local-NVMe，不是所有 data path 的必经层；
- Framework Adapter 位于 core 旁边，不是 core 上方必须经过的一层。

因此最终目录和 CMake target 按**扩展与部署边界**组织，不按层号组织。

### 3.2 稳定公共 noun，隔离私有 verb

公共层只保留稳定 noun：

- `MemoryHandle`
- `TargetHandle`
- `IoHandle`
- `IoRequest`
- `HostSubmitContext`
- `Status` / `Result<T>`
- `TargetCapabilities`

具体 data path 的 descriptor、queue、kernel、polling、cache 和 mapping 是实现 verb，不进入公共对象。

### 3.3 每种资源只有一个 owner

- Runtime registry 拥有公共 handle 对应的记录；
- 选中的 CUDA-like runtime/profile 拥有由其分配或 pin 的物理内存资源；
- Resolver 拥有 fd、inode/namespace，以及受部署契约保护的 target metadata snapshot；
- DataPath 拥有 target 私有句柄、DMA mapping、queue、descriptor 和 operation；
- `IoHandle` 对应的 runtime operation 在终态前强引用所有依赖；
- pool slot 由 operation lease 持有，禁止依赖“同一个 stream 应该已经用完”的隐式假设。

### 3.4 能力不支持必须显式失败

禁止以下行为：

- 缺少驱动或服务时静默切到 mock；
- direct path 未实现却返回初始化成功；
- alignment、memory kind 或 execution domain 不支持时继续尝试；
- 丢弃 kernel、controller 或 stream 错误后返回成功。

### 3.5 扩展接口先保持仓内 source-level

当前 SPI 通过构造注入或静态 factory 装配。`spi/` 可以演进，不承诺稳定 C++ ABI。

只有出现真实的独立第三方实现者和独立发布需求后，才评估：

- 安装 SPI headers；
- C ABI；
- 动态发现和加载；
- 跨版本兼容策略。

### 3.6 社区友好不等于“所有东西都是插件”

目标是让贡献者能沿清晰的扩展槽增加 feature，而不是修改一个全局 union/enum，或先理解全部 local-NVMe 内部实现。

可独立贡献的扩展类别为：

1. **CUDA-like profile/shim**：一种 NVIDIA CUDA 或兼容厂商的 runtime/device source-compatibility 实现；
2. **DataPath**：一种字节移动和 completion 实现；
3. **StorageTargetResolver**：一种 URI/namespace 到 target payload 的解析；
4. **Binding**：一对 Resolver/DataPath 私有 payload 契约；
5. **FrameworkAdapter**：一个上层框架的布局与生命周期适配；
6. **DataPath interop sidecar**：某个 DataPath 与某个 accelerator 的 host-launched/device-initiated device code；
7. **实现内 feature**：metadata cache、kernel strategy、queue policy 等，在 owner package 内通过配置/capability 增加。

社区扩展的最低标准：

- feature disabled 时 core、hardware-free tests 和其他 CUDA-like profile 仍能构建；
- 新增实现不要求修改 `IoRequest`、公共 handle 或 Runtime 的硬件分支；
- capability 明确说明支持范围，unsupported 必须显式失败；
- 依赖通过 target-scoped CMake 传播，不使用全局 include/link/compile flags；
- 提供 contract tests、最小示例、配置说明、依赖/支持矩阵和 owner；
- 不以生产 fallback 的方式偷偷启用 mock；
- 只有第二个真实实现证明共性后，才把 DataPath 私有组件提升成共享抽象。

以下内容通常**不是**新插件：一个新的 NVMe kernel geometry、PRP eviction policy、doorbell batch 参数或 vLLM layout 优化。它们应先作为现有 owner 内的 feature，避免接口数量随实现细节膨胀。

---

## 4. 目标组件图

### 4.1 用户态总图

```text
┌───────────────────────────────────────────────────────────────┐
│ Applications / Framework Adapters                             │
│ C++ app | VllmAdapter | LmCacheAdapter | future adapters      │
└──────────────┬────────────────────────────────┬───────────────┘
               │ storage API                    │ CUDA-like API
┌──────────────▼──────────────────┐  ┌──────────▼───────────────┐
│ StorageRuntime                  │  │ tutti/cuda_like.h         │
│ Memory/Target/Io registries     │◄─┤ CUDA | MACA | MUSA shim  │
│ routing/backpressure/completion │  │ memory/stream/event       │
└──────────────┬──────────────────┘  └──────────┬───────────────┘
               │                                │
       ┌───────▼──────────┐              device interop
       │ StorageTarget-   │                     │
       │ Resolver/Binding │                     │
       └───────┬──────────┘                     │
               └──────────────┬─────────────────┘
                              ▼
                        DataPath
                   LocalNvmeDataPath / future
```

未来 device-initiated 路径不是第二套 control plane：

```text
host StorageRuntime
  open/register/grant/export device context
                    │
                    ▼
caller device kernel
  → compile-time selected DataPath/CUDA-like-profile device sidecar
  → pre-created queue/mapping/workspace
  → device-visible completion
```

它绕过的是 host submit 调用，不绕过 host 资源建立、权限、mapping 和 teardown。当前版本只实现上图的 host 路径。

### 4.2 local-NVMe package 内部

```text
LocalNvmeDataPath
├── io
│   ├── target / mapping-domain
│   ├── memory registration
│   ├── request fan-out
│   ├── submit
│   └── completion / progress
├── metadata
│   ├── MetadataArena
│   ├── descriptor / SQE / status leases
│   ├── TieredHandleCache
│   └── DMA-correct PrpPageCache
├── control
│   ├── DirectNvmeResourceProvider
│   └── NvmeServiceResourceProvider
├── interop
│   └── cuda_like
│       ├── queue/device views
│       ├── submit kernels
│       ├── completion strategy
│       └── minimal profile overrides
├── userspace/libnvm
├── service/nvmeservice
├── include/uapi/tutti_snvme.h
└── kmod/snvme
```

### 4.3 进程与内核边界

```text
Application process
  StorageRuntime
      └── LocalNvmeDataPath
            ├── process-owned bootstrap ─────┐
            └── service-owned bootstrap ──┐  │
                                         │  │
NVMeService process                      │  │
  queue/resource grant ──────────────────┘  │
                                            │ ioctl/mmap
include/uapi/tutti_snvme.h                  │
                                            ▼
snvme.ko
  common queue/mapping lifecycle
  kernel compat ops
  peer-memory ops
  Linux NVMe core interaction
```

---

## 5. 命名与职责

| 名称 | 职责 | 明确不负责 |
|---|---|---|
| `StorageRuntime` | 应用 façade、handle registry、路由、分组、backpressure、completion | PRP、CQ、FIEMAP、CUDA kernel |
| CUDA-like API/profile | CUDA 风格 allocation、pointer、stream/event、copy 和 device primitives；编译期映射 NVIDIA/兼容厂商 | storage DMA map、NVMe/RDMA registration |
| `DataPath` | 打开 resolved target、data-path registration、submit/progress/query | framework block layout、URI/name 解析 |
| `StorageTargetResolver` | 将 URI/name 解析成带资源 lease 的 `ResolvedTarget` | 提交 IO、构造 PRP/WR |
| `FrameworkAdapter` | framework 生命周期、tensor/block layout 到 `IoRequest` 的转换 | controller、queue、filesystem extent |
| `MemoryHandle` | Runtime 已知的一段连续 memory view | 单一 backend-private 指针 |
| `TargetHandle` | Runtime 已打开的存储目标 | fd、LBA、extent、rkey 的公开 union |
| `IoHandle` | 一批异步 IO 的状态与结果 | CUDA event 或 NVMe CID 的公开表示 |

`Backend`、`Platform`、`TransferEngine` 和 `Connector` 不作为 Tutti core 的主术语：

- `Backend` 在多个项目中含义过载；Tutti 使用 `DataPath`；
- `Platform` 太泛；accelerator runtime 采用明确的 NVIDIA-first CUDA-like API/profile；
- `TransferEngine` 是 Mooncake 的标志性 public noun；Tutti 使用 `StorageRuntime`；
- Tutti 内部使用 `FrameworkAdapter`；只有 vLLM 对外协议要求的包装类可以命名为 `TuttiKVConnector`。

---

## 6. 公共对象模型

### 6.1 `Status` 与 `Result<T>`

公共 API 禁止只返回 `bool`。最低错误分类为：

```text
OK
INVALID_ARGUMENT
OUT_OF_RANGE
NOT_FOUND
UNSUPPORTED
NOT_READY
BUSY
RESOURCE_EXHAUSTED
TIMEOUT
DEVICE_ERROR
DATA_LOSS
INTERNAL
```

`Status` 可以携带：

- 稳定的 `StatusCode`；
- 可读 message；
- 可选 native domain/code，例如 CUDA、errno、NVMe status；
- DataPath 名称和 request index；
- 不暴露私有 C/C++ 类型。

`wait()` 的观察超时与 data-path 执行失败必须区分：

- **观察超时：** `wait(timeout)` 返回 `WaitOutcome{observation_status=TIMEOUT, result=null}`，operation 仍可能在飞，不释放资源；
- **执行超时：** 只有 DataPath 已停止后续 DMA，或已隔离/重置相关资源时，operation 才能进入 terminal timeout failure，并在 `IoResult` 中返回失败。

`WaitOutcome` 的语义为：

```text
WaitOutcome
  observation_status   OK | TIMEOUT | invalid-handle Status
  optional IoResult    只在 operation 已终态时存在
```

因此观察 API 的返回类型必须能同时表达“API 调用错误”和“operation 状态”，不能用一个 `IoResult` 混合两者。

### 6.2 Opaque handle

`MemoryHandle`、`TargetHandle`、`IoHandle` 必须：

- 类型安全，不能互相转换；
- 包含 runtime identity 与 generation，拒绝跨 Runtime 和 stale handle；
- 不把内部对象地址转换成整数暴露；
- 支持并发读取状态；
- 在 close/unregister/release 后确定性失效。

建议内部形式为 `{runtime_id, slot, generation}`，但具体编码不是 public ABI。

### 6.3 Memory 类型

Memory 的**位置**与**所有权**正交：

```text
MemoryKind:
  HOST
  PINNED_HOST
  DEVICE
  MANAGED

MemoryOwnership:
  RUNTIME_OWNED
  CALLER_OWNED
```

`EXTERNAL` 不再同时充当位置和所有权。外部分配或已由 caller 导入的 GPU memory 是 `DEVICE + CALLER_OWNED`；v0.1 Runtime **不提供 CUDA IPC/共享内存 import/open API**，因此也不声明 `IMPORTED` ownership。需要 import 时由 Framework Adapter/caller 先用厂商 API 打开，并保证该 native mapping 活到 `unregister_memory()` 成功。

`MemoryView` 至少包含 address、size，以及可选的 expected profile/device/kind。Runtime 通过 CUDA-like `cudaPointerGetAttributes()` 等 profile API 验证实际位置；caller 声明与实际不一致时拒绝注册。

一个 runtime `MemoryRecord` 至少包含：

- base address 与 size；
- kind、ownership、alignment；
- 编译 profile identity、`device_id`；
- allocation owner；
- 可选 `CudaLikeMemoryLease`，例如 caller pageable host memory 的 pin token；
- public lifecycle state；
- inflight reference count；
- `{DataPath instance, registration domain}` 到私有 registration 的映射表。

Host memory 规则：

- 已经 pinned 的 memory 直接记录为 `PINNED_HOST`；
- pageable `HOST` 可以注册为 runtime identity，但只有支持 staging 的 DataPath 可直接使用；
- 若提交路径要求 pin，Runtime 通过 CUDA-like `cudaHostRegister()` 创建成对的 `CudaLikeMemoryLease`，失败则回滚且不产生半注册 handle；
- lease 在所有 DataPath registrations 和 inflight IO 释放后，通过 `cudaHostUnregister()` 销毁；
- Runtime 不接管 caller allocation 本身。

公共 API 可以返回 `MemoryInfo`，但不返回 DMA IOVA、PRP、rkey 或 data-path registration pointer。

### 6.4 `IoRequest`

```text
IoRequest
  direction       READ | WRITE
  memory          MemoryHandle
  memory_offset   uint64
  target          TargetHandle
  target_offset   uint64
  length          uint64
```

语义：

- `READ`：target → memory；
- `WRITE`：memory → target；
- `memory_offset` 与 `target_offset` 完全独立；
- 所有 offset 和 length 是 bytes；
- Runtime 使用无溢出的形式验证：
  - `memory_offset <= memory.size`
  - `length <= memory.size - memory_offset`
  - `target_offset <= target.size`
  - `length <= target.size - target_offset`
- alignment、max IO、memory kind 和 execution domain 根据 target/DataPath capability 校验；
- `IoRequest` 不包含 tensor shape、KV block id、fd、extent、LBA、namespace id、PRP 或 stream。

### 6.5 IO 发起位置、执行位置与 host submit

IO 有两个正交维度：**谁发起 API**，以及**实际 data-path work 在哪里执行**。

```text
                         ExecutionDomain
                     HOST              DEVICE
InitiationDomain
HOST_API             当前支持          当前目标主路径
DEVICE_API           不适用            未来按需求实现
```

- `HOST_API + HOST`：host 调用 `StorageRuntime::submit()`，CPU 执行 data-path submission；
- `HOST_API + DEVICE`：host 调用 `StorageRuntime::submit()`，CPU 将 IO kernel 排入 accelerator stream；
- `DEVICE_API + DEVICE`：已经运行的 caller device kernel 直接调用 device-side IO surface；这是未来能力，不经过 host `StorageRuntime::submit()`；
- 当前不设计 host/device cooperative submit，也没有 `DEVICE_API + HOST`。

`StorageRuntime` 是 host control plane，不是未来唯一 IO 发起面。它负责在 device IO 前完成 target open、memory mapping、queue/resource grant 和 device-visible context 准备。未来 device-side surface 与它共享这些资源及生命周期，但不能调用 host registry、锁、`std::vector` 或 C++ virtual SPI。

当前 host API 使用：

```text
ExecutionDomain:
  HOST_EXECUTION     # host 直接执行 data-path submit
  DEVICE_EXECUTION   # host 把 device IO kernel 排入指定 stream

HostSubmitContext
  execution_domain
  device_id
  cudaStream_t stream          # DEVICE_EXECUTION 时必填；来自 tutti/cuda_like.h
```

本轮不把 `DEVICE_API` 做成公共 enum，也不冻结通用 device function 签名；只保证后述 DataPath interop sidecar 有清晰位置。

#### Host-initiated `DEVICE_EXECUTION` 的目标语义

`DEVICE_EXECUTION` 是 Tutti 的核心路径：CPU 完成 request validation、metadata 准备和资源预留，然后把 **IO kernel 及其 completion fence** 排入 caller 指定的 GPU stream。IO kernel 与模型计算 kernel 一样交给 GPU scheduler；不同 stream 上的 IO 与 compute 可以并发执行，实际 overlap 由 GPU 资源、依赖和调度决定。

`StorageRuntime::submit()` 在 `DEVICE_EXECUTION` 下返回 `IoHandle` 前必须满足：

1. 该 operation 的 IO kernel/执行链已经排入指定 stream；
2. 代表真实 IO 完成的 stream completion fence 已排入同一 stream；
3. operation 的 descriptor、PRP、status 和 event/fence lease 已独立持有；
4. 没有调用 `cudaStreamSynchronize`、`hipStreamSynchronize` 或等价全 stream 同步；
5. caller 随后排入同一 stream 的工作能够被 completion fence 正确排序。

因此“submit 返回”只表示 **IO 工作已入 stream**，不表示 storage IO 已完成；host 通过 `IoHandle::query/wait` 观察结果，GPU 工作通过 stream/event 建立顺序。

#### 同一 stream 的顺序

同一 stream 天然表达 producer/IO/consumer 顺序：

```text
GPU stream S:
  compute producer kernel
    → Tutti IO kernel / IO completion fence
      → compute consumer kernel
```

- WRITE：此前 compute kernel 产生的数据先完成，IO kernel 才能读取并写出；
- READ：只有真实 storage read 完成、completion fence 满足后，后续 compute kernel 才能消费数据；
- caller 在 `submit()` 返回后向同一 stream 排入 consumer kernel，不需要 host `wait()`；
- `DataPathOp` 内部可以使用 event、stream wait-value 或其他 accelerator fence，但必须保持上述可观察顺序。

#### 多 stream 并发与 event 顺序

同一 GPU 上允许多个 stream 并发提交，每个 operation 拥有独立 metadata lease 和 completion state：

```text
stream A: compute A ── IO A ── compute A2
stream B: compute B ── IO B ── compute B2
stream C: compute C ─────────── compute C2
```

DataPath 不能因为多个 stream 共用一个 `d_descs_`、staging buffer 或 event 而串行化或覆盖数据。queue/CID 数量不足时使用显式 backpressure，而不是让 GPU kernel 无限等待。

跨 stream 依赖由 caller/Framework Adapter 使用 accelerator event 表达：

```text
producer stream P: compute producer → record(ready_event)
IO stream I:       wait(ready_event) → Tutti IO kernel/fence → record(io_done)
consumer stream C: wait(io_done) → compute consumer
```

约束：

- event 由 caller/Adapter 创建和销毁；Runtime 不拥有 caller event；
- caller 在 `submit()` 返回后记录到 IO stream 的 event，必须排在 Tutti completion fence 之后，因此只在真实 IO 完成后 signal；
- stream/event 统一通过 `tutti/cuda_like.h` 的 `cudaStream_t`、`cudaEvent_t` 和 `cudaEventRecord/cudaStreamWaitEvent` 使用；profile shim 负责映射兼容厂商类型和函数；
- caller 保证 stream 以及其自行创建的 dependency events 活到相关 device work 完成；
- `IoHandle` 是 host 状态与资源生命周期句柄，不替代 stream/event 的 device-side ordering；
- DataPath 必须支持多个 stream 独立推进，或通过 capability 明确拒绝；不能暗中 hard-sync device/stream。

#### HOST_EXECUTION

`HOST_EXECUTION` 表示 CPU 执行实际 data-path submission，可以访问 host memory，或访问 DataPath 明确支持的 GPU buffer。它没有 GPU stream-ordering 承诺；需要与 GPU compute 协调时，caller 必须使用显式 accelerator fence/copy 语义。

因此 `HostSubmitContext` 的参数规则是：`DEVICE_EXECUTION` 必须显式传入 caller stream；`HOST_EXECUTION` 可以不带 native stream。Runtime 不为 `DEVICE_EXECUTION` 隐式选择 default stream，因为这会让 caller 无法可靠地在同一 stream 上追加 compute 或 record completion event。

当前版本没有 `COOPERATIVE_SUBMIT`。CPU 与 GPU 不共同提交同一条 storage command。

### 6.6 `IoHandle`、状态与结果

建议状态机：

```text
CREATED
   │
   ▼
ACCEPTED ──► RUNNING ──► COMPLETED
   │             ├─────► FAILED
   │             └─────► terminal TIMEOUT only after quiescence
   └────────────► FAILED
```

`IoSnapshot` 至少包含：

- overall state；
- request 总数和 terminal 数；
- 已完成 bytes；
- 可选轻量错误摘要。

`IoResult` 在终态后包含每个 request 的：

- `Status`；
- bytes completed；
- 可选 native status 数值；
- 不包含私有 completion object。

Batch 不提供事务原子性：

- Runtime 必须先完成所有通用参数校验；
- 接受后可按 `{DataPath instance, target, submission compatibility}` 拆成多个 sub-operation；
- 部分 sub-operation 可能成功、部分失败；
- caller 必须查看 per-request result，不能从 overall success 推导存储事务语义。

---

## 7. `StorageRuntime` 公共契约

以下是概念接口，不要求第一版逐字采用该 C++ 形式，但返回错误和生命周期的语义必须一致：

```text
StorageRuntime lifecycle
  create(RuntimeConfig, injected components) -> Result<StorageRuntime>
  shutdown(timeout) -> Status

Discovery
  query_cuda_like_profile() -> CudaLikeProfileInfo
  list_devices() -> Result<DeviceInfo[]>
  query_device_capabilities(device_id) -> Result<DeviceCapabilities>

Memory
  allocate_memory(spec) -> Result<MemoryAllocation {handle, address, size}>
  free_memory(handle) -> Status
  register_memory(view) -> Result<MemoryHandle>
  unregister_memory(handle) -> Status
  query_memory(handle) -> Result<MemoryInfo>

Target
  open(uri, options) -> Result<TargetHandle>
  close(target) -> Status
  query_target(target) -> Result<TargetInfo / TargetCapabilities>

IO
  submit(requests, submit_context) -> Result<IoHandle>
  query(io) -> Result<IoSnapshot>
  wait(io, timeout) -> WaitOutcome
  release_io(io) -> Status
```

### 7.1 Allocation 与 registration

- `allocate_memory()` 通过当前 CUDA-like profile 的 `cudaMalloc/cudaMallocHost` 等 API 分配，并创建 runtime-owned `MemoryRecord`；
- `register_memory()` 只接管 metadata，不接管 caller allocation；
- public registration 不等于对所有 DataPath 立即 DMA-map；
- data-path registration 按 `{DataPath instance, registration domain}` 在首次使用时惰性创建并缓存；
- 惰性 mapping 失败只使相关 submit 失败，`MemoryHandle` 仍可用于其他 DataPath；
- `free_memory()` 只接受 runtime-owned allocation；
- `unregister_memory()` 只接受 caller-owned registration；
- memory 有 inflight operation 时，两者返回 `BUSY`，不隐式阻塞，不提前 unmap/free。

如果未来真实消费方需要消除首次 IO mapping latency，可以单独评审显式 warm-up API；本版本不预设。

### 7.2 Target open/close

`open(uri)` 的流程为：

1. 按 URI scheme 和静态配置选择 resolver；
2. resolver 产生 `ResolvedTarget` 和资源 lease；
3. Runtime 选择与其兼容的 DataPath instance；
4. DataPath 打开私有 target；
5. `TargetRegistry` 同时持有 resolver lease 和 DataPath target；
6. 返回 opaque `TargetHandle`。

关闭顺序固定为：

```text
stop accepting new references
  → ensure no inflight IO
  → DataPath close private target
  → destroy ResolvedTarget / release file or namespace lease
  → invalidate TargetHandle generation
```

有 inflight IO 时 `close()` 返回 `BUSY`。Runtime 不静默延迟关闭，也不释放仍被设备访问的 target。

### 7.3 Submit

`submit()` 必须：

1. 验证 runtime 状态、handle generation 和 request bounds；
2. 查询 target capability；
3. 验证 `HostSubmitContext`、profile/device 与 memory 位置；
4. 按 registration domain 准备/复用 data-path memory registration；
5. 预留 Runtime operation record 和必要 backpressure credits；
6. 按 DataPath/target 分组；
7. 调用 DataPath submit；
8. 返回 `IoHandle`，不等待设备完成。

`submit()` 返回错误且不返回 handle 时，必须保证没有请求已经不可追踪地发出。为使该保证可实现，Runtime 与 DataPath 都遵循以下 commit 规则：

1. Runtime 在调用任何 DataPath 前创建 `IoRecord`，并保留返回 `IoHandle` 所需的 slot；
2. DataPath 在首次不可撤销 doorbell/transport submit 前完成可预见的 validation、容量预留、CID、descriptor、PRP 和 status lease 分配；
3. 若尚未发出任何 transport request，DataPath 可以返回 error 且不返回 operation；
4. 一旦发出任意 transport request，DataPath 必须返回有效 `DataPathOp`，不得只返回裸错误；
5. `DataPathOp` 记录每个 request 的 `NOT_ACCEPTED/ACCEPTED/FAILED` 初始状态，后续均可 query；
6. Runtime 只要收到任一有效 DataPathOp，就返回 `IoHandle`，并把其他尚未提交或失败的 request 记录为 per-request failure；
7. contract test 必须覆盖“第 K 个 request 发出后提交失败”。

因此 batch 允许部分执行，但任何已经发出的工作都不会失去 owner、lease 或完成观察路径。

### 7.4 Query、wait 与 release

- `query()` 非阻塞，可以驱动一次有界 progress；
- `wait()` 通过 IoRecord condition variable 与有界 progress/退避循环等待；当前不依赖通用 DataPath notification handle，也不能无界占用 GPU/CPU busy-loop；
- `wait(timeout)` 超时不取消 operation；
- operation 到 terminal 时，Runtime 立即释放重型 DataPathOp 和 metadata leases，只保留轻量结果；
- `release_io()` 释放轻量结果和 handle slot；inflight 时返回 `BUSY`；
- 未调用 `release_io()` 的 terminal result 不得自动淘汰，handle generation 也不得复用；
- terminal result 达到配置上限时，新的 `submit()` 通过 `RESOURCE_EXHAUSTED` backpressure，指标和日志提示 caller 释放旧 `IoHandle`；
- `shutdown()` 成功进入 `STOPPED` 后统一使该 Runtime 的所有公共 handle 失效；若 shutdown 观察超时，则 Runtime 仍处于 `DRAINING`，handle 仍可 query/wait/release。

### 7.5 Shutdown

Runtime 状态机至少包含：

```text
RUNNING → DRAINING → STOPPED
```

`shutdown(timeout)`：

1. 拒绝新的 open/register/submit；
2. 等待或推进 inflight operations；
3. 释放 terminal operations；
4. 关闭 targets；
5. 解除 data-path registrations；
6. 关闭 DataPaths；
7. 释放 runtime-owned memory，以及当前 CUDA-like profile 持有的 runtime/context 资源。

如果 timeout 到达且仍可能发生 DMA，返回 `TIMEOUT` 并保持相关对象存活，允许 caller 再次 drain；禁止为了退出而释放在飞资源。

---

## 8. Runtime 内部结构

### 8.1 Assembly root

`StorageRuntime` 是唯一 assembly root。建议通过 `RuntimeBuilder` 或等价构造注入装配：

```text
build profile -> CUDA-like selector/shim
resolver scheme -> StorageTargetResolver
data_path key -> DataPath
resolver output type + capability -> compatible DataPath
```

Registry 使用 string/key，不使用要求修改 core 的闭集全局 enum。

### 8.2 Registries

#### `MemoryRegistry`

负责：

- 地址范围与 handle lookup；
- allocation/registration ownership；
- profile/device identity；
- inflight refs；
- per-domain DataPath registrations；
- 并发 unregister/free 排斥。

禁止负责：

- PRP/SGL 构造；
- controller discovery；
- queue selection；
- framework tensor slicing。

#### `TargetRegistry`

每条记录持有：

- `ResolvedTarget` owner；
- DataPath instance 和私有 target handle；
- logical size；
- public capabilities；
- registration domain key；
- inflight refs；
- lifecycle state。

#### `IoRegistry`

每条记录持有：

- 原始 request 顺序；
- DataPath sub-operations；
- target/memory strong refs；
- per-request status；
- terminal result；
- timestamps 和 metrics context。

### 8.3 Routing 与 grouping

Runtime 只按通用 identity 分组：

```text
DataPath instance
+ target / registration domain
+ ExecutionDomain
+ profile/device
+ compatible HostSubmitContext
```

Runtime 不按 MDTS、extent、PRP、queue pair 或 warp 分组。这些都是 DataPath 内部 lowering。

### 8.4 Backpressure

Backpressure 分两级：

1. Runtime 全局/每 DataPath 的 operation、request 和 bytes-inflight 上限；
2. DataPath 私有 queue、CID、descriptor、PRP、metadata pool 上限。

资源不足时返回 `RESOURCE_EXHAUSTED` 或 `BUSY`，不允许：

- 覆盖共享 scratch；
- 无限阻塞 submit thread；
- 在 GPU kernel 内永久等待 queue slot；
- 无界扩容 GPU metadata memory。

### 8.5 线程安全

公共 Runtime 方法必须支持多线程调用。

最低规则：

- 不同 handle 的 open/register/submit/query 可以并发；
- 同一 memory/target 的 close 与 submit 由 registry 原子引用保护；
- close/unregister 不与 inflight operation 竞争释放；
- Runtime 保证同一 DataPath instance 最多一个并发 `progress()` 调用，除非 capability 明确允许更多；
- `DataPath::submit/query` 必须声明并遵守线程模型，默认要求实例级线程安全；
- Framework 对同一 memory 的跨 stream 数据竞争由 Framework Adapter/caller 负责，Runtime 只保证资源生命周期，不提供隐式数据一致性。

---

## 9. Memory 架构

Memory 不是一个包含所有存储 descriptor 的“万能层”。目标架构将其分成三个清晰平面。

### 9.1 平面一：Runtime-visible memory identity

由 `MemoryRegistry` 管理：

- allocation 与 external registration；
- address range、size、kind、ownership；
- profile/device location；
- handle generation；
- inflight refs；
- DataPath registration cache 的 owner。

这是 README/Roadmap 所说的 first-class memory subsystem。

### 9.2 平面二：CUDA-like memory primitives

由当前编译 profile 选中的 `tutti/cuda_like.h` 管理：

- host/device allocation 与 free；
- pointer/range inspection；
- pinned-host memory；
- device identity；
- native submit context 校验；
- 必要的 host/device copy 和 fence 原语。

这些能力不包含 storage controller DMA mapping。

### 9.3 平面三：DataPath-private registration 与 metadata

由具体 `DataPath` 管理：

- GPU page pin；
- IOMMU/controller/NIC registration；
- IOVA/rkey/cuFile handle；
- PRP/SGL/WR descriptor；
- target templates；
- GPU/pinned-host metadata workspace。

同一 memory 可能存在：

```text
MemoryHandle M
├── LocalNvmeDataPath / controller-domain A mapping
├── LocalNvmeDataPath / controller-domain B mapping
├── future GdsDataPath registration
└── future MooncakeDataPath registration
```

禁止再使用单个 `MemoryRegion::backend_private` 表示所有映射。

### 9.4 Registration domain

DMA mapping 不一定只由 DataPath 类型决定。local NVMe 可能按 controller/IOMMU domain 映射，RDMA 可能按 protection domain 注册。

因此 DataPath target 必须提供 opaque `RegistrationDomainKey`。Runtime 缓存键为：

```text
MemoryHandle
+ DataPath instance id
+ RegistrationDomainKey
```

DataPath registration 只返回私有 handle；Runtime 不解释其内容。

### 9.5 Metadata pool 归属

旧 `memory/` 中的可复用思想按以下方式迁移：

| 旧组件/思想 | 目标归属 |
|---|---|
| Memory identity、range lookup | `memory/MemoryRegistry` |
| `GpuSlotPool<T>` | 基于 CUDA-like API 的私有 allocation/fence 原语 |
| `HostSlotPool<T>` | 基于 CUDA-like API 的私有 pinned-host 原语 |
| `TieredHandleCache<T>` | `LocalNvmeDataPath/metadata/`，先保持具体实现 |
| `PrpListPool`、`PrpPageCache` | `LocalNvmeDataPath/metadata/prp/` |
| `AddressDescriptor`、IO slice table | local-NVMe data-path registration |

只有出现第二个真实使用者，才把通用 slot/arena policy 提升到共享库；不能为了代码复用重新建立一个理解 PRP 和 CUDA stream 的全局 Memory 层。

### 9.6 `MetadataArena` 与 lease

每个 DataPath operation 必须独占或引用计数持有工作集：

```text
IoHandle
  └── Runtime IoRecord
        └── DataPathOp
              ├── strong refs: target + memory registrations
              ├── DescriptorLease
              ├── Command/StatusLease
              ├── PrpLease
              └── CompletionFence
```

规则：

- arena 按 `{DataPath instance, accelerator device, registration domain}` 分片；
- capacity 有界、可配置、可观测；
- lease 到 operation terminal 后才能回收；
- 多 stream 复用必须通过 event/fence 建立顺序，不能只记录“最后使用 stream”；
- 同一 batch 的 distinct metadata working set 超过容量时显式失败或拆批，不能边用边驱逐本 batch 的早期 slot；
- PRP-list page 同时保存 host/device virtual view 与 controller DMA address；NVMe PRP2 使用 DMA address；
- cache eviction 不得销毁仍有 operation lease 的条目。

### 9.7 分层 metadata cache

对于大量 file/KV target，默认成本模型为：

```text
L2: large pinned-host metadata
    完整 target/extent/descriptor template

L1: bounded GPU DRAM metadata
    当前 hot working set
```

L1 eviction 是 downgrade，不重新执行 FIEMAP；L2 cold miss 才重新 build。需要观测：

- L1/L2 capacity 和 bytes；
- hit、promotion、eviction、cold build；
- batch promotion 次数和 H2D bytes；
- pool exhaustion；
- peak in-flight leases。

具体容量和替换算法属于 DataPath 配置，不进入 public API。

### 9.8 Memory coherence

Runtime 保证 registration 和 allocation 在 IO 终态前有效，但不自动解决应用数据竞争。

- `DEVICE_EXECUTION` 同一 stream：此前 compute producer、IO kernel/fence、此后 compute consumer 由 stream 顺序保证，无需 host `wait()`；
- `DEVICE_EXECUTION` 跨 stream：caller/Adapter 必须使用 producer event → IO stream wait，以及 IO-done event → consumer stream wait；
- WRITE 前输入 memory 必须通过同 stream 顺序或 event dependency 完成生产；
- READ 后 consumer 必须通过同 stream completion fence、跨 stream event 或 host `IoHandle::wait()` 观察完成；
- CPU 与 GPU、或多个 stream 对同一 range 的无依赖并发访问属于 caller data race；
- 当前版本不提供 region locking、自动 hazard tracking 或 CPU/GPU cooperative access。

---

## 10. NVIDIA-first CUDA-like accelerator API

### 10.1 最终决策

Tutti 最终采用与 Mooncake `cuda_alike.h` 相同的工程模型：**调用方统一使用 CUDA 风格的类型和函数名，构建时由单一 profile 选择 NVIDIA CUDA 或兼容厂商 shim。**

NVIDIA 是参考实现、语义基线和首个完整硬件支持目标：

```cpp
#include <tutti/cuda_like.h>

cudaSetDevice(device_id);
cudaMalloc(&ptr, bytes);
cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
cudaEventRecord(event, stream);
cudaStreamWaitEvent(other_stream, event, 0);
cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDefault, stream);
```

在 NVIDIA profile 下，`tutti/cuda_like.h` 直接包含 CUDA driver/runtime headers；不再为 allocation、stream、event、copy 等 CUDA-like 调用增加一层 `Accelerator` 虚接口。这样可减少样板、避免 hot path 虚分派，并让现有 CUDA 代码和使用者直接迁移。

该决定只影响 accelerator runtime/device API。Storage 语义仍由 `StorageRuntime`、`IoRequest`、handles 和 `DataPath` 定义；PRP、LBA、queue、controller mapping 仍不进入 CUDA-like 层。

### 10.2 公共 API 边界

`StorageRuntime` 的 profile-dependent stream/event 接口使用 `tutti/cuda_like.h` 提供的 CUDA-like 类型，例如 `cudaStream_t`、`cudaEvent_t`。对本文冻结的 Tutti CUDA-like 最小 contract，应用源码可在**已认证 profile 之间重新编译**；contract 外的厂商 API 不保证可移植。不同 SDK/profile 不承诺二进制 ABI，native handle 也不能跨 build/profile 传递。

保持中立的类型：

- `MemoryHandle`、`TargetHandle`、`IoHandle`；
- `IoRequest`、`Status`、`Result<T>`；
- resolver/DataPath host SPI 中与 accelerator 无关的值类型。

CUDA-like 的类型和函数：

- device enumeration/set-device；
- allocation/free、host pin；
- pointer attribute；
- stream/event；
- memcpy/memset；
- IPC/VMM 仅在 capability 和 profile contract 明确支持时开放；
- DataPath interop 的 kernel/device primitives。

禁止用 CUDA-like header 承担：

- NVMe/RDMA/GDS DMA registration；
- controller/NIC/queue 选择；
- PRP/SGL/RDMA WR 构造；
- peer-memory kernel driver 适配；
- storage operation completion 语义。

### 10.3 编译期 selector 与 profile

建议用户入口：

```text
-DTUTTI_ACCELERATOR=CUDA     # 默认，NVIDIA reference
-DTUTTI_ACCELERATOR=MACA     # 沐曦 MACA CUDA-like shim
-DTUTTI_ACCELERATOR=MUSA     # 摩尔线程 MUSA CUDA-like shim
-DTUTTI_ACCELERATOR=HOST     # 只用于 core/hardware-free tests
-DTUTTI_ACCELERATOR=<community profile>
```

CMake 为选中 profile 生成且只生成一个 target-scoped 定义：

```text
TUTTI_USE_CUDA
TUTTI_USE_MACA
TUTTI_USE_MUSA
TUTTI_USE_HOST
TUTTI_USE_<VENDOR>
```

`tutti/cuda_like.h` 是集中 selector：

```text
TUTTI_USE_CUDA  → include <cuda.h>, <cuda_runtime.h>
TUTTI_USE_MACA  → include <tutti/gpu_vendor/maca.h>
TUTTI_USE_MUSA  → include <tutti/gpu_vendor/musa.h>
TUTTI_USE_HOST  → minimal test-only types/stubs
```

兼容 shim 采用 Mooncake 当前源码中的首阶段映射方式：直接 include 厂商 CUDA-compatible headers、宏 alias、少量 inline wrapper。沐曦对应 MACA profile，典型映射为 `cudaMalloc → mcMalloc`、`cudaStream_t → mcStream_t`、`cudaEventRecord → mcEventRecord`；MUSA 对应 `musa*`/`mu*` 映射。该 runtime alias 不证明 device transport 已完成：Mooncake 当前 MACA device transport 仍有 stub；PTX/atomic/fence/barrier/device compiler 差异必须分别通过 profile override 和硬件测试。

采用 `TUTTI_` 前缀而不是裸 `USE_CUDA`，避免 Tutti 作为子项目时与 Mooncake、PyTorch 或其他依赖的全局宏冲突。

### 10.4 构建约束

1. 默认 profile 是 `CUDA`，主要面向 NVIDIA；
2. `project()` 默认只启用 `C/CXX`，选择 CUDA 后再 `enable_language(CUDA)` 和 `find_package(CUDAToolkit)`；其他 profile 使用各自 toolchain/compiler；
3. exactly one profile；未知或多个 profile 在 configure 阶段失败；
4. compile definitions、include、SDK libraries 和 flags 全部 target-scoped；禁止仓库级 `add_definitions()`、`include_directories()` 和全局 vendor flags；
5. profile 分支只集中在 `tutti/cuda_like.h`、`gpu_vendor/`、CMake profile 和 `DataPath/interop/cuda_like/profile_overrides/`；兼容实现优先复用同一 CUDA-like source，Runtime 业务逻辑中禁止散落 vendor `#ifdef`；
6. 未选择的 SDK 不参与 configure/include/link；
7. 每个 profile 必须有 feature ON/OFF compile gate；
8. `HOST` profile 只服务 contract tests，不能成为生产路径的静默 fallback。

建议扩展入口：

```text
cmake/accelerators/CUDA.cmake
cmake/accelerators/MACA.cmake
cmake/accelerators/MUSA.cmake
cmake/accelerators/HOST.cmake

include/tutti/cuda_like.h
include/tutti/gpu_vendor/maca.h
include/tutti/gpu_vendor/musa.h
```

### 10.5 “CUDA-like 可编译”不等于 DataPath 可用

CUDA-like shim 只证明所使用的 runtime/driver API 具有源码映射。以下能力必须由 profile 与具体 DataPath 单独验证：

- device compiler、kernel launch、atomics 和 memory ordering；
- stream/event 的真实顺序语义；
- P2P、IPC、VMM、dma-buf；
- GPUDirect/NVMe peer-memory；
- PCI BDF 与 topology 查询；
- doorbell MMIO、CQ polling 和 device-visible queue；
- snvme peer-memory provider 与厂商 kernel driver ABI。

因此沐曦等高 CUDA 兼容厂商的接入优先路径是：

1. 增加 profile 和 `cuda_like` shim；
2. 通过 CUDA-like contract tests；
3. 优先用同一 `LocalNvmeDataPath/interop/cuda_like` 编译；仅在必要时增加 `profile_overrides/maca`；
4. 增加对应 peer-memory/driver capability；
5. 通过真实硬件 IO 测试后才声明 local-NVMe 支持。

如果一个厂商无需 shim 即可直接编译 CUDA API，可让其 profile 直接包含兼容 headers；如果无法满足 CUDA-like 最小 contract，则不应在 Runtime 中加入 vendor 特判，而应由贡献者完善 shim/toolchain 或提出独立 RFC。

---

## 11. `DataPath` SPI

### 11.1 概念接口

```text
identity / capabilities
initialize(config, resource provider) -> Status
shutdown(timeout) -> Status

open(ResolvedTarget) -> Result<DataPathTarget>
close(DataPathTarget) -> Status
registration_domain(DataPathTarget) -> Result<RegistrationDomainKey>

register_memory(MemoryView, RegistrationDomainKey)
  -> Result<DataPathMemory>
unregister_memory(DataPathMemory) -> Status

submit(DataPathRequest[], HostSubmitContext)
  -> SubmitOutcome {Status, optional DataPathOp, per-request initial state}
progress(ProgressBudget) -> Result<ProgressResult>
query(DataPathOp) -> Result<DataPathSnapshot / terminal result>
release(DataPathOp) -> Status
```

所有 `DataPath*` noun 都是 Runtime 与具体实现之间的 opaque host-side 对象，不进入应用 public API。

`SubmitOutcome` 的不变量：

- `op=null` 表示没有任何 transport request 被不可撤销地发出；
- `op!=null` 表示至少一个 request 已发出或需要继续观察，即使 overall `Status` 表示部分提交失败；
- per-request initial state 与输入顺序一一对应；
- DataPath 必须在首次不可撤销提交前预留 operation record 和所有必要 lease；
- Runtime 只在 `op=null` 且没有其他 DataPath sub-operation 时，才可让 public `submit()` 直接返回 error 而不返回 `IoHandle`。

### 11.2 `DataPathCapabilities`

最低能力字段：

- stable name 与 source API version；
- 支持的 CUDA-like profile/memory kind；
- host API 支持的 `HOST_EXECUTION` / `DEVICE_EXECUTION`；
- 可选 device-initiated sidecar 的 source API/version（本轮为 unsupported）；
- direct / staged data movement；
- READ / WRITE；
- target、memory、length alignment；
- max single IO、max batch requests/bytes、max in-flight；
- scatter-gather 能力；
- registration scope；
- completion/progress model；
- `DEVICE_EXECUTION` 是否能在 caller stream 上建立真实 IO completion fence；
- 是否支持多 stream 并发及最大 concurrent streams/operations；
- GPU stream work 是否 device-autonomous，不依赖 host `query/wait` 才推进；
- 是否支持多 GPU、跨 device；
- optional target features。

Capabilities 是约束，不是提示。Runtime 在 submit 前验证，DataPath 在边界再次防御性验证。

### 11.3 DataPath lowering

DataPath 私有负责：

- logical request 到 transport requests 的 fan-out；
- MDTS、extent、block geometry；
- descriptor 和 command 构造；
- queue/CID/WR 分配；
- kernel/CPU submission；
- device completion 解析；
- per-request status 汇总；
- 私有 cache 和 workspace。

Runtime 禁止调用 `prepare_descriptors()` 或理解 DataPath fan-out 结果。

### 11.4 Progress contract

`ProgressBudget` 至少限制 wall-clock 时间和最大 work units。`progress(budget)` 必须返回：

```text
ProgressResult
  work_units_consumed
  operations_advanced
  operations_terminal
  more_work_likely
  optional next_poll_deadline
```

契约：

- 有界；
- 非阻塞或只在明确预算内工作；
- 不泄漏 transport completion 类型；
- 在没有工作时快速返回；
- 支持由 Runtime progress worker、`query()` 或 `wait()` 驱动；
- progress 级错误必须通过 `Result<ProgressResult>` 返回，并将受影响的 DataPathOp 标记为 terminal failure，或将整个 DataPath instance 置为 `NOT_READY/FAILED` 后终结所有受影响 operations；
- `more_work_likely=false` 和 `next_poll_deadline` 允许 Runtime 退避，避免无界 CPU polling。

DataPath 可以声明：

- `AUTONOMOUS`：硬件/device kernel/event 自动推进，`progress()` 为轻量状态收割；
- `POLLABLE`：HOST_EXECUTION 或非 stream-ordered 路径需要 Runtime 周期调用；
- `DEDICATED`：内部有专用 progress thread/kernel，但仍提供 query；
- 组合模式。

对 Tutti 的 `DEVICE_EXECUTION` 参考路径有额外硬约束：IO kernel、CQ completion 和 stream completion fence 必须在 device/内部 dedicated mechanism 上自主推进。caller 不调用 `query()`/`wait()` 时，GPU stream 也必须最终越过 completion fence；否则后续 compute kernel和 caller-recorded event 会永久等待。Runtime `progress()` 在该路径上只负责收割 host-visible status、唤醒 `IoHandle` waiter、处理故障和回收 terminal 资源，不是 storage IO 前进的必要条件。

当前 SPI **不承诺通用 notification fd/event**。Runtime 对 `POLLABLE` 实现按 `next_poll_deadline` 和配置的最小/最大退避调度；`wait()` 使用 condition variable 等待 IoRecord 状态变化，并由 progress worker 或当前 wait 调用者按预算推进。未来只有真实 DataPath 需要跨平台通知句柄时才扩展该 SPI。

禁止把无限 CQ busy-poll 伪装成有界 progress。

### 11.5 DataPathOp 生命周期

- submit 成功后 DataPathOp 必须可查询；
- `DEVICE_EXECUTION` 的 host submit 成功还表示 IO kernel/执行链和 completion fence 已经排入指定 stream；
- DataPathOp 到 terminal 前持有所有私有 lease；
- query 不销毁 operation；
- stream completion fence 只在该 operation 的所有 storage IO 已完成且数据对后续 stream work 可见后满足；
- terminal 时不再发生 DMA 或访问 caller memory；
- release 只在 terminal 后调用；
- timeout/reset 后如果无法证明 DMA 已停止，operation 仍不是 terminal，相关 mapping 必须继续保留或隔离。

### 11.6 未来 device-initiated IO 的保留边界

本轮不实现 device-initiated IO，但架构不得要求所有 IO 都经过 host `DataPath::submit()`。

未来能力放在具体组合的编译期 sidecar 中：

```text
data_paths/<path>/interop/cuda_like/
  host_prepare.*          # 由 StorageRuntime/DataPath 准备资源
  device_context.*        # trivially-copyable device-visible views
  device_api.*            # CUDA-like device compiler 可调用的函数/模板
  completion.*            # device-visible completion/error
  profile_overrides/      # 仅放无法通过 shim 兼容的最小差异
```

最小不变量：

1. host `StorageRuntime` 仍负责 open/register/map、queue grant、device context 创建和 teardown；
2. caller device kernel 只持有预先准备的 `DeviceTargetView`、`DeviceMemoryView`、queue/context 和 bounded workspace，不访问 host handle table；
3. device API 是 source-level、compile-time dispatch，不使用 host C++ virtual function、`std::vector`、动态加载或 GPU 上的通用虚分派；
4. DataPath/CUDA-like-profile 组合各自定义 device implementation，但应复用公共 request 语义：direction、memory offset、target offset、length 和明确 status；
5. device operation 的资源 lease、错误、completion 与 host teardown 必须形成可证明的生命周期；在该契约冻结前不公开稳定 device ABI；
6. 一个 DataPath 可以只支持 host-initiated 路径，capability 必须显式说明；
7. 新增 device sidecar 不应要求修改 `StorageRuntime`、其他 DataPath 或其他 CUDA-like profile。

现有 local-NVMe `__device__` queue/SQE helper 可作为迁移原型，但其 PRP/LBA/queue 私有类型不能直接提升为跨 DataPath 公共 API。等第一个真实 caller 提出需求时，再用 RFC 确定 device request、completion 和 error surface。

---

## 12. `StorageTargetResolver` SPI

### 12.1 责任

Resolver 只做 namespace/name 到 target 资源的解析：

```text
resolve(uri, options) -> Result<ResolvedTarget>
```

`ResolvedTarget` 是仓内 type-erased、带 owner 的对象，公共 SPI 壳只提供：

```text
ResolvedTarget
  resolver_type_id
  payload_type_id
  source_api_version
  logical_size
  recommended DataPath key
  shared owner of immutable payload + resource lease
```

具体 payload 不放入 common union。交接采用受检查的 source-level type erasure：

- Runtime 只比较 `{payload_type_id, source_api_version}` 和 compatibility，不读取 payload；
- DataPath 通过 `resolved.view<T>(expected_type_id, supported_version)` 获取只读 view，类型或版本不匹配返回 `UNSUPPORTED`；
- payload owner 由 `ResolvedTarget` 持有，而不是由 resolver singleton 暗中保存；
- resolver 与 DataPath 的成对 payload 类型放在独立 binding target/header，例如 `tutti_binding_ext4_local_nvme`，只被这两个实现链接；
- binding header 可以定义 extent 和 namespace identity，但不从 `tutti_api`/`tutti_runtime` PUBLIC 传播；
- payload 在 TargetHandle 关闭前 immutable，DataPath 不保存超出 owner 生命周期的裸引用。

这样既避免 `StorageTarget` 大 union，也不要求 `LocalNvmeDataPath` include 所有 resolver 的私有头。

### 12.2 组合规则

- URI scheme 选择 resolver，不直接写死 DataPath；
- Runtime 配置决定 resolver 输出与 DataPath instance 的组合；
- DataPath 必须显式声明能消费的 `{payload_type_id, source_api_version}`；
- 不要求任意 resolver 与任意 DataPath 形成笛卡尔积；
- 一个存储系统可以在同一 package 中同时实现 Resolver 和 DataPath，只要公共边界不泄漏；
- 每个实际组合必须有 binding contract test，覆盖 type/version mismatch 和 payload owner 生命周期。

### 12.3 ext4/FIEMAP 首个 resolver

`Ext4FiemapTargetResolver` 的首版只支持一个可验证的受限部署契约：

- 文件位于 Tutti 独占管理的专用 mount/device；
- 文件已完整分配并写实，拒绝 hole 以及 `UNKNOWN/DELALLOC/UNWRITTEN/ENCODED/NOT_ALIGNED/SHARED` 等不能安全直写的 FIEMAP 状态；
- extent 必须按 `fe_logical` 完整、无重叠覆盖 `[0, file_size)`；
- handle 生命周期内，部署侧使用可强制的管理方式禁止 truncate、hole-punch、reflink、defrag 和布局变化；普通 `flock` 只是协作锁，不能单独视为 extent lease；
- 通过 `st_dev`、mountinfo 和 sysfs 解析 filesystem block device、partition start、NVMe namespace/controller identity；
- FIEMAP `fe_physical` 先转换为底层 block device byte offset，再加 partition start 转换为 namespace offset/LBA；禁止直接把 `fe_physical` 除以 NVMe block size；
- 首版只接受能证明映射到单一 NVMe namespace 的简单 block device/partition；dm-crypt、LVM、md、device-mapper、COW 或无法证明映射的 block stack 全部拒绝；
- open 前按部署契约处理 page cache flush/invalidate，handle 生命周期内禁止任何 buffered/direct filesystem IO 与 Tutti 物理 extent IO 并发；
- 打开时无法证明任一条件成立就返回 `UNSUPPORTED` 或 `NOT_READY`。

这里的 `ResolvedTarget` owner 不是 Linux 提供的 extent pin，而是**受控部署契约 + 已验证 metadata snapshot**。若未来需要在不独占 mount 的环境中工作，必须引入能真正序列化布局变更的 metadata service 或 filesystem 支持，不能把 advisory lock 当安全保证。

不承诺任意既有文件都可用物理 extent 直写。首版不提供 create/delete/list、WAL、striping 或 power-loss durability协议。IO completion 只表示 DataPath 定义的命令完成，不自动等价于持久化事务提交。

---

## 13. local-NVMe 参考 DataPath

### 13.1 为什么是一个完整 package

local NVMe 的以下部分共同决定可用性，不能横切到多个“通用层”：

- controller/queue discovery；
- process/service resource ownership；
- libnvm；
- snvme UAPI 和 kernel module；
- GPU peer-memory interop；
- PRP/SGL 和 command；
- CPU/GPU submit；
- CQ completion；
- descriptor/PRP metadata pools。

它们统一位于 `data_paths/local_nvme/`，只通过 `DataPath` 对 Runtime 可见。

### 13.2 Bootstrap 与资源授予

local-NVMe 私有 `NvmeResourceProvider` 可有两个实现：

```text
DirectNvmeResourceProvider
  process owns controller/queue initialization

NvmeServiceResourceProvider
  service owns shared resources
  process receives resource grant and attach metadata
```

二者返回同一种私有 `NvmeResourceGrant`：

- controller/namespace identity；
- queue set 和 ownership；
- registration domain；
- block geometry/MDTS；
- submission capabilities；
- grant lifetime。

全局 Runtime 不提供抽象的 `VDevice`，也不要求其他 DataPath 实现 queue quota 模型。

### 13.3 Request lowering

`LocalNvmeDataPath::submit()` 内部步骤：

1. 按 file extents 和 MDTS fan-out；
2. 校验 LBA、block、memory alignment；
3. 取得 per-domain memory mapping；
4. 从 `MetadataArena` 取得 descriptor/SQE/status leases；
5. 从 DMA-correct PRP pool 取得 PRP-list pages；
6. 构造完全零初始化的 SQE；
7. `HOST_EXECUTION` 由 CPU data path 发 command；`DEVICE_EXECUTION` 由 CPU host 将 IO kernel 排入 `HostSubmitContext` 指定 stream；
8. GPU IO kernel 在 device 上取得 queue/CID、写 SQE 并 doorbell；
9. GPU kernel或内部 completion mechanism 观察 CQ，并把 NVMe status 写回每个 logical request；
10. 在同一 caller stream 上满足真实 IO completion fence，使后续 compute/event 可以继续；
11. Runtime 收割 host-visible status，将 `IoHandle` 置为 terminal；
12. operation terminal 后回收 leases。

### 13.4 GPU kernel 优化边界

以下策略都是 `LocalNvmeDataPath/interop/cuda_like/` 私有实现，兼容 profile 优先编译同一份源码：

1. one-thread-per-IO baseline；
2. lane-group/warp-per-QP；
3. batch reserve CID/SQ slot；
4. doorbell coalescing；
5. split submit/completion kernel；
6. 少量 queue-owner warps；
7. host-visible status harvest（只收割结果，不推进 `DEVICE_EXECUTION` CQ 或 completion fence）；
8. bounded persistent progress kernel；
9. descriptor template/cache；
10. profiling 证明收益后的 CUDA Graph。

Runtime 看不到 strategy enum。策略通过 DataPath 配置和 benchmark 选择。

优化必须同时验证：

- 数据正确性和 status 传播；
- active warps/SM 与 kernel residency；
- doorbell、atomic、CQ scan 次数；
- metadata H2D bytes；
- queue fairness；
- submit latency、IOPS/BW、p50/p99；
- 与模型 compute 的 overlap；
- timeout、reset 和 shutdown 行为。

### 13.5 真正异步的 GPU IO completion

目标执行模型是：

```text
CPU host thread
  validate / reserve metadata
  launch LocalNvme IO kernel into caller stream
  enqueue stream completion fence
  return IoHandle without stream synchronization

GPU scheduler
  co-schedule IO kernels and compute kernels from multiple streams
  preserve per-stream order and event dependencies
```

满足“真正异步”必须同时成立：

- caller 获得 `IoHandle` 时，IO kernel 与 completion fence 已经入指定 stream；
- CPU `submit()` 不等待 storage completion，也不调用 `cudaStreamSynchronize`；
- 同一 stream 上 IO 前后的 compute kernel严格按序；
- 不同 stream 可以并发，每个 operation 使用独立 descriptor/status/event lease；
- caller 可在 compute stream 与 IO stream 间使用 `record(event)` / `wait(event)` 表达依赖；
- caller 在 IO stream 上、`submit()` 返回后记录的 event，只能在真实 IO completion fence 后触发；
- GPU IO 的前进不依赖 host 调用 `query()`/`wait()`；
- `IoHandle` 用于 host status/error/lifetime，不是 GPU stream ordering primitive；
- kernel/controller error 可被 query/wait 观察；
- completion mechanism 不永久占满大量 SM，资源消耗可通过 kernel strategy 优化；
- target/memory/PRP/queue 在终态前不释放；
- shutdown 能 drain，故障时能隔离仍可能 DMA 的资源。

如果某项优化采用 split submit/completion、queue-owner warp 或 persistent kernel，也必须在 caller stream 上留下等价 completion fence；不能让 IO kernel提早返回后，后续 compute 在 storage read/write 尚未完成时继续执行。

---

## 14. snvme 内核边界

### 14.1 唯一事实源

仓库只能有一份被构建的 snvme 源码和一份共享 UAPI：

```text
data_paths/local_nvme/include/uapi/tutti_snvme.h
  ├── userspace libnvm include
  └── kernel module include
```

禁止用户态与内核各自复制 ioctl struct。

### 14.2 UAPI

UAPI 必须包含：

- 在任何 queue/map 操作前执行 ABI major/minor 与 capability handshake；
- 只使用 Linux UAPI 固定宽度类型 `__u8/__u16/__u32/__u64`、固定数组和显式 padding；
- 用户地址编码为 `__u64 user_addr`，禁止 native pointer、`size_t`、C++ 类型和隐式可变布局；
- 每个可扩展 payload 包含 `struct_size`、flags 和 reserved/MBZ 字段；
- queue/resource grant identifiers 与 mapping lifecycle；
- 明确的 ownership、错误码和 teardown 行为；
- 输入长度、alignment、flags、reserved 和数组计数 validation；
- ioctl 编号与 payload 演进规则，不能只依赖 `_IOC_SIZE` 偶然拒绝旧二进制；
- 32/64-bit compat ioctl 策略和支持的 endian 假设；
- 用户态与内核态对 struct size/offset 的 compile-time assertions；
- old-minor/new-minor、未知 flags、截短/扩展 struct、32/64 位布局的 ABI tests。

ABI major 不兼容时 `LocalNvmeDataPath` 初始化失败，不降级到 mock。minor 兼容必须通过 `struct_size` 和 capability 逐项协商；kernel 忽略已定义为可忽略的尾部扩展，但拒绝未知 mandatory flags。

### 14.3 内核内部拆分

```text
snvme common
├── ioctl validation
├── controller/queue lifecycle
├── mapping lifecycle
├── kernel compat ops
│   ├── supported kernel baseline A
│   └── supported kernel baseline B
└── peer_memory_ops
    ├── NVIDIA P2P
    ├── dma-buf（有真实需求和支持矩阵时）
    └── future provider
```

common mapping 流程不直接散落 `nvidia_p2p_*` 或其他项目 symbol。

### 14.4 部署

- module 在 Runtime 启动前由系统安装和加载；
- package/DKMS/systemd 策略由部署文档定义；
- Runtime 只做 readiness、ABI 和 capability 检查；
- 普通 unit/integration test 不执行 `insmod/rmmod`；
- hardware test 的模块准备和设备绑定由人工或专用受控环境完成；
- kernel support matrix 与 compile-only CI 是发布条件。

---

## 15. Framework Adapter

### 15.1 边界

Framework Adapter 负责：

- framework request 生命周期；
- tensor/KV/block layout；
- block id 到 memory/target slice；
- 批次合并；
- Runtime handle 缓存；
- `IoHandle` 到 framework request completion 的映射；
- worker shutdown/drain。

Core Runtime 不包含 framework callback、Python class、scheduler metadata 或 tensor shape。

### 15.2 vLLM

首个 `VllmAdapter` 必须锁定一个明确的 `KVConnectorBase_V1` 版本，不同时兼容多个未知版本。

建议流程：

```text
vLLM scheduler metadata
  → VllmAdapter validates version
  → worker/TP rank owns one StorageRuntime
  → register whole KV allocation once
  → block/layer ids become MemoryHandle + offsets
  → target layout becomes TargetHandle + offsets
  → submit IoRequest[]
  → retain IoHandle -> request ids
  → terminal result releases framework KV blocks
```

约束：

- IO 终态前 framework 不能回收或复用相关 KV blocks；
- Python 只通过薄 C ABI/pybind 调用 public Runtime API；
- Adapter 不 include local-NVMe headers；
- abort/shutdown 先停止新请求并 drain；当前没有 Runtime cancel；
- vLLM 对外包装可以按协议命名 `TuttiKVConnector`，内部类仍叫 `VllmAdapter`。

### 15.3 LMCache 与 Mooncake

- `LmCacheAdapter` 按 LMCache 官方生命周期实现，不与 vLLM 强行共用一个 `IConnector`；
- Mooncake 若作为 Tutti 消费者，使用独立 `MooncakeAdapter`；
- Mooncake 若作为未来数据移动实现，使用 `MooncakeDataPath`，内部委托其 `TransferEngine`；
- 两种角色不能混为一层，当前重构都只保留可接入边界，不主动实现。

---

## 16. 扩展场景验算

### 16.1 新 GPU 厂商

接入 AMD/ROCm 至少需要：

1. `ROCM/HIP` CUDA-like profile/shim，并通过最小 API contract；
2. 对应 DataPath 的 `profile_overrides/rocm`（只有共享 CUDA-like source 无法表达时才增加）；
3. driver/kernel peer-memory capability；
4. capability 和 hardware tests。

若无法满足 Tutti CUDA-like 最小 contract，则不要在 Runtime 加 vendor 特判，应提交独立 RFC 评估是否值得支持另一套 API family。

不应修改：

- `IoRequest`；
- `MemoryHandle`/`TargetHandle`/`IoHandle`；
- Framework Adapter 的 storage 逻辑；
- Runtime grouping。

### 16.2 新文件系统/namespace

接入新 resolver 需要：

1. 实现 `StorageTargetResolver`；
2. 定义仓内 opaque `ResolvedTarget` 类型和资源 lease；
3. 声明兼容 DataPath；
4. 配置 URI scheme 与 DataPath 组合；
5. 测试 target lifecycle 和布局稳定性。

不应让 Runtime 增加新的 filesystem enum/union 字段。

### 16.3 新 DataPath

接入 GDS、RDMA 或 Mooncake 需要：

1. 实现完整 `DataPath`；
2. 声明 capabilities；
3. 定义 registration domain；
4. 实现 submit/progress/query 和 terminal guarantee；
5. 通过 DataPath contract tests；
6. 静态注入 Runtime。

不应修改公共 request 或让 Runtime 理解 cuFile/RDMA WR/Mooncake request。

### 16.4 多 DataPath

同一 Runtime 可以静态注入多个 DataPath instance。每个 `TargetHandle` 绑定一个 instance；同一 batch 可被 Runtime 拆成多个 sub-operation。

同一 MemoryHandle 可被多个 DataPath 注册，但每份 mapping 独立拥有、独立失败、独立释放。

### 16.5 社区 feature 的合入路径

贡献者先判断 feature 属于哪个 owner：

| Feature 类型 | 放置位置 | 不应修改 |
|---|---|---|
| 新 CUDA-like profile/shim | `cmake/accelerators/` + `include/tutti/gpu_vendor/` | Runtime storage request/handle |
| DataPath 与 profile 互操作 | `data_paths/<path>/interop/<name>/` | DataPath core/public storage API |
| 新 DataPath | `data_paths/<name>/` | Runtime 硬件分支 |
| 新 namespace/filesystem | `target_resolvers/<name>/` + 必要 binding | 公共 target union |
| 新 framework | `adapters/<name>/` | core framework types |
| kernel/cache/pool 优化 | 对应 DataPath 私有目录 | SPI/public API |
| 未来 device-initiated IO | 具体 interop `device_api/` sidecar + RFC | host `StorageRuntime` façade |

建议每个社区扩展包含：

```text
README: use case / non-goals / owner / dependency / support matrix
CMake profile or registration module
capability declaration
contract tests + hardware-free negative/unsupported tests
optional hardware tests with explicit enable flag
minimal example
CI entry proving feature ON and OFF both build
```

评审只问三件事：

1. 是否选择了正确 owner，而不是把实现细节推入 core；
2. feature 关闭时是否完全不引入其 SDK、宏和 link dependency；
3. 是否通过已有 contract，而不是要求上层为它增加特殊分支。

社区友好的成功标准是：增加一个 feature 的 diff 主要落在一个新 package/profile 和它的 tests/docs 中；允许在集中 registry/CMake profile 列表增加一行，但不需要修改 Runtime 算法和公共 noun。

---

## 17. 目标目录与构建边界

```text
tutti/
├── cmake/
│   └── accelerators/           # CUDA/MACA/MUSA/HOST/community profiles
├── include/tutti/
│   ├── storage_runtime.h
│   ├── io_types.h
│   ├── memory_types.h
│   ├── capabilities.h
│   ├── status.h
│   ├── cuda_like.h             # profile selector; CUDA-style API
│   └── gpu_vendor/
│       ├── maca.h
│       └── musa.h
├── runtime/
│   ├── storage_runtime.cpp
│   ├── target_registry.*
│   ├── io_registry.*
│   └── data_path_registry.*
├── memory/
│   ├── memory_registry.*
│   └── memory_validation.*
├── spi/
│   ├── data_path.h
│   └── storage_target_resolver.h
├── cuda_like/
│   ├── host_test/              # hardware-free test shim only
│   └── contract_tests/
├── data_paths/
│   ├── mock/
│   └── local_nvme/
│       ├── io/
│       ├── metadata/
│       ├── control/
│       ├── interop/cuda_like/
│       │   ├── host_launch/    # 当前 host-initiated device execution
│       │   ├── device_api/     # 未来 device-initiated sidecar
│       │   └── profile_overrides/
│       ├── userspace/libnvm/
│       ├── service/nvmeservice/
│       ├── include/uapi/
│       └── kmod/snvme/
├── target_resolvers/
│   └── ext4_fiemap/
├── bindings/
│   └── ext4_local_nvme/        # pair-private payload contract
└── tests/

adapters/
├── vllm/
└── lmcache/
```

建议 CMake target 边界：

```text
tutti_api                 public headers; INTERFACE-links tutti_cuda_like
tutti_runtime             StorageRuntime + registries + memory identity
tutti_spi                 DataPath/Resolver source-level SPI
tutti_cuda_like           selected profile compile/link usage requirements
tutti_cuda_like_contract
tutti_data_path_mock
tutti_data_path_local_nvme
tutti_resolver_ext4_fiemap
tutti_binding_ext4_local_nvme
tutti_adapter_vllm
tutti_adapter_lmcache
```

约束：

- 顶层 `TUTTI_ACCELERATOR=<profile>` 恰好选择一种 CUDA-like profile，并产生对应 `TUTTI_USE_<PROFILE>` target-scoped 定义；
- `tutti_api` 必须 `INTERFACE`/`PUBLIC` link `tutti_cuda_like`，将选中的 `TUTTI_USE_<PROFILE>`、SDK include 和必要 runtime libraries 作为 usage requirements 传给 consumer；
- 导出的 CMake package 记录构建 profile，并拒绝 consumer 用不同 profile 重定义同一 Tutti 安装；
- `tutti_api` 可以经 `tutti/cuda_like.h` 暴露选中 profile 的 CUDA-like stream/event 类型，但不得直接 include 非当前 profile 或 storage 私有头；
- vendor SDK 只由 `tutti_cuda_like` 和对应 DataPath interop/sidecar target 链接；
- libnvm/NVMeService/snvme 只属于 local-NVMe package；
- `tutti_binding_ext4_local_nvme` 只向 resolver 和 DataPath implementation target 提供 PRIVATE headers，不进入 public Runtime；
- Adapter 只链接 public Runtime；
- 根工程和 standalone build 使用同一源，不定义两套同名 targets；
- mock 只能显式链接到 test，不作为依赖缺失时的生产 fallback。

---

## 18. 配置模型

配置分三层，禁止把所有实现字段塞进一个全局 YAML struct。

### 18.1 Runtime config

只包含通用项：

- registry limits；
- global/per-DataPath inflight limits；
- progress worker 策略；
- default wait/shutdown timeout；
- observability sink；
- resolver scheme 与 DataPath instance binding。

### 18.2 CUDA-like profile config

例如：

- device allowlist；
- allocation budget；
- pinned-host budget；
- vendor-specific initialization options。

这些字段由选中的 CUDA-like profile module 和 Runtime memory setup 消费。

### 18.3 DataPath config

local-NVMe 私有字段包括：

- direct/service bootstrap；
- controller/namespace allowlist；
- queue grants；
- metadata L1/L2 budgets；
- PRP pool capacity；
- max inflight；
- kernel/completion strategy；
- timeout/watchdog；
- file resolver compatibility。

Runtime 只持有构造完成的 DataPath，不解释这些字段。

配置 schema 必须有 version，未知字段和非法组合要明确报错。

---

## 19. Observability

### 19.1 Runtime 通用指标

- open memory/target/IO handles；
- requests/bytes accepted、completed、failed；
- inflight depth；
- submit/query/wait latency；
- per-request p50/p99；
- backpressure 和 resource exhaustion；
- progress 调用次数、预算和完成数；
- shutdown drain 时间；
- StatusCode 分布。

### 19.2 DataPath 私有指标

local-NVMe 至少提供：

- queue/CID occupancy；
- doorbell、atomic、CQ scans；
- NVMe status；
- MDTS/extent fan-out；
- descriptor/PRP pool occupancy；
- L1/L2 metadata hit/promotion/eviction；
- metadata H2D bytes；
- GPU kernel time、active warps/SM、residency；
- controller BW/IOPS；
- reset/timeout/quarantine。

Runtime 以 namespaced metrics 暴露，不把字段固化到公共 request ABI。

### 19.3 日志与 tracing

每个 operation 应可关联：

- runtime id；
- IoHandle generation；
- DataPath name/instance；
- target id；
- request index；
- native error domain/code。

日志不得输出用户数据或未经处理的私有地址。

---

## 20. 正确性与测试门

### 20.1 Runtime contract tests

使用 `MockDataPath` 和 `HOST` CUDA-like test shim 覆盖：

- stale/cross-runtime handles；
- offset/length overflow 和 bounds；
- mixed target/DataPath grouping；
- per-request error aggregation；
- DataPath 在第 K 个 request 发出后失败仍返回可查询 operation；
- backpressure，包括未 release terminal result 上限；
- query/wait/release 与 `WaitOutcome`；
- wait observation timeout 非取消语义；
- close/unregister 与 inflight IO；
- shutdown drain；
- progress budget；
- 多线程 submit/query。

### 20.2 Memory tests

- owned allocation 与 caller-owned external registration；
- memory kind/location/profile/device 检查与发现 API；
- pageable host pin lease 的创建、失败回滚和 unpin；
- v0.1 不接管 caller import mapping；
- same memory across multiple registration domains；
- mapping failure isolation；
- operation 强引用；
- unregister/free `BUSY`；
- generation reuse；
- metadata lease 不提前复用；
- pool exhaustion 和统计。

### 20.3 Resolver tests

- URI routing；
- target owner/close order；
- binding payload type/version mismatch 与 owner lifetime；
- holes/unwritten/shared/delalloc extents 拒绝；
- `fe_logical` 完整覆盖、file size/bounds；
- partition-start 到 namespace offset/LBA 的转换；
- 独占部署契约和 layout mutation detection；
- dm/LVM/md/COW 等 unsupported block stack 拒绝；
- common tests 不依赖 binding/DataPath 私有 header。

### 20.4 local-NVMe software tests

- SQE 全字段初始化；
- 1 page、2 pages、PRP-list；
- PRP2 使用 DMA address；
- alignment 和 MDTS fan-out；
- per-controller registration domain；
- target cache invalidation；
- queue/CID exhaustion；
- CQ error/timeout；
- CPU `submit()` 返回前已将 GPU IO kernel 与 completion fence 排入 caller stream；
- 同 stream producer → IO → consumer 顺序；
- 多 stream 并发和独立 metadata/event leases；
- 跨 stream producer-event/IO-event/consumer-event 顺序；
- 不调用 host `query/wait` 时 GPU stream 仍能自主越过 IO completion fence；
- shutdown 不释放在飞 mapping。

### 20.5 Hardware tests

在人工准备模块和设备后验证：

1. `StorageRuntime::open(file)`；
2. register/allocate GPU memory；
3. file offset write；
4. read 到另一 buffer；
5. byte-for-byte verify；
6. 覆盖跨 extent、PRP-list、不同 batch size；
7. 返回真实 NVMe error；
8. 同一 stream 上 compute producer → WRITE IO → completion event，以及 READ IO → compute consumer 的顺序；
9. 多 stream 上 compute 与多个 IO kernel 并发，并通过跨 stream event 保证依赖；
10. 不调用 host query/wait 时，device-side IO 仍完成且 caller-recorded event signal；
11. 多线程、多 GPU；
12. direct/service 两种 bootstrap；
13. 重复 lifecycle 无泄漏。

### 20.6 性能测试

建立旧实现和新 baseline，再比较：

- BW、IOPS、submit latency、p50/p99；
- GPU active warps/SM、kernel residency；
- metadata H2D；
- doorbell/atomic/CQ scan；
- pool hit/eviction；
- compute/IO overlap；
- GPU DRAM 与 pinned-host metadata budget。

不在没有 baseline 的情况下先写多套 kernel abstraction。每种优化必须证明收益且不破坏正确性门。

### 20.7 构建、profile 与 ABI 测试

- clean standalone configure/build/test；
- 根工程引用同一 source；
- `TUTTI_ACCELERATOR=HOST` 不查找任何 GPU SDK；
- `TUTTI_ACCELERATOR=CUDA/MACA/MUSA/...` 只查找并链接选中的 SDK；
- 未知 profile、零 profile 或多个 profile 在 configure 阶段明确失败；
- public storage headers 只允许通过 `tutti/cuda_like.h` 获得 CUDA-like 类型，不直接 include 未选 vendor、libnvm 或 NVMe 私有头；
- 每个社区 profile 的 feature ON/OFF build 和 CUDA-like API contract 都进入 CI；
- 示例 `MockDataPath`、HOST shim、resolver/binding 能在不修改 Runtime 的情况下注册和通过 contract tests；
- 至少两个受支持 kernel baseline compile-only；
- UAPI fixed-width size/offset assertions、32/64-bit compat、major/minor、truncated/extended struct 和 unknown-flags tests；
- production build 不链接 mock；
- hardware-free CI 不要求 root 或内核模块。

---

## 21. 迁移映射

迁移期间也只能有一个活动 build/source owner：

- 接口冻结后以目标 `tutti/` targets 为唯一新实现入口；
- 根目录旧 `memory/`、`io_engine/`、`nvme_storage/` 和 `backends/local/` 只作行为与性能参照，不与新 targets 同时组成生产 Runtime；
- libnvm、NVMeService、snvme 先选定唯一事实源，再迁入 `LocalNvmeDataPath`，禁止两棵树继续同步修复；
- 旧 `raw_device`、`block_storage`、`Coordinator` 和 StripeManager 保持非活动构建，前两者不因“接口已存在”而恢复；
- 每完成一个垂直切片就把旧对应 target 标记为 migration-reference 或删除，不能长期保留两个同名/同职生产实现。

Phase 0 的硬 gate 是：同步 `Roadmap.md`、确定唯一 build entry/feature options/target ownership，并让 clean hardware-free build 通过。未过此 gate，不开始大规模目录搬迁。

| 现有区域 | 目标去向 | 处理原则 |
|---|---|---|
| `tutti/accel/` | `include/tutti/cuda_like.h`、`gpu_vendor/`、Runtime memory registry | 拆掉虚接口；迁移 CUDA 实现为直接 CUDA-like 调用，registration 归 Runtime/DataPath |
| `tutti/device_manager/common` | Runtime registry 或 local-NVMe control | 只迁移确有使用的 grant/accounting 思想 |
| `tutti/device_manager/nvme` | `data_paths/local_nvme/control|userspace|service|kmod` | 形成单一部署单元 |
| `tutti/backends/nvme` | `data_paths/local_nvme/io|metadata|interop` | 修复正确性后接 `DataPath` |
| `tutti/io_engine` | Runtime grouping + DataPath-private lowering | 不保留 NVMe-aware generic engine |
| 根目录 `memory/` | `memory/` identity + CUDA-like primitives + DataPath metadata | 按 owner 拆迁，不整体复制 |
| 根目录 `nvme_storage/` | `LocalNvmeDataPath` | queue/target/kernel 私有化 |
| 根目录 `adapters/kv_cache` | `VllmAdapter`/`LmCacheAdapter` | 保留 layout/slice 合并思想，去除 NVMe/CUDA 类型 |
| `block_storage`/`coordinator`/`raw_device` | 不迁移现实现 | 按真实需求在稳定 Runtime 上重写 |

迁移顺序：

1. 评审本文并同步 `Roadmap.md`，修复唯一 build/source ownership 和 hardware-free build；
2. 冻结 public noun、`Status` 和三个 SPI；
3. 用 mock/fake 完成 Runtime contract；
4. 打通 data-path-owned registration；
5. 接入 file-backed `LocalNvmeDataPath`；
6. 收敛 libnvm/NVMeService/snvme；
7. 迁移 metadata pools 和 async progress；
8. 以 benchmark 驱动 GPU kernel 优化；
9. 接入首个真实 Framework Adapter；
10. 功能、正确性、性能对齐后删除旧树。

---

## 22. 架构验收清单

### 公共边界

- [ ] 应用只 include `include/tutti/`，accelerator API 统一来自 `tutti/cuda_like.h`；
- [ ] public storage headers 不直接 include 未选 vendor、libnvm/NVMe/FIEMAP 私有类型；
- [ ] public request 只含 handles、offset、length、direction；
- [ ] `Status`、`WaitOutcome` 和 per-request completion 完整；
- [ ] handle 有 generation 和明确 release，未 release 结果不被自动淘汰。

### Memory

- [ ] allocation、runtime registration、accelerator host-pin lease、DataPath mapping 四种语义分离；跨进程 import 不在 v0.1；
- [ ] 同一 memory 支持多个 registration domains；
- [ ] inflight 时不能 unmap/free；
- [ ] metadata pool 有界且 operation-owned；
- [ ] 多 stream 不共享覆盖 scratch。

### DataPath

- [ ] Runtime 不理解 descriptor/kernel；
- [ ] `DataPath` 覆盖 open/register/submit/progress/query；
- [ ] 一旦任何 transport request 发出，submit 必须返回有效 DataPathOp；
- [ ] progress 返回预算消耗、推进结果、退避 deadline 和错误；
- [ ] capability 可在提交前验证；
- [ ] 缺失依赖显式失败；
- [ ] terminal 后保证不再 DMA。

### File target

- [ ] resolver 与 DataPath owner 清楚，pair-private payload 有 type/version 校验；
- [ ] extent 稳定、partition offset、block-stack 和 coherency 条件可验证；
- [ ] 任意文件不被错误承诺；
- [ ] public API 无 raw device；
- [ ] 不预设 WAL/striping/create/delete。

### Async 与性能

- [ ] CPU submit 在返回前将 IO kernel 与真实 completion fence 排入 caller stream，且不 hard-sync；
- [ ] 同 stream compute/IO 顺序正确；跨 stream 可用 event 表达依赖；
- [ ] 多 stream 的 IO/compute 可以并发，operation 不共享覆盖 scratch/event；
- [ ] 不调用 host query/wait 时 GPU IO 仍能完成并推进 stream；
- [ ] `IoHandle` 可 query/wait，且只负责 host observation/lifetime；
- [ ] progress 有界；
- [ ] pool exhaustion 正确 backpressure；
- [ ] kernel strategy 可替换且 public API 不变；
- [ ] 指标覆盖 GPU 资源、metadata、queue 和 IO latency。

### 扩展与部署

- [ ] `TUTTI_ACCELERATOR=<profile>` 恰好选中一种实现，并只定义一个 `TUTTI_USE_XXX`；
- [ ] 未选中的 vendor SDK 不参与 configure/include/link；
- [ ] test DataPath/CUDA-like profile shim/Resolver/Binding 可接入且不修改 Runtime 公共 storage noun/算法；
- [ ] feature OFF build 与 hardware-free tests 保持通过；
- [ ] 未来 device-initiated IO 有 interop sidecar 位置，但本轮没有伪造通用 device ABI；
- [ ] Adapter 不 include DataPath 私有 header；
- [ ] local-NVMe 用户态/内核只有一个事实源；
- [ ] UAPI 使用固定宽度布局、compat 规则和 version/capability handshake；
- [ ] `Roadmap.md` 已同步且只有一个活动架构/build 事实源；
- [ ] 普通测试不自动加载/卸载内核模块。

---

## 23. 最终边界总结

目标架构的核心不是增加更多接口，而是让每类变化停留在正确位置：

```text
Framework layout changes
  → FrameworkAdapter

URI / filesystem / object namespace changes
  → StorageTargetResolver

Accelerator vendor / device memory / compiler changes
  → TUTTI_ACCELERATOR profile + cuda_like shim + DataPath interop

Host-initiated or future device-initiated IO changes
  → host StorageRuntime path or compile-time interop/device_api sidecar

NVMe / GDS / RDMA / Mooncake data movement changes
  → DataPath

Batch lifecycle / ownership / error / backpressure changes
  → StorageRuntime

PRP pool / metadata cache / kernel / CQ optimization
  → LocalNvmeDataPath private implementation

Linux kernel / peer-memory API drift
  → snvme compat + peer_memory_ops
```

只要这组边界成立，Tutti 就可以在不扰动上层 file/KV-cache 调用者的前提下，继续优化 GPU DRAM metadata 使用、GPU kernel 资源占用、异步 IO、不同存储路径和不同 GPU 厂商；反之，如果 Runtime 再次理解 PRP 或 filesystem extent，重构就只是目录搬迁，而没有真正形成可演进架构。CUDA stream/event 是最终选定的 CUDA-like 应用 API，但 NVMe queue、PRP 和 completion kernel 仍必须留在 DataPath 内。

---

## 24. 当前 Tutti 到目标架构的映射与拆分

### 24.1 结论

**当前 Tutti 可以映射到目标架构，但不能通过目录改名或继续补齐 L0-L6 完成。**

可以保留的是已经验证或有明确价值的组件：

- CUDA allocation、stream/event、copy 调用；
- GPU/pinned-host metadata pool；
- tiered handle cache；
- FIEMAP 和 extent 处理思路；
- libnvm、queue、PRP、SQE、CQ 和 GPU submit kernel；
- NVMeService 的资源授予模型；
- snvme 内核数据面；
- KV layout 到批量 IO slice 的转换思路。

必须重建的是组件之间的 owner 和接口：

- 旧 `Coordinator/IoEngine` 不能直接成为 `StorageRuntime`；
- 旧 `memory/` 不能整体成为公共 Memory 层；
- 旧 `DeviceManager` 不能作为所有 DataPath 的前置层；
- 旧 `IBackend`/`IBatchSubmitter` 不能作为目标 DataPath SPI；
- 旧 `GpuFile/StorageTarget` 不能成为公共 target；
- 旧 CUDA HAL 不保留虚接口，改用 NVIDIA-first CUDA-like API；
- 新旧 libnvm/NVMeService/snvme 不能继续双份存在。

因此迁移性质是：**保留成熟实现，重做装配和生命周期。** 没有发现必须推翻 local-NVMe 数据面的根本障碍；真正工作量集中在 Runtime contract、memory registration、async completion、file resolver、安全生命周期和 build ownership。

### 24.2 当前结构到目标结构的总图

```text
当前根目录旧实现                         当前 tutti/ 重构树
────────────────────────────             ───────────────────────────
coordinator/                              coordinator/（未构建）
memory/                                   accel/
io_engine/                                device_manager/common
nvme_storage/                             device_manager/nvme
block_storage/                            backends/common + nvme
device_manager/                           io_engine/
backends/local/                           block_storage/（未构建）
adapters/kv_cache                         raw_device/（未构建）
     │                                    kernel/libnvm 副本
     │                                      │
     └──────────────────┬───────────────────┘
                        │ 按 owner 拆分，不按旧层号搬迁
                        ▼
┌────────────────────────────────────────────────────────────────────┐
│                         Tutti Target                               │
│                                                                    │
│  FrameworkAdapter ───────────────┐                                 │
│    └── VllmAdapter/LmCacheAdapter│ cudaStream/event                 │
│                                  ▼                                 │
│  StorageRuntime ─────────── CUDA-like API/profile                  │
│    │ handle/IO lifecycle     CUDA direct | MACA/MUSA shims         │
│    │ grouping/error          memory / stream / event / device code │
│    ├──────── MemoryRegistry ◄──────────────┘                        │
│    │          allocation identity / host pin                       │
│    │          mapping registry                                     │
│    │                                                               │
│    ├── StorageTargetResolver ── Binding ──┐                        │
│    │    Ext4FiemapResolver                │                        │
│    │                                      ▼                        │
│    └────────────────────────────── LocalNvmeDataPath               │
│                                      ├── io                        │
│                                      ├── metadata pools/cache      │
│                                      ├── control/resource grant    │
│                                      ├── interop/cuda_like ◄───────┘
│                                      ├── libnvm                    │
│                                      ├── NVMeService               │
│                                      └── snvme                     │
└────────────────────────────────────────────────────────────────────┘
```

### 24.3 模块级映射表

| 当前区域 | 当前价值/问题 | 目标位置 | 处理决定 |
|---|---|---|---|
| 根 `coordinator/` | 有 assembly/rollback 思路，但固定按 registry→nvme_storage→block_storage→memory→io_engine bootstrap，直接依赖 CUDA/NVMe | `runtime/storage_runtime.*` | **迁移参考**。重写为 handle registries、静态注入和 drain；不迁移 `GpuFile`/raw 接口。 |
| `tutti/coordinator/` | façade 形状接近目标，但依赖旧 `IBackendProvider`、BlockStorage、RawDevice，当前未构建 | `StorageRuntime` | **不原样迁移**。只参考 API 意图；按本文 public contract 重写。 |
| 根 `memory/` | 混合 allocation、registry、DMA map、PRP、slice table、GPU cache | `memory/` + CUDA-like layer + `LocalNvmeDataPath/metadata` | **必须拆分**，见 24.4。 |
| `tutti/accel/` | CUDA allocation/stream/event/copy 可复用；`IAccelerator` 过宽且 teardown 不完整 | `include/tutti/cuda_like.h`、`gpu_vendor/`、`MemoryRegistry` | **拆掉虚接口**。CUDA 调用直接迁移；registration identity 归 Runtime；DMA mapping 归 DataPath。 |
| 根 `io_engine/` | host batch builder、GPU batch kernel、stream ordering 有参考价值；公共接口完全 NVMe/CUDA 化 | Runtime grouping + `LocalNvmeDataPath/io|interop` | **必须拆分**，见 24.5。 |
| `tutti/io_engine/` | 尝试上移 batch/async，但直接依赖 `nvme::IBatchSubmitter`、PRP 和共享 `d_descs_` | Runtime + `LocalNvmeDataPath` | **不能作为目标 engine 保留**。通用 validation/grouping 重写；descriptor/kernel 下沉。 |
| 根 `nvme_storage/` | queue、file handle、PRP、GPU kernel 是成熟 local-NVMe 数据面 | `data_paths/local_nvme/io|metadata|interop/cuda_like` | **保留算法并修复**。兼容 profile 优先复用源码；不能传播 NVMe 私有类型。 |
| 根 `device_manager/` | discovery/resource ownership 与旧 Coordinator 强耦合 | `LocalNvmeDataPath/control` | **局部迁移**。不建立全局 DeviceManager 层。 |
| `tutti/device_manager/common` | driver/device/grant/accounting 接口和 mock 有测试价值 | `LocalNvmeDataPath/control` 或 test utilities | **按需下沉**。只有第二个真实 DataPath 共享模型时再提升。 |
| `tutti/device_manager/nvme` | daemon/direct driver、libnvm、NVMeService、snvme 已聚拢，但构建边界混杂 | `LocalNvmeDataPath/control|userspace|service|kmod` | **主要迁移来源**。direct stub 未完成前必须返回 unsupported。 |
| 根 `backends/local/` | 旧 libnvm、service、kernel 仍被父工程构建 | 目标 local-NVMe 唯一事实源 | **迁移参考后删除**。逐文件合并差异，禁止继续双修。 |
| `tutti/backends/include` | `IBackend` 只有 lifecycle/roster/metadata，不能提交 IO | `spi/data_path.h` | **重写 SPI**。不保留 closed backend enum/union。 |
| `tutti/backends/nvme` | target、PRP builder、submit kernel 是目标 DataPath 原型；存在多项 P0 | `LocalNvmeDataPath/io|metadata|interop` | **保留并修复**。必须先完成 mapping、PRP-list DMA、SQE、status、timeout 和 operation lease。 |
| 根 `block_storage/` | 文件目录、WAL、striping、allocator 与 DataPath 混合 | 本轮无生产目标 | **不迁移现实现**。只提取 FIEMAP/target resolve 所需代码；其余需求出现后重写。 |
| `tutti/block_storage/` | 依赖已删除 SPI且未构建 | 无 | **冻结/删除候选**，不作为目标上层。 |
| `tutti/raw_device/` 与 `tutti/coordinator/{include,src}/raw_device*` | 两套接口，且不是当前需求 | local-NVMe test fixture | **不实现、不合并、不公开**。固定 LBA 只留测试。 |
| 根 `adapters/kv_cache` | KV layout、contiguous block 合并有价值；依赖旧 Coordinator、`GpuFile`、CUDA/NVMe 类型 | `adapters/vllm`、`adapters/lmcache` | **按 framework 生命周期重写**。只保留 slice 计算思路。 |
| 根 `CMakeLists.txt` + `tutti/CMakeLists.txt` | 两套 targets；无条件启用 CUDA、全局 flags/includes | `cmake/accelerators/*.cmake` + 单一目标树 | **Phase 0 首先重做**。默认 CUDA profile，MACA/MUSA shim，feature OFF 零依赖。 |
| `doc/layered`、旧 verification docs | 能解释历史，不再代表目标架构 | `doc/history` / 迁移参考 | **归档**。本文和同步后的 Roadmap 是目标事实源。 |

### 24.4 `memory/` 的具体拆分

```text
当前 memory/
├── MemoryRegion / allocation / lookup
├── CUDA allocation、stream、event、host pin
├── controller DMA mapping
├── AddressDescriptor / IoSliceTable
├── GpuSlotPool / HostSlotPool
├── TieredHandleCache
└── PrpListPool / PrpPageCache

拆成：

Runtime MemoryRegistry
├── MemoryHandle / generation
├── address range / ownership / kind
├── inflight refs
└── per-{DataPath, registration-domain} mapping table

CUDA-like API/profile
├── cudaMalloc / cudaFree
├── cudaHostRegister / cudaHostUnregister
├── cudaPointerGetAttributes
├── cudaStream / cudaEvent
└── cudaMemcpyAsync

LocalNvmeDataPath
├── memory registration / IOVA
├── IO slice / AddressDescriptor
├── MetadataArena
├── TieredHandleCache
└── DMA-correct PRP pool/cache
```

具体映射：

| 当前组件 | 目标归属 | 可复用程度 | 必须修改 |
|---|---|---|---|
| `MemoryRegion` 与 lookup | `MemoryRegistry` | 数据字段和 range lookup 可参考 | 改为 opaque generation handle；移除 `backend_private`、PRP 和 NVMe mapping。 |
| `IMemorySubsystem::allocate/register` | Runtime + CUDA-like API | allocation/ownership 语义可保留 | 不再全局 bind controllers；一个 memory 对多个 registration domain。 |
| `GpuSlotPool<T>` | local-NVMe metadata 使用的 CUDA-like primitive | 池算法可保留 | slot 改为 operation lease；多 stream event/fence；结构化 exhaustion。 |
| `HostSlotPool<T>` | pinned-host metadata/staging | 大体可保留 | 使用 CUDA-like host API、补 ownership/stats。 |
| `TieredHandleCache<T>` | `LocalNvmeDataPath/metadata` | L1 GPU + L2 pinned-host 思路应保留 | cache entry 必须被 operation pin；不能在 batch/IO 中途驱逐。 |
| `PrpPageCache`/`PrpListPool` | `LocalNvmeDataPath/metadata/prp` | policy 可参考 | PRP page 必须独立 DMA-map；`prp2` 写 IOVA，不写 CUDA virtual address。 |
| `IoSliceTable`/`AddressDescriptor` | local-NVMe registration | 预计算思路可保留 | 按 registration domain 保存；不进入 Runtime/public API。 |

结论：根 `memory/` **不是一个目标层**，而是三个 owner 的源码矿区。整体搬迁会再次把 CUDA、NVMe 和 Runtime 生命周期耦合起来。

### 24.5 `io_engine/` 的具体拆分

```text
当前 io_engine/
├── public IIoEngine / IoEngineImpl
├── request validation
├── batch flatten/grouping
├── NVMe descriptor build
├── H2D scratch staging
├── CUDA kernel launch
├── CQ polling/completion
└── stream synchronize/event

拆成：

StorageRuntime
├── public IoRequest / IoHandle
├── bounds/capability validation
├── DataPath/target grouping
├── backpressure
├── result aggregation
└── host query/wait/lifecycle

LocalNvmeDataPath
├── extent + MDTS fan-out
├── PRP/SQE/CID
├── per-operation MetadataLease
├── host-launched CUDA-like IO kernel
├── CQ/status/completion fence
└── kernel strategy / benchmark
```

保留原则：

- host batch builder 中只有**与 transport 无关**的 request ordering/grouping 可进入 Runtime；
- 任何出现 PRP、LBA、SQE、queue pair、doorbell、CQ 的逻辑都进入 local-NVMe；
- 共享 `d_descs_` 改成 per-operation lease；
- async 不再等价于 kernel launch success；必须有 `IoHandle`、真实 status 和 stream completion fence；
- kernel strategy 可替换，但不能反向增加 Runtime API。

### 24.6 local-NVMe 的具体拆分

```text
LocalNvmeDataPath
├── io/
│   ├── LocalNvmeDataPath implementation
│   ├── target + registration domain
│   ├── memory map/unmap
│   ├── extent/MDTS lowering
│   └── operation/completion
├── metadata/
│   ├── MetadataArena
│   ├── descriptor/SQE/status pools
│   ├── TieredHandleCache
│   └── PrpPageCache
├── control/
│   ├── DirectNvmeResourceProvider
│   └── NvmeServiceResourceProvider
├── interop/cuda_like/
│   ├── host_launch
│   ├── device_api             # future
│   └── profile_overrides/     # MACA/MUSA 仅在 shim 不足时增加
├── userspace/libnvm/
├── service/nvmeservice/
├── include/uapi/tutti_snvme.h
└── kmod/snvme/
```

来源选择：

| 目标子目录 | 首选迁移来源 | 补充来源 |
|---|---|---|
| `io/` | `tutti/backends/nvme` | 根 `nvme_storage` 中已验证的 queue/file 算法 |
| `metadata/` | 根 `memory` | `tutti/backends/nvme/prp_page_cache` 作为问题样例 |
| `control/` | `tutti/device_manager/nvme` | 根 `device_manager` 的 bring-up 行为 |
| `interop/cuda_like` | `tutti/backends/nvme/device` | 根 `io_engine/local_nvme` 与 `nvme_storage` kernel；兼容 profile 复用 |
| `userspace/service/kmod` | 从新旧两树审计合并 | 不能直接假设某一整棵树完整更新 |

必须先修正的数据面问题包括：

- 正式 memory registration 到 controller mapping；
- memory offset/page index；
- mixed-target grouping；
- alignment/bounds；
- PRP-list page 的 DMA address；
- SQE 全零初始化；
- per-operation scratch/PRP/status lease；
- NVMe status 与 launch error 传播；
- CQ timeout/reset/quarantine；
- target cache 释放与 generation。

### 24.7 CUDA-like 层的当前代码映射

```text
当前
  tutti/accel/include/common/iaccel.h
  tutti/accel/src/cuda/cuda_accelerator.cu
  tutti/abstraction/accel.h   # 仅作宏迁移参考，最终归 selector/device override
  根 memory/io_engine 中散落的 cuda* 调用

目标
  include/tutti/cuda_like.h
  include/tutti/gpu_vendor/maca.h
  include/tutti/gpu_vendor/musa.h
  cmake/accelerators/{CUDA,MACA,MUSA,HOST}.cmake
  cuda_like/contract_tests/
  data_paths/local_nvme/interop/cuda_like/
```

拆分规则：

1. `cudaMalloc/cudaFree/cudaStream/cudaEvent/cudaMemcpy` 等直接迁到 CUDA-like API，不保留 `Accelerator` virtual call；
2. allocation identity、range、ownership 和 inflight ref 进入 `MemoryRegistry`；
3. storage DMA map、PRP、peer-memory 进入 DataPath；
4. `launch(void* kernel)` 不进入通用 API，具体 kernel 由 DataPath interop 编译；
5. IPC/VMM 只有当前 profile contract 和真实需求都满足时才开放；
6. NVIDIA CUDA 是行为基线；MACA/MUSA shim 必须通过相同的 runtime/stream/event contract；
7. profile 通过不代表 local-NVMe 通过，后者仍需 interop、kernel driver 和硬件 IO 测试。

### 24.8 上层 file/KV 路径的映射

```text
当前 adapters/kv_cache
  framework block/tensor layout
  + GpuFile assumptions
  + cudaStream_t
  + NvmeBatchInputTensor
  + old Coordinator

目标 VllmAdapter / LmCacheAdapter
  framework metadata/lifecycle
  → whole-allocation MemoryHandle
  → TargetHandle
  → IoRequest {memory_offset, target_offset, length}
  → StorageRuntime::submit(..., cudaStream_t)
  → IoHandle / stream event completion
```

保留：KV block 合并、连续 slice 发现、按 layer/request 聚合。

删除或重写：`GpuFile`、固定 K/V file offset、NVMe batch tensor、旧 Coordinator、每 tensor 重复注册。

文件目标通过：

```text
path/URI
  → Ext4FiemapTargetResolver
  → ResolvedTarget<Ext4NvmePayload>
  → ext4_local_nvme Binding
  → LocalNvmeDataPath target
```

本轮只支持受控、预分配、extent 稳定的文件；不恢复 BlockStorage、WAL、striping 或 rawdevice。

### 24.9 构建系统的映射

当前两个根问题：

1. 根 CMake 和 `tutti/CMakeLists.txt` 都无条件启用 CUDA、全局 include/flags，并定义重叠 targets；
2. parent 仍构建旧 libnvm/snvme，新树又保留副本。

目标构建链：

```text
TUTTI_ACCELERATOR=CUDA (default)
  → cmake/accelerators/CUDA.cmake
  → TUTTI_USE_CUDA
  → enable CUDA + CUDAToolkit
  → tutti_cuda_like
  → LocalNvmeDataPath interop/cuda_like

TUTTI_ACCELERATOR=MACA
  → MACA.cmake + toolchain
  → TUTTI_USE_MACA
  → tutti/gpu_vendor/maca.h
  → 复用 interop/cuda_like；只有 shim、driver 和 capability 全通过时启用 local-NVMe device path
```

构建拆分必须先于大规模源码搬迁，否则每个目录移动都会继续被两套 target 和全局 CUDA 依赖干扰。

### 24.10 可保留、需重写和不迁移的最终清单

#### 可保留并迁移

- CUDA 调用和 kernel 的主体；
- libnvm queue/control primitives；
- NVMe SQ/CQ、doorbell、CID 算法；
- GPU/pinned-host slot pools 的算法；
- tiered metadata cache 思路；
- FIEMAP 读取和 extent walking 的基础代码；
- NVMeService resource grant 的行为；
- snvme 已验证的数据面与 kernel compatibility 修复；
- KV slice 合并算法；
- mock/contract test 思路。

#### 必须重写接口或生命周期

- `Coordinator`/`IoEngine` public API；
- `MemoryRegion` 与 registration；
- `StorageTarget`/`GpuFile`；
- `IBackend`/`IBatchSubmitter`；
- async operation/completion/error；
- metadata pool lease；
- DeviceManager 到 DataPath control 的归属；
- CUDA HAL 到 CUDA-like selector；
- build/profile/target ownership；
- kernel UAPI 与 peer-memory provider 边界。

#### 本轮不迁移

- public rawdevice；
- 两套 RawDevice 实现；
- BlockStorage 的 WAL、striping、allocator；
- 已失效的 L5/L6 实现；
- 生产 fallback mock；
- 同一 build 同时启用多个 CUDA-like profiles；
- 通用 device-initiated public ABI；
- 无需求的 GDS/RDMA/Mooncake DataPath 实现。

### 24.11 建议的实际拆分顺序

```text
Phase A  Build/profile
  单一源码 owner
  CUDA-like selector + CUDA/HOST contract
  旧/新 libnvm、snvme 选唯一事实源

Phase B  Host core contract
  Status/handles/MemoryRegistry
  StorageRuntime + MockDataPath
  StorageTargetResolver + Binding

Phase C  Local-NVMe correctness
  control/resource grant
  memory registration
  file target
  submit/completion/error

Phase D  Metadata 与 async
  MetadataArena / tiered cache / PRP pool
  per-operation lease
  stream/event completion
  kernel strategy benchmark

Phase E  Framework and compatibility
  VllmAdapter 或 LmCacheAdapter
  MACA/MUSA CUDA-like profile contract
  对应 local-NVMe interop 按真实需求验证

Phase F  Cleanup
  删除旧 Coordinator/IoEngine production targets
  删除重复 libnvm/service/kernel 树
  保留历史文档和性能基线
```

### 24.12 最终判断

当前 Tutti 不是一套需要被丢弃的实现，而是一组**边界放错位置但已有大量可复用数据面组件**的系统：

- local-NVMe 的控制面、用户态队列、内核模块和 GPU kernel 能映射到一个完整 `LocalNvmeDataPath`；
- 根 `memory/` 的 pool/cache 能映射到 Runtime memory identity、CUDA-like primitives 和 DataPath metadata 三个 owner；
- 旧 Coordinator/IoEngine 的上层意图能映射到 `StorageRuntime`，但代码本身需要重写；
- FIEMAP 和 KV adapter 能映射到 Resolver/Binding/FrameworkAdapter；
- CUDA 代码能自然映射到 NVIDIA-first CUDA-like API，MACA 等兼容厂商通过 profile/shim 扩展。

因此答案是：**架构映射成立，且适合渐进迁移；但成功条件是按 owner 拆分，而不是把当前目录一一重命名。** 第一条可运行垂直切片应是：

```text
Vllm/LmCache test adapter
  → StorageRuntime
  → MemoryRegistry + cuda_like(CUDA)
  → Ext4FiemapTargetResolver + binding
  → LocalNvmeDataPath
  → libnvm/NVMeService/snvme
  → file write/read/verify + real completion
```

该切片完成后，再以 CUDA-like contract 和 DataPath capability 接入 MACA 等兼容厂商；本轮不需要 rawdevice，也不需要先实现所有未来 feature。


R9	收口 R8：处理 S2 的 WRITE 假阳性、shutdown/in-flight、submit 不变量、progress timeout；审查并修正 S3 batch/PRP 结果
R10	完成 StorageRuntime → Resolver → DataPath 的真实 file-backed WRITE/READ 垂直链
R11	completion/error/watchdog、partial commit、mixed target、registration domain、多 controller 生命周期
R12	搬 main 的 operation-owned metadata、PRP cache/pool、backpressure、真正 multi-stream async
R13	搬首个真实 KV FrameworkAdapter，只调用 StorageRuntime；跑 layerwise overlap 端到端
R14	收敛 local-NVMe control/kernel 边界、libnvm/NVMeService/snvme 唯一事实源、UAPI/compat
R15	Mock/扩展 contract kit、HOST/CUDA feature matrix、无 SDK 污染验证
R16（可能）	删除旧 memory/、io_engine/、nvme_storage/、重复 backend，实现与性能基线最终验收