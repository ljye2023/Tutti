# Main 分支典型 IO 路径与重构搬运映射

分析对象：`main@4862157d50c8a7004cdeb166dda630ab1ef4561a`

入口：`examples/adapters/kv_cache_layerwise_overlap.cu`

本文只回答两个问题：

1. main 分支中已跑通的典型 KV-cache IO 从上到下到底经过什么代码；
2. 这些行为在当前重构里应该搬到哪里。

**工作原则：分层抽象 + 代码搬运。不要评审、重写或“修好” main 的既有实现。** 只有新接口必需的字段/生命周期适配才允许变化。

---

## 1. 一句话调用链

```text
kv_cache_layerwise_overlap.cu
  → KvCacheIoAdapter::batched_{read,write}
  → submit_chunked
  → Coordinator::submit_batch
  → LocalNvmeIoEngine::submit_batch
  → build_nvme_batch
  → ensure_prp_pages_resident
  → cudaMemcpyAsync(NvmeBatchEntry[])
  → launch_nvme_batch_xfer
  → nvme_batch_xfer_kernel
  → gpu_file_resolve
  → submit_{read,write}_one
  → resolve_lba
  → NvmeQueueGroup::d_qps / QueueAcquireHelper
  → SQE + doorbell + CQ poll
```

辅助链：

```text
文件：Coordinator → HostFsBackedBlockStorage → HostFsBackedNvmeStorage
      → fallocate/fsync/read_extents(FIEMAP) → NvmeFile/GpuFile

内存：Coordinator → HostDeviceMemorySubsystem
      → cudaMalloc(64KiB-aligned exposed pointer)
      → nvm_dma_map_data_device
      → IoSliceTable / AddressDescriptor / PRP page

句柄：GpuFileId → Coordinator::handle_for_batch
      → block_storage::acquire_device_handles_batch
      → nvme_storage::acquire_device_handles_batch
      → GPU-resident NvmeFileDeviceHandle + GpuFileHandle

控制面：Coordinator SERVICE_CLIENT
      → NvmeServiceBackedRegistry
      → nvm_ctrl_attach_client
      → NvmeQueueGroup (nvm_create_group + nvm_add_user_queue)
```

---

## 2. 示例层：几何、文件、内存、三流 overlap

### 2.1 Bootstrap

`examples/adapters/kv_cache_layerwise_overlap.cu:223-240`：

```cpp
CoordinatorConfig cfg;
cfg.mode = SERVICE_CLIENT;  // 或 IN_PROCESS
cfg.daemon_endpoint = ...;
cfg.daemon_device_ids = ...;
cfg.cuda_device = ...;
cfg.num_user_queues_per_device = ...;
cfg.max_entries_per_batch = ...;
cfg.handle_l1_gpu_budget_bytes = ...;
cfg.handle_l2_host_budget_bytes = ...;
Coordinator coord;
coord.bootstrap(cfg);
```

### 2.2 文件

`:248-317`：按 chunk 创建/复用 `GpuFileId`。每个逻辑 GpuFile 的总大小：

```text
2 * n_layers * tensor_size   // standard K + V
```

文件通过 `open_gpu_files_batch()` 创建/打开，随后 hit/miss 分为两个 `GpuFileId[]`。

### 2.3 内存

`:319-341`：每个 chunk 分配 K/V 两块 GPU memory：

```cpp
coord.allocate_device(tensor_size, DEVICE, cuda_device);
coord.register_tensor({ptr, size=tensor_size, granularity=tensor_size});
```

`granularity=tensor_size` 的意义：预先把一个 tensor fan-out 成 MDTS/PRP sub-IO descriptor 表，供 hot path 直接使用。

### 2.4 三条 stream

`:346-355`：read/write stream 使用高优先级，compute 独立 stream。

`:479-522` 的每层调度：

```text
compute(L)  等待 read(L) event，在 s_compute 异步 launch
write(L-1) 等待 compute(L-1) event，在 s_write 提交 miss KV
read(L+1)  在 s_read 预取下一层 hit KV，完成后 record ev_read[L+1]
```

adapter 的 submit 是 host-blocking，但 compute kernel 已在另一个 stream 上运行，因此 host 阻塞等待 IO 时，GPU compute 与 IO kernel 仍可并发。

最后 `sync_file()` 先同步 stream，再对每个 shard `fsync()`（`Coordinator::sync_file`, `coordinator/src/coordinator.cu:460-490`）。

---

## 3. Adapter：framework 语义降成通用 batch

`adapters/kv_cache/src/kv_cache_io_adapter.cpp`。

### 3.1 Layer offset

`:102-143`：standard K/V：

```text
base  = layer_idx * tensor_size * 2
K off = base
V off = base + tensor_size
```

每个 block 产出两条：

```cpp
NvmeBatchInputTensor{k_region, file_handle, k_off}
NvmeBatchInputTensor{v_region, file_handle, v_off}
```

MLA（`:146-179`）：每层单 tensor：

```text
offset = layer_idx * tensor_size
```

### 3.2 Chunk packing

`submit_chunked`, `:28-80`：按 `coord.batch_entry_count(region)` 取得每个 tensor 会展开出的 descriptor 数，贪心打包，使 flattened entry count 不超过 engine scratch capacity；每个 chunk 调一次 `Coordinator::submit_batch()`。

### 3.3 句柄解析

`resolve_handles`, `:182-220`：`GpuFileId[]` 经 `Coordinator::handle_for_batch` 批量解析成 GPU-visible `GpuFileHandle*[]`，解析和后续 IO 使用**同一 stream**，保持 H2D 与 kernel 的 stream 顺序。

### 新架构归属

```text
FrameworkAdapter：保留 layer/KV offset、block/chunk packing、request-id 生命周期
StorageRuntime：接受 IoRequest[] + HostSubmitContext，拥有 handles/op
LocalNvmeDataPath：不认识 K/V、layer、chunk token
```

---

## 4. Coordinator：旧 assembly root

`coordinator/src/coordinator.cu`。

### 4.1 Bootstrap 顺序

文件开头注释与 `:115-228` 明确：

```text
registry → nvme_storage → block_storage → memory → io_engine
```

SERVICE_CLIENT：

- `NvmeServiceBackedRegistry`
- 每设备请求 `build_queue_group=true`
- attach client fd + queue group

然后：

```cpp
storage_ = HostFsBackedNvmeStorage; storage_->bootstrap(devices)
block_   = HostFsBackedBlockStorage; block_->bootstrap(storage, devices)
block_->configure_handle_pool(L1, L2)
mem_     = HostDeviceMemorySubsystem; mem_->bind_devices(devices)
engine_  = LocalNvmeIoEngine(mem, max_entries)
```

### 4.2 Submit

`:505-523`：纯转发：

```cpp
return engine_->submit_batch(inputs, is_read, stream);
```

### 新架构归属

Coordinator 不保留独立层：

```text
StorageRuntime：assembly root、公开 handles、target/memory/io registry、submit/query/wait
LocalNvmeDataPath：controller/queue/target/DMA/descriptor/kernel/completion
FrameworkAdapter：KV 语义
```

---

## 5. 文件路径：ext4 → FIEMAP → GPU handle

### 5.1 `HostFsBackedBlockStorage`

`block_storage/src/host_fs_backed_block_storage.cpp:588-735`：

- `GpuFileSpec` 通过 `tensor_shape[0]` 决定 shard 数；
- flatten 每个 GpuFile 的 shard spec；
- 调 `nvme_storage::open_files_batch`；
- 记录 `GpuFile{total_size, tensor_size, shards[]}`。

### 5.2 `HostFsBackedNvmeStorage`

`nvme_storage/src/host_fs_backed_nvme_storage.cpp` create path：

```text
open → fallocate → fsync → read_extents(FIEMAP)
→ .tutti/.refs hardlink → metadata log
```

这部分已搬到：

```text
tutti/resolvers/local_file/LocalFileResolver
  → Ext4LocalNvmePayload
  → ResolvedTarget
```

### 5.3 Device handle

`nvme_storage/include/nvme_file_device_handle.h`：

```text
NvmeFileDeviceHandle
  file_id / logical_size / block_size / namespace_id
  inline extents[8] + overflow pointer
  d_qps + num_d_qps
```

`host_fs_backed_nvme_storage_device.cu`：把 host metadata 搬到 GPU cache/slot，extent >8 时额外 cudaMalloc overflow；绑定 `NvmeQueueGroup::d_qps`。

新架构应搬入 `LocalNvmeDataPath` 私有 target owner，不进入 public/SPI header。

---

## 6. 内存路径：allocation → DMA map → descriptor

本节只给概要。完整 allocation owner、DMA mapping、SINGLE/DUAL/LIST、PRP cache/fallback、submit 消费顺序与废旧代码排除清单见：

`MAIN_MEMORY_PRP_PATH.md`

`memory/src/host_device_memory_subsystem.cu`。

### 6.1 Allocation

`:360-389`：GPU allocation 多分配 64KiB，向上对齐 exposed pointer（snvme DMA map 偏好 64KiB）。

### 6.2 Registration

`:502-530` + `:553-635`：

```text
register_tensor
  → locate MemoryRegion
  → ensure_mapping_locked
  → nvm_dma_map_data_device/host
  → build_io_slice_table_locked(granularity)
```

DMA registration 已搬到 `LocalNvmeDataPath::register_memory()`（Round 7）。

### 6.3 Descriptor planning

`build_io_slice_table_locked`（`:1309+`）及 helper：

- 按 granularity/MDTS fan-out；
- 从 `nvm_dma_t::ioaddrs[]` 生成 `AddressDescriptor{prp1,prp2,data_length}`；
- SINGLE / DUAL 直接地址；LIST 需要 DMA-visible PRP-list page；
- 上传 descriptor table 到 GPU；
- `ensure_prp_pages_resident` 在 submit stream 上把 hot PRP page 提升到 GPU-DMA L1，并 patch `prp2`。

新架构归属：`LocalNvmeDataPath::MemReg` 私有 metadata/descriptor owner；Runtime 只见 `DataPathMemory`。

---

## 7. Queue owner

`device_manager/include/nvme_queue_group.h` + `src/nvme_queue_group.cu`：

```text
borrowed nvm_ctrl_t*
  → nvm_create_group
  → N × QueuePair + ring GPU allocations
  → nvm_dma_map_ring_device
  → nvm_add_user_queue
  → BAR0 doorbell host VA → GPU VA
  → cudaMemcpy QueuePair[] 到 d_qps
```

析构顺序：

```text
nvm_destroy_group → cudaFree(d_qps) → delete host QueuePair[]
```

controller 由外层 owner 持有，必须在 `NvmeQueueGroup` 析构**之后** `nvm_ctrl_free_client()`。

新架构归属：`LocalNvmeDataPath/io` 私有 RAII owner。

---

## 8. IO engine：host lowering → GPU kernel

### 8.1 `LocalNvmeIoEngine`

`io_engine/src/local_nvme/local_nvme_io_engine.cpp`：

```text
build_nvme_batch
→ ensure_prp_pages_resident(stream)
→ cudaMemcpyAsync NvmeBatchEntry[] 到单个 d_scratch
→ launch_nvme_batch_xfer(stream)
→ blocking submit 再 cudaStreamSynchronize(stream)
```

### 8.2 `build_nvme_batch`

`io_engine/src/local_nvme/host_batch_builder.cpp`：

每个 `(tensor_region, file_handle, file_offset)`：

- 取该 region 的 `IoSliceView[]`；
- 每个 sub-IO 产一个 `NvmeBatchEntry`；
- entry 持有 `d_shards_dev`、`num_shards`、`tensor_size`、`AddressDescriptor*`、`prp_idx`、`file_offset`、direction。

### 8.3 GPU kernel

`io_engine/src/local_nvme/nvme_batch_xfer_kernel.cu`：

每 thread 一 entry：

```text
gpu_file_resolve(tensor_size, num_shards, file_offset)
→ shard + shard-local base
→ file_off = base + prp_idx * sub_io
→ submit_read_one / submit_write_one
```

`nvme_storage_device.cuh`：

```text
resolve_lba(NvmeFileDeviceHandle extents)
→ QueueAcquireHelper::acquire_queue
→ issue_nvme_cmd(SQE + doorbell)
→ poll(CQ, cid)
```

main 的 kernel 内部已经同步 poll 真正 completion；kernel 结束即该 thread 的 storage IO 已完成。

新架构归属：全部搬进 `LocalNvmeDataPath/io`，`DataPathOp` 持有每批独立的 descriptor/entry/event 资源，终态后 release。

---

## 9. Round 8 搬运顺序

严格串行：

```text
Session 1：搬 NvmeQueueGroup + GPU target handle
Session 2：搬单块 submit kernel + DataPathOp/query/progress/release，做真实 4KiB write/read/verify
Session 3：搬 main 的 batch/MDTS fan-out + per-op entry/descriptor owner，做批量多 offset write/read/verify
```

为什么先单块：先证明 `ResolvedTarget + DataPathMemory + d_qps + SQE/CQ` 的最短闭环，再搬批量/PRP 优化，便于定位错误。它不是新设计，而是从同一条 main 路径中取最小 slice。

为什么 Session 3 才搬 batch：main 的典型路径包含 `IoSliceTable`、PRP cache、scratch、chunk packing。一次全搬会把 queue、target、DMA、descriptor、kernel、op lifecycle 的故障混在一起。

---

## 10. 不搬的东西

- 不恢复 `Coordinator`、旧 `block_storage`、旧 `MemoryRegion` public API；
- 不把 K/V/layer/tensor 名词放进 Runtime/DataPath；
- 不把 PRP/SQE/queue/libnvm 类型放进 public 或 SPI header；
- 不评审或改写 main 的 queue/kernel 策略；
- 不实现 raw-device public API；
- 不做第二 DataPath、RDMA/GDS 或新 filesystem。
