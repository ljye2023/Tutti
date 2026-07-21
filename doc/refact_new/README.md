# Tutti 重构 API 设计文档索引

> 完整的自底向上 API 设计分析，目标：5.15 kernel + CUDA

## 文档列表

本目录包含完整的 Tutti 重构 API 设计文档，共11个文件：

### 核心文档

| 文档 | 描述 | 状态 |
|---|---|---|
| **00-overview.md** | 重构目标、架构变化概览、文档索引 | ✅ 完成 |
| **01-missing-types.md** | 必须首先创建的共享类型（AccelStream、StorageTarget、VDevice 等） | ✅ 完成 |

### 分层 API 设计（自底向上）

| 文档 | 层次 | 描述 | 状态 |
|---|---|---|---|
| **02-layer0-abstraction.md** | Layer 0 | 宏层（TUTTI_DEVICE、TUTTI_LAUNCH_KERNEL、vendor dispatch） | ✅ 完成 |
| **03-layer1-accelerator-hal.md** | Layer 1 | IAccelerator 接口（替代 IMemorySubsystem 的通用部分） | ✅ 完成 |
| **04-layer2-device-manager.md** | Layer 2 | IVirtualNvme + VDevice + device-side queue mechanics | ✅ 完成 |
| **05-layer3-backends-spi.md** | Layer 3 | IBackendProvider 变更 + StorageTarget | ✅ 完成 |
| **06-layer4-io-engine.md** | Layer 4 | IIoEngine 变更 + backend-neutral input types | ✅ 完成 |
| **07-layer5-storage-interfaces.md** | Layer 5 | IBlockStorage 变更 + 新 IRawDevice 接口 | ✅ 完成 |

### 验证与实施

| 文档 | 描述 | 状态 |
|---|---|---|
| **08-validation.md** | 依赖规则验证 + 端到端调用流程追踪 | ✅ 完成 |
| **09-implementation-sequence.md** | 推荐实施顺序（Phase 0-7，预计4-5.5周） | ✅ 完成 |
| **10-open-questions.md** | 未决设计问题（13个问题，0个阻塞 v0.1） | ✅ 完成 |

---

## 快速导航

### 我是新手，从哪里开始？

1. **先读**: `00-overview.md` — 了解重构的目标和架构变化
2. **然后**: `01-missing-types.md` — 理解需要创建的共享类型
3. **最后**: `08-validation.md` 的"端到端调用流程"部分 — 看完整的 GPU-submit read 流程

### 我想了解特定层的设计

按照自底向上的顺序阅读：

```
Layer 0: 02-layer0-abstraction.md       （宏层）
         ↓
Layer 1: 03-layer1-accelerator-hal.md   （IAccelerator）
         ↓
Layer 2: 04-layer2-device-manager.md    （IVirtualNvme + VDevice）
         ↓
Layer 3: 05-layer3-backends-spi.md      （IBackendProvider SPI）
         ↓
Layer 4: 06-layer4-io-engine.md         （IIoEngine）
         ↓
Layer 5: 07-layer5-storage-interfaces.md（IBlockStorage + IRawDevice）
```

### 我想开始实施

1. **先读**: `09-implementation-sequence.md` — 了解推荐的实施顺序和时间线
2. **检查**: `10-open-questions.md` — 确认没有阻塞问题（目前0个阻塞 v0.1）
3. **参考**: `08-validation.md` — 每个阶段完成后用此文档验证

### 我想了解现有代码的问题

每个 layer 文档都包含：
- **Current State** 部分 — 现有代码的问题
- **CUDA Leak Fixes** 或 **Migration Checklist** — 需要修复的文件列表

---

## 关键发现摘要

### 缺失的类型（必须先创建）

| 类型 | 用途 | 建议头文件 |
|---|---|---|
| `AccelStream` / `AccelEvent` | 替换 `cudaStream_t`（opaque `void*`） | `tutti/accel/accel_types.h` |
| `StorageTarget` | Block Storage 和 Raw Device 的收敛类型 | `tutti/types/storage_target.h` |
| `VDevice` | Backend 从 DM 接收的 queue slice | `device_manager/include/vdevice.h` |
| `IVirtualNvme` | Level-2 分配器（进程内 QP 虚拟化） | `device_manager/include/virtual_nvme.h` |
| `SubSliceInfo` | IO-slice 描述符 | `tutti/types/io_types.h` |
| `IpcHandle` | 跨进程 IPC 导出令牌 | `tutti/accel/accel_types.h` |

### 主要架构变化

#### 1. Memory Layer 拆分

| 关注点 | 当前位置（v0.1） | 新位置 |
|---|---|---|
| 通用 alloc/register/DMA-map | `memory/` (IMemorySubsystem) | **Accelerator HAL** (IAccelerator) |
| PRP/SGL descriptor build | `memory/` (IMemorySubsystem) | **Backends / NVMe** |
| IO-slice fan-out | `memory/` (register_tensor) | **IO Engine** |

#### 2. Device Manager 反转

**v0.1**: DM 在 backends 之上，但包含 libnvm（反向依赖）

**新设计**: DM 是 backends **之下**的 local-NVMe 虚拟化基础层：
- 拥有物理控制器 bring-up 和 queue-pair 预算
- 给每个 backend 分发 **vDevice** (queue slice + namespace view + caps)
- Backends 向**下**拉取队列；没有东西在 DM 之上包含 libnvm

#### 3. StorageTarget 收敛

```
Block Storage (GPUFile) ──┐
                          ├──> StorageTarget ──> IO Engine ──> Backends
Raw Device (ns + LBA)  ───┘
```

两个顶层接口都产生 `StorageTarget`，IO Engine 保持 backend-neutral。

### CUDA 泄漏修复（8 个文件）

| 文件 | 问题 | 修复 |
|---|---|---|
| `io_engine/include/backend_provider.h` | `#include <cuda_runtime.h>` | 替换为 `AccelStream` |
| `io_engine/include/io_engine.h` | `cudaStream_t` 参数 | → `AccelStream` |
| `io_engine/include/local_nvme/launch_batch.h` | `cudaStream_t`, `cudaError_t` | → `AccelStream`, `bool` |
| `io_engine/include/local_nvme/local_nvme_io_engine.h` | override 签名中的 `cudaStream_t` | → `AccelStream` |
| `coordinator/include/coordinator.h` | 整个 IO API 中的 `cudaStream_t` | → `AccelStream` |
| `memory/include/gpu_slot_pool.h` | `cudaStream_t` / `cudaEvent_t` | → `AccelStream` / `AccelEvent` |
| `memory/include/host_slot_pool.h` | 直接调用 `cudaMallocHost` / `cudaFreeHost` | 迁入 HAL 实现层 |
| `memory/include/memory_subsystem.h` | `ensure_prp_pages_resident` 的 stream 参数 | 整个方法迁出 |

### 验证状态

| 验证项 | 状态 |
|---|---|
| 依赖规则强制执行 | ✅ 所有违规都有定义的修复方案 |
| DM 无热路径 API | ✅ 通过调用流程确认 |
| IO Engine backend-neutral | ✅ 通过调用流程确认 |
| HAL 边界干净 | ✅ `AccelStream` 全程 opaque |
| StorageTarget 收敛工作 | ✅ 两个顶层接口都产生它 |
| VDevice 包含 queue slice | ✅ 传递到 device kernels |
| 端到端流程映射到新 API | ✅ 所有16步已追踪 |
| 无循环依赖 | ✅ 严格自底向上顺序 |

### 实施时间线

**总计**: 约 21-27 天（4-5.5 周），单个工程师，无阻塞

| 阶段 | 工作量 | 依赖 |
|---|---|---|
| Phase 0: 基础类型 | 1 天 | 无 |
| Phase 1: Accelerator HAL | 3-4 天 | Phase 0 |
| Phase 2: Device Manager | 2-3 天 | Phase 1 |
| Phase 3: Backends | 5-6 天 | Phase 0, 1, 2 |
| Phase 4: IO Engine | 3-4 天 | Phase 1, 3 |
| Phase 5: Top Interfaces | 2-3 天 | Phase 3, 4 |
| Phase 6: Coordinator | 2-3 天 | 全部之前的 |
| Phase 7: Cutover + Validation | 3-4 天 | 全部之前的 |

### 未决问题

**13 个问题，0 个阻塞 v0.1**

所有阻塞问题都针对 post-v0.1 功能（SYCL、多 backend、超额订阅）。

---

## 设计原则

本次重构遵循以下原则：

1. **严格的自底向上依赖** — 每层只依赖其下层 + 共享名词
2. **HAL 边界不可违反** — `cuda_runtime.h` 不得出现在 HAL 之上
3. **Backend-neutral IO Engine** — 不依赖 NVMe-specific 类型
4. **DM 无热路径** — Device Manager 只在 bootstrap 调用，稳态 IO 期间空闲
5. **收敛类型优于重复** — `StorageTarget` 是两个顶层接口的共同语言
6. **明确优于隐式** — `dma_map()` 是独立调用，不在 `register_*()` 中隐藏

---

## 下一步行动

1. **审查设计文档** — 团队审查本目录中的所有文档
2. **解决未决问题** — 检查 `10-open-questions.md` 中标记为 DEFERRED 的项目是否需要在 v0.1 之前解决
3. **创建共享类型** — 从 `tutti/abstraction/accel.h` 和 `tutti/types/` 开始
4. **自底向上实施** — 遵循 `09-implementation-sequence.md` 中的顺序

---

**文档版本**: v1.0  
**创建日期**: 2026-07-21  
**目标**: Linux 5.15 kernel + CUDA (v0.1)  
**状态**: 设计完成，准备实施
