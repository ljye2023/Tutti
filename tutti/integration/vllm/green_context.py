"""Optional SM partition between model compute and Tutti IO (CUDA green context).

Tutti's IO kernel is launched on demand per layer. On a device where every
SM is held by a full-device GEMM (vLLM's sm90 fp8 cutlass kernel: 78 CTAs,
one per SM, registers and shared memory full), an IO launch waits for the
GEMM to drain -- that is the few-millisecond gap seen between per-layer read
kernels.

A green context caps the SMs a context may use. Putting *model compute* on a
green-context stream with ``total - N`` SMs leaves N SMs that compute can
never occupy; IO kernels stay on ordinary streams of the primary context and
are scheduled onto those SMs immediately, still launched on demand.

This is opt-in (``TUTTI_GREEN_CTX_IO_SMS=N``, N a multiple of 8 on sm90) and
costs compute throughput: cutlass sizes persistent grids from the device SM
count, not the green context's, so a 78-CTA GEMM on 70 SMs needs two waves.
Measure before enabling.
"""

from __future__ import annotations

import logging
import os

_LOG = logging.getLogger(__name__)
_ENV = "TUTTI_GREEN_CTX_IO_SMS"

# Keep the context alive for the process: its streams are only valid while it
# exists.
_GREEN_CTX = None


def enabled() -> bool:
    raw = os.environ.get(_ENV, "").strip()
    return bool(raw) and raw != "0"


def reserve_io_sms(device_index: int) -> int:
    """Move this thread's current CUDA stream into a green context that leaves
    ``TUTTI_GREEN_CTX_IO_SMS`` SMs to IO. Returns the SMs reserved (0 = off).

    Must run on the worker's forward thread before the model's first kernel,
    so vLLM's ``current_stream()`` picks the green-context stream up.
    """
    global _GREEN_CTX
    raw = os.environ.get(_ENV, "").strip()
    if not raw or raw == "0":
        return 0
    io_sms = int(raw)
    import torch
    from torch.cuda.green_contexts import SUPPORTED, GreenContext

    if not SUPPORTED:
        raise RuntimeError(f"{_ENV} set but this PyTorch has no green context support")
    total = torch.cuda.get_device_properties(device_index).multi_processor_count
    if io_sms <= 0 or io_sms >= total:
        raise ValueError(f"{_ENV} must be in (0, {total}), got {io_sms}")
    if _GREEN_CTX is None:
        _GREEN_CTX = GreenContext.create(num_sms=total - io_sms, device_id=device_index)
    stream = _GREEN_CTX.Stream()
    torch.cuda.set_stream(stream)
    _LOG.info(
        "TUTTI_GREEN_CTX compute_sms=%d io_sms=%d device=%d stream=%#x",
        total - io_sms, io_sms, device_index, stream.cuda_stream,
    )
    return io_sms
