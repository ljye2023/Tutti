"""ChunkIndex 测试（纯逻辑零依赖）。

运行：
    cd /data/home/ryeqiu/Tutti/integration/vllm-connector && \
    python -m pytest tests/unit/ -v
"""

import re
import sys
from pathlib import Path

# engine 包位于 integration/vllm-connector/（本测试文件的上两级目录）
_CONNECTOR_ROOT = Path(__file__).resolve().parents[2]
if str(_CONNECTOR_ROOT) not in sys.path:
    sys.path.insert(0, str(_CONNECTOR_ROOT))

import pytest

from engine.chunk_index import CHUNK_SIZE, ChunkIndex, hash_chunk

CS = 4  # 小 chunk_size 方便构造（构造参数可覆盖）


def chunk_tokens(base: int, n: int = CS) -> list[int]:
    return list(range(base, base + n))


def key_of(base: int) -> bytes:
    return hash_chunk(tuple(range(base, base + CS)))


def fill(bi: ChunkIndex, bases: list[int]) -> list[bytes]:
    """allocate + complete_store 一批 chunk（base 为各 chunk 首 token）。"""
    keys = [key_of(b) for b in bases]
    alloc, evicted = bi.allocate(keys)
    assert evicted == []
    assert len(alloc) == len(keys)
    bi.complete_store(keys)
    return keys


# ---------- hash_chunk ----------

def test_hash_chunk_deterministic():
    assert hash_chunk((1, 2, 3)) == hash_chunk((1, 2, 3))
    assert hash_chunk(()) == hash_chunk(())
    k = hash_chunk((5, 6, 7))
    assert isinstance(k, bytes)
    assert len(k) == 16  # blake2b digest_size=16


def test_hash_chunk_distinct():
    assert hash_chunk((1, 2)) != hash_chunk((1, 2, 3))
    assert hash_chunk((1, 2)) != hash_chunk((12,))  # 无分隔歧义
    assert hash_chunk((1, 2)) != hash_chunk((2, 1))
    assert hash_chunk((0,)) != hash_chunk((-1,))


# ---------- hash_chunk 链式性质（D-007 / Rework-1） ----------

def test_hash_chunk_parent_default_empty():
    """parent=b"" 与无 parent 调用兼容；parent 影响结果。"""
    assert hash_chunk((1, 2, 3)) == hash_chunk((1, 2, 3), parent=b"")
    h_a = hash_chunk((7,))
    assert hash_chunk((1, 2, 3), parent=h_a) != hash_chunk((1, 2, 3))
    # 同 tokens + parent 必同 key
    assert hash_chunk((1, 2), parent=h_a) == hash_chunk((1, 2), parent=bytes(h_a))


def test_hash_chunk_chained_property():
    """H(prefix_a + b) == hash_chunk(b, parent=H_a)：链式指纹整个前缀。"""
    bi = ChunkIndex(paths=8, chunk_size=CS)
    a_tokens = list(range(2 * CS))          # 前缀 a：两个 chunk
    b_chunk = list(range(100, 100 + CS))    # 追加 chunk b
    full = bi.keys_for_tokens(a_tokens + b_chunk)

    keys_a, last_parent = bi.keys_and_last_parent(a_tokens)
    assert keys_a == full[:2]
    # H(a ‖ b) == hash_chunk(b, parent=H_a)
    assert hash_chunk(tuple(b_chunk), parent=last_parent) == full[2]
    # 增量续算与全量一致
    assert bi.keys_for_tokens(
        a_tokens + b_chunk, start_chunk=2, parent=last_parent) == full[2:]


# ---------- lookup_prefix ----------

def test_lookup_prefix_3_of_5_chunks():
    bi = ChunkIndex(paths=8, chunk_size=CS)
    tokens = list(range(5 * CS))
    keys = bi.keys_for_tokens(tokens)
    assert len(keys) == 5
    # 存前 3 个 chunk（链式 key：key_i 依赖 H_{i-1}）
    bi.allocate(keys[:3])
    bi.complete_store(keys[:3])
    assert bi.lookup_prefix(tokens) == 3 * CS


def test_lookup_prefix_stops_at_first_miss():
    bi = ChunkIndex(paths=8, chunk_size=CS)
    tokens = list(range(5 * CS))
    keys = bi.keys_for_tokens(tokens)
    # 只存 chunk0 与 chunk2（跳过 chunk1）
    bi.allocate([keys[0], keys[2]])
    bi.complete_store([keys[0], keys[2]])
    assert bi.lookup_prefix(tokens) == 1 * CS  # chunk1 未命中即停


def test_lookup_prefix_ignores_pending():
    bi = ChunkIndex(paths=8, chunk_size=CS)
    tokens = chunk_tokens(0)
    key = bi.keys_for_tokens(tokens)[0]
    bi.allocate([key])  # pending，未 complete_store
    assert bi.lookup_prefix(tokens) == 0


def test_lookup_prefix_empty_or_partial_input():
    bi = ChunkIndex(paths=8, chunk_size=CS)
    assert bi.lookup_prefix([]) == 0
    assert bi.lookup_prefix([1, 2, 3]) == 0  # 不满一个 chunk


# ---------- keys_for_tokens ----------

def test_keys_for_tokens_truncates_tail():
    bi = ChunkIndex(paths=8, chunk_size=CS)
    tokens = list(range(2 * CS + 2))  # 2 整 chunk + 2 个尾部 token
    keys = bi.keys_for_tokens(tokens)
    assert len(keys) == 2
    # 链式：H_0 的 parent 为 b""，H_1 的 parent 为 H_0
    assert keys[0] == hash_chunk(tuple(tokens[:CS]))
    assert keys[1] == hash_chunk(tuple(tokens[CS : 2 * CS]), parent=keys[0])


def test_keys_for_tokens_start_chunk_offset():
    bi = ChunkIndex(paths=8, chunk_size=CS)
    tokens = list(range(3 * CS))
    full = bi.keys_for_tokens(tokens)
    # 无 parent：在 start_chunk 处重新起链（parent=b""）
    assert bi.keys_for_tokens(tokens, start_chunk=1) == [
        hash_chunk(tuple(tokens[CS : 2 * CS])),
        hash_chunk(tuple(tokens[2 * CS :]),
                   parent=hash_chunk(tuple(tokens[CS : 2 * CS]))),
    ]
    assert bi.keys_for_tokens(tokens, start_chunk=2) == [
        hash_chunk(tuple(tokens[2 * CS :]))
    ]
    assert bi.keys_for_tokens(tokens, start_chunk=3) == []
    # 带 parent 的增量调用与全序列一致
    assert bi.keys_for_tokens(tokens, start_chunk=1, parent=full[0]) == full[1:]
    assert bi.keys_for_tokens(tokens, start_chunk=2, parent=full[1]) == full[2:]


def test_keys_and_last_parent_incremental():
    """增量接口：返回 (keys, last_parent)，空产出时原样回传 parent。"""
    bi = ChunkIndex(paths=8, chunk_size=CS)
    tokens = list(range(2 * CS + 2))  # 2 整 chunk + 尾部舍弃
    keys, last_parent = bi.keys_and_last_parent(tokens)
    assert keys == bi.keys_for_tokens(tokens)
    assert last_parent == keys[-1]

    # 不满一个 chunk：keys 空，last_parent 原样回传
    marker = b"\x01\x02\x03"
    empty_keys, back = bi.keys_and_last_parent([1, 2], parent=marker)
    assert empty_keys == []
    assert back == marker

    # 跨调用增量续算 == 全量
    more = tokens + list(range(100, 100 + CS))  # 3 整 chunk
    full_keys, full_last = bi.keys_and_last_parent(more)
    inc_keys, inc_last = bi.keys_and_last_parent(
        more, start_chunk=2, parent=last_parent)
    assert inc_keys == full_keys[2:]
    assert inc_last == full_last


# ---------- allocate / 驱逐 ----------

def test_allocate_basic_and_all_existing():
    bi = ChunkIndex(paths=4, chunk_size=CS)
    keys = fill(bi, [0, CS])
    assert len(bi.stored) == 2
    assert bi.pending_store == {}
    # 全已存在 → ([], [])
    assert bi.allocate(keys) == ([], [])
    # 混合：一个已存在、一个新
    k2 = key_of(2 * CS)
    alloc, evicted = bi.allocate([keys[0], k2])
    assert evicted == []
    assert len(alloc) == 1  # 只为新 key 分配
    bi.complete_store([k2])
    assert len(bi.stored) == 3


def test_allocate_evicts_lru_when_full():
    bi = ChunkIndex(paths=3, chunk_size=CS)
    k0, k1, k2 = key_of(0), key_of(CS), key_of(2 * CS)
    bi.allocate([k0, k1, k2])
    bi.complete_store([k0, k1, k2])
    assert list(bi.stored.keys()) == [k0, k1, k2]  # 头 = LRU
    k3 = key_of(3 * CS)
    alloc, evicted = bi.allocate([k3])
    assert evicted == [k0]  # LRU 头被驱逐
    assert k0 not in bi.stored
    assert len(alloc) == 1
    bi.complete_store([k3])
    assert bi.lookup_prefix(chunk_tokens(0)) == 0  # k0 数据已没了
    assert bi.lookup_prefix(chunk_tokens(CS)) == CS


def test_touch_protects_from_eviction():
    bi = ChunkIndex(paths=2, chunk_size=CS)
    k0, k1 = key_of(0), key_of(CS)
    bi.allocate([k0, k1])
    bi.complete_store([k0, k1])
    bi.touch([k0])  # k0 → MRU，k1 成为 LRU 头
    k2 = key_of(2 * CS)
    _, evicted = bi.allocate([k2])
    assert evicted == [k1]  # 被 touch 的 k0 不驱逐
    bi.complete_store([k2])
    assert k0 in bi.stored


def test_pinned_not_evicted():
    bi = ChunkIndex(paths=2, chunk_size=CS)
    k0, k1 = key_of(0), key_of(CS)
    bi.allocate([k0, k1])
    bi.complete_store([k0, k1])
    eids = bi.pin([k0])
    assert eids == [bi.stored[k0]]
    k2 = key_of(2 * CS)
    _, evicted = bi.allocate([k2])  # 只能驱逐 k1
    assert evicted == [k1]
    bi.complete_store([k2])
    bi.unpin([k0])
    # 解 pin 后 k0 变为 LRU 头（stored: k0, k2），可被驱逐
    k3 = key_of(3 * CS)
    _, evicted = bi.allocate([k3])
    assert evicted == [k0]
    bi.complete_store([k3])


def test_allocate_none_when_no_evictable():
    bi = ChunkIndex(paths=1, chunk_size=CS)
    k0 = fill(bi, [0])[0]
    bi.pin([k0])
    assert bi.allocate([key_of(CS)]) is None  # 唯一 path pinned
    assert list(bi.stored.keys()) == [k0]  # 状态未变
    bi.unpin([k0])


def test_allocate_none_leaves_state_intact():
    # 需 3 个新 path，可驱逐只有 2 个 → None 且零副作用
    bi = ChunkIndex(paths=3, chunk_size=CS)
    k0, k1, k2 = key_of(0), key_of(CS), key_of(2 * CS)
    bi.allocate([k0, k1, k2])
    bi.complete_store([k0, k1, k2])
    bi.pin([k0])
    before = list(bi.stored.keys())
    assert bi.allocate([key_of(10), key_of(20), key_of(30)]) is None
    assert list(bi.stored.keys()) == before  # k1/k2 未被误驱逐
    assert bi.free == []
    assert bi.pending_store == {}
    bi.unpin([k0])


# ---------- complete_store(success=False) ----------

def test_complete_store_failure_recycles_path():
    bi = ChunkIndex(paths=2, chunk_size=CS)
    k0 = key_of(0)
    alloc, _ = bi.allocate([k0])
    eid = alloc[0]
    assert bi.pinned == {eid: 1}
    bi.complete_store([k0], success=False)
    assert bi.stored == {}
    assert bi.pending_store == {}
    assert eid in bi.free  # path 回收
    assert bi.pinned == {}  # 解 pin
    assert bi.lookup_prefix(chunk_tokens(0)) == 0


# ---------- pin / unpin ----------

def test_pin_unpin_refcount():
    bi = ChunkIndex(paths=4, chunk_size=CS)
    k0 = fill(bi, [0])[0]
    e1 = bi.pin([k0])[0]
    e2 = bi.pin([k0])  # 重复 pin → refcount 2
    assert e2 == [e1]
    assert bi.pinned[e1] == 2
    bi.unpin([k0])
    assert bi.pinned[e1] == 1
    bi.unpin([k0])
    assert e1 not in bi.pinned


def test_pin_missing_raises_keyerror():
    bi = ChunkIndex(paths=4, chunk_size=CS)
    fill(bi, [0])
    with pytest.raises(KeyError):
        bi.pin([key_of(99)])
    # 部分 key 缺失 → 整体失败，不留半截 pin
    with pytest.raises(KeyError):
        bi.pin([key_of(0), key_of(98)])
    assert bi.pinned == {}


def test_unpin_not_pinned_raises():
    bi = ChunkIndex(paths=4, chunk_size=CS)
    k0 = fill(bi, [0])[0]
    with pytest.raises(ValueError):
        bi.unpin([k0])  # 从未 pin


# ---------- reset / 属性 ----------

def test_reset_clears_all():
    bi = ChunkIndex(paths=3, chunk_size=CS)
    k0 = fill(bi, [0, CS])[0]
    bi.pin([k0])
    bi.allocate([key_of(5 * CS)])  # 留一个 pending
    assert bi.pending_store != {}
    assert bi.pinned != {}
    bi.reset()
    assert bi.stored == {}
    assert bi.pinned == {}
    assert bi.pending_store == {}
    assert bi.free == ["slot://0", "slot://1", "slot://2"]
    # reset 后可继续正常使用
    k = fill(bi, [0])[0]
    assert k in bi.stored


def test_num_paths_and_chunk_size_constants():
    assert ChunkIndex(paths=7, chunk_size=CS).capacity == 7
    assert CHUNK_SIZE == 256


def test_constructor_validation():
    with pytest.raises(ValueError):
        ChunkIndex(paths=0, chunk_size=CS)
    with pytest.raises(ValueError):
        ChunkIndex(paths=4, chunk_size=0)


# ---------- 零依赖 ----------

def test_no_heavy_imports_in_sys_modules():
    """chunk_index 自净：子进程隔离验证（同进程其他测试可能已 import torch）。"""
    import subprocess

    code = (
        "import sys; sys.path.insert(0, %r); "
        "import engine.chunk_index; "
        "bad = [m for m in ('vllm', 'torch', 'tutti_runtime', 'numpy') "
        "if m in sys.modules]; "
        "assert not bad, f'heavy imports: {bad}'"
    ) % str(_CONNECTOR_ROOT)
    subprocess.run([sys.executable, "-c", code], check=True)


def test_chunk_index_module_imports_stdlib_only():
    import engine.chunk_index as bi_mod

    src = Path(bi_mod.__file__).read_text(encoding="utf-8")
    imported = set(re.findall(r"^\s*(?:import|from)\s+([A-Za-z_][A-Za-z0-9_]*)", src, re.M))
    assert imported <= {"hashlib", "collections"}, imported
