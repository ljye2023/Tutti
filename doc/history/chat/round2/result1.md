# T-004 Worker Result

## 1. Modified Files

- `/data/home/ryeqiu/Tutti/CMakeLists.txt` — single targeted edit before `add_subdirectory(tutti)`

No other files were modified or created by this worker (aside from this result file).

## 2. Minimal Fix Description

**Root cause:** The root `CMakeLists.txt` calls `add_subdirectory(tutti)`. The nested `tutti/CMakeLists.txt` includes `cmake/accelerators/CUDA.cmake`, which sets `TUTTI_BUILD_HARDWARE_STACK ON CACHE BOOL` (without FORCE). When `TUTTI_BUILD_HARDWARE_STACK` is ON, `tutti/` recursively adds `accel`, `device_manager`, `backends`, and `io_engine` subdirectories, which re-declare `libnvm`, `nvmeservice`, `nvmeservice_daemon_example`, `nvmeservice_client_example`, and `tutti_device_manager` — the same targets the root already declares from `backends/local/`, `device_manager/`, etc. CMake's global target namespace then fails with "cannot create target ... because another target with the same name already exists".

**Fix:** In the root `CMakeLists.txt`, immediately before the `add_subdirectory(tutti)` block, add:

```cmake
set(TUTTI_BUILD_HARDWARE_STACK OFF CACHE BOOL
    "Nested tutti/ hardware stack (forced OFF when included from repo root)" FORCE)
```

The `FORCE` keyword overwrites any existing cache value (including a previously cached `ON`). When `tutti/CMakeLists.txt` subsequently includes `CUDA.cmake`, its `set(TUTTI_BUILD_HARDWARE_STACK ON CACHE BOOL ...)` executes without `FORCE`, so the existing `OFF` cache value is preserved. The `if(TUTTI_BUILD_HARDWARE_STACK)` guard in `tutti/CMakeLists.txt` then evaluates to false, and the nested hardware stack is skipped.

**Standalone `cmake -S tutti` is unaffected:** the root `CMakeLists.txt` is never processed, so the `FORCE` set never runs. `CUDA.cmake` sets the default `ON` in the standalone build's own cache directory.

**Constraints honored:**
- No target renamed.
- No `if(NOT TARGET ...)` guards added around `add_library`/`add_executable`.
- No source tree deleted.
- No new user-visible option created (`TUTTI_BUILD_HARDWARE_STACK` already exists in `CUDA.cmake`).
- HOST/CUDA standalone build contract unchanged.
- Cache with prior `ON` value is reliably overwritten by `FORCE`.

## 3. Root Configure Command and Result

Command:
```bash
cmake -S /data/home/ryeqiu/Tutti -B /data/home/ryeqiu/Tutti/build
```

Exit code: `0`

Key output lines:
```
-- Added tutti subdirectory (hardware stack disabled from root)
-- Configuring done (0.2s)
-- Generating done (0.1s)
-- Build files have been written to: /data/home/ryeqiu/Tutti/build
```

## 4. Duplicate-Target Check

Command:
```bash
cmake -S /data/home/ryeqiu/Tutti -B /data/home/ryeqiu/Tutti/build 2>&1 | grep -iE 'cannot create target|already exists|duplicate'
```

Result: no matches (grep exit code 1). The five duplicate-target errors (`libnvm`, `nvmeservice`, `nvmeservice_daemon_example`, `nvmeservice_client_example`, `tutti_device_manager`) are eliminated.

Production targets verified present in `build/Makefile`:
```
libnvm
nvmeservice
nvmeservice_daemon_example
nvmeservice_client_example
tutti_device_manager
tutti_daemon
```

## 5. nvmeservice_client_example Build Result

Command:
```bash
cmake --build /data/home/ryeqiu/Tutti/build --target nvmeservice_client_example -j8
```

Exit code: `0`

Binary:
```
/data/home/ryeqiu/Tutti/build/bin/nvmeservice_client
-rwxr-xr-x 1 root root 2087504 Jul 30 15:19 /data/home/ryeqiu/Tutti/build/bin/nvmeservice_client
ELF 64-bit LSB executable, x86-64, dynamically linked
```

## 6. tutti_daemon Build Result

Command:
```bash
cmake --build /data/home/ryeqiu/Tutti/build --target tutti_daemon -j8
```

Exit code: `0`

Binary:
```
/data/home/ryeqiu/Tutti/build/bin/tutti_daemon
-rwxr-xr-x 1 root root 2761280 Jul 30 15:21 /data/home/ryeqiu/Tutti/build/bin/tutti_daemon
ELF 64-bit LSB executable, x86-64, dynamically linked
```

## 7. git diff --check Result

Command:
```bash
git diff --check -- CMakeLists.txt
```

Exit code: `0` (no whitespace errors)

Full diff:
```diff
diff --git a/CMakeLists.txt b/CMakeLists.txt
index 152ce5a..10912ef 100644
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -330,10 +330,21 @@ install (TARGETS libnvm DESTINATION "lib")
 #   device_manager  -> libnvm + nvmeservice (gRPC client)
 # ---------------------------------------------------------------------------

-# Add tutti subdirectory if it exists
+# Add tutti subdirectory if it exists.
+#
+# When configuring from the repository root, the root CMakeLists.txt owns all
+# production hardware targets (libnvm, nvmeservice, nvmeservice examples,
+# device_manager, etc.).  The nested tutti/ subdirectory must only contribute
+# Phase 0 contract targets (tutti_cuda_like, tutti_types, ...).  Force the
+# nested hardware stack OFF so it does not re-declare the same targets.
+#
+# Standalone `cmake -S tutti` is unaffected: it uses its own build directory
+# and cmake/accelerators/CUDA.cmake still defaults TUTTI_BUILD_HARDWARE_STACK=ON.
+set(TUTTI_BUILD_HARDWARE_STACK OFF CACHE BOOL
+    "Nested tutti/ hardware stack (forced OFF when included from repo root)" FORCE)
 if(EXISTS "${PROJECT_SOURCE_DIR}/tutti/CMakeLists.txt")
     add_subdirectory(tutti)
-    message(STATUS "Added tutti subdirectory")
+    message(STATUS "Added tutti subdirectory (hardware stack disabled from root)")
 endif()

 add_subdirectory(memory)
```

## 8. Final Status

PASS

## 总指挥验收

验收结论：`PASS`。

独立核验结果：

- 根 `CMakeLists.txt` 只有一处针对性改动：在纳入 `tutti/` 前以 cache `FORCE` 将嵌套 hardware stack 设为 `OFF`。
- 根构建 cache 为 `TUTTI_BUILD_HARDWARE_STACK:BOOL=OFF`。
- `build/bin/nvmeservice_client` 存在且可执行。
- `build/bin/tutti_daemon` 存在且可执行。
- 五组 duplicate-target 配置错误已解除，历史根目录 production targets 保持可构建。
- `git diff --check -- CMakeLists.txt chat/round2/result1.md` 通过。
- `CMakeLists.txt` 和本结果文件均无 IDE/linter 诊断。
- 未发现 worker 修改允许列表外的源码；`tutti/CMakeLists.txt` 的既有改动来自已完成的 T-002，不属于本 session。
- 未启动 daemon，未访问 block device，未执行内核模块操作或硬件 IO。

后续决定：Round 2 Session 2、3、4 可以开始执行；三者的可修改文件集合互不重叠。
