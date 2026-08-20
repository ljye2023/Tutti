"""LocalStoreBackend 专属测试（T-115 验收）。

覆盖任务卡验收条目：
- bind_staging：register_memory 恰 1 次 + io_granularity==segment_bytes
- e2e：store → load 数据一致（字节级 fake）+ stub runtime 状态机
- partial-commit：被拒窗口化重发；连续两轮零接受 → RuntimeError
- 术语纪律：既有 engine 层文件零介质词（test_term_discipline.py）
- 真机 1GiB 写读（GPU+盘就绪时；否则 skip 并打印原因）
"""

import ctypes
import json
import os
import sys
import time

import pytest

from engine.local_store_backend import LocalStoreBackend
from fakes import ByteLevelFakeRuntime

NUM_LAYERS = 2
SEG = 16
CHUNK_BYTES = SEG * NUM_LAYERS
NUM_CHUNKS = 4
NUM_SLOTS = 4


def make_backend(runtime, root, num_chunks=NUM_CHUNKS, chunk_bytes=CHUNK_BYTES):
    return LocalStoreBackend(
        root=root, num_chunks=num_chunks, chunk_bytes=chunk_bytes, runtime=runtime
    )


def make_bound(runtime, root):
    backend = make_backend(runtime, root)
    staging = ctypes.create_string_buffer(NUM_SLOTS * SEG)
    backend.bind_staging(
        ctypes.addressof(staging), CHUNK_BYTES, NUM_SLOTS, SEG, None
    )
    return backend, staging


# ---------- SPI 契约（细节见 tests/tutti_engine 契约套件） ----------


def test_chunk_paths_logical_ids(tmp_path):
    runtime = ByteLevelFakeRuntime()
    backend = make_backend(runtime, str(tmp_path))
    paths = backend.chunk_paths(NUM_CHUNKS)
    assert paths == [f"chunk://local/{i}" for i in range(NUM_CHUNKS)]
    with pytest.raises(ValueError):
        backend.chunk_paths(0)
    with pytest.raises(ValueError):
        backend.chunk_paths(NUM_CHUNKS + 1)


def test_constructor_validates_arguments(tmp_path):
    runtime = ByteLevelFakeRuntime()
    with pytest.raises(ValueError):
        LocalStoreBackend(root="", num_chunks=1, chunk_bytes=1, runtime=runtime)
    with pytest.raises(ValueError):
        LocalStoreBackend(root=str(tmp_path), num_chunks=0, chunk_bytes=1,
                          runtime=runtime)
    with pytest.raises(ValueError):
        LocalStoreBackend(root=str(tmp_path), num_chunks=1, chunk_bytes=0,
                          runtime=runtime)


def test_backend_creates_chunk_files(tmp_path):
    """介质资源私有化：构造时自建 <root>/chunks/<i>.bin。"""
    runtime = ByteLevelFakeRuntime()
    make_backend(runtime, str(tmp_path), num_chunks=2)
    chunk_dir = tmp_path / "chunks"
    assert sorted(p.name for p in chunk_dir.iterdir()) == ["0.bin", "1.bin"]


# ---------- 验收 3：bind_staging / register_memory 恰 1 次 ----------


def test_register_memory_exactly_once_with_granularity(tmp_path):
    runtime = ByteLevelFakeRuntime()
    staging = ctypes.create_string_buffer(NUM_SLOTS * SEG)
    backend = make_backend(runtime, str(tmp_path))
    addr = ctypes.addressof(staging)

    backend.bind_staging(addr, CHUNK_BYTES, NUM_SLOTS, SEG, None)

    assert len(runtime.register_calls) == 1
    call = runtime.register_calls[0]
    assert call[0] == addr
    assert call[1] == NUM_SLOTS * SEG
    assert call[2] == "host"  # 实测偏差：stub 为 host-only（见模块 docstring）
    assert call[4] == SEG  # io_granularity == segment_bytes


def test_repeat_bind_staging_raises(tmp_path):
    runtime = ByteLevelFakeRuntime()
    backend, _ = make_bound(runtime, str(tmp_path))
    staging = ctypes.create_string_buffer(NUM_SLOTS * SEG)
    with pytest.raises(RuntimeError):
        backend.bind_staging(
            ctypes.addressof(staging), CHUNK_BYTES, NUM_SLOTS, SEG, None
        )
    # 且未发生第二次注册
    assert len(runtime.register_calls) == 1


def test_caps_alignment_violation_raises(tmp_path):
    runtime = ByteLevelFakeRuntime().with_caps(target_alignment=32)
    backend = make_backend(runtime, str(tmp_path))
    staging = ctypes.create_string_buffer(NUM_SLOTS * SEG)
    with pytest.raises(RuntimeError, match="caps"):
        backend.bind_staging(
            ctypes.addressof(staging), CHUNK_BYTES, NUM_SLOTS, SEG, None
        )
    assert runtime.register_calls == []


# ---------- 验收 4：e2e 数据一致 + 请求描述符 ----------


def test_e2e_write_read_data_consistency(tmp_path):
    runtime = ByteLevelFakeRuntime()
    backend, staging = make_bound(runtime, str(tmp_path))
    paths = backend.chunk_paths(NUM_CHUNKS)

    pattern_a = bytes(range(SEG))
    pattern_b = b"\xAB" * SEG

    staging[0:SEG] = pattern_a
    backend.write_chunk_segment(paths[1], layer_idx=0, slot=0).wait()
    staging[SEG:2 * SEG] = pattern_b  # slot=1 的槽
    backend.write_chunk_segment(paths[1], layer_idx=1, slot=1).wait()
    backend.write_chunk_segment(paths[3], layer_idx=1, slot=2).wait()

    # 请求描述符：packed 层段 + 槽布局 + 固定粒度
    (target, t_off, mem, m_off, length, direction), *_ = runtime.submit_calls[0]
    assert t_off == 0 * SEG and m_off == 0 * SEG
    assert length == SEG and direction == "write"
    t1, t_off1, m1, m_off1, l1, d1 = runtime.submit_calls[1][0]
    assert t1 == target and t_off1 == 1 * SEG and m_off1 == 1 * SEG

    # 清空 staging 证明 read 是真拷贝（数据只能来自池）
    staging[0:NUM_SLOTS * SEG] = b"\x00" * (NUM_SLOTS * SEG)
    backend.read_chunk_segment(paths[1], layer_idx=0, slot=0).wait()
    assert bytes(staging[0:SEG]) == pattern_a
    backend.read_chunk_segment(paths[1], layer_idx=1, slot=1).wait()
    assert bytes(staging[SEG:2 * SEG]) == pattern_b
    # 未写过的段读出为零
    backend.read_chunk_segment(paths[3], layer_idx=0, slot=3).wait()
    assert bytes(staging[3 * SEG:4 * SEG]) == b"\x00" * SEG


def test_write_read_state_machine_with_stub_runtime(tmp_path):
    """真 tutti_runtime stub（T-101 测试模式）：submit → force → COMPLETED。"""
    tutti_runtime = pytest.importorskip("tutti_runtime")
    try:
        runtime = tutti_runtime.make_stub_runtime()
        backend, staging = make_bound(runtime, str(tmp_path))
        paths = backend.chunk_paths(NUM_CHUNKS)

        staging[0:SEG] = bytes(range(SEG))
        handle = backend.write_chunk_segment(paths[0], 0, 0)
        assert handle.query() is False  # 在途（stub 不自动完成）

        for ticket in handle.io_tickets:
            runtime.testing_force_complete(ticket)
        assert handle.query() is True  # 非阻塞探测：终态已到
        handle.wait()  # COMPLETED（wait 后句柄释放，query 不再有定义）

        read_handle = backend.read_chunk_segment(paths[0], 0, 0)
        for ticket in read_handle.io_tickets:
            runtime.testing_force_complete(ticket)
        read_handle.wait()
        backend.wait_idle()
        backend.shutdown()
        backend.shutdown()  # 幂等
    finally:
        # 避免污染同进程后续测试的 sys.modules 断言（test_engine 依赖）
        sys.modules.pop("tutti_runtime", None)


def test_handle_query_true_after_immediate_completion(tmp_path):
    runtime = ByteLevelFakeRuntime()
    backend, staging = make_bound(runtime, str(tmp_path))
    paths = backend.chunk_paths(NUM_CHUNKS)
    staging[0:SEG] = b"\x01" * SEG
    handle = backend.write_chunk_segment(paths[0], 0, 0)
    assert handle.query() is True
    handle.synchronize()


# ---------- 验收 5：partial-commit 窗口化重发 ----------


def _write_request_fingerprint(backend, path, layer_idx, slot):
    seg = SEG
    index = int(path.rsplit("/", 1)[-1])
    return (
        backend._target_tickets[index], layer_idx * seg, backend._mem_ticket,
        slot * seg, seg, "write",
    )


def test_partial_commit_retries_rejected_subset(tmp_path):
    """第一轮拒 1 条 → 第二轮仅补交被拒请求，成功。"""
    runtime_probe = ByteLevelFakeRuntime()
    backend, staging = make_bound(runtime_probe, str(tmp_path))
    paths = backend.chunk_paths(NUM_CHUNKS)
    fingerprint = _write_request_fingerprint(backend, paths[0], 0, 0)
    backend.shutdown()

    runtime = ByteLevelFakeRuntime(reject_plan=[{fingerprint}])
    backend, staging = make_bound(runtime, str(tmp_path))
    staging[0:SEG] = bytes(range(SEG))
    backend.write_chunk_segment(paths[0], 0, 0).wait()

    assert len(runtime.submit_calls) == 2  # 首轮 + 补交轮
    assert runtime.submit_calls[0] == [fingerprint]
    assert runtime.submit_calls[1] == [fingerprint]  # 仅被拒子集
    assert runtime.file_bytes(backend._target_tickets[0])[0:SEG] == bytes(range(SEG))


def test_partial_commit_two_zero_rounds_raises(tmp_path):
    """连续两轮零接受 → RuntimeError。"""
    runtime_probe = ByteLevelFakeRuntime()
    backend, _ = make_bound(runtime_probe, str(tmp_path))
    paths = backend.chunk_paths(NUM_CHUNKS)
    fingerprint = _write_request_fingerprint(backend, paths[0], 0, 0)
    backend.shutdown()

    runtime = ByteLevelFakeRuntime(
        reject_plan=[{fingerprint}, {fingerprint}]
    )
    backend, staging = make_bound(runtime, str(tmp_path))
    with pytest.raises(RuntimeError, match="零接受"):
        backend.write_chunk_segment(paths[0], 0, 0)
    assert len(runtime.submit_calls) == 2  # 恰两轮后放弃


# ---------- runtime=None 默认构造 ----------


def test_default_runtime_requires_preset(tmp_path, monkeypatch):
    monkeypatch.delenv("TUTTI_NVME_PRESET", raising=False)
    with pytest.raises(RuntimeError, match="TUTTI_NVME_PRESET"):
        LocalStoreBackend(root=str(tmp_path), num_chunks=1, chunk_bytes=CHUNK_BYTES)


def test_default_runtime_reads_preset_from_env(tmp_path, monkeypatch):
    """preset JSON（无效设备值即可）：只验证转交路径，不要求真设备。"""
    monkeypatch.setenv("TUTTI_NVME_PRESET", json.dumps({
        "device": {"ssnvme_path": "/dev/nonexistent"}, "gpu_id": 0,
    }))
    tutti_runtime = pytest.importorskip("tutti_runtime")
    orig = tutti_runtime.make_local_nvme_runtime
    calls = []
    tutti_runtime.make_local_nvme_runtime = lambda preset: calls.append(preset)
    try:
        with pytest.raises(Exception):
            # open_batch 会因 preset 未真初始化 runtime 而失败——只关心 preset 已转交
            LocalStoreBackend(root=str(tmp_path), num_chunks=1,
                              chunk_bytes=CHUNK_BYTES)
    finally:
        tutti_runtime.make_local_nvme_runtime = orig
        sys.modules.pop("tutti_runtime", None)
    assert calls and calls[0]["device"]["ssnvme_path"] == "/dev/nonexistent"


# ---------- 验收 7：真机 1GiB 写读（GPU+盘就绪时，否则 skip） ----------


def _real_preset():
    raw = os.environ.get("TUTTI_NVME_PRESET")
    if not raw:
        return None
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return None


def test_real_machine_roundtrip_1gib(tmp_path):
    if _real_preset() is None:
        pytest.skip("真机未就绪：未设置 TUTTI_NVME_PRESET（JSON preset）")
    tutti_runtime = pytest.importorskip("tutti_runtime")
    preset = _real_preset()
    ssnvme = preset.get("device", {}).get("ssnvme_path", "")
    if not (ssnvme and os.path.exists(ssnvme)):
        pytest.skip(f"真机未就绪：{ssnvme!r} 不存在")

    runtime = tutti_runtime.make_local_nvme_runtime(preset)

    # 1GiB 池：64 chunks × 16MiB（4 层 × 4MiB 段）
    chunk_bytes = 16 * 1024 * 1024
    num_chunks = 64
    seg = chunk_bytes // 4
    num_slots = 4

    root = str(tmp_path / "realstore")
    backend = LocalStoreBackend(
        root=root, num_chunks=num_chunks, chunk_bytes=chunk_bytes, runtime=runtime
    )
    staging = ctypes.create_string_buffer(num_slots * seg)
    backend.bind_staging(
        ctypes.addressof(staging), chunk_bytes, num_slots, seg, None
    )
    paths = backend.chunk_paths(num_chunks)

    pattern = bytes((i * 7 + 3) & 0xFF for i in range(seg))
    zero = b"\x00" * seg

    t0 = time.perf_counter()
    for chunk in range(num_chunks):
        for layer in range(4):
            staging[0:seg] = pattern
            backend.write_chunk_segment(paths[chunk], layer, 0).wait()
    t_write = time.perf_counter() - t0

    t1 = time.perf_counter()
    for chunk in range(num_chunks):
        for layer in range(4):
            staging[0:seg] = zero
            backend.read_chunk_segment(paths[chunk], layer, 0).wait()
            assert bytes(staging[0:seg]) == pattern, f"数据不一致 @chunk{chunk}L{layer}"
    t_read = time.perf_counter() - t1

    total = num_chunks * chunk_bytes
    print(f"\n[真机 1GiB] 写 {total / 1e9:.2f}GB @ {total / t_write / 1e9:.2f} GB/s；"
          f"读 {total / 1e9:.2f}GB @ {total / t_read / 1e9:.2f} GB/s（含校验）")
    backend.shutdown()
