"""TuttiEngine — KV 语义引擎（D-010/D-011，ARCHITECTURE.md §3.1）。

唯一引擎（具体类，非 Protocol 多实现）：计划态（chunk 索引 / LRU /
pin）+ 执行态（staging 环形窗口 + 逐层 load/store + 背压）。组合通用
组件 ChunkIndex 与一个 StorageBackend 实现。零 vllm 依赖。

staging 环形窗口（§2b.3）：窗口总字节 W = 2 × max_chunks_per_wave ×
segment_bytes（读写双流水，num_slots = 2 × max_chunks_per_wave）；第
i 批（每次 load_layer/store_layer 调用 = 一批）写窗口 i mod 2；覆盖
安全由事件链保证：批 i 开始前 wait 批 i-2 的完成事件（无分配、无
归还、无 CPU 轮询）。批内第 j 个 chunk → slot (i mod 2)×mcw + j。

完成句柄：torch.cuda.Event（有 torch 且有 CUDA）或假 event（降级）；
句柄可 wait()/synchronize()/query()。

scatter/gather（staging↔paged 布局适配）R1 为可注入 fn 钩子，默认仅
记录调用（真 kernel 见 T-117）。
"""

import ctypes
from dataclasses import dataclass

from engine.backend import ChunkId, ChunkKey, StorageBackend
from engine.chunk_index import ChunkIndex
from engine.memory_backend import MemoryBackend


# ---------- 计划态数据结构（§3.1） ----------


@dataclass(frozen=True)
class LoadPlan:
    keys: "tuple[ChunkKey, ...]"
    chunk_ids: "tuple[ChunkId, ...]"
    dst_first_blocks: "tuple[int, ...]"


@dataclass(frozen=True)
class StorePlan:
    keys: "tuple[ChunkKey, ...]"
    chunk_ids: "tuple[ChunkId, ...]"
    src_first_blocks: "tuple[int, ...]"
    evicted: "tuple[ChunkKey, ...]"


# ---------- 完成句柄 / 事件 ----------


class _FakeEvent:
    """无 torch（或无 CUDA）时的降级事件。"""

    def record(self, stream=None) -> None:
        pass

    def wait(self, stream=None) -> None:
        pass

    def synchronize(self) -> None:
        pass

    def query(self) -> bool:
        return True


def _make_event():
    """完成事件工厂：torch.cuda.Event（有 torch 且有 CUDA）或假 event。"""
    try:
        import torch

        if torch.cuda.is_available():
            return torch.cuda.Event()
    except ImportError:  # pragma: no cover - 环境相关
        pass
    return _FakeEvent()


class _LayerHandle:
    """一次 load_layer/store_layer 的完成句柄（backend 句柄 + 批事件合成）。"""

    __slots__ = ("_event", "_backend_handles", "_done")

    def __init__(self, event, backend_handles):
        self._event = event
        self._backend_handles = tuple(backend_handles)
        self._done = False

    def wait(self) -> None:
        if self._done:
            return
        for h in self._backend_handles:
            w = getattr(h, "wait", None)
            if callable(w):
                w()
        self._event.synchronize()
        self._done = True

    synchronize = wait

    def query(self) -> bool:
        if self._done:
            return True
        ok = self._event.query() and all(
            getattr(h, "query", lambda: True)() for h in self._backend_handles
        )
        if ok:
            self._done = True
        return ok


# ---------- 默认 scatter/gather 钩子（R1：仅记录） ----------


def _noop_gather(self, plan, layer_idx, slots, first_blocks):
    self._op_log.append(("gather", plan, layer_idx, tuple(slots), tuple(first_blocks)))


def _noop_scatter(self, plan, layer_idx, slots, first_blocks):
    self._op_log.append(("scatter", plan, layer_idx, tuple(slots), tuple(first_blocks)))


# ---------- 引擎 ----------


class TuttiEngine:
    """chunk 计划态 + 逐层执行态 的 KV 语义引擎（adapter 的唯一依赖）。"""

    def __init__(self, config: dict):
        if not isinstance(config, dict):
            raise TypeError(f"config must be dict, got {type(config).__name__}")

        # ---- 配置键（硬件无关，§3.1）----
        backend_cfg = config["backend"]
        self._chunk_tokens = config["chunk_tokens"]
        self._chunk_kv_bytes = config["chunk_kv_bytes"]
        capacity_bytes = config["capacity_bytes"]
        self._max_chunks_per_wave = config["max_chunks_per_wave"]

        if not isinstance(self._chunk_tokens, int) or self._chunk_tokens < 1:
            raise ValueError(f"chunk_tokens must be int >= 1, got {self._chunk_tokens!r}")
        if not isinstance(self._chunk_kv_bytes, int) or self._chunk_kv_bytes < 1:
            raise ValueError(f"chunk_kv_bytes must be int >= 1, got {self._chunk_kv_bytes!r}")
        if not isinstance(capacity_bytes, int) or capacity_bytes < 1:
            raise ValueError(f"capacity_bytes must be int >= 1, got {capacity_bytes!r}")
        if not isinstance(self._max_chunks_per_wave, int) or self._max_chunks_per_wave < 1:
            raise ValueError(
                f"max_chunks_per_wave must be int >= 1, got {self._max_chunks_per_wave!r}"
            )

        num_chunks = capacity_bytes // self._chunk_kv_bytes
        if num_chunks < 1:
            raise ValueError(
                f"capacity_bytes ({capacity_bytes}) < chunk_kv_bytes "
                f"({self._chunk_kv_bytes}): no chunk fits"
            )

        # ---- backend 选择（§3.1：config["backend"] 选实现）----
        if isinstance(backend_cfg, str):
            if backend_cfg != "memory":
                raise ValueError(
                    f"unknown backend {backend_cfg!r} (R1: 'memory' or a "
                    f"StorageBackend instance)"
                )
            self._backend = MemoryBackend(num_chunks=num_chunks, chunk_bytes=self._chunk_kv_bytes)
        elif isinstance(backend_cfg, StorageBackend):
            # 注入实例（T-114 单测等）：池容量由调用方保证与 capacity_bytes 匹配
            self._backend = backend_cfg
        else:
            raise TypeError(
                f"config['backend'] must be str or StorageBackend, got {backend_cfg!r}"
            )

        # ---- 计划态索引：路径池由 backend 生成并解释（D-011：opaque）----
        self._index = ChunkIndex(
            self._backend.chunk_paths(num_chunks), chunk_size=self._chunk_tokens
        )

        # ---- 执行态初始状态 ----
        self._staging = None
        self._staging_addr = None
        self._num_layers = None
        self._segment_bytes = None
        self._num_slots = None
        self._kv_caches = None
        self._blocks_per_chunk = None
        self._wave_seq = 0
        self._wave_events: dict = {}  # wave 序号 → 完成事件（保留最近两个）
        self._op_log: list = []

        # ---- 可注入 scatter/gather 钩子（R1 默认仅记录）----
        self.gather_fn = config.get("gather_fn") or _noop_gather.__get__(self)
        self.scatter_fn = config.get("scatter_fn") or _noop_scatter.__get__(self)

    # ---------- 属性 ----------

    @property
    def backend(self) -> StorageBackend:
        return self._backend

    @property
    def index(self) -> ChunkIndex:
        return self._index

    @property
    def chunk_tokens(self) -> int:
        return self._chunk_tokens

    @property
    def chunk_kv_bytes(self) -> int:
        return self._chunk_kv_bytes

    @property
    def max_chunks_per_wave(self) -> int:
        return self._max_chunks_per_wave

    @property
    def capacity_chunks(self) -> int:
        return self._index.capacity

    @property
    def num_layers(self):
        return self._num_layers

    @property
    def segment_bytes(self):
        return self._segment_bytes

    @property
    def num_slots(self):
        return self._num_slots

    @property
    def staging_addr(self):
        return self._staging_addr

    @property
    def op_log(self) -> list:
        return self._op_log

    # ---------- 计划态（scheduler 进程） ----------

    def lookup_prefix(self, token_ids) -> int:
        """链式哈希滚动：返回命中前缀 token 数（转发 ChunkIndex）。"""
        return self._index.lookup_prefix(list(token_ids))

    def plan_load(self, keys) -> LoadPlan:
        """对全命中 keys 出加载计划并 pin（任一 miss → KeyError）。"""
        keys = list(keys)
        paths = self._index.pin(keys)  # KeyError if any miss
        return LoadPlan(
            keys=tuple(keys),
            chunk_ids=tuple(paths),
            dst_first_blocks=(),
        )

    def plan_store(self, keys) -> "StorePlan | None":
        """为不在 stored/pending 的 keys 分配路径（LRU 驱逐，pinned 保护）。

        容量耗尽且无可驱逐 → None。返回的 StorePlan.keys/chunk_ids 只含
        本次新分配（需要写盘）的 chunk；已存在的不重复分配。
        """
        keys = list(keys)
        deduped = list(dict.fromkeys(keys))  # 去重保序
        new_keys = [
            k
            for k in deduped
            if k not in self._index.stored and k not in self._index.pending_store
        ]
        result = self._index.allocate(keys)
        if result is None:
            return None
        paths, evicted = result
        # allocate 跳过已存在项，返回的 paths 与 new_keys 同序
        return StorePlan(
            keys=tuple(new_keys),
            chunk_ids=tuple(paths),
            src_first_blocks=(),
            evicted=tuple(evicted),
        )

    def complete_store(self, plan: StorePlan, success: bool = True) -> None:
        """success → pending 移入 stored；False → 路径回 free（回收）。"""
        self._index.complete_store(list(plan.keys), success)

    def complete_load(self, plan: LoadPlan) -> None:
        """load 结束解除 pin。"""
        self._index.unpin(list(plan.keys))

    def reset(self) -> None:
        """清空计划态（stored/pinned/pending）与执行态波次状态。"""
        self._index.reset()
        self._wave_seq = 0
        self._wave_events.clear()
        self._op_log.clear()

    # ---------- 执行态（worker 进程） ----------

    def bind(self, kv_caches: dict, num_layers: int, blocks_per_chunk: int) -> None:
        """注册 paged KV cache 层表，分配 staging 并绑定 backend。

        segment_bytes = chunk_kv_bytes // num_layers（packed 布局，
        层段等分；不整除 → ValueError）。
        """
        if not isinstance(kv_caches, dict) or not kv_caches:
            raise TypeError(f"kv_caches must be a non-empty dict, got {kv_caches!r}")
        if not isinstance(num_layers, int) or num_layers < 1:
            raise ValueError(f"num_layers must be int >= 1, got {num_layers!r}")
        if not isinstance(blocks_per_chunk, int) or blocks_per_chunk < 1:
            raise ValueError(f"blocks_per_chunk must be int >= 1, got {blocks_per_chunk!r}")
        if self._chunk_kv_bytes % num_layers != 0:
            raise ValueError(
                f"chunk_kv_bytes ({self._chunk_kv_bytes}) must be divisible by "
                f"num_layers ({num_layers}) for packed layer segments"
            )
        segment_bytes = self._chunk_kv_bytes // num_layers
        num_slots = 2 * self._max_chunks_per_wave
        staging = ctypes.create_string_buffer(num_slots * segment_bytes)

        self._kv_caches = kv_caches
        self._num_layers = num_layers
        self._blocks_per_chunk = blocks_per_chunk
        self._segment_bytes = segment_bytes
        self._num_slots = num_slots
        self._staging = staging
        self._staging_addr = ctypes.addressof(staging)
        self._wave_seq = 0
        self._wave_events.clear()

        self._backend.bind_staging(
            self._staging_addr,
            self._chunk_kv_bytes,
            num_slots,
            segment_bytes,
            None,  # io_stream：R1 无 CUDA stream 管理（T-117 接线）
        )

    def load_layer(self, plan: LoadPlan, layer_idx: int, dst_first_blocks=None) -> object:
        """backend.read（→staging 段）→ scatter 到 paged 目标；返回完成句柄。"""
        n, layer_idx, first_blocks = self._check_layer_call(plan, layer_idx, dst_first_blocks)
        wave, slots = self._acquire_wave(n)

        handles = [
            self._backend.read_chunk_segment(cid, layer_idx, slot)
            for cid, slot in zip(plan.chunk_ids, slots)
        ]
        self.scatter_fn(plan, layer_idx, slots, first_blocks)

        event = self._finish_wave(wave)
        return _LayerHandle(event, handles)

    def store_layer(self, plan: StorePlan, layer_idx: int, src_first_blocks=None) -> object:
        """gather（paged→staging 段）→ backend.write；返回完成句柄。"""
        n, layer_idx, first_blocks = self._check_layer_call(plan, layer_idx, src_first_blocks)
        wave, slots = self._acquire_wave(n)

        self.gather_fn(plan, layer_idx, slots, first_blocks)
        handles = [
            self._backend.write_chunk_segment(cid, layer_idx, slot)
            for cid, slot in zip(plan.chunk_ids, slots)
        ]

        event = self._finish_wave(wave)
        return _LayerHandle(event, handles)

    def wait_idle(self) -> None:
        """等全部在途波次完成（host 级），再等 backend 清空。"""
        for ev in self._wave_events.values():
            ev.synchronize()
        self._backend.wait_idle()

    def shutdown(self) -> None:
        self._backend.shutdown()

    # ---------- 内部 ----------

    def _require_bound(self) -> None:
        if self._staging is None:
            raise RuntimeError("TuttiEngine: bind() not called yet")

    def _check_layer_call(self, plan, layer_idx, first_blocks_override):
        self._require_bound()
        n = len(plan.chunk_ids)
        if not isinstance(layer_idx, int) or not (0 <= layer_idx < self._num_layers):
            raise ValueError(
                f"layer_idx must be in [0, {self._num_layers}), got {layer_idx!r}"
            )
        if n == 0:
            raise ValueError("plan has no chunk")
        if n > self._max_chunks_per_wave:
            raise ValueError(
                f"plan has {n} chunks > max_chunks_per_wave "
                f"({self._max_chunks_per_wave}); adapter must split waves"
            )
        if first_blocks_override is not None:
            first_blocks = tuple(first_blocks_override)
        else:
            first_blocks = getattr(plan, "dst_first_blocks", ()) or getattr(
                plan, "src_first_blocks", ()
            )
        if len(first_blocks) != n:
            raise ValueError(
                f"first_blocks length {len(first_blocks)} != chunk count {n}"
            )
        return n, layer_idx, first_blocks

    def _acquire_wave(self, n: int) -> "tuple[int, list[int]]":
        """取一批 staging 槽；覆盖保护：wait 环形前批（i-2）事件（背压）。"""
        wave = self._wave_seq
        prev = self._wave_events.get(wave - 2)
        if prev is not None:
            prev.wait()
        base = (wave % 2) * self._max_chunks_per_wave
        return wave, [base + j for j in range(n)]

    def _finish_wave(self, wave: int):
        """记录批完成事件，推进波次序号，清理过期事件（保留最近两个）。"""
        event = _make_event()
        event.record()
        self._wave_events[wave] = event
        stale = [w for w in self._wave_events if w < wave - 1]
        for w in stale:
            del self._wave_events[w]
        self._wave_seq = wave + 1
        return event
