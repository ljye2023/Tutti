"""ByteLevelFakeRuntime — tutti_runtime 字节级测试替身（T-115）。"""

import ctypes


class _SubmitResult:
    __slots__ = ("status_ok", "status_msg", "io_handle", "initial_states", "rejected")

    def __init__(self, status_ok, status_msg, io_handle, initial_states, rejected):
        self.status_ok = status_ok
        self.status_msg = status_msg
        self.io_handle = io_handle
        self.initial_states = initial_states
        self.rejected = rejected


class ByteLevelFakeRuntime:
    """字节级 fake：真搬数据 + 调用记录 + 可编程拒收 + 即时完成。

    reject_plan：逐轮拒收指纹集合列表（指纹 = 请求 6 元组）。第 n 轮
    submit 拒绝命中 reject_plan[n] 的请求；计划用尽后不再拒绝。
    """

    def __init__(self, reject_plan=None):
        self._next_ticket = 1000
        self._files = {}       # target ticket → bytearray
        self._uri_of = {}      # target ticket → uri
        self._issued_io = set()
        self._released_io = set()
        self._shutdown = False
        self._memory = None    # (addr, size)

        self.register_calls = []   # (addr, size, kind, accel_id, io_granularity)
        self.submit_calls = []     # 每轮 list(request)
        self.reject_plan = [set(p) for p in (reject_plan or [])]

    # ---------- caps ----------

    def caps(self):
        return {
            "max_in_flight_operations": 0,
            "max_single_io_bytes": 0,
            "max_batch_requests": 0,
            "length_alignment_bytes": 1,
            "memory_alignment_bytes": 1,
            "target_alignment_bytes": 1,
        }

    def with_caps(self, length_alignment=1, memory_alignment=1, target_alignment=1):
        """测试钩子：覆写对齐 caps（返回 self，便于链式构造）。"""
        self.caps = lambda: {
            "max_in_flight_operations": 0,
            "max_single_io_bytes": 0,
            "max_batch_requests": 0,
            "length_alignment_bytes": length_alignment,
            "memory_alignment_bytes": memory_alignment,
            "target_alignment_bytes": target_alignment,
        }
        return self

    # ---------- runtime API ----------

    def open_batch(self, uris):
        tickets = []
        for uri in uris:
            ticket = self._next_ticket
            self._next_ticket += 1
            self._files[ticket] = bytearray()
            self._uri_of[ticket] = uri
            tickets.append(ticket)
        return tickets

    def register_memory(self, addr, size, kind, accel_id=-1, io_granularity=0):
        self.register_calls.append((addr, size, kind, accel_id, io_granularity))
        ticket = self._next_ticket
        self._next_ticket += 1
        self._memory = (addr, size)
        return ticket

    def submit(self, requests, accel_id, stream, execution="device"):
        if self._shutdown:
            raise RuntimeError("runtime 已 shutdown")
        round_index = len(self.submit_calls)
        self.submit_calls.append(list(requests))
        plan = (
            self.reject_plan[round_index]
            if round_index < len(self.reject_plan)
            else set()
        )

        accepted, rejected = [], []
        for index, request in enumerate(requests):
            if tuple(request) in plan:
                rejected.append(index)
            else:
                accepted.append(request)

        io_handle = None
        if accepted:
            for request in accepted:
                self._move_bytes(request)
            io_handle = self._next_ticket
            self._next_ticket += 1
            self._issued_io.add(io_handle)

        return _SubmitResult(
            status_ok=True,
            status_msg="" if not rejected else "SIMULATED_REJECTION",
            io_handle=io_handle,
            initial_states=["COMPLETED"] * len(accepted),
            rejected=rejected,
        )

    def wait(self, io_handle, timeout_ms):
        if io_handle in self._issued_io:
            return ("OK", "COMPLETED")
        raise ValueError(f"未知 io_handle：{io_handle}")

    def release_io(self, io_handle):
        self._released_io.add(io_handle)

    def shutdown(self, timeout_ms):
        self._shutdown = True

    def testing_force_complete(self, io_handle, state="COMPLETED"):
        self._issued_io.add(io_handle)

    # ---------- 数据视图（测试断言用） ----------

    def file_bytes(self, target_ticket):
        return bytes(self._files[target_ticket])

    # ---------- 内部 ----------

    def _move_bytes(self, request):
        target, target_offset, memory, memory_offset, length, direction = request
        addr, size = self._memory
        if memory_offset + length > size:
            raise RuntimeError("staging 越界（fake 校验）")
        buf = self._files[target]
        if len(buf) < target_offset + length:
            buf.extend(b"\x00" * (target_offset + length - len(buf)))
        if direction == "write":
            data = ctypes.string_at(addr + memory_offset, length)
            buf[target_offset:target_offset + length] = data
        elif direction == "read":
            ctypes.memmove(
                addr + memory_offset,
                bytes(buf[target_offset:target_offset + length]),
                length,
            )
        else:
            raise RuntimeError(f"未知 direction：{direction}")
