# Tutti Refactor — Bottom-Up API Design

> **Status**: Design specification for refactoring Tutti into the layered architecture.  
> **Target**: Linux 5.15 kernel + CUDA for v0.1  
> **Date**: 2026-07-21

## Overview

This refactor reorganizes the Tutti codebase from a monolithic eight-layer stack into a strict bottom-up dependency order with clear layer boundaries. The new architecture establishes two foundational base layers (Abstraction and Accelerator HAL), a local-NVMe virtualization layer (Device Manager) below the backends that consume it, and two symmetric top-level data interfaces (Block Storage and raw device) that converge on a shared `StorageTarget` noun.

## Goals

1. **No CUDA leaks above the HAL** — Currently 8 headers violate this by including `cuda_runtime.h`
2. **No libnvm leaks above Device Manager** — Backends currently include libnvm headers directly
3. **Backend-neutral IO Engine** — Current `NvmeBatchInputTensor` is NVMe-specific
4. **Unified stream/event types** — Replace 3 independent `void*`/`cudaStream_t` spellings with `AccelStream`/`AccelEvent`
5. **Vendor portability foundation** — Abstract CUDA-isms into macros for future ROCm/SYCL/CANN support

## Document Structure

This `doc/refact_new/` directory contains the complete API design:

- **00-overview.md** (this file) — Refactor goals and document index
- **01-missing-types.md** — Shared types that must be created first
- **02-layer0-abstraction.md** — Macro layer for vendor dispatch
- **03-layer1-accelerator-hal.md** — IAccelerator interface (replaces IMemorySubsystem split)
- **04-layer2-device-manager.md** — IVirtualNvme + VDevice + device-side queue mechanics
- **05-layer3-backends-spi.md** — IBackendProvider changes + StorageTarget
- **06-layer4-io-engine.md** — IIoEngine changes + backend-neutral input types
- **07-layer5-storage-interfaces.md** — IBlockStorage changes + new IRawDevice
- **08-validation.md** — Dependency rules + end-to-end call flow validation
- **09-implementation-sequence.md** — Recommended build order for new directory
- **10-open-questions.md** — Design decisions needed before implementation

## Key Architectural Changes

### Memory Layer Split

The current `memory/` layer conflates three concerns:

| Concern | Current location | New location |
|---|---|---|
| Generic alloc/free, DMA-map, MemoryRegion registry | `memory/` (IMemorySubsystem) | **Accelerator HAL** (IAccelerator) |
| PRP/SGL descriptor build, PRP-page cache | `memory/` (IMemorySubsystem) | **Backends / NVMe** |
| IO-slice fan-out (tensor→sub-IOs) | `memory/` (register_tensor) | **IO Engine** |

### Device Manager Inversion

The v0.1 `device_manager/` sat above backends but included libnvm (backwards dependency). In the redesign it becomes the **local-NVMe virtualization base** below backends:

- Owns physical controller bring-up and queue-pair budget
- Hands each backend a **vDevice** = queue slice + namespace view + caps
- Backends pull queues *down* from it; nothing above includes libnvm
- Device-side queue mechanics (`acquire_queue`, `issue_nvme_cmd`, `poll`) move from `nvme_storage/` to DM

### StorageTarget Convergence

Both top interfaces (Block Storage and raw device) produce a `StorageTarget` before dispatching to the IO Engine:

```
Block Storage (GPUFile) ──┐
                          ├──> StorageTarget ──> IO Engine ──> Backends
raw device (ns + LBA)  ───┘
```

This keeps the IO Engine backend-neutral — it never sees `NvmeFile*` or NVMe-specific types.

## What Stays, What Moves, What's New

### Stays (minimal changes)

- `IDeviceRegistry`, `LocalNvmeDirectRegistry`, `NvmeServiceBackedRegistry` — controller bring-up
- `ILeaseManager` — cross-process heartbeat
- `NvmeQueueGroup` — still wraps GPU-resident `d_qps[]`, now exposed via `VDevice`
- `IBackendProvider` SPI — exists but needs signature updates
- `IBlockStorage` — mostly correct, minor stream type fixes

### Moves

- `queue_acquire_helper.cuh` — from `nvme_storage/` to `device_manager/`
- `nvme_storage/` layer — absorbed into `backends/local_nvme/`
- `IMemorySubsystem` methods:
  - `register_tensor()` / `lookup_io_slice()` → IO Engine
  - `ensure_prp_pages_resident()` / `descriptor_slice()` → NVMe Backend
  - Generic alloc/register/dma_map → Accelerator HAL

### New (must be created)

- `tutti/abstraction/accel.h` — macro layer (TUTTI_DEVICE, TUTTI_LAUNCH_KERNEL, etc.)
- `IAccelerator` — HAL interface replacing IMemorySubsystem's generic half
- `IVirtualNvme` + `VDevice` — Level-2 allocator (split process QP grant into per-backend slices)
- `StorageTarget` — convergence noun (NVME_FILE / NVME_RAW / RDMA_REMOTE)
- `IRawDevice` — top interface for fileless (namespace + LBA) access
- `AccelStream` / `AccelEvent` — unified opaque types (currently 3 independent `void*` spellings)
- `SubSliceInfo` — IO-slice descriptor (currently forward-declared, not defined)

## Design Validation Status

✅ **Dependency rules checked** — Each layer depends only on layers below + shared nouns  
✅ **End-to-end call flow traced** — GPU-submit read flow maps cleanly to new APIs  
✅ **CUDA leak audit complete** — 8 headers identified, fix strategy defined  
✅ **Gap analysis done** — All missing types and interfaces documented  
⚠️ **Open questions flagged** — 5 design decisions need resolution (see `10-open-questions.md`)

## Next Steps

1. **Review** — Validate the API design documents in this directory
2. **Resolve open questions** — Make decisions on items in `10-open-questions.md`
3. **Create shared types** — Start with `tutti/abstraction/accel.h` and `tutti/types/`
4. **Implement bottom-up** — Follow sequence in `09-implementation-sequence.md`

---

See individual layer documents for detailed API specifications, gap analyses, and migration plans.
