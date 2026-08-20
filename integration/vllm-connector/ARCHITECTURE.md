# Tutti vLLM Connector 架构

> 状态：定稿（2026-08-19，D-010）。权威文档。
> 结构对齐 LMCache/legacy 的验证过的四层：adapter / engine /
> storage_backend / 传输 kernel。

---

## 0. 术语纪律

| 层 | 允许的词汇 | 禁止词汇 |
|---|---|---|
| adapter（integration/vllm-connector/adapter/） | chunk/chunk_key/chunk_id(opaque)/layer/KVBlock | extent、shard、NVMe、PRP、FIEMAP、文件、盘、DMA |
| engine 公开面 + 通用组件（engine/chunk_index、kernels） | chunk、staging 槽、完成句柄、布局（EngineKVFormat/packed） | 同上 |
| backend 实现内（local_store_backend.py 及其私有模块） | 全部 | — |

检验标准：换存储后端 = 新增一个 backend 实现，engine 及以上零改动。

## 0a. 接口纯净原则（最高优先级）

1. **adapter / engine / SPI 只携带 chunk / 路径 / 层段 / 完成句柄
   语义**——绝对兼容、可移植、可适配任意存储。介质与布局优化只许
   出现在 backend 实现内部。
2. **路径 opaque**：`chunk_id` 是 backend 生成并解释的字符串
   （`StorageBackend.chunk_paths()`），上层只透传，从不解析。
3. **tutti C++ 职责边界**：StorageRuntime = URI→open/submit/wait 的
   IO runtime；空间分配是策略，留在 Python backend。
4. **session 纪律（每个 session 必读）**：领卡后先核对任务卡与
   本文档（§0/§0a/§3）是否一致——发现越层抽象、介质词上溢、过时
   路径或与已定稿决策冲突，**立即停止，不许照做**，写回执说明
   冲突点上抛。任务卡与本文档冲突时以本文档为准。
5. **测试收编**：全部测试按需求分类收编在 `tests/`（contract /
   unit / adapter / kernels / bindings / perf / scale / overlap，
   见 tests/README.md）。新功能往既有类别补用例，**不建 per-任务 /
   per-round 测试目录**。

## 1. 分层总图

```
┌──────────────────────────────────────────────────────────────────────┐
│ vLLM（零改动；KVConnectorBase_V1 回调驱动一切）                       │
└──────────────┬──────────────────────────────┬────────────────────────┘
               ▼ scheduler 进程               ▼ worker 进程（每 GPU）
┌──────────────────────────────────────────────────────────────────────┐
│ ★ adapter（integration/vllm-connector/adapter/）——纯 vllm 翻译      │
│   vllm_adapter.py  TuttiConnectorV1 双角色壳 + scheduler 侧           │
│                 （RequestTracker/ReqMeta/slot_mapping + vllm 策略：     │
│                  min_retrieve_tokens / max_tokens_per_load）           │
│   worker.py     逐层编排翻译 + 在途句柄上限（背压，T-114）             │
└──────────────┬───────────────────────────────────────────────────────┘
               ▼ TuttiEngine（引擎接口：chunk 计划态 + 逐层执行态）
┌──────────────────────────────────────────────────────────────────────┐
│ ★ engine/（integration/vllm-connector/engine/）——KV 语义引擎           │
│   （框架无关、介质无关：只见 chunk/路径/层段语义）                     │
│                                                                      │
│   core.py        TuttiEngine（唯一引擎：计划态/逐层时序/环形窗口/     │
│                  背压）                                               │
│   chunk_index.py 通用：chunk_key→chunk_id 索引 + LRU + pin            │
│   （gather/scatter 布局适配 = kernels/ 独立包 tutti_kv_kernels，      │
│    paged↔packed，16 种引擎布局→1 种 packed；LMCache 平移）            │
│                 ┌──────────────────────────────────────────────┐     │
│                 │ backend.py  StorageBackend SPI（唯一存储契约）  │     │
│                 │   write_chunk_segment / read_chunk_segment /  │     │
│                 │   bind_staging / shutdown（§3）                │     │
│                 └──────────┬───────────────────────────────┬───┘     │
└────────────────────────────┼───────────────────────────────┼─────────┘
                             ▼                               ▼
        ┌────────────────────────────────┐  ┌────────────────────────────┐
        │ MemoryBackend（memory_backend） │  │ LocalStoreBackend           │
        │ staging 槽 ↔ GPU 内存 chunk 池  │  │ （local_store_backend）     │
        │ chunk_id := 内存槽号             │  │ staging 槽 ↔ chunk 文件层段│
        │                                │  │ chunk_id := 文件路径 key    │
        │                                │  │ tutti_runtime submit        │
        │                                │  │（io_granularity 预建 PRP）  │
        └────────────────────────────────┘  └──────────────┬─────────────┘
                                                （未来：RemoteBackend 等
                                                 ——同 SPI 新增即可）
                                                                ▼
                                        tutti C++ StorageRuntime →
                                        DataPath SPI → NVMe（换 GPU 存储
                                        的插拔点在 tutti 自有 SPI）
```

## 2. 设计原则

1. **四层对应 legacy/LMCache 验证过的结构**（§5 对应表）；
2. **布局适配在 engine，不在 backend**：gather/scatter/staging 归引擎，
   backend 只搬字节（staging 槽 ↔ 持久化），零布局感知——换后端时
   布局适配零重写；
3. **chunk_index/staging/kernels 是引擎通用组件**（用户裁决：换后端
   必须复用），不隶属任何 backend；
4. **engine 唯一**（具体类，非 Protocol 多实现）；按介质命名的是
   backend（MemoryBackend/LocalStoreBackend/...）；
5. chunk 尺寸由模型决定（chunk_kv_bytes = bpc × Σ padded_page，
   D-002）；chunk_id 全程 opaque；
6. 无盘可测：MemoryBackend 使 adapter+engine 全部单测脱离真实存储介质。

## 2a. 背压（四级，各管一段）

生产/消费速率差使流水线必须限流（槽有限：8 槽典型 ~320MiB）：

```
vllm 调度层     max_tokens_per_load      ← 命中上报封顶（防 scheduler 一步发太多）
adapter worker  在途句柄上限              ← 执行侧总量限（第 2 级）
engine staging  槽位背压（满则等最老完成）← 防数据损坏/OOM（第 1 级，核心）
LocalStoreBackend     partial-commit 拒收+窗口重发 ← 存储侧最后防线（R14 S4：
                                              被拒不重发 = 静默丢数据）
```

**带宽分级与背压触发分析（2026-08-19 校准，勿用单盘口径）**：

| 场景 | 可用带宽 |
|---|---|
| 单盘直连 | ~7 GB/s |
| 单 GPU NUMA 4 盘 striped 突发 | ~28 GB/s |
| 全机 8 盘聚合峰值（实测） | 50+ GB/s（保守一半 20+ GB/s） |
| 稳态 8 GPU 均分 | ~7 GB/s / GPU |

- **槽深吞吐 = num_slots × 可用 DMA 带宽**（槽是流水线：一槽 DMA
  完成即归还接新 chunk）。8 槽 × 28GB/s 突发 = 224GB/s 供给能力，
  远超单 GPU 任何需求——**稳态下背压不触发，只防并发突发**（大
  prefill 批、多请求同时命中）。
- 高带宽下的真实瓶颈转移：**store 稳态瓶颈回到 KV 生成速率**（如
  70B prefill KV 产出 ~26GB/s，低于突发 DMA），**load 稳态瓶颈在盘**
  （20-50GB/s）而 attention 消费近乎无限快——即背压守护的是
  "突发雪崩"而非"稳态排队"，槽深设计以满足突发持续时间为准，
  不为吞吐而加深。
- 设计铁律：每级"等待"只挡住它该挡的流量，不阻塞无关路径
  （槽满时 load 路径有空槽照常走）。

## 2b. 并发与零开销预排（2026-08-19 定稿）

### 2b.1 tutti 批量提交是基础

- **请求粒度 = 一个 chunk 的一层段（segment）**，并发由 fused
  kernel 批量承担：`submit(N 条)` → 每 DataPath 1 次 H2D +
  1 次 fused launch → kernel 内 N 个工作项并发。
- **批参数全部可调**（`local_nvme_data_path.h:117-163`，非架构限制）：
  `max_batch_entries`（默认 256，实测 4096）、
  `max_in_flight_operations`、`max_batch_requests`（独立旋钮）、
  `max_request_bytes_override`；单条请求可至
  `batch_entries × MDTS`（4096×128KiB=512MiB，内部 fan-out）。
  上限只在显存预算。
- launch 开销：每步每 DataPath 个位数批 → μs 级可忽略。
- **tutti 按层提交 IO**（层段粒度）；store 逐层提交的唯一动机是
  数据依赖（L 层算完才发，保 overlap），非 launch 开销。
- 层组参数 k（多层合一条大 IO）为可选优化，默认 k=1。

### 2b.2 零开销预排（CPU 每步 O(1)，GPU 自闭环）

```
start_load_kv（CPU 每步一次，唯一入口）：
  numpy 一次算出全部 IO 描述符（N 层 × M chunk）
  → 一次 H2D 到 GPU arena
  → io_stream enqueue 全部 IO kernel（第 L 层等 er[L]）
  → scatter/gather stream enqueue 对应 kernel（等 IO 完成 / 等 ec[L]）
CPU 返回后，本步全部层的 read∥compute∥write 流水 GPU 自闭环；
vllm 每层 connector 回调（wait_for_layer_load / save_kv_layer）
= no-op 或层指针递增——依赖链已在 stream 中。
跨步：vllm 调度每步变化 → start_load_kv 每步一次是框架下限，
即"CPU 每步 O(1) 次出现"的 tutti 目标本身。
CUDA Graph 为可选增强（计算 piecewise 图 + IO 图外 event 同步），
非正确性依赖。
```

### 2b.3 staging = 固定环形窗口（无分配/归还/CPU 轮询）

- **窗口大小 = 2 × 波次 chunk 数 × segment_bytes**（读写双流水，
  波次上限 max_chunks_per_wave 可调，如 1024 chunk）；
  第 i 批写窗口 i mod W；**覆盖安全由事件链保证**：本批 IO 写
  窗口 X 前 wait 第 i-W 批 scatter/gather 完成事件——与
  `examples/layerwise_kv_overlap` 的 er/ec 双 buffer 轮转同模式，
  无分配、无归还、无 CPU 轮询（背压 §2a 第 1 级由此落地）。
- **尺寸公式（与上下文总长无关，由波次上限决定）**：

| 项 | GLM5.2（256-token chunk，segment=288KiB 天然对齐） |
|---|---|
| 波次窗口（1024 chunk 单层段） | 288 MiB |
| 双流水（读层+写层） | **576 MiB** |
| + MetadataArena（batch=4096，官方账 ~260MiB） | **~800 MiB** |

  GLM5.2：segment = 256tok × 1.125KiB = 288KiB（恰为 4096×72，
  无 padding；bpc=16 下 vllm block=16 也自然落在对齐点）。
- **大 prompt 事实**：512K token 全量 KV（Llama-70B GQA=160GiB）
  超 HBM——vllm 必然分波（max_tokens_per_load + block pool），
  "一次 kernel 处理 prompt"是波次内批量（如 8 chunk × 78 层段
  = 624 条 < 4096 单批），staging 只需波次窗口而非 prompt 总量。

### 2b.4 LMCache kernel 核查结论（kernels/ 包，T-117 已平移）

- `single_layer_kv_transfer_kernel`（mem_kernels.cu:148-211）：
  **per-layer 按层分片**（grid=(2, num_tokens/2)，block=num_heads，
  slot_mapping 读 token→slot；grid.x 随 token 数自适应）——与
  tutti 按层提交 IO 天然同构：每层 = 1 次 IO kernel + 1 次
  scatter/gather kernel。
- 布局适配 16 种 EngineKVFormat（`page_buffer_offset` 编译期特化）。
- **平移范围**：single_layer 主用（按层分片）；multi_layer /
  _fused_ptr 可选（若将来多层合批）。LMCache kernel 产出布局 =
  staging 槽布局（token-major 层段连续、层间 chunk-major），
  `io_granularity` 按此布局注册预建 PRP。

## 3. 两级 SPI

### 3.1 TuttiEngine（core.py；adapter 的唯一依赖）

```python
ChunkKey = bytes      # 链式哈希（D-007）
ChunkId = str         # 文件路径 key（D-011，opaque：backend 生成并解释）

@dataclass(frozen=True) class LoadPlan:
    keys: tuple[ChunkKey,...]; chunk_ids: tuple[ChunkId,...]
    dst_first_blocks: tuple[int,...]
@dataclass(frozen=True) class StorePlan:
    keys: tuple[ChunkKey,...]; chunk_ids: tuple[ChunkId,...]
    src_first_blocks: tuple[int,...]; evicted: tuple[ChunkKey,...]

class TuttiEngine:    # 具体类，组合通用组件 + 一个 StorageBackend
    # 构造：TuttiEngine(config: dict) —— config["backend"] 选实现，
    # 其余键硬件无关（chunk_tokens/chunk_kv_bytes/capacity_bytes/...）
    # ---- 计划态（scheduler 进程）----
    def lookup_prefix(token_ids) -> int                 # 链式哈希滚动
    def plan_load(keys) -> LoadPlan
    def plan_store(keys) -> StorePlan | None            # LRU/pin（chunk_index）
    def complete_store(plan, success); def complete_load(plan); def reset()
    # ---- 执行态（worker 进程）----
    def bind(kv_caches: dict[str, Tensor], num_layers, blocks_per_chunk)
    def load_layer(plan, layer_idx, dst_first_blocks) -> object   # 完成句柄
        # 内部：backend.read_chunk_segment(...→staging槽层段) →
        #        scatter(staging→paged dst) ；句柄=两者事件合成
    def store_layer(plan, layer_idx, src_first_blocks) -> object
        # 内部：gather(paged src→staging槽层段) →
        #        backend.write_chunk_segment(staging槽层段→...)
    def wait_idle(); def shutdown()
```

> 实例共享（2026-08-20 裁决）：adapter 双角色（scheduler/worker）经
> 注册表按 (id(vllm_config), 配置规范化键) 复用同一 TuttiEngine——
> chunk 索引的 pin/pending/resident 必须单实例；多进程部署下的索引
> 同步属集成轮问题。

### 3.2 StorageBackend（backend.py；存储介质契约——engine 的唯一存储依赖）

```python
class StorageBackend(Protocol):
    """staging 槽 ↔ 持久化 之间的 chunk 段搬运。零布局感知：
    不知道 vllm、不知道 gather/scatter、不知道 paged。"""

    def chunk_paths(self, num_chunks: int) -> list[str]:
        """生成 chunk 路径池（opaque，自产自解释；engine 构造时调用）。"""

    def bind_staging(self, staging_buffer_addr: int,
                     chunk_bytes: int, num_slots: int,
                     segment_bytes: int, io_stream) -> None:
        """注册 staging 缓冲（介质所需的 DMA 注册等在此做）。"""

    def write_chunk_segment(self, chunk_id: ChunkId, layer_idx: int,
                            slot: int) -> object:
        """异步：staging[slot] 的 layer 段 → 持久化。返回完成句柄。"""
    def read_chunk_segment(self, chunk_id: ChunkId, layer_idx: int,
                           slot: int) -> object:
        """异步：持久化 → staging[slot] 的 layer 段。返回完成句柄。"""
    def wait_idle(self) -> None; def shutdown(self) -> None
```

- MemoryBackend：chunk 池为 GPU tensor，段搬运 = cudaMemcpyAsync
  （或 tensor copy）；chunk_id := `mem://slot/<i>`。
- LocalStoreBackend：chunk_id := 文件路径 key；层段 offset =
  layer_idx × segment_bytes（packed）；submit 固定粒度（预建 PRP），
  partial-commit 窗口重发。
- 未来 RemoteBackend/分层后端：同 SPI 新增；engine/adapter/通用组件
  零改动。

## 4. 对接 vLLM（adapter 逐回调映射，零改动）

| vllm 回调 | adapter 实现 | engine 调用 |
|---|---|---|
| `get_num_new_matched_tokens` | need=external−num_computed（全命中−1）+ min_retrieve/max_tokens_per_load | `lookup_prefix` |
| `update_state_after_alloc` | RequestTracker 记录 | — |
| `build_connector_meta` | ReqMeta/slot_mapping | `plan_load`/`plan_store` |
| `register_kv_caches` | 层表 | `bind` |
| `start_load_kv` | 发起第 0 层 | `load_layer(plan,0)` |
| `wait_for_layer_load(L)` | 等 L 层句柄；发 L+1（逐层预取） | `load_layer(plan,L+1)` |
| `save_kv_layer(L)` | 发起 L 层存储（与计算并行） | `store_layer(plan,L)` |
| `wait_for_save` | no-op | — |
| `get_finished` | 收割 | `complete_*` |

## 5. 与 LMCache/legacy 对应表（本架构的血统证明）

| LMCache / legacy infinikv | tutti（本架构） |
|---|---|
| `vllm_v1_adapter.py` | `integration/vllm-connector/adapter/` |
| `LMCacheEngine`/`infinikv_engine` | `core.py` TuttiEngine |
| `token_database` | `chunk_index.py` |
| `memory_management`（MemoryObj/池） | `core.py` 环形窗口 |
| gpu_connector + `mem_kernels.cu` | `kernels/`（tutti_kv_kernels）gather/scatter |
| `storage_backend/abstract` | `backend.py` StorageBackend SPI |
| LocalCPUBackend | MemoryBackend |
| LocalDisk/GDS/**GeminiFS**（legacy 改造点） | LocalStoreBackend（tutti DMA） |

legacy 当年 = 换一个 backend 实现；我们今天 = 同样结构，backend 换成
tutti GPU-direct，且布局适配层（被 legacy 砍掉的）在 engine 恢复。

## 6. 现状与任务卡

| 组件 | 位置 | 状态 |
|---|---|---|
| TuttiEngine + StorageBackend SPI + MemoryBackend | `engine/`（core/backend/memory_backend） | 已实现（T-116） |
| ChunkIndex（key→路径，LRU/pin） | `engine/chunk_index.py` | 已实现（T-111） |
| gather/scatter kernels（LMCache 平移） | `kernels/`（tutti_kv_kernels） | 已实现（T-117） |
| tutti_runtime（pybind） | `bindings/python/` | 已实现（T-101） |

（路径均相对 `integration/vllm-connector/`。）

- **T-113**：adapter 骨架（vllm_adapter.py：壳 + scheduler 侧翻译）。
- **T-114**：worker 翻译（调 TuttiEngine；MemoryBackend 注入单测）。
- **T-115**：LocalStoreBackend（tutti_runtime 接线 + e2e；内部布局自决）。

## 7. 决策链

D-005（外部 connector）→ D-007（链式哈希）→ D-008（术语纪律）→
D-009（引擎层）→ D-010（唯一引擎 + StorageBackend SPI，
对齐 LMCache 四层）→ D-011（chunk 身份=文件路径）。
§0a 为最高优先级纪律。
