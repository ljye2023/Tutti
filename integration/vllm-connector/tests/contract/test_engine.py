"""TuttiEngine 测试（T-116）：计划态 + 执行态 + 环形窗口 + 零依赖。"""

import ctypes
import sys
from pathlib import Path

import pytest

_CONNECTOR_ROOT = Path(__file__).resolve().parents[2]

import engine.core as eng_mod
from engine.chunk_index import hash_chunk
from engine.core import LoadPlan, StorePlan, TuttiEngine

CS = 4  # chunk_tokens
NL = 2  # num_layers
SEG = 16  # segment_bytes
CB = SEG * NL  # chunk_kv_bytes
NC = 4  # capacity: 4 chunks
MCW = 2  # max_chunks_per_wave


def make_config(**over):
    cfg = dict(
        backend="memory",
        chunk_tokens=CS,
        chunk_kv_bytes=CB,
        capacity_bytes=NC * CB,
        max_chunks_per_wave=MCW,
    )
    cfg.update(over)
    return cfg


def chain_keys(n: int, base: int = 0) -> "list[bytes]":
    """链式 keys（D-007）：H_i = blake2b(H_{i-1} ‖ tokens_i)。"""
    parent = b""
    keys = []
    for i in range(n):
        parent = hash_chunk(
            tuple(range(base + i * CS, base + (i + 1) * CS)), parent=parent
        )
        keys.append(parent)
    return keys


def tokens_for(n: int, base: int = 0) -> "list[int]":
    return list(range(base, base + n * CS))


def fill_stored(engine: TuttiEngine, keys) -> None:
    plan = engine.plan_store(keys)
    assert plan is not None
    engine.complete_store(plan)


def make_bound_engine(**over) -> TuttiEngine:
    e = TuttiEngine(make_config(**over))
    e.bind({"l0": object(), "l1": object()}, NL, blocks_per_chunk=1)
    return e


# ---------- 构造 / 配置 ----------


def test_config_validation():
    with pytest.raises(KeyError):
        TuttiEngine({k: v for k, v in make_config().items() if k != "chunk_tokens"})
    with pytest.raises(ValueError):
        TuttiEngine(make_config(backend="unknown"))  # 未知实现名
    with pytest.raises(ValueError):
        TuttiEngine(make_config(chunk_tokens=0))
    with pytest.raises(ValueError):
        TuttiEngine(make_config(capacity_bytes=CB - 1))  # 装不下一个 chunk
    with pytest.raises(ValueError):
        TuttiEngine(make_config(max_chunks_per_wave=0))
    with pytest.raises(TypeError):
        TuttiEngine(make_config(backend=123))


def test_memory_backend_selected_and_path_pool():
    e = TuttiEngine(make_config())
    assert e.capacity_chunks == NC
    assert e.index.free == [f"mem://slot/{i}" for i in range(NC)]


# ---------- 计划态 ----------


def test_lookup_prefix_chain_hits():
    e = TuttiEngine(make_config())
    keys = chain_keys(3)
    fill_stored(e, keys[:3])
    # 存 3 chunk 后查 5-chunk 序列 → 3 × chunk_tokens
    assert e.lookup_prefix(tokens_for(5)) == 3 * CS


def test_lookup_prefix_stops_at_gap_and_ignores_pending():
    e = TuttiEngine(make_config())
    keys = chain_keys(3)
    # 只存 chunk0 与 chunk2（跳过 chunk1）→ 断在中间
    plan = e.plan_store([keys[0], keys[2]])
    assert plan is not None
    assert len(plan.keys) == 2
    e.complete_store(plan)
    assert e.lookup_prefix(tokens_for(3)) == CS

    e2 = TuttiEngine(make_config())
    p = e2.plan_store(chain_keys(1))  # pending，未 complete → 不命中
    assert p is not None
    assert e2.lookup_prefix(tokens_for(1)) == 0
    e2.complete_store(p, success=False)
    assert e2.lookup_prefix(tokens_for(1)) == 0


def test_plan_load_pins_and_missing_raises():
    e = TuttiEngine(make_config())
    keys = chain_keys(2)
    fill_stored(e, keys)
    plan = e.plan_load(keys)
    assert isinstance(plan, LoadPlan)
    assert plan.keys == tuple(keys)
    assert plan.chunk_ids == tuple(e.index.stored[k] for k in keys)
    assert plan.dst_first_blocks == ()
    # 已 pin：容量耗尽时不可驱逐
    assert set(e.index.pinned.keys()) == set(plan.chunk_ids)
    # miss → KeyError
    with pytest.raises(KeyError):
        e.plan_load([keys[0], b"\x00" * 16])
    e.complete_load(plan)
    assert e.index.pinned == {}


def test_plan_store_dedup_and_existing_skipped():
    e = TuttiEngine(make_config())
    keys = chain_keys(2)
    fill_stored(e, keys)
    plan = e.plan_store(keys)  # 全已存在
    assert plan == StorePlan(keys=(), chunk_ids=(), src_first_blocks=(), evicted=())
    # 混合：1 已存在 + 1 新
    k2 = chain_keys(3)[2]
    plan = e.plan_store([keys[0], k2])
    assert plan.keys == (k2,)
    assert len(plan.chunk_ids) == 1
    assert plan.evicted == ()
    e.complete_store(plan)


def test_plan_store_evicts_lru_when_full():
    e = TuttiEngine(make_config())  # NC=4
    keys = chain_keys(4)
    fill_stored(e, keys)
    k4 = chain_keys(5)[4]
    plan = e.plan_store([k4])
    assert plan is not None
    assert plan.evicted == (keys[0],)  # LRU 头驱逐
    assert keys[0] not in e.index.stored
    e.complete_store(plan)
    assert e.lookup_prefix(tokens_for(5)) == 0  # k0 已被驱逐


def test_plan_store_pinned_not_evicted_then_none():
    e = TuttiEngine(make_config())  # NC=4
    keys = chain_keys(4)
    fill_stored(e, keys)
    lp = e.plan_load([keys[0]])  # pin k0
    k4 = chain_keys(5)[4]
    # k0 pinned → 只能驱逐 k1/k2/k3，仍够 1 个
    plan = e.plan_store([k4])
    assert plan is not None
    assert plan.evicted == (keys[1],)
    e.complete_store(plan)
    e.complete_load(lp)
    # 再压满后 pin 全部 → 无可驱逐 → None
    e2 = TuttiEngine(make_config(max_chunks_per_wave=MCW))
    ks = chain_keys(4)
    fill_stored(e2, ks)
    lp2 = e2.plan_load(ks)  # 全 pin
    assert e2.plan_store([chain_keys(5)[4]]) is None
    e2.complete_load(lp2)


def test_complete_store_failure_recycles_path():
    e = TuttiEngine(make_config())  # NC=4
    keys = chain_keys(4)
    fill_stored(e, keys)
    k4 = chain_keys(5)[4]
    plan = e.plan_store([k4])
    assert plan.evicted == (keys[0],)
    path = plan.chunk_ids[0]
    e.complete_store(plan, success=False)
    # 失败回收：路径回 free，k4 不可见
    assert k4 not in e.index.stored and k4 not in e.index.pending_store
    assert path in e.index.free
    assert e.lookup_prefix(tokens_for(5)) == 0
    # 回收的路径可被再次分配
    plan2 = e.plan_store([k4])
    assert plan2 is not None
    assert plan2.chunk_ids[0] == path
    e.complete_store(plan2)
    # k4 已可命中（链式前缀因 k0 被驱逐而从 0 断开，用索引层断言）
    assert e.index.stored[k4] == path
    assert e.lookup_prefix(tokens_for(5)) == 0  # k0 已驱逐：链断在首个 chunk


def test_reset_clears_all():
    e = TuttiEngine(make_config())
    keys = chain_keys(2)
    fill_stored(e, keys)
    e.plan_load(keys)  # pin 中
    e.reset()
    assert e.index.stored == {} and e.index.pinned == {}
    assert e.index.pending_store == {}
    assert e.index.free == [f"mem://slot/{i}" for i in range(NC)]
    assert e.lookup_prefix(tokens_for(2)) == 0
    # reset 后可继续正常使用
    fill_stored(e, keys)
    assert e.lookup_prefix(tokens_for(2)) == 2 * CS


# ---------- 执行态：bind ----------


def test_bind_validates_and_geometry():
    e = TuttiEngine(make_config())
    with pytest.raises(TypeError):
        e.bind({}, NL, 1)
    with pytest.raises(ValueError):
        e.bind({"l0": object()}, 0, 1)
    with pytest.raises(ValueError):
        e.bind({"l0": object()}, NL, 0)
    # chunk_kv_bytes 不被 num_layers 整除 → packed 层段无法等分
    e_bad = TuttiEngine(make_config(chunk_kv_bytes=CB + 1, capacity_bytes=NC * (CB + 1)))
    with pytest.raises(ValueError):
        e_bad.bind({"l0": object()}, NL, 1)

    e.bind({"l0": object(), "l1": object()}, NL, blocks_per_chunk=1)
    assert e.num_layers == NL
    assert e.segment_bytes == SEG
    assert e.num_slots == 2 * MCW
    assert e.staging_addr == ctypes.addressof(e._staging)
    # backend 已收到同一 staging 地址
    assert e.backend._staging_addr == e.staging_addr


def test_layer_ops_require_bind():
    e = TuttiEngine(make_config())
    plan = StorePlan(keys=(b"\x01" * 16,), chunk_ids=("mem://slot/0",), src_first_blocks=(0,), evicted=())
    with pytest.raises(RuntimeError):
        e.store_layer(plan, 0, (0,))


def test_load_store_validation():
    e = make_bound_engine()
    keys = chain_keys(1)
    fill_stored(e, keys)
    lp = e.plan_load(keys)
    with pytest.raises(ValueError):
        e.load_layer(lp, layer_idx=NL)  # 层越界
    with pytest.raises(ValueError):
        e.load_layer(lp, 0, dst_first_blocks=())  # 长度不匹配
    sp = e.plan_store(chain_keys(3))  # 3 chunks > MCW=2
    with pytest.raises(ValueError):
        e.store_layer(sp, 0, (0, 1, 2))


# ---------- 执行态：store→load 往返（真拷贝） ----------


def test_store_load_roundtrip_with_real_hooks():
    e = make_bound_engine()
    assert e.num_slots == 2 * MCW

    chunk_payload = {
        i: bytes([(0x10 + i + j) & 0xFF for j in range(CB)]) for i in range(2)
    }
    paged_src = {fb: chunk_payload[i] for i, fb in enumerate((10, 11))}
    paged_dst: dict = {}

    def gather(plan, layer_idx, slots, first_blocks):
        st = e._staging
        for cid, slot, fb in zip(plan.chunk_ids, slots, first_blocks):
            seg = e.segment_bytes
            off = slot * seg
            st[off : off + seg] = paged_src[fb][layer_idx * seg : (layer_idx + 1) * seg]

    def scatter(plan, layer_idx, slots, first_blocks):
        st = e._staging
        for cid, slot, fb in zip(plan.chunk_ids, slots, first_blocks):
            seg = e.segment_bytes
            off = slot * seg
            paged_dst.setdefault(fb, bytearray(CB))[
                layer_idx * seg : (layer_idx + 1) * seg
            ] = bytes(st[off : off + seg])

    e.gather_fn = gather
    e.scatter_fn = scatter

    # ---- store 两层 ----
    keys = chain_keys(2)
    sp = e.plan_store(keys)
    assert sp is not None and len(sp.keys) == 2
    for layer in range(NL):
        h = e.store_layer(sp, layer, src_first_blocks=(10, 11))
        h.wait()  # 句柄可 wait
        assert h.query() is True
    e.complete_store(sp)

    # ---- load 两层（另一 plan/目标块） ----
    lp = e.plan_load(keys)
    for layer in range(NL):
        h = e.load_layer(lp, layer, dst_first_blocks=(20, 21))
        h.wait()
    e.complete_load(lp)

    assert bytes(paged_dst[20]) == chunk_payload[0]
    assert bytes(paged_dst[21]) == chunk_payload[1]


def test_default_hooks_record_only():
    e = make_bound_engine()
    keys = chain_keys(1)
    fill_stored(e, keys)
    lp = e.plan_load(keys)
    e.load_layer(lp, 0, dst_first_blocks=(7,))
    e.complete_load(lp)
    sp = e.plan_store(chain_keys(2, base=100))
    e.store_layer(sp, 0, src_first_blocks=(8, 9))
    e.complete_store(sp)
    ops = [op[0] for op in e.op_log]
    assert ops == ["scatter", "gather"]
    assert e.op_log[0][2] == 0 and e.op_log[0][3] == (0,)  # layer/slots
    assert e.op_log[0][4] == (7,)


# ---------- 执行态：环形窗口（§2b.3） ----------


class RecEvent:
    """记录 record/wait 调用的假事件（mock）。"""

    def __init__(self):
        self.recorded = 0
        self.waited = 0

    def record(self, stream=None):
        self.recorded += 1

    def wait(self, stream=None):
        self.waited += 1

    def synchronize(self):
        pass

    def query(self):
        return True


def test_ring_window_backpressure_on_third_wave(monkeypatch):
    """max_chunks_per_wave=2 → 窗口双缓冲；第 3 批必须 wait 第 1 批事件。"""
    events: "list[RecEvent]" = []

    def fake_make_event():
        ev = RecEvent()
        events.append(ev)
        return ev

    monkeypatch.setattr(eng_mod, "_make_event", fake_make_event)

    e = make_bound_engine(max_chunks_per_wave=2)
    keys = chain_keys(1)
    fill_stored(e, keys)
    lp = e.plan_load(keys)

    # 批 0：窗口 0（slots 0..1），无等待
    e.load_layer(lp, 0, dst_first_blocks=(0,))
    assert events[0].recorded == 1 and events[0].waited == 0

    # 批 1：窗口 1（slots 2..3），无等待
    e.load_layer(lp, 1, dst_first_blocks=(0,))
    assert events[1].waited == 0 and events[0].waited == 0

    # 批 2：复用窗口 0 → 覆盖保护：wait 批 0 的事件（背压触发）
    e.load_layer(lp, 0, dst_first_blocks=(0,))
    assert events[0].waited == 1

    # 批 3：复用窗口 1 → wait 批 1 的事件
    e.load_layer(lp, 1, dst_first_blocks=(0,))
    assert events[1].waited == 1

    e.complete_load(lp)


def test_ring_window_slots_alternate():
    e = make_bound_engine(max_chunks_per_wave=2)
    keys = chain_keys(2)
    fill_stored(e, keys)
    lp = e.plan_load(keys)
    seen = []
    orig = e.scatter_fn

    def spy(plan, layer_idx, slots, first_blocks):
        seen.append(tuple(slots))

    e.scatter_fn = spy
    e.load_layer(lp, 0, dst_first_blocks=(0, 1))  # 批 0 → slots (0,1)
    e.load_layer(lp, 1, dst_first_blocks=(0, 1))  # 批 1 → slots (2,3)
    e.load_layer(lp, 0, dst_first_blocks=(0, 1))  # 批 2 → slots (0,1)
    e.complete_load(lp)
    assert seen == [(0, 1), (2, 3), (0, 1)]


def test_wait_idle_and_shutdown():
    e = make_bound_engine()
    keys = chain_keys(1)
    fill_stored(e, keys)
    lp = e.plan_load(keys)
    e.load_layer(lp, 0, dst_first_blocks=(0,)).wait()
    e.wait_idle()
    e.complete_load(lp)
    e.shutdown()  # 不抛即可


# ---------- 零依赖 ----------


def test_engine_does_not_import_vllm():
    """engine 自净：子进程隔离验证（同进程 adapter 测试会 import vllm）。"""
    import subprocess

    code = (
        "import sys; sys.path.insert(0, %r); "
        "import engine.core; "
        "bad = [m for m in ('vllm', 'tutti_runtime') if m in sys.modules]; "
        "assert not bad, f'leaked imports: {bad}'"
    ) % str(_CONNECTOR_ROOT)
    subprocess.run([sys.executable, "-c", code], check=True)


def test_engine_sources_have_no_vllm_import():
    from pathlib import Path

    for mod in ("core.py", "backend.py", "memory_backend.py", "chunk_index.py"):
        src = (Path(eng_mod.__file__).parent / mod).read_text(encoding="utf-8")
        assert "vllm" not in src.replace("零 vllm 依赖", "").replace(
            "不知道 vllm", ""
        ).replace("对齐 vllm fork", ""), f"{mod} 不应引用 vllm"
