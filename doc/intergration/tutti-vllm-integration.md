# Tutti 接入 vLLM 设计文档

> 架构（D-005）：**外部 TuttiConnectorV1，零 vLLM 侵入**。
> 决策：`ai-orch/decisions/`（D-001 红线、D-002 存储契约、D-005 架构）。
> 操作手册：`tutti-vllm-manual.md`。编排：`ai-orch/` + `rounds/`。

---

## 1. tutti 旧版（legacy）接入 vLLM 复盘（既有资产）

### 1.1 总体架构：三层解耦

| 层 | 位置 | 职责 |
|---|---|---|
| vLLM fork（薄改） | `third_pkgs/tutti-legacy/vllm-fork` | KVConnector 挂点补丁约 8 文件 |
| Python adapter（厚） | `third_pkgs/tutti-legacy/engine/pkg/integration/vllm-connector/vllm_v1_adapter.py`（约 1300 行） | 全部 vLLM 侧调度：请求追踪、前缀命中、slot 映射、逐层 IO 编排 |
| C++/CUDA 引擎 | `legacy-tutti/csrc/tutti 前身` | NVMe 队列、GPU P2P DMA、GPU-direct IO kernel |

核心：**调度逻辑不改 vLLM，全部实现在 KVConnectorBase_V1 回调里**。

### 1.2 vLLM fork 侧修改清单（历史参考，新版全部原生具备）

1. `legacy-tutti_connector.py`：64KB 对齐（`_align_num_blocks_for_legacy-tutti`）、
   `prefer_cross_layer_blocks`、`requires_piecewise_for_cudagraph`。
2. `factory.py` 注册；`kv_transfer_utils.py` 的 `maybe_transfer_kv_layer`
   装饰器；跨层 KV buffer（`allocate_uniform_kv_caches`）。
3. 启动：`kv_transfer_config={"kv_connector": "LegacyTuttiConnector",
   "kv_role": "kv_both"}`，block_size=128=chunk_size，环境变量配置。

### 1.3 存储文件组织

- 一个 KV chunk（chunk_size token × 全部 layer × K+V）= 一个 GPUFile；
  每 GPUFile 最多 4 盘各建一个物理文件条带化，per 盘大小对齐 64KB。
- 文件描述符 GPUFileDesc{file_id, 单层 block_size, tensor_shape[3],
  CompactNVMeMapping(17B)}，GPU-resident。

### 1.4 索引设计

- 逻辑索引（CPU/Python）：token chunk 哈希 → CacheEngineKey →
  GPUFileId 分配 + LRU evictor；
- 跨进程查询：scheduler 经 ZMQ/Redis lookup server 查 worker 的索引
  （`get_num_new_matched_tokens` 用）；
- 物理索引（GPU/C++）：CompactNVMeMapping 内联，kernel 直接算盘上
  offset，不回 CPU。

### 1.5 读写接口与调度时机

**Scheduler 进程（每调度步）**：
```
get_num_new_matched_tokens()   # lookup 查外部命中 → LoadSpec
update_state_after_alloc()     # 分配 block 后 can_load=True
build_connector_meta()         # ReqMeta{token_ids, slot_mapping, block_ids, save/load_spec}
                               # slot_mapping = block_id*block_size + offset
```
`ReqMeta.from_request_tracker()` 负责 chunk 边界对齐
（discard_partial_chunks）、skip_leading_tokens 去重、slot_mapping 展开。

**Worker 进程（每次 forward）**：
```
start_load_kv()                # prepare_for_load/save + 预取第 0 层 + IO 预算
每层 attention 前:  wait_for_layer_load(layer)   # retrieve_layer(L+1) 逐层预取
每层 attention 后:  save_kv_layer(layer)          # store_layer(L) 逐层落盘
forward 出口:       wait_for_save()               # no-op（stream 异步）
```
- overlap：StreamController（green context SM 分区）切 IO stream
  （load 16 SM / save 8 SM），与计算 stream 并行；逐层 prefetch 重叠
  第 N+1 层读取与第 N 层计算。
- IO 任务：`IOSpec{gpu_file, block_id, layer}` 逐层递增；请求释放时
  withdraw_tasks 回收。

### 1.6 Debug 与 nsys 标记

- Python NVTX：`_tutti_legacy_nvtx_annotate` 装饰器（domain="legacy-tutti"，
  函数名 hash 分色）铺满 adapter/engine；区间 `nvtx.start_range`。
- C++ 侧无 NVTX（靠 kernel 名区分）；独立 logger + observability 统计。

---

## 2. Tutti 与 tutti 的接入面对应关系

| 概念 | tutti 前身 | Tutti |
|---|---|---|
| 引擎入口 | legacy-tutti_engine（get/put chunk） | `StorageRuntime`（open/register_memory/submit/wait） |
| GPU 显存注册 | register_kv_caches → DMA 注册 | `register_memory(MemoryView{DEVICE})` |
| 一次性下发 | 多次 per-layer engine 调用 | **`submit(IoRequest[], DEVICE_EXECUTION, stream)` 一次调用 = 一次 fused kernel launch** |
| 文件/条带 | GPUFile + 4 盘自动条带 | URI：`file://`（FIEMAP 一次）、`striped://`（N 盘融合 kernel） |
| partial commit | 无显式契约 | `IoSubmitOutcome.initial_states` 逐项；**被拒项必须窗口化重提交** |
| 环境依赖 | 内核模块随引擎 | 内核模块 → tutti_daemon → mount 三步；daemon 仅资源 broker |

关键 gap：tutti 无 Python 绑定 → T-101 补（唯一新 C++ 面）。

---

## 3. 新版 vLLM 现状（接入可行性依据）

1. `maybe_transfer_kv_layer` 原生存在（`kv_transfer_utils.py:15-61`），
   已挂 unified attention——逐层 hook 零改动可用。
2. 跨层 KV buffer 机制原生存在（`allocate_uniform_kv_caches` 等）。
3. **外部包零侵入注册**：`kv_connector_module_path` 直接 import 外部
   connector 类（`factory.py:105-114`，LMCache 同款）——本方案核心依据。
4. 官方参考实现齐全（LMCache/Nixl/OffloadingConnector 等）。
5. HMA：connector 不支持时需 `--disable-hybrid-kv-cache-manager`。
6. 无 legacy-tutti/legacy-tutti/tutti 残留，代码干净。

---

## 4. TuttiConnectorV1 设计（当前权威）

### 4.1 组件与仓库位置（全在 tutti 仓库）

```
integration/vllm-connector/bindings/python/
                                # tutti_runtime 包：pybind 封装
                                #   make_{stub,local_nvme,striped_nvme}_runtime
                                #   open_batch / register_memory / submit / wait
integration/vllm-connector/engine/
  core.py         # TuttiEngine 唯一引擎（计划态 + 逐层执行态 + 环形窗口）
  chunk_index.py  # chunk hash → 文件路径 key + LRU + pin（纯 stdlib）
  backend.py      # StorageBackend SPI（chunk_paths/bind_staging/段搬运）
  memory_backend.py # MemoryBackend（无盘可测）
integration/vllm-connector/tests/
                  # 按测试需求分类（唯一测试落点，见 tests/README.md）：
                  # contract(读写接口契约) / unit(纯逻辑) / adapter /
                  # kernels / bindings / perf / scale / overlap
integration/vllm-connector/adapter/
  vllm_adapter.py # TuttiConnectorV1(KVConnectorBase_V1) 双角色壳
                  # + scheduler 侧：RequestTracker/ReqMeta/slot_mapping
                  # （平移 legacy tutti）+ engine 计划态进程内前缀命中
  worker.py       # register_kv_caches → engine.bind；逐层 load/store 编排
  nvtx_utils.py
integration/vllm-connector/kernels/
                  # tutti_kv_kernels：gather/scatter（LMCache 平移，T-117）
# LocalStoreBackend（T-115）：engine/local_store_backend.py
```

### 4.2 配置

```python
kv_transfer_config = {
    "kv_connector": "TuttiConnectorV1",
    "kv_connector_module_path": "adapter.connector",
    "kv_role": "kv_both",
    "extra_config": {"capacity_bytes": ...,   # 必需；硬件无关
                     "blocks_per_chunk": 0}}  # 0=自动（segment≥256KiB 对齐）
```

### 4.3 存储布局与对齐契约（D-002 摘要，以 ARCHITECTURE.md §0a 为准）
- chunk 身份 = 文件路径 key（opaque，backend 自产自解释）；文件
  布局 = packed（层段 offset = layer_idx × segment_bytes）。
- 池化/聚合/fd 收敛/建池校验：LocalStoreBackend 私有自决，不进接口
  （若做预分配：零实写，禁 fallocate——FIEMAP fail-closed）。
- 注册：staging 环形窗口是唯一注册对象（基址 64KiB 对齐，
  io_granularity=segment_bytes 预建描述符）；IO 的
  memory_offset/length/target_offset 4KiB 对齐；单请求 ≤
  caps().max_single_io_bytes（默认 32MiB）。
- submit 前 `io_stream.wait_stream(gather stream)`；被拒请求
  内部窗口重发，对上层恒 True；每层 CUDA event。

### 4.3a staging gather/scatter 内核（D-006：复用 LMCache 资产）

legacy 引擎即 LMCache fork 改名版，其 staging kernel 被砍只剩 DMA；
当前设计恰好接回这条路径，内核资产现成：

| LMCache 资产 | 位置 | 复用方式 |
|---|---|---|
| `single_layer_kv_transfer` | `csrc/cuda/mem_kernels.cu:1012`（模板特化 :1131） | **T-117 已平移**：单层 packed ↔ paged（slot_mapping），16 种 EngineKVFormat 全覆盖 |
| `multi_layer_kv_transfer` | 同文件 :904（+`_fused_ptr` :869） | 跨层 `[2,L,T,H]` ↔ 每层独立 paged tensor（`key_value_ptrs[L]`——正是 vllm register_kv_caches 形态） |
| `load_and_reshape_flash`/`reshape_and_cache_back_flash` | :1160/:1209 | "load and reshape" 本体，语义参照 |
| `EngineKVFormat` + `FormatFacts` | `csrc/engine_kv_format.h:36-292` | paged 布局描述（16 种，编译期谓词） |
| `normalize_kv_and_discover_format` | `lmcache/v1/gpu_connector/utils.py:186` | register_kv_caches 时运行时布局探测 |
| `TensorMemoryObj` 池管理 | `lmcache/v1/memory_management.py:635+` | 槽管理参照（ref_count/pin/unpin） |

平移方式：拷 `mem_kernels.cu` 相关部分 + `engine_kv_format.h` 至
`kernels/`（独立包 tutti_kv_kernels）自建最小 torch extension——
不依赖 lmcache pip 包（Apache-2.0，保留版权头）。
staging 槽布局与 LMCache `KV_2LTD`（[2,L,T,H]）同构，kernel 参数化
适配。engine 的 gather_fn/scatter_fn 钩子即注入点（T-117 已交付
single_layer 平移内核，集成接线在后续卡完成）。

### 4.4 调用时序

**Scheduler**：
```
get_num_new_matched_tokens → engine.lookup_prefix（进程内，无 RPC）
update_state_after_alloc → RequestTracker 记录 + can_load
build_connector_meta → ReqMeta（slot_mapping 手算，平移 legacy tutti）
                       + engine.plan_load/plan_store（chunk_id 路径 opaque 透传）
```

**Worker**：
```
register_kv_caches     # engine.bind（staging 是唯一注册对象，注册在 backend 内部）
start_load_kv          # engine.load_layer(plan, 0)
wait_for_layer_load(L) # 等 L 层句柄；engine.load_layer(plan, L+1)；末层 complete_load
save_kv_layer(L)       # engine.store_layer(plan, L)（与计算并行）
wait_for_save          # no-op
get_finished           # complete_store/complete_load 收割
```

**Overlap 语义**（与 legacy-tutti 对齐）：load 层间预取 + save 逐层落盘，
IO stream 与计算 stream 并行——这是选择外部 connector 路线的核心收益。

### 4.5 与官方 Offloading 体系的关系

刻意不依赖 vllm 的 OffloadingSpec/Manager/Worker 抽象：完整实现
KVConnectorBase_V1 回调，索引/驱逐自管（engine/chunk_index.py）。理由：
逐层 hook 粒度 + 不受 experimental API 演进影响（D-005 第 2 条）。

---

## 5. 架构契合度分析（tutti 现状 vs vLLM 需求）

### 5.1 模型多样性：MoE / Mamba / 混合架构 — 满足

- tutti 是 opaque-byte 引擎（无 reshape/paged 逻辑，IO kernel 只做
  PRP+doorbell），结构知识全在 vLLM 侧。
- MoE 不改变 KV 形状；Mamba/GDN state 由 connector 的 kv_caches
  dict 收集（按 layer 独立 tensor 注册即可，`MambaSpec` 页同样走
  4KiB 对齐约束，非 4KiB 倍数由 vllm 的 page_size_padded 处理或跳过）。
- 唯一约束：各 group page_size_bytes(padded) % 4096 == 0。

### 5.2 GPU-NVMe 拓扑与带宽

实测拓扑：2 NUMA × 4 PCIe switch，每 switch 1 GPU(x16)+1 NVMe(x4)。
即每 GPU 直连 1 盘（distance 0），NUMA 内其余 3 盘 distance 1，
跨 NUMA distance 2。

- **推荐**：每 NUMA 4 盘组 striped 池，同 NUMA 4 GPU 各起独立
  TuttiRuntime open 同一批池文件——全部 IO NUMA 内闭合，零跨
  NUMA P2P。chunk 路径空间按 rank 分区（无锁），或共享 canonical
  去重（二期）。
- 带宽模型（分级）：单盘直连 ~7GB/s；单 GPU NUMA 4 盘突发 ~28GB/s；
  全机 8 盘聚合峰值 50+GB/s（保守一半 20+GB/s）；稳态 8 GPU 均分
  ~7GB/s/GPU
  （GPU x16 上行 ~32GB/s 封顶）；瓶颈在盘数（8×Gen4x4 ≈ 56-60GB/s）。
- NUMA0 四盘尚未 daemon bring-up（用前补三步）；跨 GPU 不互访 KV
  （TP 分片语义 + tutti stream 归属校验），共享池无正确性风险。

### 5.3 条带化与持久化

- `striped://` 文件级静态条带（unit + rot），物理盘映射
  `((offset/unit)+rot) % N` 隐式完成，vLLM 侧只见 opaque chunk 路径。
- 数据持久（文件应用层创建，重启可读）；**索引不持久**（R1 接受
  重启冷缓存；V2 加 sidecar 索引 dump/load）。

### 5.4 内存注册：staging 是唯一注册对象

- 契约（实证）：注册基址 64KiB 硬性（`local_nvme_data_path.cpp:985`）；
  IO memory_offset/length 4KiB（`submit_one.cuh:38-43`）。
- 落地：staging 环形窗口一次注册（`io_granularity=segment_bytes`
  预建描述符，全部 IO 走 pre-built 路径）；vllm paged 池**不注册**——
  数据进出只经 gather/scatter kernel（HBM 域 μs 级），IO 的 GPU 端
  恒为 staging 内偏移。小 block（16/32）下的粒度碎片、对齐非法、
  预建描述符失效问题由此全部消解（详见 ARCHITECTURE.md §2b）。

### 5.5 容量与 fd 收敛（LocalStoreBackend 私有自决）

- 一 chunk 一文件在真实容量下是十万级 fd——fd 收敛（聚合大文件）
  是真实需求，但属 backend 内部实现自决（ARCHITECTURE.md §0a）：
  不进接口、不进路径语义、不进 engine/adapter。
- 硬约束（若做预分配）：禁 fallocate（FIEMAP fail-closed 拒
  UNWRITTEN，须实写零）；单文件物理 extent ≤ 124
  （kMaxTotalExtents），实写大文件通常几个~几十个，可控。

### 5.6 md/dm RAID0 直通（未立项，结论存档）

md-raid0 上 FIEMAP 只给 md 逻辑偏移（ext4 的 m_pblk 是所在块设备
块号；raid0 的 bio remap 在 ext4 之下的块层，OpenCloudOS 5.4.241
raid0.c `map_sector` 证实）。可行路径 = snvme 块设备（保留 gendisk）
组 md0 + tutti 新增 md_raid0 resolver 复刻映射（同构盘退化为线性
公式，参数从 /sys 运行时读）。**收益**：单一 ext4 大文件随便放 +
模型权重挂载零修改聚合加载；**代价**：一个新 resolver + 部署纪律。
D-001 红线：本轮不做，如立项另出决策。

---

## 6. 调用路径图（外部 connector 版）

```
┌────────────────────────── Scheduler 进程 ──────────────────────────┐
│ vllm/v1/core/sched/scheduler.py（官方）                             │
│   get_num_new_matched_tokens / update_state_after_alloc /           │
│   build_connector_meta                                             │
│                 │ 官方 KVConnectorBase_V1 扩展点                    │
│                 v                                                  │
│ ★ adapter.connector.TuttiConnectorV1(SCHEDULER)                   │
│   → vllm_adapter.py（scheduler 侧）                                │
│     engine 计划态：lookup_prefix / plan_load / plan_store           │
│     （进程内，无 RPC；chunk_id 路径 opaque 透传）                   │
│       ReqMeta{token_ids, slot_mapping, block_ids, load/save_spec}   │
└──────────────────┬──────────────────────────────────────────────────┘
                   │ KVConnectorMetadata（官方 IPC）
┌──────────────────v─────────── Worker 进程（每 GPU）─────────────────┐
│ vllm gpu_model_runner（官方）+ maybe_transfer_kv_layer 装饰器       │
│   forward 前 start_load_kv；每层前 wait_for_layer_load；            │
│   每层后 save_kv_layer；出口 wait_for_save                          │
│                 │                                                   │
│                 v                                                   │
│ ★ adapter.connector.TuttiConnectorV1(WORKER)                         │
│   → worker.py WorkerImpl（纯编排翻译 + 在途上限背压）               │
│     → TuttiEngine 执行态：环形窗口 staging + gather/scatter         │
│       （_kernels，LMCache 平移）                                    │
│       → StorageBackend（LocalStoreBackend：路径自产自解释、          │
│         固定粒度 submit、partial-commit 重发；池化/fd 收敛私有）     │
│                 │ pybind                                            │
│                 v                                                   │
│ ★ tutti_runtime._core → tutti/presets make_*_runtime（只读链接）    │
│   StorageRuntime.submit(reqs, DEVICE_EXECUTION, io_stream)          │
│     → 按 DataPath 分组 → 1×H2D 描述符 → 1 次 fused kernel launch    │
│       IO kernel @ io_stream ∥ 计算 stream ← overlap                 │
└──────────────────┬──────────────────────────────────────────────────┘
                   │ PCIe P2P GPU-direct DMA（不经 host）
                   v
     NUMA 内 4 盘 striped 池（文件布局=packed；聚合/fd 收敛 backend 自决）
```

★ = 新增代码（全在 tutti 仓库）；vllm fork 零改动。

---

## 7. 环境与操作

见 `tutti-vllm-manual.md`（tutti-env 完全隔离环境 / env-tutti.sh /
snvme bring-up 三步 / 冒烟与 bench 流程 / FAQ）。
