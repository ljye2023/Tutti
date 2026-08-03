# Main 典型 IO 的内存分配、DMA 注册与 PRP-list 准备路径

分析基线：`main@4862157d50c8a7004cdeb166dda630ab1ef4561a`

顶层入口：`examples/adapters/kv_cache_layerwise_overlap.cu`

配套总链路：`MAIN_IO_PATH.md`

本文只固化 main 典型示例**实际可达**的内存与 PRP 行为，供后续 session 搬运。工作原则仍是：

> 分层抽象 + 代码搬运。不要借此评审、修正或重新设计 main 的既有实现。

---

## 1. 权威源与阅读纪律

### 1.1 权威顺序

后续 session 必须按以下顺序判断实现来源：

1. 固定 commit 上的实际源码：`git show 4862157d50c8a7004cdeb166dda630ab1ef4561a:<path>`；
2. 本文与 `MAIN_IO_PATH.md` 的调用链说明；
3. 当前 `refact` 工作树只用于查看已经搬到哪里，**不能反推 main 的旧行为**。

不要通过“类名看起来相似”来选实现。本仓库同时存在多套 `memory`、`io_engine`、`backend` 与 PRP 代码。

### 1.2 本文的 canonical source 集合

内存与 PRP：

```text
main:memory/include/memory_region.h
main:memory/include/memory_subsystem.h
main:memory/include/host_device_memory_subsystem.h
main:memory/include/prp_list_pool.h
main:memory/include/prp_page_cache.h
main:memory/src/host_device_memory_subsystem.cu
```

典型 IO 消费端：

```text
main:io_engine/include/local_nvme/nvme_batch.h
main:io_engine/src/local_nvme/host_batch_builder.cpp
main:io_engine/src/local_nvme/local_nvme_io_engine.cpp
main:io_engine/src/local_nvme/nvme_batch_xfer_kernel.cu
```

示例与装配：

```text
main:examples/adapters/kv_cache_layerwise_overlap.cu
main:coordinator/src/coordinator.cu
main:coordinator/include/coordinator_config.h
```

`main:memory/CMakeLists.txt` 证明 `tutti_memory` 的实现源只有 `src/host_device_memory_subsystem.cu`；`main:io_engine/CMakeLists.txt` 证明典型 Local-NVMe engine 由 `host_batch_builder.cpp`、`local_nvme_io_engine.cpp`、`nvme_batch_xfer_kernel.cu` 三个文件组成。

---

## 2. 一句话链路

```text
kv_cache_layerwise_overlap Phase B
  → Coordinator::allocate_device
  → HostDeviceMemorySubsystem::allocate_device
  → cudaMalloc(size + 64 KiB)
  → 暴露 64 KiB 对齐的 device_ptr，保留 raw allocation owner

  → Coordinator::register_tensor
  → HostDeviceMemorySubsystem::register_tensor
  → ensure_mapping_locked
  → nvm_dma_map_data_device
  → nvm_dma_t::ioaddrs[]
  → build_io_slice_table_locked
  → MDTS fan-out
  → AddressDescriptor[]
  → SINGLE / DUAL / LIST PRP
  → descriptor H2D
  → PRP page cache L2 admit，或 owned always-resident fallback

submit hot path
  → build_nvme_batch
  → ensure_prp_pages_resident(same stream)
  → patch descriptor.prp2
  → NvmeBatchEntry[] H2D
  → nvme_batch_xfer_kernel
  → submit_{read,write}_one(prp1, prp2, length)
```

---

## 3. 典型 layerwise 示例的真实几何

`main:examples/adapters/kv_cache_layerwise_overlap.cu:163-180` 的默认值：

```text
layers          = 80
context tokens  = 131072
chunk tokens    = 256
chunks          = 512
tensor_size     = 512 KiB，每个 chunk/layer 的 K 或 V
max entries     = 8192
```

Phase B（`:319-341`）为每个 chunk 分配 K/V 两块 GPU memory：

```cpp
coord.allocate_device(tensor_size, DEVICE, cuda_device);
coord.register_tensor({ptr, size=tensor_size, granularity=tensor_size});
```

因此默认共有：

```text
512 chunks × 2(K/V) = 1024 个 512 KiB registered regions
```

每个 region 只有一个 business slice（`granularity == size == 512 KiB`），但会按 cluster 最小 MDTS fan-out：

```text
effective_io  = min(512 KiB, cluster_min_mdts)
ios_per_slice = ceil(512 KiB / effective_io)
```

例如：

```text
min MDTS 128 KiB → 每个 K/V region 4 个 AddressDescriptor
min MDTS  64 KiB → 每个 K/V region 8 个 AddressDescriptor
```

只要 `effective_io > 2 * 4 KiB`，每个 sub-IO 都走 PRP LIST。

### 容易混淆的两个预算

示例的 `--l1-mib=512` / `--l2-mib=2048` 写入的是：

```text
handle_l1_gpu_budget_bytes
handle_l2_host_budget_bytes
```

它们是 **file/device-handle cache 预算，不是 PRP page cache 预算**。

PRP cache 沿用 `CoordinatorConfig` 默认值：

```text
prp_l1_gpu_budget_bytes  = 64 MiB
prp_l2_host_budget_bytes = 1 GiB
```

来源：`main:coordinator/include/coordinator_config.h:171-172`。

---

## 4. 分配阶段：GPU allocation 与 owner

来源：`main:memory/src/host_device_memory_subsystem.cu:360-388`。

DEVICE allocation 原样行为：

1. `cudaSetDevice(device_id)`；
2. `cudaMalloc(&raw, size + 65536)`；
3. 将 exposed `device_ptr` 向上对齐到 64 KiB；
4. `MemoryRegion` 对外保存 aligned `device_ptr` 与逻辑 `size`；
5. 私有 `Slot::raw_device_ptr` 保存 allocator 返回的 `raw` owner。

必须区分：

```text
raw_device_ptr = cudaMalloc 返回值，只能它交给 cudaFree
region.device_ptr = 64 KiB 对齐后的可用 view，用于 DMA map 和用户访问
```

这是搬运时必须保留的 owner/view 分离，不应只保存 aligned pointer。

`MemoryRegion` 在旧层是公开句柄；新架构不恢复该 API。对应语义应进入 `LocalNvmeDataPath` 的私有 memory-registration owner，公开层仍只见 `DataPathMemory`。

---

## 5. 注册阶段：一个 data DMA mapping

来源：

- `register_tensor`：`host_device_memory_subsystem.cu:553-642`
- `ensure_mapping_locked`：`:502-550`

`register_tensor` 的顺序：

1. 按 `spec.ptr` 查找已有 `MemoryRegion`；
2. 若没有，使用 `cudaPointerGetAttributes` 分类并注册 host/device pointer；
3. `ensure_mapping_locked` 保证 data buffer 已 DMA-map；
4. `granularity > 0` 时构造 IO-slice/descriptor/PRP table；
5. 已有 table 时幂等返回，不按新 granularity 重建。

DEVICE memory 的实际 mapping：

```cpp
nvm_dma_map_data_device(&dma, ctrl, region.device_ptr, region.size);
```

HOST memory 对应：

```cpp
nvm_dma_map_data_host(&dma, ctrl, region.host_ptr, region.size);
```

旧实现只保存一个 `Slot::data_dma`。它选择 `bound_devices_.front()` 的 controller 作为 ioctl proxy；该部署路径假设单次 ioctl 已处理所有 open controller，`nvm_dma_t::ioaddrs[]` 可被 cluster-bound controller 共用。

后续 PRP 构造的地址来源必须是：

```text
nvm_dma_t::ioaddrs[]
```

不能把 CUDA virtual address 直接写进 NVMe `PRP1/PRP2`。

Round 7 已把这一层的核心搬到 `LocalNvmeDataPath::register_memory()`；Round 8 后续只应扩展该 registration 的私有 metadata/owner，不应恢复旧 `MemoryRegion` public API。

---

## 6. IO-slice 与 MDTS plan

来源：`host_device_memory_subsystem.cu:698-760`。

Host-only plan：

```cpp
struct IoSliceBuildPlan {
    page_size;
    bytes_per_slice;
    effective_io;
    pages_per_io;
    ios_per_slice;
    num_slices;
    total_ios;
    needs_prp_list;
};
```

计算公式必须原样保持：

```text
bytes_per_slice = granularity == 0 ? region_size : granularity
effective_io    = min(bytes_per_slice, cluster_min_mdts)
pages_per_io    = ceil(effective_io / page_size)
ios_per_slice   = ceil(bytes_per_slice / effective_io)
num_slices      = region_size / bytes_per_slice
total_ios       = num_slices * ios_per_slice
needs_prp_list  = pages_per_io > 2
```

`cluster_min_mdts` 是所有 bound devices 的 `max_io_bytes` 最小值；page size 要求所有 bound devices 一致。来源：`bind_devices`, `:155-202`。

对齐约束：

```text
ptr % page_size == 0
size % page_size == 0
granularity == 0 或 granularity % page_size == 0
size % granularity == 0（granularity > 0 时）
```

典型路径的 page size 是 4 KiB。

---

## 7. 三类 AddressDescriptor

值类型来源：`main:memory/include/memory_subsystem.h:93-98`。

```cpp
struct AddressDescriptor {
    uint64_t prp1;
    uint64_t prp2;
    uint64_t data_length;
};
```

填充来源：`host_device_memory_subsystem.cu:828-893`。

每个 sub-IO：

```text
io_byte_off = slice_index * bytes_per_slice
            + sub_io_index * effective_io
start_page  = io_byte_off / page_size
prp1        = data_dma->ioaddrs[start_page]
data_length = 本 sub-IO 实际字节数
```

三种分支：

| sub-IO 覆盖页数 | `prp1` | `prp2` |
| --- | --- | --- |
| 1（SINGLE） | data page 0 IOVA | `0` |
| 2（DUAL） | data page 0 IOVA | data page 1 IOVA |
| >2（LIST） | data page 0 IOVA | PRP-list page 的 DMA IOVA |

LIST page 内容：

```text
entry[0] = data_dma->ioaddrs[start_page + 1]
entry[1] = data_dma->ioaddrs[start_page + 2]
...
entry[N] = 最后一个 data page IOVA
其余 entry = 0
```

每个 LIST sub-IO 预留一个完整 `page_size` 的 PRP-list page。host build buffer 布局：

```text
PRP page for io 0
PRP page for io 1
...
PRP page for io total_ios-1
```

### 必须保留的地址语义

```text
AddressDescriptor.prp1 = data DMA IOVA
AddressDescriptor.prp2 = 第二 data page IOVA，或 PRP-list page DMA IOVA
```

`prp2` 绝不是：

```text
CUDA virtual pointer
host pointer
AddressDescriptor* 本身
```

---

## 8. Descriptor blob 与 IoSliceView

来源：

- descriptor H2D：`host_device_memory_subsystem.cu:895-923`
- view build：`:945-969`
- 值类型：`memory_subsystem.h:197-214`

注册时：

```text
host AddressDescriptor[total_ios]
  → cudaMalloc(d_all_descriptors)
  → cudaMemcpy H2D
```

然后建立 host-side、按 `slice_addr` 排序的 `IoSliceView[]`：

```cpp
struct IoSliceView {
    uint64_t slice_addr;
    uint32_t num_ios;
    uint64_t total_bytes;
    const AddressDescriptor* d_ios;  // GPU pointer
};
```

其中：

```text
views[g].d_ios = d_all_descriptors + g * ios_per_slice
```

`d_ios` 是 GPU pointer。host 只做地址运算，不解引用其中的 descriptor。

---

## 9. PRP-list preferred path：L2 admit，submit 时 L1 promote

### 9.1 Pool 初始化

来源：

- `bind_devices`: `host_device_memory_subsystem.cu:204-224`
- `PrpListPool`: `memory/include/prp_list_pool.h`
- pool 实现：`host_device_memory_subsystem.cu:1070-1231`

一个 `PrpPageCache` 拥有：

```text
L1：单个 GPU allocation + 单次 nvm_dma_map_data_device
    每 slot = 一个 PRP page，controller 可 DMA 读取

L2：单个 cudaMallocHost allocation
    每 slot = 一个 PRP page 的持久 backing content
```

默认：

```text
L1 = 64 MiB
L2 = 1 GiB
slot_bytes = NVMe page_size（典型 4 KiB）
```

L1 allocation 额外多分配 64 KiB并手工对齐，mapping size 向 64 KiB 取整，然后用 `nvm_dma_map_data_device` 建立 DMA IOVA。

cache 初始化失败不是致命错误；需要 LIST 的 tensor 走 owned fallback。

### 9.2 Register-time L2 admit

来源：

- `build_io_slice_table_locked`: `:1378-1454`
- `PrpPageCache::admit`: `prp_page_cache.h:178-202`

cache path 构造 descriptor 时：

```text
prp1 = data page IOVA
prp2 = 0（L1 slot 尚未分配）
```

先上传 `d_all_descriptors`，取得每个 descriptor 中 `prp2` 字段的稳定 GPU 地址：

```text
key = &d_all_descriptors[io_idx].prp2
```

每个 PRP page content 被复制进一个 L2 pinned slot，并以该 key 建立 cache entry。此阶段不分配 L1 slot、不 patch `prp2`。

若 L2 在一半处耗尽：回滚本 tensor 已 admit 的 key，然后整个 tensor 改走 owned fallback。

### 9.3 Submit-time L1 promote + `prp2` patch

来源：

- `HostDeviceMemorySubsystem::ensure_prp_pages_resident`: `host_device_memory_subsystem.cu:235-264`
- `PrpPageCache::ensure_resident_batch`: `prp_page_cache.h:205-312`
- scatter patch kernel：`host_device_memory_subsystem.cu:1233-1262`

旧 IO engine 在 kernel launch 前，对 batch 中每个不重复 region 收集：

```text
&d_all_descriptors[0].prp2
...
&d_all_descriptors[total_descriptors-1].prp2
```

注意：这是 main 当前行为——它收集每个 region 的**全部 descriptor**，不是只收集本次 entry 引用的子集。搬运时不要擅自改变。

批量 ensure：

1. batch working set 必须能同时放进 L1；
2. 已 resident → L1 hit；
3. 未 resident → 必要时驱逐不属于本 batch protect-set 的 LRU page；
4. 若复用旧 L1 slot，先在当前 stream 等待该 slot 上次 touch stream 的 event；
5. `cudaMemcpyAsync(L2 → L1, same stream)`；
6. 从 `PrpListPool::l1_ioaddr(slot)` 取得新 DMA IOVA；
7. 一次 scatter kernel 把变化后的 IOVA 写入各 descriptor 的 `prp2`；
8. 所有 promote 与 patch 都排在同一 submit stream 的 NVMe kernel 之前。

L1 slot 的 GPU pointer 只是拷贝目的地；真正写入 `prp2` 的仍是 `l1_dma_->ioaddrs[]` 中的 IOVA。

---

## 10. PRP-list owned fallback

来源：

- `dma_alloc_device_data`: `host_device_memory_subsystem.cu:971-1058`
- fallback 分支：`:1453-1496`

触发条件：

```text
PRP cache 未初始化
或
为本 tensor admit L2 page 时耗尽
```

原样步骤：

1. `user_bytes = total_ios * page_size`；
2. mapping size 向 64 KiB 取整；
3. `cudaMalloc(raw, aligned_bytes + 64 KiB)`；
4. 从 raw 中取 64 KiB aligned view；
5. `nvm_dma_map_data_device(&prp_dma, ctrl, aligned_view, aligned_bytes)`；
6. 校验 `prp_dma->page_size == NVMe page_size` 且 `n_ioaddrs >= total_ios`；
7. 重新填 descriptor：
   `descriptor[io].prp2 = prp_dma->ioaddrs[io]`；
8. descriptor blob 重新 H2D；
9. PRP page contents H2D 到 aligned view；
10. table 持有 `prp_dma`、aligned view、raw allocation 与 aligned bytes。

### 后续 session 必须避免的简化错误

不能只写：

```cpp
cudaMalloc(prp_pages, total_ios * 4096);
nvm_dma_map_data_device(..., prp_pages, total_ios * 4096);
```

main 的 fallback 明确要求：

```text
64 KiB aligned GPU vaddr
64 KiB multiple mapping size
raw owner 与 aligned view 分离
```

释放顺序：

```text
nvm_dma_unmap(prp_dma)
→ cudaFree(raw allocation)
→ cudaFree(d_all_descriptors)
```

不能 `cudaFree(aligned view)`。

---

## 11. Submit 如何消费 descriptor/PRP

### 11.1 Host batch lowering

来源：`main:io_engine/src/local_nvme/host_batch_builder.cpp:21-129`。

对每个 input tensor：

1. `list_io_slices(region)`；
2. 遍历所有 `IoSliceView`；
3. 每个 `view.d_ios[sub]` 产一个 `NvmeBatchEntry`；
4. `prp_idx` 在整个 tensor 内单调递增，不在每个 slice 归零。

核心 entry：

```text
shards      = GPU-resident file target table
num_shards
tensor_size
prp_entry   = view.d_ios + sub       // GPU AddressDescriptor pointer
prp_idx     = tensor-global sub-IO index
file_offset = tensor base byte offset
is_read
```

### 11.2 Launch ordering

来源：`main:io_engine/src/local_nvme/local_nvme_io_engine.cpp:61-122`。

同一 stream 上严格排序：

```text
build_nvme_batch(host)
→ ensure_prp_pages_resident(stream)
→ cudaMemcpyAsync(NvmeBatchEntry[] H2D, stream)
→ launch_nvme_batch_xfer(stream)
```

cache path 的 `prp2` patch 因此先于 kernel 读取 descriptor。owned fallback/no-list 路径的 ensure 是 no-op。

### 11.3 Kernel 消费

来源：`main:io_engine/src/local_nvme/nvme_batch_xfer_kernel.cu:43-118`。

每 thread 处理一个 entry：

```text
sub_io = prp_entry->data_length
file_off = shard-local base + prp_idx * sub_io
submit_read_one / submit_write_one(
    target,
    prp_entry->prp1,
    prp_entry->prp2,
    file_off,
    sub_io)
```

`submit_*_one` 内部完成 target extent → LBA、SQE、doorbell、CQ poll。

---

## 12. 生命周期与 owner 总表

| 资源 | 创建 | owner | 使用期 | 释放 |
| --- | --- | --- | --- | --- |
| data raw GPU allocation | `allocate_device` | memory slot / 新 `MemReg` owner | region lifetime | `cudaFree(raw)` |
| aligned data view | raw 内偏移 | borrowed view | data DMA + kernel | 不单独 free |
| data `nvm_dma_t` | `ensure_mapping_locked` | memory slot / 新 `MemReg` | registration lifetime | `nvm_dma_unmap` |
| descriptor GPU blob | `build_io_slice_table_locked` | `IoSliceTable` / 新 private descriptor owner | registration 或 op lifetime | `cudaFree` |
| PRP cache L2 slot | register-time `admit` | `PrpPageCache` | registration lifetime | `erase` |
| PRP cache L1 slot | submit-time promote | `PrpPageCache` | batch working-set residency | event-fenced LRU release |
| owned PRP raw allocation | fallback | `IoSliceTable` / per-op owner | registration 或 op lifetime | unmap 后 `cudaFree(raw)` |
| owned PRP aligned view | raw 内偏移 | borrowed view | PRP DMA mapping | 不单独 free |
| owned PRP `nvm_dma_t` | fallback map | same owner | descriptor usable期 | `nvm_dma_unmap` |
| per-batch entry array | engine scratch / 新 per-op workspace | IO engine / `DataPathOp` | kernel + completion | terminal release |

旧层 unregister 顺序：

```text
PRP cache entries 或 owned PRP mapping/buffer
→ descriptor GPU blob
→ data nvm_dma mapping
→ underlying allocation（若 runtime-owned）
```

新架构的 close/unregister/release 必须额外服从 in-flight op 引用，不得在 kernel/completion 前回收这些资源。

---

## 13. 已有测试证据应如何使用

### 13.1 `memory/test/memory_smoke.cu`

这是 descriptor/PRP prepare 的直接测试源：

```text
1 MiB DEVICE region
granularity = 128 KiB
num_slices = 8
每 slice 1 个 128 KiB IO（测试环境 MDTS 128 KiB）
32 个 4 KiB data pages → LIST path
```

它验证：

```text
register_tensor 幂等
IoSliceView 数量/地址/num_ios
descriptor.prp1 == data_dma.ioaddrs[0]
descriptor.data_length == 128 KiB
descriptor.prp2 != 0（测试运行环境中 LIST path 已准备）
非法 alignment/granularity 被拒绝
```

**证据边界：** 该测试在读回 `prp2` 前没有显式调用 `ensure_prp_pages_resident()`，因此它不能单独证明两级 cache 的 L2→L1 promote/patch；非零 `prp2` 也可能来自 owned fallback。不要把这一条扩大解释成 cache path 已覆盖。

### 13.2 `examples/io_engine/io_engine_host_smoke.cu`

用于核对 host lowering 的 entry 数、`prp_idx`、file offset 与 shard 公式。它不能代替真实 NVMe completion 证据。

### 13.3 `examples/io_engine/io_engine_smoke.cu`

真实 1 MiB tensor / 128 KiB granularity / PRP LIST 的 read-write-read roundtrip，覆盖：

```text
register_tensor
→ descriptor/PRP prepare
→ build_nvme_batch
→ ensure_prp_pages_resident（engine 内部调用）
→ NVMe kernel
→ byte verify
```

它证明「无论 cache path 还是 owned fallback，submit 前 descriptor/PRP 已可被 controller 正确消费」，但未输出 `PrpPageCache::Stats`，不能单独区分实际走了哪条 backing path。

### 13.4 `examples/adapters/kv_cache_layerwise_overlap.cu`

这是性能与层级语义的最终典型路径。它证明 K/V geometry、chunk packing、三 stream overlap 如何使用上面同一套 memory/PRP 结构；不要从 adapter 反向把 K/V 类型带进 DataPath。

---

## 14. 明确排除：不要把这些当作典型路径来源

### 14.1 `tutti/backends/nvme/src/nvme_submission.cpp`

当前文件的 `submit_batch_cpu_sync()` 明写 `placeholder stub`，没有发 NVMe 命令，却返回：

```text
success=true
completed_count=n_descs
```

不能作为 submit、completion 或 PRP 行为来源。

### 14.2 `tutti/backends/nvme/device/submit_batch_kernel.cu`

当前 kernel 有：

```text
TODO: poll completion
TODO: report error
For now, assume synchronous completion
```

它不是 layerwise example 的已跑通 kernel。权威 kernel 是：

```text
main:io_engine/src/local_nvme/nvme_batch_xfer_kernel.cu
```

### 14.3 `tutti/backends/nvme/include/prp_page_cache.h` 与对应 `.cpp`

这是另一套 backend package 中的并行实现，不在 `kv_cache_layerwise_overlap` 的 canonical build/link 链上。即使类名相似，也不要与：

```text
main:memory/include/prp_list_pool.h
main:memory/include/prp_page_cache.h
```

混搭。

### 14.4 `tutti/io_engine/**`

它是当前仓库中的另一套/过渡 IO engine。典型 main 示例链接的是顶层：

```text
main:io_engine/src/local_nvme/host_batch_builder.cpp
main:io_engine/src/local_nvme/local_nvme_io_engine.cpp
main:io_engine/src/local_nvme/nvme_batch_xfer_kernel.cu
```

### 14.5 `IMemorySubsystem::descriptor_slice()`

`main:memory/include/memory_subsystem.h:382-387` 和实现明确说明它是 ad-hoc slice 的 v0.1 stub。

典型路径使用：

```text
register_tensor(granularity > 0)
→ list_io_slices / lookup_io_slice
```

不要调用或搬运 `descriptor_slice()` 来实现 Round 8 PRP。

### 14.6 `BackendProvider::submit_batch_cpu_sync`

它是 parallel backend interface，不是 layerwise example 的实际 submit 链。不要因为名字含 `submit_batch` 就绕开 canonical `LocalNvmeIoEngine`。

### 14.7 libnvm legacy `createDma` / `nvm_dma_map_device`

main 的 data buffer 和 owned PRP fallback明确使用 B6 DATA ABI：

```text
nvm_dma_map_data_device
nvm_dma_map_data_host
```

`createDma` 的 legacy/UNSPECIFIED mapping 不是本路径的数据/PRP source。

### 14.8 `Coordinator::cache_stats()`

`kv_cache_e2e_stress.cu` 读取的类型是 `INvmeStorage::CacheStats`，统计的是 NVMe file/device-handle cache（cold build、L1/L2 handle promote/evict），**不是** `PrpPageCache::Stats`。不要把这些 promotion/eviction 数字当作 PRP page cache 证据。

### 14.9 `build/` 中的旧二进制与 `.o.d`

构建目录可能残留已经从当前工作树删除的源对应物。它只能证明历史上编过，不能作为源码依据。

---

## 15. 搬到新 `LocalNvmeDataPath` 的边界

### 已完成

```text
Round 7：DataPathMemory + nvm_dma_map_data_{device,host}
Round 8 S1：NvmeQueueGroup + GPU device target handle
Round 8 S2：最小 4 KiB submit/completion（执行中）
```

### Round 8 S3 应搬

```text
private AddressDescriptor
MDTS + target extent + memory page fan-out
SINGLE / DUAL / LIST fill 公式
owned PRP-list fallback 的 64 KiB alignment + DATA DMA map
per-op entry/descriptor/PRP owner
same-stream H2D + kernel + event ordering
```

当前 SPI 没有旧 `granularity` 注册参数，因此 S3 prompt 允许在 submit 时为请求构造 per-op descriptor。这是新接口所必需的机械适配；但以下行为不能变：

```text
IOVA 来源
SINGLE/DUAL/LIST 公式
MDTS 边界
PRP page content
64 KiB mapping 约束
owner/free 顺序
same-stream ordering
```

### 本轮不搬

Round 8 S3 明确先搬 main 的 **owned fallback**，不搬 `PrpPageCache` 两级缓存。两级 cache 是同一 canonical 路径中的后续性能 owner，可另开 session 搬；不能用 `tutti/backends/nvme/prp_page_cache` 替代。

### Public/SPI 边界

以下类型必须保持 `LocalNvmeDataPath` 私有：

```text
nvm_dma_t
AddressDescriptor
PRP page/pool/cache
QueuePair
NvmeBatchEntry
CUDA event/workspace
```

`StorageRuntime` 与 public SPI 只见：

```text
DataPathMemory
DataPathTarget
DataPathRequest
DataPathOp
DataPathSnapshot
```

---

## 16. 后续 session 的最小 source→port 对照要求

任何实现 PRP/batch 的结果文件至少要填：

| main source symbol | 新 private symbol | 允许的机械适配 |
| --- | --- | --- |
| `TensorRegistrationSpec/granularity` | request/op lowering 参数 | SPI 没有 granularity，改为 submit-time plan |
| `IoSliceBuildPlan` | per-request/per-op fan-out plan | 加入 target extent 边界 |
| `AddressDescriptor` | private PRP descriptor | 命名空间/owner 改动 |
| `fill_address_descriptors` | PRP builder | 输入从 `MemReg.dma` 取得 |
| `dma_alloc_device_data` | owned PRP buffer owner | per-op lifetime，公式与 alignment 不变 |
| `NvmeBatchEntry` | private per-sub-IO entry | 加 target/request index/direction |
| `build_nvme_batch` | `DataPathRequest[]` lowering | 适配 SPI identity/bounds/partial commit |
| `nvme_batch_xfer_kernel` | DataPath private kernel | 使用 S1/S2 private target/submit helper |

若 port 与 main 行为不同，理由只能是：

```text
新 SPI 所必需
per-op 生命周期所必需
```

不能写：

```text
旧实现不够好
我重新设计了更安全/更快的方案
另一个同名 backend 看起来更方便
```
