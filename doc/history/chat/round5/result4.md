# T-019 Result — Physical migration of in-repo SPI headers into the public include tree

Task: move the two SPI headers from `tutti/spi/` to `tutti/include/tutti/spi/`
so a single include root (`tutti/include`) serves both public and SPI headers,
eliminating the build-tree "two include roots" asymmetry, and update all
affected build definitions. Logical include paths (`<tutti/spi/...>`) are
unchanged; no `#include` statements change.

## 1. Concurrency check

```
$ ps -eo pid,etime,cmd | grep -E '[c]make|[c]test' | head
(empty)
```

No concurrent cmake/ctest processes. Proceeded with the migration.

## 2. Modified / moved files

| Path | Action |
| --- | --- |
| `tutti/spi/data_path.h` | moved → `tutti/include/tutti/spi/data_path.h` |
| `tutti/spi/storage_target_resolver.h` | moved → `tutti/include/tutti/spi/storage_target_resolver.h` |
| `tutti/spi/` (directory) | removed (`rmdir` succeeded — was empty) |
| `tutti/CMakeLists.txt` | modified (tutti_spi include interface + comment; removed `install(DIRECTORY spi/ ...)`) |
| `tests/data_path_contract/CMakeLists.txt` | modified (dropped repo-root include) |
| `tests/storage_target_resolver_contract/CMakeLists.txt` | modified (dropped repo-root include) |
| `chat/round5/result4.md` | created (this file) |

No SPI header content was modified — only file location.

## 3. md5 before/after (content zero-change proof)

```
=== md5 BEFORE ===
3fbe95085db2c35837143097ac3e4743  tutti/spi/data_path.h
6fa71c0b2d81ed820e7155fc7feaec81  tutti/spi/storage_target_resolver.h
=== md5 AFTER ===
3fbe95085db2c35837143097ac3e4743  tutti/include/tutti/spi/data_path.h
6fa71c0b2d81ed820e7155fc7feaec81  tutti/include/tutti/spi/storage_target_resolver.h
```

Both checksums match one-to-one. `rmdir tutti/spi` succeeded (directory was
empty). Old directory removed: PASS.

## 4. `tutti_spi` before/after CMake

Before:

```cmake
add_library(tutti_spi INTERFACE)
target_include_directories(tutti_spi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>   # repo root
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(tutti_spi INTERFACE tutti_api)
```

After:

```cmake
add_library(tutti_spi INTERFACE)
target_include_directories(tutti_spi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>   # public include root
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(tutti_spi INTERFACE tutti_api)
```

The comment block above it was rewritten to describe the new single-root
layout (no stale "repository include root" wording).

## 5. Why `tutti_spi` is retained, and its post-migration difference from `tutti_api`

Retained for three reasons:

1. It is the explicit "I consume the in-repo SPI" declaration point, distinct
   from "I only use the public API" (`tutti_api`) — a meaningful semantic
   boundary even when the include roots coincide.
2. It is already a named member of the `tutti_targets` export set; removing it
   would break installed-package consumers that link `tutti_spi`.
3. The target architecture's target list keeps `tutti_spi` as a separate entry.

Post-migration actual difference from `tutti_api`: **only semantic**. Both now
expose the identical include interface
(`$<BUILD_INTERFACE:.../include>` + `$<INSTALL_INTERFACE:include>`) and both
transitively pull `tutti_cuda_like` (so the `TUTTI_USE_<PROFILE>` macro is
identical). There is no longer a build-tree include-root difference. The
distinction is the named target / intent, not the usage requirements.

## 6. Install-rule change

Removed the now-redundant rule:

```cmake
# (deleted)
install(DIRECTORY spi/ DESTINATION include/tutti/spi
        FILES_MATCHING PATTERN "*.h")
```

The pre-existing recursive rule already installs the SPI headers from their new
location:

```cmake
install(DIRECTORY include/tutti/ DESTINATION include/tutti
        FILES_MATCHING PATTERN "*.h")
```

This recursively installs `include/tutti/spi/*.h` to `include/tutti/spi/`, so
SPI headers land at the correct install-tree path with no duplicate and no
path misalignment (verified in section 12).

## 7. Single-root compile verification (step 2)

```
$ printf '#include <tutti/memory_types.h>\n#include <tutti/spi/storage_target_resolver.h>\n#include <tutti/spi/data_path.h>\n#include <tutti/storage_runtime.h>\nint main(){ return 0; }\n' \
  | c++ -std=c++17 -Wall -Wextra -Werror -DTUTTI_USE_HOST=1 -I.../tutti/include -x c++ -fsyntax-only -
probe: PASS
```

A single root (`tutti/include`) resolves both public and SPI headers.

`grep -n -A4 'add_library(tutti_spi' tutti/CMakeLists.txt` shows
`BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include` — no `/..`.

## 8. HOST full configure/build/ctest (step 4)

```
$ cmake -S tutti -B build/round5-session4 -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
-- Tutti: added tests/cuda_like (contract test)
-- Tutti: added tests/public_api (public API usage test)
-- Tutti: added tests/spi_consumer (SPI usage-requirements test)
-- Configuring done (0.4s)
```

No CUDA / gRPC / yaml-cpp / libnvm / NVMe discovery (HOST forces
`TUTTI_BUILD_HARDWARE_STACK=OFF`).

```
$ cmake --build build/round5-session4 -j8
[100%] Built target tutti_spi_consumer_test
```

```
$ ctest --test-dir build/round5-session4 --output-on-failure
1/3 Test #1: cuda_like_contract_test ..........   Passed
2/3 Test #2: tutti_public_api_usage_test ......   Passed
3/3 Test #3: tutti_spi_consumer_test ..........   Passed
100% tests passed, 0 tests failed out of 3
```

## 9. `tutti_spi_consumer_test` passed without modifying its CMake

`tests/spi_consumer/CMakeLists.txt` was **not touched** by this task (only
links `tutti_spi`). It built and passed (Test #3 above) because `tutti_spi`'s
usage requirements transparently followed the migration — the consumer now
gets `-I.../tutti/include` (single root) instead of `-I.../tutti/..`.

## 10. Two standalone contract tests (step 5)

```
data_path_contract:           1/1 Test #1: tutti_data_path_contract_test ....   Passed
storage_target_resolver_contract: 1/1 Test #1: tutti_storage_target_resolver_contract_test ...   Passed
```

Both built with the single `tutti/include` root (repo root dropped) and passed.

## 11. `tests/binding_contract` (step 6)

`tests/binding_contract/` exists. It was **not modified** by this task. Built
and run with its own CMake:

```
1/1 Test #1: tutti_binding_contract_test ......   Passed
```

It is resilient to the migration because it deliberately provides both the
repo root and `tutti/include`; after migration the SPI headers resolve via the
`tutti/include` root it already supplies.

## 12. Install smoke + install-tree single-root compile + installed file list (step 7)

```
$ cmake --install build/round5-session4 --prefix build/round5-session4/_install
-- Installing: .../include/tutti/spi
-- Installing: .../include/tutti/spi/data_path.h
-- Installing: .../include/tutti/spi/storage_target_resolver.h
```

Full installed header list (sorted):

```
include/tutti/abstraction/accel.h
include/tutti/cuda_like.h
include/tutti/gpu_vendor/host.h
include/tutti/io_types.h
include/tutti/memory_types.h
include/tutti/spi/data_path.h
include/tutti/spi/storage_target_resolver.h
include/tutti/status.h
include/tutti/storage_runtime.h
```

SPI headers present at `include/tutti/spi/`; no duplicates, no path misalignment.

Install-tree single-root compile:

```
$ (cd .../_install && printf '#include <tutti/spi/data_path.h>\n#include <tutti/spi/storage_target_resolver.h>\nint main(){ return 0; }\n' \
  | c++ -std=c++17 -Wall -Wextra -Werror -DTUTTI_USE_HOST=1 -Iinclude -x c++ -fsyntax-only -)
install-tree compile: PASS
```

## 13. SPI header content zero-change md5 proof (step 8)

```
3fbe95085db2c35837143097ac3e4743  tutti/include/tutti/spi/data_path.h
6fa71c0b2d81ed820e7155fc7feaec81  tutti/include/tutti/spi/storage_target_resolver.h
```

Identical to the pre-migration checksums (section 3). Content unchanged.

## 14. Residual old-path grep (step 9), each hit explained

`grep -rn 'tutti/spi/' --include=CMakeLists.txt . | grep -v 'include/tutti/spi'`:

1. `tests/data_path_contract/CMakeLists.txt:13:# After the SPI-header migration, <tutti/spi/...> and <tutti/status.h> /` — a comment referencing the **logical** include path `<tutti/spi/...>` (not a filesystem path). Benign; the actual include root is `tutti/include`.
2. `tests/spi_consumer/CMakeLists.txt:5:#   - repository include root   -> <tutti/spi/...> resolves` — a comment in a file this task is **forbidden to modify**. The wording "repository include root" is now stale (the root is the public include root), but functionally the consumer links `tutti_spi` and builds correctly. Recorded as a known stale comment; not fixed per task constraints.
3. `tests/binding_contract/CMakeLists.txt:19:#   - /data/home/ryeqiu/Tutti : current layout (tutti/spi/*.h)` — a comment in another worker's file this task is **forbidden to modify**. It describes the old layout; the test still builds and passes (it supplies `tutti/include` too). Recorded; not fixed.
4. `tutti/CMakeLists.txt:127:# (tutti/include, which now also hosts <tutti/spi/...>), and -- transitively` — my updated comment referencing the logical include path. Benign.

`grep -rn '"\.\./\.\./spi/\|"\.\./spi/' --include=*.h --include=*.cpp .`: **no hits** — no relative-path `#include` of SPI headers anywhere.

No residual filesystem-path references that would break; the only hits are
logical-path mentions in comments (two of which are in files I may not modify).

## 15. Full `git diff -- tutti/CMakeLists.txt`

Note: `tutti/CMakeLists.txt` was already `modified` (uncommitted) at session
start — the working tree already contained prior workers' profile-gating
refactor (`TUTTI_ACCELERATOR` HOST/CUDA, moving `find_package` behind
`TUTTI_BUILD_HARDWARE_STACK`, adding `tutti_cuda_like`/`tutti_api`/`tutti_types`,
the `cuda_like`/`public_api`/`spi_consumer` test wiring, and the
`tutti_cuda_like`/`tutti_api`/`tutti_spi` install rules). The diff below is vs
the last commit, so it includes that prior work. **This task's migration
changes are exactly two:** (a) the `tutti_spi` block now uses
`${CMAKE_CURRENT_SOURCE_DIR}/include` (not `/..`) with an updated comment, and
(b) the `install(DIRECTORY spi/ ...)` rule is absent (it was a prior-round
addition, now removed — net zero vs HEAD). No existing content was refactored
by this task beyond those two.

```diff
diff --git a/tutti/CMakeLists.txt b/tutti/CMakeLists.txt
index 0021ad6..<new> 100644
--- a/tutti/CMakeLists.txt
+++ b/tutti/CMakeLists.txt
@@ -20,7 +20,27 @@
 
 cmake_minimum_required(VERSION 3.18)
 
-project(Tutti LANGUAGES C CXX CUDA)
+# Accelerator profile (must be defined and validated before project())
+set(TUTTI_ACCELERATOR "CUDA" CACHE STRING "Tutti accelerator profile: CUDA|HOST")
+string(TOUPPER "${TUTTI_ACCELERATOR}" TUTTI_ACCELERATOR)
+if(NOT TUTTI_ACCELERATOR MATCHES "^(CUDA|HOST)$")
+    message(FATAL_ERROR "TUTTI_ACCELERATOR='${TUTTI_ACCELERATOR}' is not supported. "
+        "Supported values: CUDA, HOST")
+endif()
+
+if(TUTTI_ACCELERATOR STREQUAL "CUDA")
+    project(Tutti LANGUAGES C CXX CUDA)
+else()
+    project(Tutti LANGUAGES C CXX)
+endif()
@@ -32,46 +52,15 @@ endif()
 set(CMAKE_C_STANDARD 99)
 set(CMAKE_CXX_STANDARD 17)
 set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
-... (prior refactor: CMAKE_CUDA_* and find_package moved behind profile / TUTTI_BUILD_HARDWARE_STACK) ...
+if(TUTTI_ACCELERATOR STREQUAL "CUDA")
+    set(CMAKE_CUDA_STANDARD 17)
+    ...
+endif()
@@ -101,6 +92,53 @@ include_directories("${CMAKE_CURRENT_SOURCE_DIR}")
+include(cmake/accelerators/${TUTTI_ACCELERATOR}.cmake)
+
+add_library(tutti_cuda_like INTERFACE)
+tutti_configure_cuda_like(tutti_cuda_like)
+
+add_library(tutti_api INTERFACE)
+target_include_directories(tutti_api INTERFACE
+    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
+    $<INSTALL_INTERFACE:include>
+)
+target_link_libraries(tutti_api INTERFACE tutti_cuda_like)
+
+# tutti_spi -- in-repo SPI usage-requirements target          <-- THIS TASK (a)
+# Consumers link only tutti_spi to obtain: the public include root
+# (tutti/include, which now also hosts <tutti/spi/...>), and -- transitively
+# via tutti_api -> tutti_cuda_like -- the TUTTI_USE_<PROFILE> macro. No source
+# files; no direct CUDA links; no repeated profile definition. Not gated by
+# TUTTI_BUILD_HARDWARE_STACK: the SPI headers are hardware-free source-level
+# contracts available in every profile, like tutti_api. After the SPI-header
+# migration the build-tree and install-tree layouts match (single include
+# root), so tutti_spi and tutti_api share the same include interface; tutti_spi
+# is retained as the explicit "I consume the in-repo SPI" declaration point
+# and as a named member of the tutti_targets export set.
+
+add_library(tutti_spi INTERFACE)
+target_include_directories(tutti_spi INTERFACE
+    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
+    $<INSTALL_INTERFACE:include>
+)
+target_link_libraries(tutti_spi INTERFACE tutti_api)
@@ -116,23 +157,59 @@ target_include_directories(tutti_types INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
+target_link_libraries(tutti_types INTERFACE tutti_cuda_like)
-# Layer 1: Accelerator HAL (libtutti_accel)
-add_subdirectory(accel)
-... (prior refactor: layers moved behind if(TUTTI_BUILD_HARDWARE_STACK)) ...
+if(TUTTI_BUILD_HARDWARE_STACK)
+    find_package(CUDAToolkit REQUIRED)
+    ...
+    add_subdirectory(accel)
+    add_subdirectory(device_manager)
+    ...
+endif()
@@ -146,47 +220,70 @@ option(BUILD_TESTING "Build Tutti tests" OFF)
 if(BUILD_TESTING)
     include(CTest)
+    add_subdirectory(".../tests/cuda_like" ...)
+    add_subdirectory(".../tests/public_api" ...)
+    add_subdirectory(".../tests/spi_consumer" ...)
     if(TUTTI_BUILD_HARDWARE_STACK)
         ... (existing hardware-stack test subdirs) ...
     endif()
 endif()
@@ -200,4 +297,36 @@ install(DIRECTORY types/ ...)
+install(DIRECTORY include/tutti/ DESTINATION include/tutti
+        FILES_MATCHING PATTERN "*.h")
+install(TARGETS tutti_cuda_like EXPORT tutti_targets ...)
+install(TARGETS tutti_api EXPORT tutti_targets ...)
+install(TARGETS tutti_spi EXPORT tutti_targets ...)
+# NOTE: no install(DIRECTORY spi/ ...) rule -- removed by THIS TASK (b);
+#       the include/tutti/ recursive rule above installs the SPI headers.
 # Layer-specific headers installed by their own CMakeLists.txt
```

(The abridged `...` sections are the prior uncommitted profile-gating refactor,
verbatim from the working tree before this task; they are not this task's
changes. The complete diff is 383 lines / 225 insertions / 97 deletions vs the
last commit; the 97 deletions and the non-`tutti_spi` insertions are all prior
work. This task's two changes are `+`/removal only within the `tutti_spi` block
and the install section — no other existing lines touched.)

The two contract-test CMakeLists are untracked, so `git diff` does not show
them; their changes are shown in section 4's style (repo-root line removed,
single `tutti/include` root kept):

`tests/data_path_contract/CMakeLists.txt`:
```cmake
# After the SPI-header migration, <tutti/spi/...> and <tutti/status.h> /
# <tutti/io_types.h> all resolve from the single public include root.
target_include_directories(tutti_data_path_contract_test PRIVATE
    /data/home/ryeqiu/Tutti/tutti/include)
```

`tests/storage_target_resolver_contract/CMakeLists.txt`:
```cmake
target_include_directories(tutti_storage_target_resolver_contract_test PRIVATE
    /data/home/ryeqiu/Tutti/tutti/include
)
```

(Target names, CTest names, compile options, and source lists unchanged.)

## 16. File-boundary & whitespace checks (step 10)

```
$ git diff --check -- tutti/CMakeLists.txt tests/data_path_contract/CMakeLists.txt tests/storage_target_resolver_contract/CMakeLists.txt
(clean, exit=0)
```

Trailing-whitespace scan over the three modified files: empty (`ws_exit=1`).
EOF-newline: all three OK.

Touched files (allowed list only):

```
 M tutti/CMakeLists.txt
?? tests/data_path_contract/CMakeLists.txt          (untracked; CMakeLists edited this task)
?? tests/storage_target_resolver_contract/CMakeLists.txt (untracked; CMakeLists edited this task)
?? tutti/include/tutti/spi/data_path.h              (moved here)
?? tutti/include/tutti/spi/storage_target_resolver.h (moved here)
```

`tutti/spi/` no longer appears in `git status` (directory removed). The
`tests/binding_contract/**`, `tests/spi_consumer/**`, and the contract test
`.cpp` files appear as untracked but were **not** modified by this task (other
workers' / prior rounds' files). No file outside the allowed list was modified.

## 17. Final status

PASS

All success criteria met:

1. Both SPI headers now at `tutti/include/tutti/spi/`; `tutti/spi/` removed.
2. md5 before/after identical (content zero-change).
3. Single include root `tutti/include` resolves public + SPI headers.
4. `tutti_spi` `BUILD_INTERFACE` no longer contains the repo root.
5. `tutti_spi` retained; post-migration difference from `tutti_api` is semantic
   only (named intent / export-set member), documented.
6. Redundant `install(DIRECTORY spi/ ...)` removed; SPI headers still install to
   `include/tutti/spi/` via the recursive `include/tutti/` rule.
7. Install-tree single-root compile passes.
8. HOST full configure/build/ctest passes (3/3).
9. `tutti_spi_consumer_test` passes with its CMake unmodified.
10. Both standalone contract tests pass (1/1 each).
11. `tests/binding_contract` passes (unmodified).
12. No residual relative-path SPI includes; remaining `tutti/spi/` hits are
    logical-path comment references (two in files I may not modify, recorded).
13. `tutti/CMakeLists.txt` changes are minimal/surgical (two migration edits);
    no existing content refactored.
14. No file outside the allowed list modified.
15. No module, daemon, or IO operation executed.
16. Whitespace / EOF-newline checks pass.

## 总指挥验收

验收结论：`PASS`（含 1 项范围外遗留，需在 Round 6 处理）。

### 独立核验结果

- `tutti/spi/` 已消失；两个头位于 `tutti/include/tutti/spi/`。
- md5 与 worker 记录的迁移前值**逐一相等**：`3fbe95085db2c35837143097ac3e4743`（`data_path.h`）、`6fa71c0b2d81ed820e7155fc7feaec81`（`storage_target_resolver.h`）。内容零改动成立。
- `tutti_spi` 的 `BUILD_INTERFACE` 现为 `${CMAKE_CURRENT_SOURCE_DIR}/include`（第 140 行），`/..` 已消除。
- 多余的 `install(DIRECTORY spi/ ...)` 确已删除；安装树布局为 `include/tutti/spi/{data_path,storage_target_resolver}.h`，无重复、无错位。
- 注释块措辞已更新，无「repository include root」这类过时描述。
- 三处 `EXPORT tutti_targets` 保留，`tutti_spi` 仍在 export 集合内。
- 我独立重跑：HOST 全量 `3/3 Passed`；三个独立构建目录（`dpc`/`strc`/`bind`）各 `1/1 Passed`。
- 两个 contract test 的 include root 已收敛为单一 `tutti/include`；`add_executable` / `add_test` / `target_compile_options` 的名称与内容未变，符合「只改 include 路径」的纪律。
- 无残留相对路径 SPI include（grep 零命中）。
- 公共契约头零改动；`tutti/include/tutti/**` 除新增 spi 目录外无变更。
- `tutti/CMakeLists.txt` 尾随空白与 EOF newline OK；`git diff --check` 通过。
- 未执行 sudo、模块操作、daemon 或任何硬件 IO。

### 我补了两个否定实验：证明迁移是真实的、不是双路可达

worker 证明了「新路径可用」，但没有证明「旧路径已不可用」。若两条路都通，迁移就只是**新增**而非**迁移**，旧的暴露面依然存在。我补做：

仓库根单独作为 root：

```text
c++ -I/data/home/ryeqiu/Tutti  <<< '#include <tutti/spi/data_path.h>'
  -> fatal error: tutti/spi/data_path.h: No such file or directory
```

`tutti/` 目录单独作为 root（即那个既有的全局 `include_directories`）：

```text
c++ -I/data/home/ryeqiu/Tutti/tutti  <<< '#include <tutti/spi/data_path.h>'
  -> fatal error: tutti/spi/data_path.h: No such file or directory
```

两条旧路径均已断开，SPI 头的解析**唯一**来自 `tutti/include`。这两个否定实验合起来才构成迁移的硬证据。

### `tutti_spi_consumer_test` 的通过是真凭据，我核到了 flag 层

我从 `compile_commands.json` 抽出该 target 的实际 flag：

```text
-DTUTTI_USE_HOST=1
-I/data/home/ryeqiu/Tutti/tutti
-I/data/home/ryeqiu/Tutti/tutti/include
```

结合上面第二个否定实验（`-I.../tutti` 单独无法解析 SPI），可以断定该测试的 SPI 解析确实来自 `tutti_spi` 贡献的 `tutti/include`，而非被别处顺带提供。其 CMakeLists mtime 为 17:10（早于本 session 的 22:52 约 5.7 小时），确证未被修改。`tests/binding_contract/CMakeLists.txt` 同理（19:37），未被触碰而仍通过 —— 因为它同时提供了 `tutti/include`，Session 3 的布局无关设计生效了。

### 关于「问题 2：暴露面过大」——只解决了一半，需 Round 6 收尾

prompt 列出的三个迁移动机中，问题 1（build/install 布局不对称）与问题 3（contract test 硬编码仓库根）已彻底解决。但**问题 2 只解决了一半**，这一点 worker 未察觉，我在核 flag 时发现：

`tutti_spi` 不再贡献仓库根，但 `tutti/CMakeLists.txt:91` 有一个**既有的全局** `include_directories(${CMAKE_CURRENT_SOURCE_DIR})`，它把 `tutti/` 注入到该工程内**所有** target。而 `tutti/` 下有：

```text
tutti/accel/  tutti/backends/  tutti/bindings/  tutti/block_storage/
tutti/coordinator/  tutti/device_manager/  tutti/io_engine/  tutti/raw_device/
```

因此 tutti 工程内的 SPI 消费者依然能 `#include <backends/...>`、`#include <io_engine/...>` 等。迁移把暴露面从「仓库根」缩到「`tutti/`」，量级变小了，但受控边界仍未真正闭合。

**责任不在 worker。** 该全局是既有内容，prompt 第 209-213 行明确禁止改动既有结构、禁止「顺手改进」。worker 严格遵守了外科手术式纪律，这是正确的。另外要说明：**通过 `tutti_spi` 从外部消费的路径已经干净了** —— 独立的三个 contract test 构建目录都不经过 `tutti/CMakeLists.txt`，因此不受该全局影响。受影响的只有 tutti 工程内部的 target。

Round 6 应立项：审查并收敛 `tutti/CMakeLists.txt:91` 的全局 `include_directories`。这是独立任务，需逐个确认现有 target 是否真的依赖该全局路径（大概率有一批 `.cu`/`.cpp` 靠它解析 `backends/...` 之类的 include），风险高于本次迁移，不能顺手做。

### 非阻塞观察（记录，不返工）

1. **两处过时注释留在禁改文件里。** `tests/spi_consumer/CMakeLists.txt:5` 写「repository include root -> `<tutti/spi/...>` resolves」，`tests/binding_contract/CMakeLists.txt:19` 写「`/data/home/ryeqiu/Tutti` : current layout (`tutti/spi/*.h`)」。两者现已与事实不符。worker 按 prompt 禁令未改并如实记录，处置正确。这些注释不影响构建（两个测试都通过），但会误导后续读者。Round 6 顺手更正即可，一并归入上述 include root 收敛任务。

2. **`tests/binding_contract` 的双 root 现在有一个是冗余的。** 它提供的仓库根迁移后已不解析任何东西。保留无害（它正是这个"布局无关"设计让它免于被本次迁移波及），但将来可精简为单根。

3. **`tutti_spi` 与 `tutti_api` 现在的 usage requirements 完全相同**，只剩语义差异。worker 如实写明了这一点，没有粉饰。我认可保留决定：export 集合成员名一旦发布就不能随意删，且「我消费仓内 SPI」这一意图声明有独立价值。但需留意——两者一旦长期完全等价，将来可能有人误以为可以互换。若日后 SPI 需要不同的 usage requirements（例如额外的 warning 级别或 `SYSTEM` 属性），那正是它们分化的时机。

4. `result4.md` 有 2 处尾随空白，均为 `" "`（单空格）且落在嵌入的 ```` ```diff ```` 块内（第 258、260 行），是 diff 上下文空行的正确形式，非真实卫生问题。worker 对三个源文件的检查结论准确。

5. worker 在第 15 节明确区分了「本任务的两处改动」与「工作树中既有的未提交 profile 门控重构」，并说明后者不属本次。我在 Round 2/Round 4 已分别验收过那部分，归属说明准确。这种在共享文件上主动划清责任边界的做法值得肯定。

### 后续决定

T-019 完成，不需要返工。**Round 5 四个 session 全部通过。** SPI 头布局收敛完成，build tree 与 install tree 物理一致，逻辑 include 路径全程未变。
