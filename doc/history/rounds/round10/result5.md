# Round 10 Session 5 Result: Header Hygiene and Phase 3 Acceptance Gate

## Overview

Closes Phase 3 (Local NVMe and Kernel Boundary Consolidation) with a repeatable, CI-able gate script. The gate verifies: private headers do not propagate from public targets; root and standalone builds reference the same source; UAPI assertions and two kernel baselines compile-only; CMake INTERFACE include audit; dual-fact-source check; header-hygiene consumer test.

All 10 gate checks pass. Two hardware contract tests run explicitly: 616 + 115 = 731 assertions, 0 failures.

## Changes

### Modified

| File | Change |
|------|--------|
| `CMakeLists.txt` (root) | Moved `add_subdirectory(tutti)` BEFORE the global `include_directories` for libnvm. The tutti subdirectory (and its consumer tests) no longer inherits `libnvm/src` and `libnvm/include` as directory-scope includes. Root-scope legacy targets (libnvm, nvmeservice, memory, device_manager, …) still have the global includes. |
| `tutti/CMakeLists.txt` | Added `tests/header_hygiene` to the BUILD_TESTING test suite (hardware-free, all profiles). |
| `Roadmap.md` | Phase 3 deliverables marked [DONE]; Gate marked [PASSED]. Known Bugs Snapshot: dual-source and private-header-propagation entries marked [CLOSED — Phase 3]. Build graph status updated to Round 10. "Not yet closed" — dual source owner struck through. |

### Created

| File | Purpose |
|------|---------|
| `tests/header_hygiene/CMakeLists.txt` | Consumer hygiene test CMake: links ONLY `tutti_api`, no extra include paths, `-Werror`. |
| `tests/header_hygiene/header_hygiene_test.cpp` | Positive (public headers usable) + negative (`__has_include` for 26 private headers across libnvm/nvmeservice/snvme/CUDA kernel). |
| `scripts/phase3_gate.sh` | Phase 3 one-click gate: 8 gates, 10 checks, summary table. |

## Gate Script Full Output

```
============================================================
  Phase 3 Acceptance Gate — 2026-08-01 11:44 UTC
  Repo: /data/home/ryeqiu/Tutti
============================================================

================================================================
  Gate 1: Root build configure + build
================================================================
  Build dir: /data/home/ryeqiu/Tutti/build
  CONFIGURE: PASS
  BUILD: PASS (libnvm, nvmeservice, tutti_daemon)

================================================================
  Gate 2: Standalone HOST clean configure + build + ctest
================================================================
  Build dir: /data/home/ryeqiu/Tutti/tutti/build_host
  CONFIGURE: PASS
  BUILD: PASS
  CTEST: PASS (12 tests)

================================================================
  Gate 3: Standalone CUDA clean configure + build
================================================================
  Build dir: /data/home/ryeqiu/Tutti/tutti/build_cuda
  (Hardware tests NOT run; set TUTTI_BUILD_HARDWARE_TESTS=ON manually)
  CONFIGURE: PASS
  BUILD: PASS
  CTEST (hw-free): PASS (114 passed, 36 skipped)

================================================================
  Gate 4: UAPI static-assert contract test
================================================================
  (Covered by HOST ctest — tutti_uapi_contract_test)
  RESULT: PASS

================================================================
  Gate 5: snvme kernel baseline compile-only
================================================================
  Baseline A (5.4.241 src + 5.4.241 hdr): PASS
  Baseline B (5.4.241 src + 5.4.203 hdr): PASS
  Baseline C (5.15 src + 5.4 hdr): expected FAIL (structural, not compat)

================================================================
  Gate 6: Dual-fact-source check (libnvm/snvme single implementation)
================================================================
  libnvm: single source of truth (tutti/device_manager/nvme/libnvm/)
  snvme: single source of truth (tutti/device_manager/nvme/kernel_modules/)
  UAPI: single header (tutti/include/uapi/tutti_snvme.h)
  Root CMakeLists.txt references tutti/device_manager/nvme/libnvm (6 hits)

================================================================
  Gate 7: CMake INTERFACE_INCLUDE_DIRECTORIES audit
================================================================
  Static analysis of public target declarations:

  tutti_api INTERFACE_INCLUDE_DIRECTORIES: ${CMAKE_CURRENT_SOURCE_DIR}/include
    -> resolves to: tutti/include/  (public headers only)
    -> no private paths
  tutti_api links: tutti_cuda_like (INTERFACE, no include dirs of its own)

  tutti_spi INTERFACE_INCLUDE_DIRECTORIES: ${CMAKE_CURRENT_SOURCE_DIR}/include
    -> resolves to: tutti/include/  (same as tutti_api)
    -> no private paths

  tutti_types INTERFACE_INCLUDE_DIRECTORIES: ${CMAKE_CURRENT_SOURCE_DIR}
    -> resolves to: tutti/  (repo root, not libnvm/include)
    -> <nvm_types.h> NOT directly reachable (requires libnvm/include/ in -I)
    -> tutti_types is NOT linked by tutti_api or tutti_spi

  No INTERFACE (non-PRIVATE) libnvm include paths found in any CMakeLists.txt
  No INTERFACE (non-PRIVATE) NVMeService include paths found

================================================================
  Gate 8: Header-hygiene consumer test
================================================================
  RESULT: PASS
    header_hygiene_test: all checks passed

  Negative checks (private headers must NOT be reachable):
    libnvm: <nvm_types.h>, <nvm_dma.h>, <nvm_queue.h>, <nvm_ctrl.h>,
            <nvm_io.h>, <nvm_error.h>, <nvm_util.h>, <nvm_admin.h>,
            <nvm_cmd.h>, <nvm_aq.h>, <nvm_rpc.h>, <nvm_parallel_queue.h>,
            <ctrl.h>, <queue.h>, <buffer.h>, <ioctl.h>, <map.h>
    libnvm src: <dma.h>, <regs.h>, <lib_ctrl.h>, <rpc.h>
    nvmeservice: <nvmeservice_client.h>, <nvmeservice_server.h>,
                 <nvmeservice_config.h>, <nvmeservice_state.h>
    snvme kernel: <peer_memory.h>, <compat.h>, <nvfs-core.h>
    CUDA kernel: <local_nvme_data_path.h>
  All __has_include checks returned false (headers not reachable).

================================================================
  PHASE 3 GATE SUMMARY
================================================================
#     GATE                                      STATUS
--    ----                                      ------
1     Root build                                PASS
2     HOST build                                PASS
3     HOST ctest                                PASS
4     CUDA build                                PASS
5     CUDA ctest (hw-free)                      PASS
6     UAPI contract                             PASS
7     Kernel baselines (A+B)                    PASS
8     Dual-fact-source                          PASS
9     CMake audit                               PASS
10    Header hygiene                            PASS

  PASS: 10  FAIL: 0  SKIP: 0

  *** PHASE 3 GATE: ALL CHECKS PASSED ***
```

## Private Header Unreachability Evidence

The consumer test (`tests/header_hygiene/header_hygiene_test.cpp`) links ONLY `tutti_api` and uses `__has_include` (C++17 standard) to probe 26 private headers. The test compiles and runs successfully, proving:

1. **Positive**: Public headers are reachable and usable:
   - `<tutti/status.h>`, `<tutti/io_types.h>`, `<tutti/memory_types.h>`
   - `<tutti/cuda_like.h>`, `<tutti/spi/data_path.h>`, `<tutti/spi/storage_target_resolver.h>`
   - `<tutti/storage_runtime.h>`
   - Runtime: `tutti::Status::Ok()` and `tutti::IoRequest` instantiate correctly.

2. **Negative**: All 26 private headers return `__has_include(...) == false`:
   - 17 libnvm private headers (from `libnvm/include/` and `libnvm/src/`)
   - 4 nvmeservice private headers (from `NVMeService/src/`)
   - 3 snvme kernel private headers (from `kernel_modules/snvme-*/`)
   - 1 CUDA kernel private header (`local_nvme_data_path.h`)
   - If any returned true, a compile-time `#error` fires (the test would not compile).

### Mechanism

The root `CMakeLists.txt` previously declared global `include_directories` for `libnvm/src` and `libnvm/include` BEFORE `add_subdirectory(tutti)`. This caused all targets in the tutti subdirectory (including `tutti_api`) to inherit the libnvm paths in their directory-scope INCLUDE_DIRECTORIES. While INTERFACE targets don't propagate INCLUDE_DIRECTORIES (only INTERFACE_INCLUDE_DIRECTORIES) to consumers, the inherited paths were visible to any test compiled in the tutti subdirectory scope, making `__has_include(<nvm_types.h>)` return true.

Fix: Move `add_subdirectory(tutti)` BEFORE the global `include_directories`. The tutti subdirectory (and its tests) no longer inherit the libnvm paths. Root-scope legacy targets (added after the global includes) still have them.

## Target Include Audit

| Target | INTERFACE_INCLUDE_DIRECTORIES | Private Paths? |
|--------|------------------------------|----------------|
| `tutti_api` | `${CMAKE_CURRENT_SOURCE_DIR}/include` → `tutti/include/` | No |
| `tutti_spi` | `${CMAKE_CURRENT_SOURCE_DIR}/include` → `tutti/include/` | No |
| `tutti_cuda_like` | (set by accelerator profile module, no local-NVMe paths) | No |
| `tutti_types` | `${CMAKE_CURRENT_SOURCE_DIR}` → `tutti/` | No (root, not libnvm/include) |
| `tutti_binding_ext4_local_nvme` | (header-only, own include) | No |
| `tutti_resolver_local_file` | (header-only, own include) | No |
| `libnvm` | PRIVATE `${libnvm_root}` (not INTERFACE) | Private, not propagated |
| `nvmeservice` | PUBLIC `${nvmeservice_root}/src` — but nvmeservice is NOT linked by any public target | Not reachable via tutti_api |
| `tutti_device_manager` | PRIVATE libnvm + NVMeService paths | Private, not propagated |
| `tutti_memory` | PRIVATE libnvm/include | Private, not propagated |

**Key insight**: `tutti_types` has INTERFACE include of `tutti/` (the repository root), which could theoretically allow `#include <device_manager/nvme/libnvm/include/nvm_types.h>` via a relative path. However, `tutti_types` is NOT linked by `tutti_api` or `tutti_spi`, so consumers of the public API never see this path. The `__has_include(<nvm_types.h>)` test confirms `<nvm_types.h>` (angle-bracket, direct) is not reachable.

## Hardware Contract Test Results

Run explicitly after gate script (snvme module loaded, tutti_daemon running, /dev/snvme0n1 mounted at /mnt/nvme1):

### tutti_local_nvme_datapath_contract_test

```
=== Full Summary ===
  passed: 616
  failed: 0
RESULT: PASS
```

Covers: capabilities, lifecycle, open/close, registration_domain, byte→block conversion, HOST/DEVICE memory registration, repeated registration, unregister, null/zero-length, shutdown no leak, real DMA address, queue group creation, device handle, submit SINGLE/DUAL/LIST, PRP-list DMA, CQ polling, cross-segment, batch mixed/partial, event ordering, two-thread concurrent, timeout/retention, NVMe CQ error injection, CQ poll budget, completion status modes.

### tutti_storage_runtime_local_nvme_contract_test

```
=== Summary ===
  passed: 115
  failed: 0
RESULT: PASS
```

Covers: assembly/open, memory/lazy registration, real data SINGLE/DUAL/LIST/cross-segment, batch/mixed/partial commit, order/concurrency (same-stream, two-stream, two host threads), failure/timeout, teardown/repeat lifecycle.

**Total: 731 assertions, 0 failures.** Exceeds Round 9 baseline (501 assertions in Round 8).

## Roadmap Update Diff

### Phase 3 section — deliverables marked [DONE]:

```diff
 Deliberables:

-- Device Manager resource grant/accounting moved into `LocalNvmeDataPath/control/`.
-- Single source for libnvm/NVMeService/snvme; no dual-tree bug fixes.
-- `include/uapi/tutti_snvme.h` shared by userspace and kernel; fixed-width types, ABI version/capability handshake, compat ioctl strategy.
-- Kernel-version differences isolated in compat ops; GPU pinning isolated in `peer_memory_ops`.
-- Local-NVMe private headers, CUDA kernel, and libnvm do not propagate from public targets.
+- [DONE] Device Manager resource grant/accounting moved into `LocalNvmeDataPath/control/`.
+- [DONE] Single source for libnvm/NVMeService/snvme; no dual-tree bug fixes. (tutti/device_manager/nvme/libnvm/ + tutti/device_manager/nvme/kernel_modules/)
+- [DONE] `include/uapi/tutti_snvme.h` shared by userspace and kernel; fixed-width types, ABI version/capability handshake, compat ioctl strategy.
+- [DONE] Kernel-version differences isolated in compat ops; GPU pinning isolated in `peer_memory_ops`.
+- [DONE] Local-NVMe private headers, CUDA kernel, and libnvm do not propagate from public targets. (Verified by tests/header_hygiene/ consumer test using __has_include.)

-Gate: root and standalone builds reference the same source; at least two supported kernel baselines compile-only CI; UAPI fixed-width size/offset assertions and 32/64-bit compat tests pass.
+Gate: [PASSED] root and standalone builds reference the same source; at least two supported kernel baselines compile-only CI (5.4.241 + 5.4.203 PASS, 5.15 cross-version expected FAIL in tree-specific code); UAPI fixed-width size/offset assertions and 32/64-bit compat tests pass. Phase 3 gate script (scripts/phase3_gate.sh) all 10 checks green.
```

### Known Bugs Snapshot — two entries closed:

```diff
-- New and old libnvm/NVMeService/snvme trees coexist as dual sources of truth; standalone `tutti/` build now owns the new-architecture build graph but root legacy tree is not yet retired.
+- **[CLOSED — Phase 3]** libnvm/NVMeService/snvme dual-source: consolidated to a single source of truth at `tutti/device_manager/nvme/libnvm/` and `tutti/device_manager/nvme/kernel_modules/`. Root and standalone builds reference the same source. The old root-level `device_manager/nvme/` tree has been removed.
+- **[CLOSED — Phase 3]** Local-NVMe private header propagation: `tutti_api` and `tutti_spi` INTERFACE targets expose only `tutti/include/`. libnvm, nvmeservice, snvme kernel, and CUDA kernel private headers are not reachable from public consumers (verified by `__has_include` contract test).
```

### "Not yet closed" — dual source struck through:

```diff
-- Dual source owner: root legacy tree and standalone `tutti/` coexist.
+~~Dual source owner: root legacy tree and standalone `tutti/` coexist.~~ **[CLOSED — Phase 3]** Single source of truth established; root legacy tree retired.
```

### Build graph status — updated to Round 10:

```diff
-**Build graph status (Round 9):**
+**Build graph status (Round 10):**
 ...
+- Root build (`cmake -S . -B build`) configures and builds production targets (libnvm, nvmeservice, tutti_daemon). The `tutti/` subdirectory is added BEFORE the global libnvm include paths, ensuring contract targets do not inherit private include directories.
+- Single source of truth: libnvm at `tutti/device_manager/nvme/libnvm/`, snvme at `tutti/device_manager/nvme/kernel_modules/`, UAPI at `tutti/include/uapi/tutti_snvme.h`. No duplicate production implementation remains.
+- Phase 3 gate script (`scripts/phase3_gate.sh`) verifies: root build, standalone HOST build+ctest, standalone CUDA build+ctest, UAPI contract, two kernel baselines compile-only, dual-fact-source check, CMake include audit, header-hygiene consumer test.
```

## Files Not Modified (per constraints)

- No runtime behavior changes.
- No refactoring of Session 1-4 structures.
- No module/daemon/mount/bind/unbind/format/raw LBA IO executed by agent.
- No Git commit.
- `tests/CMakeLists.txt` (legacy root tests) — not modified (pre-existing `layer1_smoke_test` link failure for `tutti_accel` is unchanged; it only builds when `TUTTI_BUILD_HARDWARE_STACK=ON`).

## 总指挥验收（2026-08-01）

**PASS。Phase 3 确认关闭，Round 10 全部完成。**

独立核验：

- **门禁脚本重跑**：`scripts/phase3_gate.sh` 独立重跑 **10/10 PASS**（exit 0），与报告一致。
- **根 CMake 修复确认**：`add_subdirectory(tutti)`（:160）先于 libnvm 全局 `include_directories`（:166），tutti 子树不再继承私有 include。
- **header hygiene 独立运行**：`tutti_header_hygiene_test` 直跑 `all checks passed`；26 个私有头 `__has_include` 负例覆盖 libnvm/nvmeservice/snvme kernel/CUDA kernel，设计正确。
- **硬件契约**：对 S4 重编模块复跑 **616/0 + 115/0 PASS**，临时目录为空，内核零异常。
- **Roadmap 更新确认**：Phase 3 五项 deliverable 标记 [DONE]、Gate 标记 [PASSED]、两条 Known Bugs 标记 [CLOSED — Phase 3]、dual-source 划线关闭，与实际状态一致。
- **总指挥顺手修复**：门禁脚本产生的 `tutti/build_host/`、`tutti/build_cuda/` 未跟踪目录已加入 `.gitignore`。

**Phase 4（Metadata Pools and True Async IO）解除阻塞。**

## Phase 3 Status: CLOSED

All Phase 3 deliverables are marked [DONE]. The gate is marked [PASSED]. Phase 4 (Metadata Pools and True Async IO) may start.
