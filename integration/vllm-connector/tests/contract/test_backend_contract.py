"""StorageBackend 契约套件（T-116）。

任何 StorageBackend 实现都应通过本套件；新增实现时在
BACKEND_FACTORIES 注册构造工厂即可（R1：MemoryBackend）。
"""

import ctypes

import pytest

import sys
import tempfile
from pathlib import Path

from engine.backend import StorageBackend
from engine.local_store_backend import LocalStoreBackend
from engine.memory_backend import MemoryBackend

_CONTRACT_DIR = Path(__file__).resolve().parent
if str(_CONTRACT_DIR) not in sys.path:
    sys.path.insert(0, str(_CONTRACT_DIR))

from fakes import ByteLevelFakeRuntime

NUM_LAYERS = 2
SEG = 16
CHUNK_BYTES = SEG * NUM_LAYERS
NUM_CHUNKS = 4
NUM_SLOTS = 4


def memory_backend_factory():
    return MemoryBackend(num_chunks=NUM_CHUNKS, chunk_bytes=CHUNK_BYTES)


def local_store_backend_factory():
    """LocalStoreBackend：注入字节级 fake runtime（即时完成、真搬数据）。"""
    return LocalStoreBackend(
        root=tempfile.mkdtemp(prefix="contract-ls-"),
        num_chunks=NUM_CHUNKS,
        chunk_bytes=CHUNK_BYTES,
        runtime=ByteLevelFakeRuntime(),
    )


BACKEND_FACTORIES = {
    "memory": memory_backend_factory,
    "local_store": local_store_backend_factory,
}


@pytest.fixture(params=sorted(BACKEND_FACTORIES))
def make_backend(request):
    return BACKEND_FACTORIES[request.param]


@pytest.fixture
def bound(make_backend):
    """(backend, staging)：已 bind_staging 的后端 + ctypes staging 缓冲。"""
    backend = make_backend()
    staging = ctypes.create_string_buffer(NUM_SLOTS * SEG)
    backend.bind_staging(
        ctypes.addressof(staging), CHUNK_BYTES, NUM_SLOTS, SEG, None
    )
    return backend, staging


# ---------- SPI 形态 ----------


def test_conforms_to_storage_backend_spi(make_backend):
    assert isinstance(make_backend(), StorageBackend)


# ---------- 路径池（D-011：backend 生成并解释） ----------


def test_chunk_paths_contract(make_backend):
    backend = make_backend()
    paths = backend.chunk_paths(NUM_CHUNKS)
    assert len(paths) == NUM_CHUNKS
    assert len(set(paths)) == NUM_CHUNKS
    assert all(isinstance(p, str) and p for p in paths)
    with pytest.raises(ValueError):
        backend.chunk_paths(0)
    with pytest.raises(ValueError):
        backend.chunk_paths(NUM_CHUNKS + 1)


def test_backend_issued_paths_are_usable(bound):
    """backend 自产的路径必须被自身 write/read 接受（engine 只透传）。"""
    backend, staging = bound
    paths = backend.chunk_paths(NUM_CHUNKS)
    data = bytes(range(SEG))
    staging[0:SEG] = data
    backend.write_chunk_segment(paths[0], 0, 0).wait()
    staging[0:SEG] = b"\x00" * SEG
    backend.read_chunk_segment(paths[0], 0, 0).wait()
    assert bytes(staging[0:SEG]) == data


# ---------- 生命周期校验 ----------


def test_ops_before_bind_staging_raise(make_backend):
    backend = make_backend()
    path = backend.chunk_paths(1)[0]  # 路径从不硬编码：backend 自产（opaque）
    with pytest.raises(RuntimeError):
        backend.write_chunk_segment(path, 0, 0)
    with pytest.raises(RuntimeError):
        backend.read_chunk_segment(path, 0, 0)


def test_bind_staging_validates_arguments(make_backend):
    backend = make_backend()
    staging = ctypes.create_string_buffer(NUM_SLOTS * SEG)
    addr = ctypes.addressof(staging)
    # chunk_bytes 与池不匹配
    with pytest.raises(ValueError):
        backend.bind_staging(addr, CHUNK_BYTES + 1, NUM_SLOTS, SEG, None)
    # segment_bytes 不整除 chunk_bytes
    with pytest.raises(ValueError):
        backend.bind_staging(addr, CHUNK_BYTES, NUM_SLOTS, SEG + 1, None)
    # 非法 num_slots / segment_bytes / staging_addr
    with pytest.raises(ValueError):
        backend.bind_staging(addr, CHUNK_BYTES, 0, SEG, None)
    with pytest.raises(ValueError):
        backend.bind_staging(addr, CHUNK_BYTES, NUM_SLOTS, 0, None)
    with pytest.raises(ValueError):
        backend.bind_staging(0, CHUNK_BYTES, NUM_SLOTS, SEG, None)


# ---------- 段搬运往返 ----------


def test_write_then_read_roundtrip(bound):
    backend, staging = bound
    paths = backend.chunk_paths(NUM_CHUNKS)
    data = bytes(range(SEG))
    staging[0:SEG] = data
    h = backend.write_chunk_segment(paths[1], layer_idx=1, slot=0)
    h.wait()
    # 清空 staging 证明 read 是真拷贝（数据只能来自池）
    staging[0:SEG] = b"\x00" * SEG
    h2 = backend.read_chunk_segment(paths[1], layer_idx=1, slot=0)
    h2.wait()
    assert bytes(staging[0:SEG]) == data


def test_layer_segments_do_not_overlap(bound):
    backend, staging = bound
    path = backend.chunk_paths(NUM_CHUNKS)[2]
    l0 = bytes([0xAA]) * SEG
    l1 = bytes([0x55]) * SEG
    staging[0:SEG] = l0
    backend.write_chunk_segment(path, layer_idx=0, slot=0).wait()
    staging[0:SEG] = l1
    backend.write_chunk_segment(path, layer_idx=1, slot=0).wait()
    # 读层 0 不得碰层 1（packed 布局：层段 offset = layer_idx × segment_bytes）
    staging[0:SEG] = b"\x00" * SEG
    backend.read_chunk_segment(path, layer_idx=0, slot=0).wait()
    assert bytes(staging[0:SEG]) == l0
    staging[0:SEG] = b"\x00" * SEG
    backend.read_chunk_segment(path, layer_idx=1, slot=0).wait()
    assert bytes(staging[0:SEG]) == l1


# ---------- 参数校验 ----------


def test_invalid_chunk_id_raises(bound):
    backend, _ = bound
    # 非法形态：任何 backend 都必须拒绝非自产路径（opaque 契约）
    for bad in ("slot://1", "bogus://x", "", 3, None, "file://a/b"):
        with pytest.raises(ValueError):
            backend.write_chunk_segment(bad, 0, 0)
        with pytest.raises(ValueError):
            backend.read_chunk_segment(bad, 0, 0)
    # 自产形态但越界：把末位数字改成超容量值
    import re

    issued = backend.chunk_paths(NUM_CHUNKS)[0]
    out_of_pool = re.sub(r"\d+", str(NUM_CHUNKS + 100), issued)
    with pytest.raises(ValueError):
        backend.write_chunk_segment(out_of_pool, 0, 0)


def test_invalid_layer_or_slot_raises(bound):
    backend, _ = bound
    path = backend.chunk_paths(1)[0]
    with pytest.raises(ValueError):
        backend.write_chunk_segment(path, layer_idx=NUM_LAYERS, slot=0)
    with pytest.raises(ValueError):
        backend.write_chunk_segment(path, layer_idx=-1, slot=0)
    with pytest.raises(ValueError):
        backend.write_chunk_segment(path, layer_idx=0, slot=NUM_SLOTS)


# ---------- wait_idle / shutdown ----------


def test_wait_idle_and_shutdown_idempotent(bound):
    backend, _ = bound
    backend.write_chunk_segment(backend.chunk_paths(1)[0], 0, 0).wait()
    backend.wait_idle()
    backend.wait_idle()
    backend.shutdown()
    backend.shutdown()  # 幂等


def test_handle_query_true_after_sync_op(bound):
    backend, staging = bound
    staging[0:SEG] = b"\x01" * SEG
    h = backend.write_chunk_segment(backend.chunk_paths(1)[0], 0, 0)
    assert h.query() is True
    h.synchronize()


# ---------- MemoryBackend 形态 ----------


def test_memory_pool_form():
    backend = MemoryBackend(num_chunks=2, chunk_bytes=CHUNK_BYTES)
    pool = backend.pool
    try:
        import torch

        assert isinstance(pool, torch.Tensor)
        assert pool.dtype == torch.uint8
        assert pool.numel() == 2 * CHUNK_BYTES
    except ImportError:  # pragma: no cover
        assert isinstance(pool, bytearray)
        assert len(pool) == 2 * CHUNK_BYTES


def test_memory_backend_exposes_geometry():
    backend = MemoryBackend(num_chunks=3, chunk_bytes=CHUNK_BYTES)
    assert backend.num_chunks == 3
    assert backend.chunk_bytes == CHUNK_BYTES
