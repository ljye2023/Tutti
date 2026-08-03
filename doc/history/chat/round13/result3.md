# Round 13 Session 3 Result: 第二批退役（NVMeService 双树收敛与生产资产迁移）

## 概述

NVMeService 收敛为 tutti 侧唯一事实源；`tutti_daemon` 迁移到 tutti 侧；删除 `backends/local/NVMeService/`、`backends/local/nvme/`、`memory/`、`device_manager/`（root）、`examples/`。编辑期遵守 PARALLEL.md 行级分治协议，未自行构建验证。

## 迁移/删除清单

### 迁移

| 资产 | 原位置 | 新位置 | 说明 |
|------|--------|--------|------|
| `tutti_daemon.cpp` | `examples/tutti_daemon.cpp` | `tutti/device_manager/nvme/nvmeservice/examples/tutti_daemon.cpp` | 生产 daemon 源码，内容不变 |
| `tutti_daemon` target (root build) | `examples/CMakeLists.txt` | root `CMakeLists.txt` 行 427-432 | root build 直接引用 tutti 侧源码 |
| `tutti_daemon` target (standalone) | N/A（standalone 无此 target） | `tutti/device_manager/nvme/nvmeservice/examples/CMakeLists.txt` | standalone build 新增 |
| `nvmeservice` target source path | `backends/local/NVMeService/src/` | `tutti/device_manager/nvme/nvmeservice/src/` | root CMake 重定向 |

### 删除

| 目录 | 说明 |
|------|------|
| `backends/local/NVMeService/` | 双树中的 root 侧副本，tutti 侧为唯一源 |
| `backends/local/nvme/` | 独立 Makefile smoke，无 CMake 接线，无生产消费 |
| `backends/local/` | 父目录（子目录全删后清空） |
| `backends/` | 父目录（同上） |
| `memory/` | cache 概念已迁移到 `tutti/data_paths/local_nvme/metadata/`（Round 11 S2） |
| `device_manager/` (root) | 控制面已迁移到 `tutti/data_paths/local_nvme/control/`（Round 10 S2） |
| `examples/` | `tutti_daemon.cpp` 已迁移；其余 smoke 随 legacy 树退役（S2 已删 nvme_storage/block_storage/io_engine/coordinator/adapters） |

### 归档

| 文件 | 说明 |
|------|------|
| `doc/history/round13_retired_trees.md` | S2+S3 退役树清单与资产去向 |

## 一行接线 diff

root `CMakeLists.txt` 的 S3 改动（仅 S3 拥有区域）：

```diff
# memory/ — retired (Round 13 S3)
-add_subdirectory(memory)
+# memory/ — retired (Round 13 S3); cache concepts migrated to
+# tutti/data_paths/local_nvme/metadata/ (Round 11 S2).

# nvmeservice source root redirected
-set (nvmeservice_root "${PROJECT_SOURCE_DIR}/backends/local/NVMeService")
+set (nvmeservice_root "${PROJECT_SOURCE_DIR}/tutti/device_manager/nvme/nvmeservice")

# NVMeService examples + device_manager → tutti_daemon target
-add_subdirectory(backends/local/NVMeService/examples)
-# device_manager depends on nvmeservice ...
-add_subdirectory(device_manager)
+# tutti_daemon: production daemon (source migrated to tutti side, Round 13 S3).
+add_executable(tutti_daemon "${nvmeservice_root}/examples/tutti_daemon.cpp")
+target_link_libraries(tutti_daemon PRIVATE
+    nvmeservice libnvm gRPC::grpc++ protobuf::libprotobuf Threads::Threads CUDA::cudart)
+set_target_properties(tutti_daemon PROPERTIES
+    BUILD_RPATH "$ORIGIN:$ORIGIN/../lib" INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib")
+install(TARGETS tutti_daemon DESTINATION "bin")

# examples/ — retired
-add_subdirectory(examples)
+# examples/ — retired (Round 13 S3); tutti_daemon migrated to
+# tutti/device_manager/nvme/nvmeservice/examples/, smokes deleted with legacy trees.
```

tutti 侧新增（`tutti/device_manager/nvme/nvmeservice/examples/CMakeLists.txt`）：
```cmake
# ---- tutti_daemon (production) -------------------------------------------
add_executable(tutti_daemon tutti_daemon.cpp)
target_link_libraries(tutti_daemon PRIVATE
    nvmeservice libnvm gRPC::grpc++ protobuf::libprotobuf Threads::Threads CUDA::cudart)
set_target_properties(tutti_daemon PROPERTIES
    BUILD_RPATH "$ORIGIN:$ORIGIN/../lib" INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib")
install(TARGETS tutti_daemon DESTINATION "bin")
```

## 唯一源证据

### NVMeService 唯一实现

```console
$ find . -path "*/nvmeservice/src/*.cpp" -not -path "*/build/*" -not -path "*/third_pkgs/*" -not -path "*/.git/*"
./tutti/device_manager/nvme/nvmeservice/src/nvmeservice_client.cpp
./tutti/device_manager/nvme/nvmeservice/src/nvmeservice_config.cpp
./tutti/device_manager/nvme/nvmeservice/src/nvmeservice_server.cpp

$ find . -path "*/nvmeservice/CMakeLists.txt" -not -path "*/build/*" -not -path "*/third_pkgs/*"
./tutti/device_manager/nvme/nvmeservice/CMakeLists.txt
```

**唯一 NVMeService 实现**：`tutti/device_manager/nvme/nvmeservice/`。root build 的 `nvmeservice` target 从此路径编译（`nvmeservice_root` 重定向）。

### tutti_daemon 唯一源码

```console
$ find . -name "tutti_daemon.cpp" -not -path "*/build/*" -not -path "*/third_pkgs/*"
./tutti/device_manager/nvme/nvmeservice/examples/tutti_daemon.cpp
```

### backends/local 残留引用

```console
$ git grep -l "backends/local" -- ':!build/' ':!doc/history/' ':!third_pkgs/' ':!.gitignore'
CMakeLists.txt   # 仅注释（"backends/local/NVMeService/ deleted"）
Roadmap.md       # 历史文档引用
doc/architecture/system-architecture.md
doc/build_and_test.md
doc/design/*.md
doc/refactor/*.md
```

**活动代码零残留**：CMakeLists.txt 中的 `backends/local` 仅出现在退役注释中；其余命中均为历史文档（不在本 session 修改范围）。

**注**：`third_pkgs/Tutti/` 是第三方历史快照（非活动构建），内含旧 `backends/local/NVMeService/` 副本——不在 S1 决议范围内，不动。

## daemon 兼容验证

### 构建产物路径

| 维度 | 迁移前 | 迁移后 | 兼容 |
|------|--------|--------|------|
| 源码位置 | `examples/tutti_daemon.cpp` | `tutti/device_manager/nvme/nvmeservice/examples/tutti_daemon.cpp` | 源码内容不变 |
| root build target | `examples/CMakeLists.txt` 定义 | root `CMakeLists.txt` 行 427 定义 | ✅ |
| standalone build target | 无 | `tutti/.../examples/CMakeLists.txt` 定义 | ✅ 新增 |
| 构建产物路径 | `build/bin/tutti_daemon` | `build/bin/tutti_daemon` | ✅ 不变 |
| 启动命令 | `sudo ./build/bin/tutti_daemon --config sys_config.yaml` | 同左 | ✅ 不变 |
| 链接库 | nvmeservice + libnvm + gRPC + protobuf + Threads + CUDA | 同左 | ✅ 不变 |
| 配置格式 | `sys_config.yaml` | 同左 | ✅ 不变 |

### nvmeservice 库

| 维度 | 迁移前 | 迁移后 |
|------|--------|--------|
| 源码 | `backends/local/NVMeService/src/` (13 files) | `tutti/device_manager/nvme/nvmeservice/src/` (13 files, 内容相同) |
| proto 生成 | root binary dir | root binary dir（路径不变，`nvmeservice_root` 变但 proto 输出目录不变） |
| target 链接 | libnvm + CUDA + Threads + gRPC + protobuf + yaml-cpp | 同左 |

## PARALLEL.md 遵守

| 约束 | 遵守情况 |
|------|---------|
| 行级分治 | ✅ 仅修改 S3 拥有区域（memory 341、nvmeservice 347-432、device_manager 434、examples 446-447） |
| 不触碰 S2 区域 | ✅ 未修改 nvme_storage/block_storage/io_engine/coordinator/adapters/kv_cache 行 |
| 编辑期不构建 | ✅ 未执行 cmake/make/ctest |
| 留底 patch | ✅ `/tmp/r13-s3-before.patch` + `/tmp/r13-s3.patch` |
| standalone tutti/ 一致性 | ✅ tutti 侧 nvmeservice 为唯一源，standalone build 自洽 |

## 联合验证（待总指挥执行）

编辑期已完成，以下验证待 S2+S3 联合后由总指挥执行：

1. **三端构建**：root build + standalone HOST + standalone CUDA
2. **ctest 基线**：HOST 14/14 + CUDA 134/134（或当前基线）
3. **daemon 构建**：`cmake --build build --target tutti_daemon -j8`
4. **operator 重启 daemon**：`sudo ./build/bin/tutti_daemon --config sys_config.yaml`
5. **硬件契约**：datapath 735/0 + runtime 115/0

## 未改动项

- **nvmeservice 库逻辑**：源码内容不变（tutti 侧与已删 root 侧完全相同）
- **daemon 行为/协议/配置格式**：`tutti_daemon.cpp` 内容不变，`sys_config.yaml` 不变
- **libnvm/kernel_modules**：位置不变（`tutti/device_manager/nvme/libnvm/` + `kernel_modules/`）
- **standalone tutti/ 源码**：仅新增 `tutti_daemon.cpp` + examples CMakeLists.txt 中新增 target 定义
- **S2 拥有区域**：未触碰
- **Git**：未提交

## 已知 stale 文档引用

以下文档中仍有 `backends/local` 引用（历史文档，不在本 session 修改范围）：
- `Roadmap.md`
- `doc/architecture/system-architecture.md`
- `doc/build_and_test.md`
- `doc/design/gpu-abstraction.md`、`doc/design/kernel-portability.md`、`doc/design/storage-extensibility.md`
- `doc/refactor/LegacyDecomposition.md`、`doc/refactor/R5b_gpu_submit_plan.md`

建议在 S4（Phase 7 门禁）或后续文档清理 session 中统一更新。

## 总指挥验收（2026-08-02）

**PASS（联合验证 + daemon 迁移实证）。**

- **唯一源**：`tutti_daemon.cpp` 与 nvmeservice src 仅在 tutti 侧（find 复核）；`backends/local` 零残留引用（§6 grep 复核）。
- **daemon 迁移实证**：root build `tutti_daemon` target 构建通过；operator 以新二进制 `build/bin/tutti_daemon` 重启后两硬件契约 **735/0 + 115/0**，daemon 功能连续。
- **standalone daemon**：tutti 侧 target 存在，受 `gRPC_FOUND` 守卫（本机 standalone 未找到 gRPC 未构建——与可选 nvmeservice 设计一致，生产路径不受影响）。
- **联合验证**：HOST 14/14、CUDA 134/134、root 生产 target 全 PASS；`git diff --check` clean。
- **遗留文档**：`backends/local` 历史文档引用（Roadmap.md、doc/architecture、doc/build_and_test 等）由 S4 统一清理，归属明确。
