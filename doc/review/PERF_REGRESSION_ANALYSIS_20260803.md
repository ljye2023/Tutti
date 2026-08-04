# Tutti 重构性能退化分析（以 kv_cache_layerwise_overlap 为锚点）

**分析日期：** 2026-08-03
**范围：** 以 `third_pkgs/Tutti/examples/adapters/kv_cache_layerwise_overlap.cu`（老版本全部能力的代表测试）为锚点，逐环节对比老版本（`third_pkgs/Tutti/`）与当前重构（`tutti/`）在**元数据管理、内存注册、PRP cache 管理、元数据调度、IO 编排、存储数据布局**六个方面的差异及其性能影响。纯代码分析，未运行程序。
**前提：** 用户明确指出老版本性能已是最优，本次重构只应优化抽象，核心设计不应改动。因此本报告逐条标注"哪些差异是抽象等价迁移（可接受）"与"哪些差异改变了核心性能设计（退化，需对齐）"。

---

## 1. 结论摘要

当前重构在六方面的差异中，**四方面实质改动了老版本的核心性能设计**：

| 维度 | 老版本核心设计 | 新版本做法 | 判定 |
|---|---|---|---|
| 内存注册 / PRP 预构建 | **注册时预构建** GPU 常驻 `AddressDescriptor[]`，submit 零 PRP 计算 | `register_memory` 只做 DMA map；**每次 submit 现场算 PRP**（`classify_prp` + `fill_prp_list_page` + H2D） | **退化** |
| 元数据管理（handle cache） | **双层**：L2 host-pinned 模板 + L1 GPU 工作集，L1 eviction = downgrade（一次 memcpy 恢复） | **单层** GPU LRU，evict 即释放 GPU handle，miss 需完整重建（cudaMalloc+H2D） | **退化** |
| PRP cache 管理 | 双层（L2 内容 + L1 DMA working set），`scatter kernel` 一次 patch 所有 prp2 | 单层 content-addressed LRU，miss 时 H2D fill | **差异**（命中路径等价，miss 路径多 H2D） |
| 元数据调度 | GpuSlotPool/HostSlotPool + stream-fenced slot reuse + **batch promote**（一次连续 memcpy 覆盖整批） | `MetadataArena` 预分配 + acquire/release 槽位 | 基本等价（合理简化） |
| IO 编排 | `submit_batch` **阻塞单调用**（内部 `cudaStreamSynchronize`），batch 深度 8192，单 kernel 驱动全部 N 盘 | submit 非阻塞 + wait + release_io，每轮显式 `cudaStreamSynchronize`，in-flight=4 | **退化**（host 往返增加、深度下降） |
| 存储数据布局 | GpuFile 2 shard（K/V）× n_layers，**round-robin 跨 4 设备**，单 kernel 聚合带宽 | 单文件单设备（默认）；`--striped4` 才走 StripedDataPath | **退化**（默认单盘） |

**最大影响按序：** ① 布局（4 盘聚合 → 默认单盘）；② PRP 注册时预构建 → 每次现场构建；③ handle cache 双层 → 单层；④ IO 编排深度/往返。

---

## 2. 老版本 kv_cache_layerwise_overlap.cu 的性能机制（基线）

关键参数（`kv_cache_layerwise_overlap.cu:167-180`）：80 层、128K ctx、256 chunk → 512 chunks、90% hit（~461 hit + ~51 miss/层）、tensor=512 KiB、L1=512 MiB GPU、L2=2048 MiB host-pinned、`max_entries=8192`、三流 `s_read/s_compute/s_write`（hi/lo/hi 优先级）。

其性能链路：

```
open_gpu_files_batch ─ 批量建/打开 n_chunks 个 GpuFile（每文件 2 shard: K/V）
      │                  shard_placement round-robin 跨所有 device（{dev0,dev1},{dev2,dev3}…）
      ▼
register_tensor ────── build_io_slice_table：
      │                ① validate_alignment ② compute_io_slice_plan(MDTS fan-out)
      │                ③ validate dma ④ allocate+dma_map PRP buf ⑤ 校验
      │                ⑥ fill_address_descriptors  ←  PRP1/PRP2/LIST 内容全算好
      │                ⑦ upload_descriptors_to_gpu  ←  一次 cudaMalloc+cudaMemcpy
      │                ⑧ upload_prp_list_pages      ←  PRP 页内容一次上传 GPU
      │                ⑨ build_slice_views          ←  host 侧 slice 索引
      ▼
resolve_handles ────── handle_for_batch：一次扁平 acquire 全部 shard
      │                （get_or_build_batch：一次 host build + 一次 L2 写 +
      │                  一次 L1 cudaMemcpyAsync 连续段；L1 满时 LRU evict =
      │                  DOWNGRADE，L2 副本保留，下次 promote 只需一次 memcpy）
      ▼
batched_read/write ── submit_chunked（8192 entries 深度）→ submit_batch：
      │                build_nvme_batch = 纯指针算术（e.prp_entry = v.d_ios + sub）
      │                ensure_prp_pages_resident：L1 promote + scatter kernel patch prp2
      │                H2D NvmeBatchEntry 数组 → 单 kernel launch
      │                （kernel 内部 GPU 侧 resolve_lba + queue acquire + CQ poll）
      ▼
cudaStreamSynchronize（阻塞；三流由 cudaEvent DAG 实现 read(L+1)/compute(L)/write(L-1) 在飞）
```

**要点：** 老版本把"PRP 构建、handle 模板、PRP 页内容"三件重活全部**前移到注册/打开阶段**（一次成本），IO 热路径只剩"指针算术 + 数组 H2D + 单 kernel"。这是其性能最优的核心原因。

---

## 3. 六维差异逐项分析

### 3.1 内存注册（PRP 预构建）—— 退化，影响最大之一

**老版本（`third_pkgs/Tutti/memory/src/host_device_memory_subsystem.cu`）：**
- `register_tensor` → `build_io_slice_table` 分 9 个 stage（文件头注释 636-696 行）。
- Stage 6 `fill_address_descriptors`（828-893）：遍历 (slice, io) 网格，算出**每个 sub-IO 的 `AddressDescriptor{data_length, prp1, prp2}`**；LIST 路径同时把 `data_dma->ioaddrs[start_page+1 .. +pages_in_io-1]` 打包进 PRP-list 页内容。
- Stage 7 `upload_descriptors_to_gpu`：一次 `cudaMalloc` + `cudaMemcpy` 把整个 `AddressDescriptor[]` 放到 GPU 常驻。
- Stage 8 `upload_prp_list_pages`：PRP-list 页内容一次上传 GPU。
- 结果：**IO 时 `build_nvme_batch`（`host_batch_builder.cpp`）只是 `e.prp_entry = v.d_ios + sub` 指针算术**，`NvmeBatchEntry` 40 字节里存 GPU 地址而非值，零 PRP 计算、零 PRP 页 H2D。

**新版本（`tutti/data_paths/local_nvme/local_nvme_data_path.cpp`）：**
- `register_memory` 只做 `nvm_dma_map_data_device`（拿到 `ioaddrs[]`），**不预构建 PRP**。
- `submit()` 每个 request 现场：遍历 extents 求边界 → 算 `start_page` / `pages_in_io` → `classify_prp`（SINGLE/DUAL/LIST）→ `prp1 = mreg->dma->ioaddrs[start_page]` → LIST 时 `fill_prp_list_page`（构造 PRP 页）→ `cudaMemcpyAsync` H2D（PRP cache miss 时）。
- `DeviceSubmitEntry` 里存的是**值**（prp1/prp2），每次 submit 都要构造 + H2D。

**影响：** PRP 构建成本从"注册一次、GPU 常驻、热路径零成本"变成"每次 submit 现场计算 + H2D"。对 512 KiB tensor（MDTS 下 fan-out 8 entries），每层 read ≈ 461 hit × 2 (K/V) = 922 tensor × 8 = 7376 entries，每次 submit 都要在 host 重算所有 entry 的 PRP 并 H2D。虽然 PRP cache 能命中 LIST 页，但**首次/evict 后的成本、以及 host 侧计算量**都回到每次 IO 上。

### 3.2 元数据管理（handle cache）—— 退化

**老版本（`third_pkgs/Tutti/memory/include/tiered_handle_cache.h`）：**
- 两层：L2 host-pinned 大容量（默认 2 GiB / 192B/entry ≈ 16M slots）保存完整模板（含 FIEMAP/extent 结果）；L1 GPU 小容量（512 MiB）保存当前工作集。
- `get_or_build_batch`：冷子集**一次 host build + 一次连续 L2 写 + 一次连续 L1 cudaMemcpyAsync**（batch promote，不打散成逐文件 H2D）。
- L1 eviction = **DOWNGRADE**：L2 副本保留，evict 只释放 L1 slot；下次 promote 只需一次 memcpy 回来。
- `GpuSlotPool` stream-fenced slot reuse（`cudaEvent` 栅栏异步回收）。

**新版本（`tutti/data_paths/local_nvme/metadata/handle_workspace_cache.h`）：**
- 单层 GPU LRU + `open_refcount`（Round 16 P0-1 已修 reopen 悬空）。
- miss 时 `build_device_target`（`cudaMalloc` + H2D extent 表）；evict 时 `free_entry_gpu_`（**释放 GPU handle**），无 L2 兜底 → 再次访问需完整重建。
- 无 batch promote（逐 entry build）。

**影响：** 对 layerwise 场景（全部 chunk 文件 open 后长驻），稳态影响小；但**超容量/短生命周期文件场景**：老版本 L1 满了是"降级到 L2，回来一次 memcpy"；新版本是"释放，回来完整重建（cudaMalloc+H2D）"。二者差距在文件数 > cache 容量时放大。

### 3.3 PRP cache 管理 —— 差异（命中等价，miss 路径退化）

**老版本（`third_pkgs/Tutti/memory/include/prp_page_cache.h`）：**
- 两层：L2 host-pinned 保存 PRP 页**内容**（注册时一次 admit）；L1 GPU DMA 工作集。
- `ensure_resident_batch`：批量 promote（L1 满时 LRU evict，只降级 L2）；`prp2` 修改用**一个 scatter kernel 一次性 patch** 所有 changed prp2（绝不打散成多次 H2D）。
- event-fenced slot reuse。

**新版本（`tutti/data_paths/local_nvme/metadata/prp_page_cache.h`）：**
- 单层 content-addressed GPU LRU（固定 IOVA 池，无 prp2 patch 需求）。
- miss 时 `fill_prp_list_page` + `cudaMemcpyAsync` H2D 填充。
- 无 L2 内容备份。

**影响：** 命中时（PRP 页内容不变）与老版本 L1 命中等价，这是热路径稳态。差异在 miss 路径：老版本内容已在 GPU（注册时上传），新版本每次 miss 一次 H2D。对 layerwise（同一批 PRP 页反复用）命中率高，此维度影响相对小。

### 3.4 元数据调度（arena / slot pool）—— 基本等价（合理简化）

- 老版本：`GpuSlotPool`/`HostSlotPool` + stream-fenced slot reuse + batch promote。
- 新版本：`MetadataArena` 预分配 event/entries/status/PRP 池，submit acquire、完成 release；容量 = `2 * max_in_flight * max_batch_entries * mdts`。
- 判定：正常路径均零分配、GPU 内存复用，吞吐等价。**唯一缺失是 batch promote 的"一次连续 memcpy"**（已并入 3.2）。

### 3.5 IO 编排 —— 退化

**老版本：**
- `submit_batch`（`local_nvme_io_engine.cpp:133-144`）**阻塞**：`launch_async_locked` + `cudaStreamSynchronize(stream)`。一次调用 = 一个 8192 entries 的大 batch = 一次 kernel。
- 三流并行靠 cudaEvent DAG：`s_read`（L+1 预取）`/ s_compute`（L 计算）`/ s_write`（L-1 落盘），IO 由 kernel 内完成 + 阻塞等待。
- 每层每方向一次调用，host 往返 = O(layers×2)。

**新版本（`tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu:159-247`）：**
- `windowed_submit_wait`：每轮 `rt->submit` → `rt->wait` → `release_io` → **显式 `cudaStreamSynchronize(stream)`**（239 行）。
- `max_in_flight_operations=4`（290-323 行构造），`max_batch_entries=4096`（单盘）/8192（striped）。
- 注释明确：read/write 非同时在飞，overlap 主要来自 compute 与 IO 的交错（Option B 交错式）。

**影响：** ① 每层每方向从"1 次阻塞调用"变成"submit + wait + release + sync"4 次 host 往返；② IO 深度从 8192（单批全量）降到 in-flight=4 的窗口化推进；③ `wait` 无内部 `cudaStreamSynchronize`（R11 S3 设计），示例自己补了显式 sync，但**sync 在每一轮**，阻止了读与写的真正同时在飞。虽然结果正确、stream 顺序保持，但**编排深度与 host 开销都比老版本差**。

### 3.6 存储数据布局 —— 退化（默认单盘）

**老版本：**
- `open_gpu_files_batch` 每文件 2 shard（K/V）；`shard_placement` round-robin 跨**所有** device（`kv_cache_layerwise_overlap.cu:287-294` 要求 ≥2 设备）。
- 一次 `submit_batch` 的 entries 混合所有设备上的 shard，**单 kernel 驱动 N 台 NVMe 队列聚合带宽**。

**新版本：**
- 默认单文件单设备（`file://` + `/dev/ssnvme0`），单 kernel 只驱动 1 台 NVMe。
- `--striped4` 走 `StripedDataPath`（R15 S5 已实现单 kernel 融合 N 盘），但**示例默认未启用**，且 Runtime 单实例默认 `supports_multi_gpu=false`。

**影响：** 相同 workload 下，老版本默认 4 盘聚合，新版本默认 1 盘——**这是几何级的带宽差异来源**。`StripedDataPath` 存在可弥补，但默认配置下 layerwise 示例不复现老版本的多盘聚合语义。

---

## 4. 综合性能影响评估

以 layerwise 的稳态每层 read（≈461×2×512 KiB ≈ 472 MB）为例：

1. **布局**：老版本 472 MB 由 4 盘并行；新版本默认 1 盘 → **稳态带宽上限差 ~4×**（striped4 可回补）。
2. **PRP**：老版本 PRP 值在 GPU（注册一次）；新版本每次 submit host 重算 + H2D entries（7376 条）。对 512 KiB 粒度、每层 7376 entries，host 侧 PRP 计算 + 数组 H2D 是**每次 IO 的固定开销**，随 batch 数线性放大。
3. **handle cache**：layerwise 长驻场景影响小；容量敏感场景（文件 > cache）差距显著（重建 vs downgrade）。
4. **IO 编排**：每层 4 次 host 往返 + 每轮显式 sync → 重叠窗口变小；对深 pipeline（80 层三流在飞）有累积影响。

> 注意：以上是**静态代码差异推演**，未实测。若需精确数字，应按 §6 的对比方案用同一几何分别在老版本（多盘聚合）与新版本（单盘 / striped4）下跑 `kv_cache_layerwise_overlap` 与 `layerwise_kv_overlap` 计时对比。

---

## 5. 建议（按"对齐老版本核心设计"优先级）

**P0（必须对齐，否则核心性能设计被改动）：**
1. **PRP 预构建回归**：在 `register_memory`（或新增 `register_tensor` 语义）时预构建 GPU 常驻 `AddressDescriptor[]`（含 SINGLE/DUAL/LIST 与 PRP-list 页内容），`DeviceSubmitEntry` 改存 GPU 描述符指针而非值。这是老版本热路径零 PRP 计算的根基，也是"核心设计一丝一毫不能改"最直接的一条。
2. **默认多盘聚合语义**：`layerwise_kv_overlap` 默认启用 StripedDataPath（或至少让默认配置等价于老版本的多设备 round-robin shard），避免"4 盘 → 1 盘"的几何退化。

**P1（显著影响，建议对齐）：**
3. **handle cache 加 L2 兜底**：evict 时保留 host/L2 模板，re-promote 走 memcpy 而非完整重建；恢复 batch promote（一次连续 memcpy 覆盖整批冷 shard）。
4. **IO 编排去每轮显式 sync**：将示例的 windowed 循环改为"读/写各自主流 + event DAG"，恢复 read(L+1)/write(L-1) 真正同时在飞；in-flight 深度对齐老版本 batch 深度（非 4）。

**P2（差异可接受，但要文档注明不等价）：**
5. PRP cache 单层（命中路径等价）与 MetadataArena（等价）为合理简化，保留。
6. 在架构文档中明确列出"已等价迁移 / 已有意简化 / 与原版不等价"三张表，防止后续维护者误以为六维完全一致。

---

## 6. 附录：建议的量化对比方案（供后续执行）

同一台机器、同一几何（80 层 / 128K ctx / 256 chunk / 90% hit），分别：

- **老版本**：`third_pkgs/Tutti` 构建 + `kv_cache_layerwise_overlap`（多设备聚合，记录 read/write GB/s 与 time_io）。
- **新版本单盘**：`build/r15base` + `layerwise_kv_overlap`（无 `--striped4`）。
- **新版本 striped4**：`build/r15base` + `layerwise_kv_overlap --striped4`。

对比：read/write 带宽、time_io/层、host 往返次数（DpSeam 计数器）、kernel 数/层。此对比可把 §3 的每一条差异折算成可量化的吞吐/延迟损失。
