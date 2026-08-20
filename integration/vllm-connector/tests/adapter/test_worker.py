# SPDX-License-Identifier: Apache-2.0
"""T-114 tests: WorkerImpl（worker 侧回调 → TuttiEngine 执行态翻译）。

Run:
    source /data/home/ryeqiu/env-tutti.sh
    cd integration/vllm-connector && python -m pytest tests/adapter/ -v
"""

import re
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest
import torch

# integration/vllm-connector（engine + adapter 顶层包根）
_ROOT = Path(__file__).resolve().parents[2]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from vllm.distributed.kv_transfer.kv_connector.v1.base import (  # noqa: E402
    KVConnectorBase_V1,
    KVConnectorRole,
)

from adapter.connector import (  # noqa: E402
    LoadSpec,
    ReqMeta,
    SaveSpec,
    TuttiConnectorMetadata,
    TuttiConnectorV1,
)
from adapter.worker import WorkerImpl  # noqa: E402
from engine.core import TuttiEngine  # noqa: E402

CS = 128        # chunk_tokens
BS = 128        # vllm block_size（bpc = CS//BS = 1）
NL = 2          # num_layers
SEG = 128       # segment_bytes
CB = SEG * NL   # chunk_kv_bytes = 256
NC = 8          # capacity chunks
MCW = 2         # max_chunks_per_wave

L0, L1 = "model.layers.0.self_attn", "model.layers.1.self_attn"
LAYERS = {L0: object(), L1: object()}


# ---------------------------------------------------------------- helpers


def make_engine(**over) -> TuttiEngine:
    cfg = dict(
        backend="memory",
        chunk_tokens=CS,
        chunk_kv_bytes=CB,
        capacity_bytes=NC * CB,
        max_chunks_per_wave=MCW,
    )
    cfg.update(over)
    return TuttiEngine(cfg)


def make_worker(engine=None, **over):
    e = engine if engine is not None else make_engine()
    w = WorkerImpl(cfg={"block_size": BS}, engine=e, **over)
    w.register_kv_caches(LAYERS)
    return w, e


def make_req(req_id, tokens, block_ids, load_spec=None, save_spec=None) -> ReqMeta:
    return ReqMeta(
        req_id=req_id,
        token_ids=torch.tensor(list(tokens), dtype=torch.long),
        slot_mapping=torch.zeros(len(tokens), dtype=torch.long),  # kernel 接线用
        block_ids=torch.tensor(list(block_ids), dtype=torch.long),
        save_spec=save_spec,
        load_spec=load_spec,
    )


def make_meta(*reqs) -> TuttiConnectorMetadata:
    meta = TuttiConnectorMetadata()
    for r in reqs:
        meta.add_request(r)
    return meta


def record_engine(e) -> dict:
    """记录 engine 执行态调用序列（load/store/complete_*）。"""
    calls = {"load": [], "store": [], "complete_load": [], "complete_store": []}
    orig = (e.load_layer, e.store_layer, e.complete_load, e.complete_store)

    def load(plan, layer_idx, dst_first_blocks=None):
        calls["load"].append((layer_idx, tuple(plan.chunk_ids), tuple(plan.dst_first_blocks)))
        return orig[0](plan, layer_idx, dst_first_blocks)

    def store(plan, layer_idx, src_first_blocks=None):
        calls["store"].append((layer_idx, tuple(plan.chunk_ids), tuple(plan.src_first_blocks)))
        return orig[1](plan, layer_idx, src_first_blocks)

    def complete_load(plan):
        calls["complete_load"].append(tuple(plan.keys))
        return orig[2](plan)

    def complete_store(plan, success=True):
        calls["complete_store"].append((tuple(plan.keys), success))
        return orig[3](plan, success)

    e.load_layer, e.store_layer = load, store
    e.complete_load, e.complete_store = complete_load, complete_store
    return calls


class RecHandle:
    """句柄代理：记录 wait 调用（背压调用序断言用）。"""

    def __init__(self, inner, log, hid):
        self._inner, self._log, self._hid = inner, log, hid

    def wait(self):
        self._log.append(("wait", self._hid))
        self._inner.wait()

    def synchronize(self):
        self._inner.synchronize()

    def query(self):
        return self._inner.query()


def record_handles(e, log):
    """包装 load_layer/store_layer：返回记录句柄，log 记 issue/wait 序。"""
    orig_load, orig_store = e.load_layer, e.store_layer
    seq = [0]

    def load(plan, layer_idx, dst_first_blocks=None):
        h = orig_load(plan, layer_idx, dst_first_blocks)
        seq[0] += 1
        log.append(("issue", seq[0]))
        return RecHandle(h, log, seq[0])

    def store(plan, layer_idx, src_first_blocks=None):
        h = orig_store(plan, layer_idx, src_first_blocks)
        seq[0] += 1
        log.append(("issue", seq[0]))
        return RecHandle(h, log, seq[0])

    e.load_layer, e.store_layer = load, store


def prepare_load(e, tokens, n_chunks):
    """计划态预置：store 满 n_chunks 后 plan_load（pin）。"""
    keys = e.index.keys_for_tokens(list(tokens))
    sp = e.plan_store(keys[:n_chunks])
    assert sp is not None
    e.complete_store(sp)
    return e.plan_load(keys[:n_chunks])


# ---------------------------------------------------------------- 验收 1：
# register_kv_caches → engine.bind


def test_register_kv_caches_binds_engine():
    e = make_engine()
    w = WorkerImpl(cfg={"block_size": BS}, engine=e)
    kv = {L0: object(), L1: object()}
    w.register_kv_caches(kv)

    assert e.num_layers == NL
    assert e._kv_caches is kv            # 层表透传
    assert e._blocks_per_chunk == CS // BS
    assert e.staging_addr is not None    # staging 已分配并绑定 backend
    assert e.backend._staging_addr == e.staging_addr

    with pytest.raises(RuntimeError):
        w.register_kv_caches(kv)  # 恰一次

    # 重复注册未引发第二次 bind：staging 地址未变
    assert e.backend._staging_addr == e.staging_addr


def test_register_kv_caches_validates():
    e = make_engine()
    w = WorkerImpl(cfg={}, engine=e)  # 缺 block_size
    with pytest.raises(ValueError, match="block_size"):
        w.register_kv_caches({L0: object()})
    w2 = WorkerImpl(cfg={"block_size": 7}, engine=e)  # chunk 不按块对齐
    with pytest.raises(ValueError, match="multiple of"):
        w2.register_kv_caches({L0: object()})
    with pytest.raises(TypeError):
        WorkerImpl(cfg={"block_size": BS}, engine=e).register_kv_caches({})
    assert e.staging_addr is None  # 校验失败未 bind


# ---------------------------------------------------------------- 验收 2：
# start_load_kv 发起第 0 层；wait_for_layer_load 逐层推进；末层 complete_load


def test_start_load_kv_issues_layer0_with_wave_split():
    w, e = make_worker()
    calls = record_engine(e)
    tokens = list(range(3 * CS))  # 3 chunks > MCW=2 → 两波
    lp = prepare_load(e, tokens, 3)

    req = make_req("r1", tokens, [5, 100, 357],
                   load_spec=LoadSpec("r1", 0, 3 * CS, True, lp.chunk_ids))
    w.start_load_kv(make_meta(req), forward_context=None)

    # 第 0 层已发起，且按 max_chunks_per_wave 切波（2+1）
    assert [(l, ids) for l, ids, _ in calls["load"]] == [
        (0, tuple(lp.chunk_ids[:2])),
        (0, (lp.chunk_ids[2],)),
    ]
    # dst first blocks 逐 chunk 对应（bpc=1 → block 号即块表位置）
    assert [fb for _, _, fb in calls["load"]] == [(5, 100), (357,)]
    # keys 重建与 plan_load 一致（unpin 有效性的前提）
    assert calls["complete_load"] == []

    w.wait_for_layer_load(L0)
    w.wait_for_layer_load(L1)  # 末层 → complete_load
    assert len(calls["complete_load"]) == 1


def test_wait_for_layer_load_progresses_layer_by_layer():
    w, e = make_worker()
    calls = record_engine(e)
    tokens = list(range(2 * CS))
    lp = prepare_load(e, tokens, 2)

    req = make_req("r1", tokens, [7, 9],
                   load_spec=LoadSpec("r1", 0, 2 * CS, True, lp.chunk_ids))
    w.start_load_kv(make_meta(req), None)
    assert [l for l, _, _ in calls["load"]] == [0]

    w.wait_for_layer_load(L0)  # 等 0 层 → 发第 1 层
    assert [l for l, _, _ in calls["load"]] == [0, 1]
    assert calls["complete_load"] == []
    assert e.index.pinned  # pin 仍在（complete_load 未到）

    w.wait_for_layer_load(L1)  # 末层 → complete_load（解除 pin）
    assert [l for l, _, _ in calls["load"]] == [0, 1]
    assert len(calls["complete_load"]) == 1
    # 重建的 keys 恰好解除 plan_load 的 pin（重建正确性）
    assert e.index.pinned == {}
    # 收割后请求状态清空（重复回调无副作用）
    w.wait_for_layer_load(L1)
    assert len(calls["complete_load"]) == 1


def test_wait_for_layer_load_without_loads_is_noop():
    w, e = make_worker()
    calls = record_engine(e)
    w.wait_for_layer_load(L0)
    w.wait_for_layer_load(L1)
    assert calls["load"] == [] and calls["complete_load"] == []


# ---------------------------------------------------------------- 验收 3：
# save_kv_layer → store_layer；get_finished 触发 complete_store


def test_save_kv_layer_and_get_finished_harvest():
    w, e = make_worker()
    calls = record_engine(e)
    tokens = list(range(1000, 1000 + 3 * CS))
    sp = e.plan_store(e.index.keys_for_tokens(tokens))
    assert sp is not None and len(sp.keys) == 3

    req = make_req("r2", tokens, [5, 100, 357],
                   save_spec=SaveSpec("r2", 0, True, sp.keys, sp.chunk_ids))
    meta = make_meta(req)

    w.save_kv_layer(L0, None, None, meta)
    assert [(l, ids) for l, ids, _ in calls["store"]] == [
        (0, tuple(sp.chunk_ids[:2])), (0, (sp.chunk_ids[2],)),
    ]
    assert [fb for _, _, fb in calls["store"]] == [(5, 100), (357,)]

    w.wait_for_save()  # no-op（可调用即可）
    assert calls["complete_store"] == []  # 未到末层不收割

    w.save_kv_layer(L1, None, None, meta)
    result = w.get_finished(set())
    assert result == (None, None)
    assert calls["complete_store"] == [(tuple(sp.keys), True)]
    assert set(sp.keys) <= set(e.index.stored)  # pending → stored
    assert not e.index.pending_store

    # 收割后重复 get_finished 无副作用
    assert w.get_finished(set()) == (None, None)
    assert len(calls["complete_store"]) == 1


def test_save_kv_layer_skips_non_saving_requests():
    w, e = make_worker()
    calls = record_engine(e)
    tokens = list(range(2 * CS))
    lp = prepare_load(e, tokens, 2)
    load_req = make_req("r1", tokens, [0, 1],
                        load_spec=LoadSpec("r1", 0, 2 * CS, True, lp.chunk_ids))
    save_req = make_req("r2", tokens, [2, 3],
                        save_spec=SaveSpec("r2", 0, False))  # can_save=False
    w.save_kv_layer(L0, None, None, make_meta(load_req, save_req))
    assert calls["store"] == []


# ---------------------------------------------------------------- 验收 4：
# store→load 数据往返（真 gather/scatter 钩子）


def test_store_load_roundtrip_with_real_hooks():
    e = make_engine()
    paged_src = {fb: bytes([(0x30 + fb + j) & 0xFF for j in range(CB)])
                 for fb in (5, 100, 357)}
    paged_dst: dict = {}

    def gather(plan, layer_idx, slots, first_blocks):
        st = e._staging
        for cid, slot, fb in zip(plan.chunk_ids, slots, first_blocks):
            seg = e.segment_bytes
            off = slot * seg
            st[off: off + seg] = paged_src[fb][layer_idx * seg: (layer_idx + 1) * seg]

    def scatter(plan, layer_idx, slots, first_blocks):
        st = e._staging
        for cid, slot, fb in zip(plan.chunk_ids, slots, first_blocks):
            seg = e.segment_bytes
            off = slot * seg
            paged_dst.setdefault(fb, bytearray(CB))[
                layer_idx * seg: (layer_idx + 1) * seg
            ] = bytes(st[off: off + seg])

    e.gather_fn, e.scatter_fn = gather, scatter
    w = WorkerImpl(cfg={"block_size": BS}, engine=e)
    w.register_kv_caches(LAYERS)

    # ---- store：3 chunk × 2 层（离散块 5/100/357）----
    tokens = list(range(3 * CS))
    sp = e.plan_store(e.index.keys_for_tokens(tokens))
    save_req = make_req("r1", tokens, [5, 100, 357],
                        save_spec=SaveSpec("r1", 0, True, sp.keys, sp.chunk_ids))
    meta = make_meta(save_req)
    w.save_kv_layer(L0, None, None, meta)
    w.save_kv_layer(L1, None, None, meta)
    w.wait_for_save()
    assert w.get_finished(set()) == (None, None)

    # ---- load：另一请求，离散目标块 200/300/400 ----
    lp = e.plan_load(sp.keys)
    load_req = make_req("r2", tokens, [200, 300, 400],
                        load_spec=LoadSpec("r2", 0, 3 * CS, True, lp.chunk_ids))
    w.start_load_kv(make_meta(load_req), None)
    w.wait_for_layer_load(L0)
    w.wait_for_layer_load(L1)

    assert bytes(paged_dst[200]) == paged_src[5]
    assert bytes(paged_dst[300]) == paged_src[100]
    assert bytes(paged_dst[400]) == paged_src[357]
    assert e.index.pinned == {}  # complete_load 已解除 pin


# ---------------------------------------------------------------- 验收 5：
# 背压：max_in_flight_layers=1 → 第二层发起前最老句柄已 wait


def test_backpressure_load_path():
    e = make_engine()
    log: list = []
    record_handles(e, log)
    w = WorkerImpl(cfg={"block_size": BS}, engine=e, max_in_flight_layers=1)
    w.register_kv_caches(LAYERS)

    tokens = list(range(2 * CS))
    lp = prepare_load(e, tokens, 2)
    req = make_req("r1", tokens, [0, 1],
                   load_spec=LoadSpec("r1", 0, 2 * CS, True, lp.chunk_ids))
    w.start_load_kv(make_meta(req), None)
    assert log == [("issue", 1)]  # 第 0 层发起

    w.wait_for_layer_load(L0)
    # 第 1 层（句柄 2）发起之前，最老句柄（1）已 wait
    assert log == [("issue", 1), ("wait", 1), ("issue", 2)]
    w.wait_for_layer_load(L1)


def test_backpressure_store_path():
    e = make_engine()
    log: list = []
    record_handles(e, log)
    w = WorkerImpl(cfg={"block_size": BS}, engine=e, max_in_flight_layers=1)
    w.register_kv_caches(LAYERS)

    tokens = list(range(2 * CS))
    sp = e.plan_store(e.index.keys_for_tokens(tokens))
    req = make_req("r1", tokens, [0, 1],
                   save_spec=SaveSpec("r1", 0, True, sp.keys, sp.chunk_ids))
    meta = make_meta(req)

    w.save_kv_layer(L0, None, None, meta)
    assert log == [("issue", 1)]
    w.save_kv_layer(L1, None, None, meta)  # 背压：先 wait 句柄 1 再发句柄 2
    assert log == [("issue", 1), ("wait", 1), ("issue", 2)]
    w.get_finished(set())


def test_backpressure_multi_request_load():
    """多请求同层并发：第二请求第 0 层发起前，第一请求句柄已 wait。"""
    e = make_engine()
    log: list = []
    record_handles(e, log)
    w = WorkerImpl(cfg={"block_size": BS}, engine=e, max_in_flight_layers=1)
    w.register_kv_caches(LAYERS)

    tokens = list(range(2 * CS))
    # 每请求各 plan_load 一次（scheduler 真实流程：pin 计数 2，
    # worker 按请求 complete_load 各解一次）
    lp_a = prepare_load(e, tokens, 2)
    lp_b = prepare_load(e, tokens, 2)
    ra = make_req("a", tokens, [0, 1],
                  load_spec=LoadSpec("a", 0, 2 * CS, True, lp_a.chunk_ids))
    rb = make_req("b", tokens, [2, 3],
                  load_spec=LoadSpec("b", 0, 2 * CS, True, lp_b.chunk_ids))
    w.start_load_kv(make_meta(ra, rb), None)
    assert log == [("issue", 1), ("wait", 1), ("issue", 2)]
    w.wait_for_layer_load(L0)
    w.wait_for_layer_load(L1)


# ---------------------------------------------------------------- 验收 6：
# 离散块与连续块的 engine 调用序列完全一致


def _drive_load(blocks):
    w, e = make_worker()
    calls = record_engine(e)
    tokens = list(range(3 * CS))
    lp = prepare_load(e, tokens, 3)
    req = make_req("r1", tokens, blocks,
                   load_spec=LoadSpec("r1", 0, 3 * CS, True, lp.chunk_ids))
    w.start_load_kv(make_meta(req), None)
    w.wait_for_layer_load(L0)
    w.wait_for_layer_load(L1)
    return calls, lp


def test_discrete_blocks_same_call_sequence():
    discrete_calls, lp = _drive_load([5, 100, 357])
    contiguous_calls, _ = _drive_load([0, 1, 2])

    strip_fb = lambda calls: [  # noqa: E731
        (kind, l, ids)
        for kind in ("load",)
        for (l, ids, _) in calls["load"]
    ]
    assert strip_fb(discrete_calls) == strip_fb(contiguous_calls)
    assert len(discrete_calls["complete_load"]) == 1
    # 块号纯透传（离散由 gather/scatter 吸收，本层零变换）
    assert [fb for _, _, fb in discrete_calls["load"]] == [
        (5, 100), (357,), (5, 100), (357,),
    ]
    assert [fb for _, _, fb in contiguous_calls["load"]] == [
        (0, 1), (2,), (0, 1), (2,),
    ]


# ---------------------------------------------------------------- 验收 7/8：
# 壳接线：WORKER 角色完整生命周期


def make_vllm_config(extra=None):
    extra = extra or {
        "tutti_engine_config": {
            "backend": "memory",
            "chunk_kv_bytes": CB,
            "capacity_bytes": NC * CB,
            "max_chunks_per_wave": MCW,
        },
        "chunk_size": CS,
    }
    return SimpleNamespace(
        cache_config=SimpleNamespace(block_size=BS),
        kv_transfer_config=SimpleNamespace(kv_connector_extra_config=extra),
    )


def test_full_connector_worker_lifecycle():
    """真 vllm 基类 isinstance + metadata 绑定 + 回调驱动一个完整步。"""
    conn = TuttiConnectorV1(make_vllm_config(), KVConnectorRole.WORKER)
    assert isinstance(conn, KVConnectorBase_V1)
    assert isinstance(conn, TuttiConnectorV1)
    engine = conn.engine

    conn.register_kv_caches(LAYERS)
    assert engine.staging_addr is not None
    assert engine.num_layers == NL

    calls = record_engine(engine)
    tokens = list(range(2 * CS))
    sp = engine.plan_store(engine.index.keys_for_tokens(tokens))
    save_req = make_req("r1", tokens, [8, 9],
                        save_spec=SaveSpec("r1", 0, True, sp.keys, sp.chunk_ids))

    conn.bind_connector_metadata(make_meta(save_req))
    conn.start_load_kv(None)             # 本步无 load → no-op
    conn.save_kv_layer(L0, None, None)   # kwargs 无 → metadata 走绑定
    conn.save_kv_layer(L1, None, None)
    conn.wait_for_save()
    assert conn.get_finished(set()) == (None, None)
    conn.clear_connector_metadata()

    assert [l for l, _, _ in calls["store"]] == [0, 1]
    assert set(sp.keys) <= set(engine.index.stored)

    # load 半步：绑定新 metadata 后逐层推进
    lp = engine.plan_load(sp.keys)
    load_req = make_req("r2", tokens, [10, 11],
                        load_spec=LoadSpec("r2", 0, 2 * CS, True, lp.chunk_ids))
    conn.bind_connector_metadata(make_meta(load_req))
    conn.start_load_kv(None)
    conn.wait_for_layer_load(L0)
    conn.wait_for_layer_load(L1)
    conn.clear_connector_metadata()
    assert engine.index.pinned == {}


def test_worker_metadata_unbound_is_defensive_empty():
    conn = TuttiConnectorV1(make_vllm_config(), KVConnectorRole.WORKER)
    conn.register_kv_caches(LAYERS)
    conn.start_load_kv(None)  # 未 bind metadata：不崩，空步
    conn.wait_for_save()
    assert conn.get_finished(set()) == (None, None)


def test_max_in_flight_layers_from_extra_config():
    extra = {
        "tutti_engine_config": {
            "backend": "memory",
            "chunk_kv_bytes": CB,
            "capacity_bytes": NC * CB,
            "max_chunks_per_wave": MCW,
        },
        "chunk_size": CS,
        "max_in_flight_layers": 3,
    }
    conn = TuttiConnectorV1(make_vllm_config(extra), KVConnectorRole.WORKER)
    assert conn._worker._max_in_flight == 3


# ---------------------------------------------------------------- 验收 9：
# 术语纪律


def test_worker_py_term_discipline():
    """worker.py 源码不含介质词汇（§0 铁律，大小写不敏感）。"""
    source = (_ROOT / "adapter" / "worker.py").read_text(encoding="utf-8")
    banned = re.compile(
        r"extent|shard|nvme|prp|fiemap|register_memory|io_granularity|_dma",
        re.IGNORECASE,
    )
    hits = sorted(set(m.group(0) for m in banned.finditer(source)))
    assert not hits, f"worker.py 出现介质词汇：{hits}"
