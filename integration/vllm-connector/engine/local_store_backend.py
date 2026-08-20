"""LocalStoreBackend — StorageBackend 的本地持久化实现（T-115，D-010/D-011）。

全系统唯一允许出现介质概念的文件：文件路径、URI、open/submit/wait、
对齐（caps）、内存注册（register_memory）、partial-commit 重发等词汇与
逻辑只活在本模块内（ARCHITECTURE.md §0/§0a：上层只见 opaque chunk_id）。

对外 chunk_id 形态（自产自解释，纯逻辑 id）：``chunk://local/<i>``——
文件路径 ``<root>/chunks/<i>.bin`` 完全是模块私有细节，永不外泄。

布局（R1 默认）：一 chunk 一文件；层段 target offset = layer_idx ×
segment_bytes（packed，与 staging 槽布局一致）。

tutti_runtime 接线（仅经其公开 Python API）：
- 构造：``open_batch`` 全部 chunk 文件 → target ticket 表
- bind_staging：``register_memory`` 恰 1 次（io_granularity=segment_bytes）
- 段搬运：固定粒度（segment_bytes）请求 + ``execution="host"`` 提交；
  被拒请求窗口化重发（仅重发被拒子集；连续两轮零接受 → RuntimeError）
- 完成句柄：包装 ``runtime.wait`` 轮询至终态（COMPLETED/FAILED）

与任务卡的两处实测偏差（以实测 API 为准，回执已列）：
1. staging 注册 kind 用 ``"host"``（stub runtime 为 host-only，实测拒绝
   ``"device"``；engine.bind 传入的 staging 本就是 host 缓冲）。
2. ``runtime=None`` 的真机构造需 preset（make_local_nvme_runtime 的必选
   参数），从环境变量 ``TUTTI_NVME_PRESET``（JSON）读取；缺失即报错提示。
"""

import ctypes
import json
import os
import re

_CHUNK_DIR = "chunks"
_CHUNK_ID_RE = re.compile(r"^chunk://local/(\d+)$")
_POLL_TIMEOUT_MS = 100  # wait 轮询粒度
_SHUTDOWN_TIMEOUT_MS = 5000

# 介质词汇豁免：本文件是 D-011 指定的唯一介质实现文件（术语纪律测试豁免）
_MEDIA_TERM_EXEMPT = True


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


class _IoHandle:
    """一次段搬运的完成句柄（聚合 partial-commit 重发各轮产生的 io ticket）。"""

    __slots__ = ("_backend", "_tickets")

    def __init__(self, backend, tickets):
        self._backend = backend
        self._tickets = tuple(tickets)

    @property
    def io_tickets(self):
        """（测试钩子）底层 io ticket 只读视图，供 stub 模式 force_complete。"""
        return self._tickets

    def query(self):
        """非阻塞探测：全部 io 均到终态 COMPLETED 才算完成。"""
        for ticket in self._tickets:
            observation, state = self._backend._runtime.wait(ticket, 0)
            if observation == "TIMEOUT":
                return False
            if state != "COMPLETED":
                return False
        return True

    def wait(self):
        """阻塞至全部 io 终态；FAILED → RuntimeError；完成后释放 io。"""
        runtime = self._backend._runtime
        try:
            for ticket in self._tickets:
                while True:
                    observation, state = runtime.wait(ticket, _POLL_TIMEOUT_MS)
                    if observation == "TIMEOUT":
                        continue  # 未终态，继续轮询
                    if state == "COMPLETED":
                        break
                    raise RuntimeError(f"tutti_runtime io 失败：state={state}")
        finally:
            self._release()
        self._backend._retire(self)

    synchronize = wait

    def _release(self):
        for ticket in self._tickets:
            try:
                self._backend._runtime.release_io(ticket)
            except Exception:  # noqa: BLE001 — 释放尽力而为
                pass


class LocalStoreBackend:
    """StorageBackend SPI 的本地持久化实现（chunk → 本地文件，介质细节全私有）。"""

    def __init__(self, root, num_chunks, chunk_bytes, runtime=None):
        if not isinstance(root, str) or not root:
            raise ValueError(f"root 必须为非空字符串，got {root!r}")
        if not _is_int(num_chunks) or num_chunks < 1:
            raise ValueError(f"num_chunks 必须 >= 1，got {num_chunks!r}")
        if not _is_int(chunk_bytes) or chunk_bytes < 1:
            raise ValueError(f"chunk_bytes 必须 >= 1，got {chunk_bytes!r}")

        self._root = os.path.abspath(root)
        self._num_chunks = num_chunks
        self._chunk_bytes = chunk_bytes
        self._runtime = runtime if runtime is not None else self._make_default_runtime()
        self._closed = False

        # 介质资源私有管理：自建 chunk 文件目录（一 chunk 一文件，R1 布局）
        chunk_dir = os.path.join(self._root, _CHUNK_DIR)
        os.makedirs(chunk_dir, exist_ok=True)
        self._file_paths = []
        for i in range(num_chunks):
            path = os.path.join(chunk_dir, f"{i}.bin")
            if not os.path.exists(path):
                with open(path, "wb"):
                    pass
            self._file_paths.append(path)

        # 对外 chunk_id：纯逻辑 id（opaque，本 backend 专属形态），映射私有化
        self._chunk_ids = [f"chunk://local/{i}" for i in range(num_chunks)]

        # open_batch：fail-closed（任一 URI 失败整体失败，由 runtime 抛出）
        self._target_tickets = list(
            self._runtime.open_batch([f"file://{p}" for p in self._file_paths])
        )

        # bind_staging 后有效
        self._staging_addr = None
        self._num_slots = None
        self._segment_bytes = None
        self._mem_ticket = None

        self._active = []  # 在途句柄

    # ---------- StorageBackend SPI ----------

    def chunk_paths(self, num_chunks):
        if not _is_int(num_chunks) or not 1 <= num_chunks <= self._num_chunks:
            raise ValueError(
                f"num_chunks 须在 [1, {self._num_chunks}]，got {num_chunks!r}"
            )
        return list(self._chunk_ids[:num_chunks])

    def bind_staging(self, staging_addr, chunk_bytes, num_slots, segment_bytes, io_stream):
        if self._closed:
            raise RuntimeError("backend 已 shutdown")
        # 参数校验（ValueError）
        if not _is_int(staging_addr) or staging_addr <= 0:
            raise ValueError(f"staging_addr 必须为正整数，got {staging_addr!r}")
        if not _is_int(chunk_bytes) or chunk_bytes != self._chunk_bytes:
            raise ValueError(
                f"chunk_bytes 与池不符：期望 {self._chunk_bytes}，got {chunk_bytes!r}"
            )
        if not _is_int(num_slots) or num_slots < 1:
            raise ValueError(f"num_slots 必须 >= 1，got {num_slots!r}")
        if not _is_int(segment_bytes) or segment_bytes < 1:
            raise ValueError(f"segment_bytes 必须 >= 1，got {segment_bytes!r}")
        if self._chunk_bytes % segment_bytes != 0:
            raise ValueError(
                f"segment_bytes={segment_bytes} 不整除 chunk_bytes={self._chunk_bytes}"
            )
        if self._mem_ticket is not None:
            raise RuntimeError("staging 已注册（bind_staging 恰允许一次）")

        # caps 对齐校验（RuntimeError）
        caps = self._runtime.caps()
        for name in ("target_alignment_bytes", "memory_alignment_bytes",
                     "length_alignment_bytes"):
            if segment_bytes % caps[name] != 0:
                raise RuntimeError(
                    f"segment_bytes={segment_bytes} 违反 runtime caps 对齐 "
                    f"{name}={caps[name]}"
                )

        self._mem_ticket = self._runtime.register_memory(
            staging_addr, num_slots * segment_bytes, "host",
            accel_id=-1, io_granularity=segment_bytes,
        )
        self._staging_addr = staging_addr
        self._num_slots = num_slots
        self._segment_bytes = segment_bytes

    def write_chunk_segment(self, chunk_id, layer_idx, slot):
        return self._transfer(chunk_id, layer_idx, slot, "write")

    def read_chunk_segment(self, chunk_id, layer_idx, slot):
        return self._transfer(chunk_id, layer_idx, slot, "read")

    def wait_idle(self):
        for handle in list(self._active):
            handle.wait()

    def shutdown(self):
        if self._closed:
            return
        self._closed = True
        self.wait_idle()
        self._runtime.shutdown(_SHUTDOWN_TIMEOUT_MS)

    # ---------- 内部 ----------

    def _make_default_runtime(self):
        raw = os.environ.get("TUTTI_NVME_PRESET")
        if not raw:
            raise RuntimeError(
                "LocalStoreBackend 需要 tutti_runtime：请注入 runtime= 参数，"
                "或设置 TUTTI_NVME_PRESET（JSON，形态同 make_local_nvme_runtime "
                "的 preset 参数）"
            )
        import tutti_runtime

        return tutti_runtime.make_local_nvme_runtime(json.loads(raw))

    def _check_request(self, chunk_id, layer_idx, slot):
        if self._staging_addr is None:
            raise RuntimeError("bind_staging 尚未调用（staging 未注册）")

        seg = self._segment_bytes
        num_layers = self._chunk_bytes // seg
        if not _is_int(layer_idx) or not 0 <= layer_idx < num_layers:
            raise ValueError(f"layer_idx 须在 [0, {num_layers})，got {layer_idx!r}")
        if not _is_int(slot) or not 0 <= slot < self._num_slots:
            raise ValueError(f"slot 须在 [0, {self._num_slots})，got {slot!r}")

        if not isinstance(chunk_id, str):
            raise ValueError(f"chunk_id 必须为字符串，got {chunk_id!r}")
        m = _CHUNK_ID_RE.match(chunk_id)
        if not m:
            raise ValueError(f"无法解释的 chunk_id：{chunk_id!r}")
        index = int(m.group(1))
        if index >= self._num_chunks:
            raise ValueError(f"chunk_id 超出池容量：{chunk_id!r}")

        if (layer_idx + 1) * seg > self._chunk_bytes:
            raise ValueError(f"层段越界：layer_idx={layer_idx}")
        return index

    def _transfer(self, chunk_id, layer_idx, slot, direction):
        index = self._check_request(chunk_id, layer_idx, slot)
        seg = self._segment_bytes
        request = (
            self._target_tickets[index],   # target（open_batch ticket）
            layer_idx * seg,               # target_offset（packed 层段）
            self._mem_ticket,              # memory（register_memory ticket）
            slot * seg,                    # memory_offset（staging 槽）
            seg,                           # length（固定粒度）
            direction,                     # "write" | "read"
        )
        tickets = self._submit_with_retry([request])
        handle = _IoHandle(self, tickets)
        self._active.append(handle)
        return handle

    def _submit_with_retry(self, requests):
        """固定粒度提交 + partial-commit 窗口化重发（仅被拒子集）。

        连续两轮零接受（无任何请求被受理）→ RuntimeError。
        """
        pending = list(requests)
        tickets = []
        zero_accept_rounds = 0
        while pending:
            result = self._runtime.submit(
                pending, accel_id=-1, stream=None, execution="host"
            )
            if result.io_handle is not None:
                tickets.append(result.io_handle)
            rejected = list(result.rejected) if result.rejected else []
            accepted = (len(pending) - len(rejected)) if result.status_ok else 0
            if not rejected and result.status_ok:
                break  # 全部受理
            if accepted == 0:
                zero_accept_rounds += 1
                if zero_accept_rounds >= 2:
                    raise RuntimeError(
                        "partial-commit 失败：连续两轮零接受"
                        f"（status={result.status_msg!r}）"
                    )
            else:
                zero_accept_rounds = 0
            pending = [pending[i] for i in rejected]
        return tickets

    def _retire(self, handle):
        if handle in self._active:
            self._active.remove(handle)
