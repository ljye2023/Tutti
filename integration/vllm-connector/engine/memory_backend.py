"""MemoryBackend — StorageBackend 的内存实现（D-010/D-011，T-116）。

staging 槽 ↔ 内存 chunk 池 的段搬运，使 engine 及以上全部单测脱离
真实存储介质（ARCHITECTURE.md §2 原则 6：无盘可测）。

- chunk 池：有 torch 时为 ``torch.Tensor``（R1 为 CPU uint8 张量，
  GPU 化随 T-117 kernels 接线）；无 torch 时退化为 ``bytearray`` 模拟。
- 段搬运：bytes copy 语义（``ctypes.memmove`` 经 ``data_ptr``/缓冲
  地址直拷，对 tensor 等价于 tensor copy 的字节效果）。R1 同步完成，
  返回已完成句柄（wait/query 即刻成功）。
- chunk_id := ``mem://slot/<i>``（engine 侧 ChunkIndex 路径池生成），
  槽号 i 即池内第 i 个 chunk；层段 offset = ``layer_idx ×
  segment_bytes``（D-011 packed 布局）。
"""

import ctypes

_MEM_SLOT_PREFIX = "mem://slot/"


def _parse_mem_slot(chunk_id) -> int:
    """解析 ``mem://slot/<i>`` → 槽号 i；非法形态抛 ValueError。"""
    if not isinstance(chunk_id, str) or not chunk_id.startswith(_MEM_SLOT_PREFIX):
        raise ValueError(
            f"MemoryBackend: invalid chunk_id {chunk_id!r} (expect 'mem://slot/<i>')"
        )
    suffix = chunk_id[len(_MEM_SLOT_PREFIX) :]
    try:
        return int(suffix)
    except ValueError:
        raise ValueError(
            f"MemoryBackend: invalid slot number in chunk_id {chunk_id!r}"
        ) from None


class _SyncHandle:
    """同步搬运的完成句柄（R1 内存搬运即时完成）。"""

    def wait(self) -> None:
        pass

    def synchronize(self) -> None:
        pass

    def query(self) -> bool:
        return True


class MemoryBackend:
    """内存 chunk 池后端（StorageBackend SPI 实现）。"""

    def __init__(self, num_chunks: int, chunk_bytes: int):
        if num_chunks < 1:
            raise ValueError(f"num_chunks must be >= 1, got {num_chunks}")
        if chunk_bytes < 1:
            raise ValueError(f"chunk_bytes must be >= 1, got {chunk_bytes}")
        self._num_chunks = num_chunks
        self._chunk_bytes = chunk_bytes
        try:
            import torch

            self._torch = torch
        except ImportError:  # pragma: no cover - 环境相关
            self._torch = None

        total = num_chunks * chunk_bytes
        if self._torch is not None:
            # R1：CPU uint8 张量；GPU 化随 T-117（真 kernel + cudaMemcpyAsync）
            self._pool_tensor = self._torch.empty(total, dtype=self._torch.uint8)
            self._pool_bytearray = None
            self._pool_addr = self._pool_tensor.data_ptr()
        else:
            self._pool_tensor = None
            self._pool_bytearray = bytearray(total)
            # from_buffer 为共享可写视图；不 resize 则地址稳定
            self._pool_addr = ctypes.addressof(
                (ctypes.c_char * total).from_buffer(self._pool_bytearray)
            )
        # bind_staging 后填
        self._staging_addr = None
        self._num_slots = None
        self._segment_bytes = None

    # ---------- 属性 ----------

    @property
    def num_chunks(self) -> int:
        return self._num_chunks

    @property
    def chunk_bytes(self) -> int:
        return self._chunk_bytes

    @property
    def pool(self):
        """chunk 池载体（torch.Tensor 或 bytearray；shutdown 后为 None）。"""
        if self._pool_tensor is not None:
            return self._pool_tensor
        return self._pool_bytearray

    # ---------- SPI ----------

    def chunk_paths(self, num_chunks: int) -> "list[str]":
        """路径池：``mem://slot/<i>``（槽号 i 即池内第 i 个 chunk）。"""
        if (
            not isinstance(num_chunks, int)
            or isinstance(num_chunks, bool)
            or not 1 <= num_chunks <= self._num_chunks
        ):
            raise ValueError(
                f"num_chunks must be in [1, {self._num_chunks}], got {num_chunks!r}"
            )
        return [f"{_MEM_SLOT_PREFIX}{i}" for i in range(num_chunks)]

    def bind_staging(
        self,
        staging_addr: int,
        chunk_bytes: int,
        num_slots: int,
        segment_bytes: int,
        io_stream,
    ) -> None:
        if not isinstance(staging_addr, int) or staging_addr <= 0:
            raise ValueError(f"staging_addr must be a positive int, got {staging_addr!r}")
        if chunk_bytes != self._chunk_bytes:
            raise ValueError(
                f"chunk_bytes mismatch: backend pool is {self._chunk_bytes}, "
                f"staging declared {chunk_bytes}"
            )
        if num_slots < 1:
            raise ValueError(f"num_slots must be >= 1, got {num_slots}")
        if segment_bytes < 1:
            raise ValueError(f"segment_bytes must be >= 1, got {segment_bytes}")
        if chunk_bytes % segment_bytes != 0:
            raise ValueError(
                f"chunk_bytes ({chunk_bytes}) must be a multiple of "
                f"segment_bytes ({segment_bytes})"
            )
        self._staging_addr = staging_addr
        self._num_slots = num_slots
        self._segment_bytes = segment_bytes

    def write_chunk_segment(self, chunk_id, layer_idx: int, slot: int) -> object:
        """staging[slot] 的 layer 段 → chunk 池（同步，返回已完成句柄）。"""
        i = self._check_request(chunk_id, layer_idx, slot)
        seg = self._segment_bytes
        dst = self._pool_addr + i * self._chunk_bytes + layer_idx * seg
        src = self._staging_addr + slot * seg
        ctypes.memmove(dst, src, seg)
        return _SyncHandle()

    def read_chunk_segment(self, chunk_id, layer_idx: int, slot: int) -> object:
        """chunk 池的 layer 段 → staging[slot]（同步，返回已完成句柄）。"""
        i = self._check_request(chunk_id, layer_idx, slot)
        seg = self._segment_bytes
        src = self._pool_addr + i * self._chunk_bytes + layer_idx * seg
        dst = self._staging_addr + slot * seg
        ctypes.memmove(dst, src, seg)
        return _SyncHandle()

    def wait_idle(self) -> None:
        """同步搬运：无在途操作。"""

    def shutdown(self) -> None:
        self._pool_tensor = None
        self._pool_bytearray = None
        self._pool_addr = 0
        self._staging_addr = None

    # ---------- 内部 ----------

    def _check_request(self, chunk_id, layer_idx: int, slot: int) -> int:
        if self._staging_addr is None:
            raise RuntimeError("MemoryBackend: bind_staging() not called yet")
        if not isinstance(layer_idx, int) or layer_idx < 0:
            raise ValueError(f"layer_idx must be a non-negative int, got {layer_idx!r}")
        if not isinstance(slot, int) or slot < 0:
            raise ValueError(f"slot must be a non-negative int, got {slot!r}")
        i = _parse_mem_slot(chunk_id)
        if i >= self._num_chunks:
            raise ValueError(f"chunk slot {i} out of range (num_chunks={self._num_chunks})")
        if layer_idx * self._segment_bytes + self._segment_bytes > self._chunk_bytes:
            raise ValueError(
                f"layer_idx {layer_idx} out of range "
                f"(chunk holds {self._chunk_bytes // self._segment_bytes} layers)"
            )
        if slot >= self._num_slots:
            raise ValueError(f"slot {slot} out of range (num_slots={self._num_slots})")
        return i
