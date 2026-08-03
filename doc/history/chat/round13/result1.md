# Round 13 Session 1 Result: Legacy 退役审计与性能基线捕获

## 1. 执行摘要

**结论：审计完成，基线已捕获。**

- 10 棵 legacy 树已逐树审计，产出依赖图、决议表与退役顺序。
- NVMeService 双树确认：`backends/local/NVMeService/` 与 `tutti/device_manager/nvme/nvmeservice/` 内容完全一致（`diff -rq` 无差异），与 Round 10 S1 前的 libnvm 同类风险。
- `tutti_daemon` 源码在 `examples/tutti_daemon.cpp`，是唯一需要迁移的生产资产。
- legacy `tests/` 的 `layer1_smoke_test` 存在 pre-existing 链接失败（`-ltutti_accel` not found，根 build 强制 `TUTTI_BUILD_HARDWARE_STACK=OFF` 不定义 `tutti_accel`）。
- 性能基线：datapath contract 735/0，runtime contract 115/0，3 轮 wall-clock 中位 18320ms。
- 复跑脚本：`scripts/round13_baseline.sh`。

## 2. 逐树依赖图

### 2.1 `memory/`

| 维度 | 证据 |
|---|---|
| **谁 include 它** | 无外部 `#include "memory/include/..."` 路径引用；消费通过 CMake target 的 PUBLIC include（bare header name） |
| **谁链接它 (CMake)** | `block_storage`（`tutti_memory` for GpuSlotPool）、`io_engine`（IMemorySubsystem, IoSliceView）、`nvme_storage`（GpuSlotPool）、`coordinator`（IMemorySubsystem, DescriptorFormat）、`examples`（memory_smoke, tiered_handle_cache_smoke 等） |
| **standalone `tutti/` 是否依赖** | **否**。`tutti/CMakeLists.txt` 无 `add_subdirectory(../memory)` 或 link `tutti_memory` |
| **生产运行是否依赖** | **否**。`tutti_daemon` 不链接 `tutti_memory`；生产路径是 `tutti/data_paths/local_nvme/`（Round 11 已迁移 cache 概念到 `metadata/handle_workspace_cache.h` + `prp_page_cache.h`） |
| **target 名** | `tutti_memory`（root build only） |

### 2.2 `device_manager/` (root)

| 维度 | 证据 |
|---|---|
| **谁 include 它** | `tests/layer3_integration_simple.cu`、`tests/layer3_integration_test.cu`（`#include "device_manager/include/common/vdevice.h"`） |
| **谁链接它 (CMake)** | `block_storage`（tutti_device_manager）、`nvme_storage`（tutti_device_manager）、`coordinator`（tutti_device_manager）、`examples`（多个 smoke target） |
| **standalone `tutti/` 是否依赖** | **否**。`tutti/` 有自己的 `tutti/device_manager/`（libnvm + nvmeservice + kernel_modules）。root `device_manager/` 是独立的旧实现（LocalNvmeDirectRegistry, NvmeServiceBackedRegistry, NvmeQueueGroup 等） |
| **生产运行是否依赖** | **否**。`tutti_daemon` 不链接 root `tutti_device_manager` |
| **target 名** | `tutti_device_manager`（root build，与 `tutti/` 侧的同名 target 冲突——CMake target namespace 全局，root build 中两者共存是历史遗留） |
| **内容差异** | root `device_manager/` 含 `local_nvme_direct_registry.*`、`nvmeservice_backed_registry.*`、`nvme_queue_group.*`、`device_registry.h`、`lease_manager.h`、`local_nvme_device.h`；与 `tutti/data_paths/local_nvme/control/`（Round 10 S2 迁移后的控制面）是不同实现 |

### 2.3 `nvme_storage/`

| 维度 | 证据 |
|---|---|
| **谁 include 它** | 无外部路径引用；通过 CMake target 的 PUBLIC include |
| **谁链接它 (CMake)** | `block_storage`（tutti_nvme_storage）、`io_engine`（transitive）、`coordinator`（transitive）、`examples`（多个 smoke） |
| **standalone `tutti/` 是否依赖** | **否** |
| **生产运行是否依赖** | **否** |
| **target 名** | `tutti_nvme_storage`（root build only） |

### 2.4 `block_storage/`

| 维度 | 证据 |
|---|---|
| **谁 include 它** | 无外部路径引用 |
| **谁链接它 (CMake)** | `io_engine`（transitive）、`coordinator`（tutti_block_storage）、`examples`（多个 smoke） |
| **standalone `tutti/` 是否依赖** | **否** |
| **生产运行是否依赖** | **否** |
| **target 名** | `tutti_block_storage`（root build only） |

### 2.5 `io_engine/`

| 维度 | 证据 |
|---|---|
| **谁 include 它** | 无外部路径引用；`device_manager/CMakeLists.txt:32` 引用 `${PROJECT_SOURCE_DIR}/io_engine/include`（for `backend_type.h`, `capability_set.h`） |
| **谁链接它 (CMake)** | `coordinator`（transitive）、`adapters/kv_cache`（transitive via coordinator）、`examples`（io_engine_smoke 等） |
| **standalone `tutti/` 是否依赖** | **否**（`tutti/` 有自己的 `tutti/io_engine/`） |
| **生产运行是否依赖** | **否** |
| **target 名** | `tutti_io_engine_local_nvme`（root build only） |

### 2.6 `coordinator/`

| 维度 | 证据 |
|---|---|
| **谁 include 它** | `device_manager/CMakeLists.txt:31` 引用 `${PROJECT_SOURCE_DIR}/coordinator/include`（for `coordinator/device.h`） |
| **谁链接它 (CMake)** | `adapters/kv_cache`（tutti_coordinator）、`examples`（e2e_smoke 等） |
| **standalone `tutti/` 是否依赖** | **否**（standalone `tutti/CMakeLists.txt:283` 注释了 `# add_subdirectory(coordinator)`） |
| **生产运行是否依赖** | **否** |
| **target 名** | `tutti_coordinator`（root build only） |

### 2.7 `adapters/kv_cache/`

| 维度 | 证据 |
|---|---|
| **谁 include 它** | `examples/adapters/kv_cache_adapter_smoke.cu`（`#include "../../adapters/kv_cache/include/kv_cache_io_adapter.h"`） |
| **谁链接它 (CMake)** | `examples`（kv_cache_adapter_smoke, kv_cache_e2e_stress） |
| **standalone `tutti/` 是否依赖** | **否** |
| **生产运行是否依赖** | **否** |
| **target 名** | `tutti_kv_adapter`（root build only） |

### 2.8 `backends/local/NVMeService/`

| 维度 | 证据 |
|---|---|
| **谁 include 它** | 无外部路径引用（通过 nvmeservice target PUBLIC include） |
| **谁链接它 (CMake)** | root `CMakeLists.txt:347-422`：定义 `nvmeservice` target + `add_subdirectory(backends/local/NVMeService/examples)` |
| **standalone `tutti/` 是否依赖** | **否**（standalone `tutti/` 使用 `tutti/device_manager/nvme/nvmeservice/`，内容完全相同） |
| **生产运行是否依赖** | **是**——root build 的 `nvmeservice` target 被 `tutti_daemon` 链接 |
| **双树差异** | `diff -rq backends/local/NVMeService/src/ tutti/device_manager/nvme/nvmeservice/src/` → **无差异**（完全相同的 13 个文件） |
| **target 名** | `nvmeservice`（root build；standalone `tutti/` 也有同名 target，但两者不共存于同一 build） |

### 2.9 `examples/`

| 维度 | 证据 |
|---|---|
| **谁 include 它** | N/A（可执行文件，无消费者） |
| **谁链接它 (CMake)** | root `CMakeLists.txt:457: add_subdirectory(examples)` |
| **standalone `tutti/` 是否依赖** | **否** |
| **生产运行是否依赖** | **是**——`examples/tutti_daemon.cpp` 是生产 daemon 源码（链接 `nvmeservice` + `libnvm` + gRPC） |
| **内容** | 19 个 smoke/bench .cu + 2 个 .cpp（含 `tutti_daemon.cpp`）；其余 smoke 依赖全部 legacy 树 |
| **target 名** | `tutti_daemon`（生产）+ 多个 smoke target |

### 2.10 legacy `tests/`

| 维度 | 证据 |
|---|---|
| **谁 include 它** | N/A（可执行文件，无消费者） |
| **谁链接它 (CMake)** | root `CMakeLists.txt:462: add_subdirectory(tests)` |
| **standalone `tutti/` 是否依赖** | **否** |
| **生产运行是否依赖** | **否** |
| **内容** | 4 个 .cu 文件：`layer1_smoke_test.cu`、`layer3_smoke_test.cu`、`layer3_integration_test.cu`、`layer3_integration_simple.cu` |
| **pre-existing 失败** | `layer1_smoke_test` 链接失败：`cannot find -ltutti_accel`（root build 强制 `TUTTI_BUILD_HARDWARE_STACK=OFF`，不定义 `tutti_accel` target） |
| **target 名** | `layer1_smoke_test`、`layer3_smoke_test`、`layer3_integration_test`、`layer3_integration_simple` |

### 2.11 `backends/local/nvme/`（附带审计）

| 维度 | 证据 |
|---|---|
| **内容** | `test/` 目录下有 `snvme_smoke_libnvm*` 文件 + 编译产物 + Makefile；无 CMakeLists.txt |
| **谁链接它** | 无 CMake target；独立 Makefile 构建 |
| **决议** | 删除（无 CMake 接线，无生产消费，纯历史 smoke） |

## 3. 决议表

| # | Legacy 树 | 决议 | 资产去向 | 理由 |
|---|---|---|---|---|
| 1 | `memory/` | **迁移后删除** | cache 概念已迁移到 `tutti/data_paths/local_nvme/metadata/handle_workspace_cache.h` + `prp_page_cache.h`（Round 11 S2）。`GpuSlotPool`/`HostSlotPool` 思想由 DataPath 私有 arena 替代。 | 被 root legacy stack 消费但 standalone `tutti/` 不依赖；生产路径已替代 |
| 2 | `device_manager/` (root) | **迁移后删除** | 控制面已迁移到 `tutti/data_paths/local_nvme/control/`（Round 10 S2）。libnvm/nvmeservice/kernel_modules 保留在 `tutti/device_manager/nvme/`（Session 1 决议） | root 版本与 `tutti/` 侧是不同实现；无生产消费 |
| 3 | `nvme_storage/` | **删除** | 功能已被 `tutti/data_paths/local_nvme/` 的 LocalNvmeDataPath 完全替代 | 无 standalone `tutti/` 依赖，无生产消费 |
| 4 | `block_storage/` | **删除** | 同上 | 无 standalone `tutti/` 依赖，无生产消费 |
| 5 | `io_engine/` | **删除** | `tutti/` 有自己的 `tutti/io_engine/`（standalone build 中已接线） | 无 standalone `tutti/` 依赖，无生产消费 |
| 6 | `coordinator/` | **删除** | standalone `tutti/CMakeLists.txt` 已注释 `# add_subdirectory(coordinator)`；功能由 StorageRuntime 替代 | 无 standalone `tutti/` 依赖，无生产消费 |
| 7 | `adapters/kv_cache/` | **删除** | 无 `tutti/` 等价物，但 legacy stack 整体退役后无消费者 | 仅被 examples 消费 |
| 8 | `backends/local/NVMeService/` | **迁移后删除** | `tutti/device_manager/nvme/nvmeservice/` 为唯一源（Session 1 决议）。root 侧 `backends/local/NVMeService/` 内容完全相同（`diff -rq` 无差异）。root CMake 的 `nvmeservice` target 定义应重定向到 `tutti/` 侧 | 生产 `tutti_daemon` 依赖 nvmeservice，需先确保 `tutti/` 侧 nvmeservice 在 root build 中可用 |
| 9 | `backends/local/nvme/` | **删除** | 无 CMake target，无生产消费 | 纯历史 smoke |
| 10 | `examples/` | **迁移后删除** | `tutti_daemon.cpp` 迁往 `tutti/` 侧（建议 `tutti/daemon/` 或 `tutti/device_manager/nvme/nvmeservice/examples/`，已有 `nvmeservice_daemon_example` 在该路径）。其余 smoke 随 legacy 树一起删除 | `tutti_daemon` 是唯一生产资产 |
| 11 | legacy `tests/` | **删除** | 功能已被 `tutti/tests/` 完全替代（13/13 HOST + 133/133 CUDA + 735+115 硬件契约） | `layer1_smoke_test` pre-existing 链接失败；standalone `tutti/` 有完整测试 |

## 4. 退役顺序

按依赖反向排序（叶子先删，根后删）。每步给出验证命令。

| 步骤 | 操作 | 依赖前提 | 验证命令 |
|---|---|---|---|
| **S1** | 删除 legacy `tests/` + 从 root CMake 移除 `add_subdirectory(tests)` | 无依赖 | `cmake -S . -B build && cmake --build build -j8` (root build 应无 layer1_smoke 链接失败) |
| **S2** | 迁移 `tutti_daemon.cpp` 到 `tutti/` 侧 + 删除 `examples/` + 从 root CMake 移除 `add_subdirectory(examples)` | S1 | `cmake -S . -B build && cmake --build build --target tutti_daemon -j8` (daemon 应从 `tutti/` 侧编译) |
| **S3** | 删除 `adapters/kv_cache/` + 从 root CMake 移除 `add_subdirectory(adapters/kv_cache)` | S2 (examples 已删，无消费者) | `cmake -S . -B build && cmake --build build -j8` |
| **S4** | 删除 `coordinator/` + 从 root CMake 移除 `add_subdirectory(coordinator)` | S3 (adapters 已删) | `cmake -S . -B build && cmake --build build -j8` |
| **S5** | 删除 `io_engine/` + 从 root CMake 移除 `add_subdirectory(io_engine)` | S4 (coordinator 已删) | `cmake -S . -B build && cmake --build build -j8` |
| **S6** | 删除 `block_storage/` + 从 root CMake 移除 `add_subdirectory(block_storage)` | S5 (io_engine 已删) | `cmake -S . -B build && cmake --build build -j8` |
| **S7** | 删除 `nvme_storage/` + 从 root CMake 移除 `add_subdirectory(nvme_storage)` | S6 (block_storage 已删) | `cmake -S . -B build && cmake --build build -j8` |
| **S8** | 删除 root `device_manager/` + 从 root CMake 移除 `add_subdirectory(device_manager)` | S7 (nvme_storage 已删) | `cmake -S . -B build && cmake --build build -j8` |
| **S9** | 删除 `memory/` + 从 root CMake 移除 `add_subdirectory(memory)` | S8 (device_manager 已删) | `cmake -S . -B build && cmake --build build -j8` |
| **S10** | 删除 `backends/local/NVMeService/` + `backends/local/nvme/` + 从 root CMake 重定向 `nvmeservice` target 到 `tutti/device_manager/nvme/nvmeservice/` + 移除 `add_subdirectory(backends/local/NVMeService/examples)` | S2 (tutti_daemon 已迁移到 `tutti/` 侧) | `cmake -S . -B build && cmake --build build --target tutti_daemon -j8` + `cmake --build build -j8` (全量) |
| **每步后** | standalone `tutti/` 回归 | — | `cmake -S tutti -B build/verify -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON -DTUTTI_BUILD_HARDWARE_TESTS=ON && cmake --build build/verify -j8 && ctest --test-dir build/verify -E hardware && LD_LIBRARY_PATH=build/verify/lib ./build/verify/bin/tutti_local_nvme_datapath_contract_test && LD_LIBRARY_PATH=build/verify/lib ./build/verify/bin/tutti_storage_runtime_local_nvme_contract_test` |

**关键约束**：
- S2 必须在 S10 之前（`tutti_daemon` 依赖 nvmeservice，迁移前 nvmeservice 仍需可用）
- S10 是最后一步（nvmeservice 重定向影响 root build 全局）
- 每步后 standalone `tutti/` 回归不受影响（legacy 树删除不影响 standalone build，因为 standalone `tutti/` 不依赖任何 root legacy 树）

## 5. 性能基线

### 5.1 复跑脚本

```
scripts/round13_baseline.sh [BUILD_DIR]
  BUILD_DIR defaults to build/round11-s2-cuda-hw
```

### 5.2 基线数据（2026-08-02）

```
=== Round 13 Baseline — 2026-08-02T13:08:40+08:00 ===
BUILD_DIR=build/round11-s2-cuda-hw

--- Hardware Contract Tests ---
[datapath contract test]
  passed: 735
  failed: 0
  RESULT: PASS

[runtime contract test]
  passed: 115
  failed: 0
  RESULT: PASS

--- Sequential IO Baseline (3 rounds, median) ---
round 1: 18302ms total
round 2: 18347ms total
round 3: 18292ms total
median: 18320ms

--- Per-test-case timing ---
  26. E2E 4KiB write/read/verify
  33. batch SINGLE write+read+verify
  34. batch LIST write+read+verify (1MiB)
  46. DUAL 8KiB E2E + descriptor
  47. LIST 1MiB fan-out + PRP2 IOVA
  57. completion status SINGLE/DUAL/LIST
  66. PRP cache: repeated LIST hit
  70. arena exhaustion and recovery
  71. arena zero-alloc hot path + reuse
  72. arena LIST PRP from pool + regression
```

### 5.3 基线说明

| 指标 | 值 | 说明 |
|---|---|---|
| datapath contract | 735 passed / 0 failed | 含 4K SINGLE、8K DUAL、1M LIST 全路径 + cache + arena + 错误注入 |
| runtime contract | 115 passed / 0 failed | StorageRuntime 完整生命周期 |
| wall-clock median | 18320ms (3 rounds: 18302, 18347, 18292) | 全套 datapath 契约测试 3 轮中位；变异系数 < 0.3% |
| resolver_test 残留 | 空 | `/mnt/nvme1/GPU0/resolver_test/` 干净 |

**注**：此为退役对比用的轻量基线。Round 11 S4 完整基准（4K/1M × READ/WRITE × 多轮 IOPS/BW 独立测量）仍暂缓。退役后复跑此脚本，断言数不得减少（≥ 735+115），wall-clock 中位不应恶化超过 10%。

### 5.4 数据文件

基线原始数据追加于 `chat/round13/baseline_data.txt`。

## 6. NVMeService 双树分析

### 6.1 现状

```
backends/local/NVMeService/src/          (root, 13 files)
  ├── nvmeservice.proto
  ├── nvmeservice_client.cpp/.h
  ├── nvmeservice_config.cpp/.h
  ├── nvmeservice_server.cpp/.h
  └── nvmeservice_state.cu/.h

tutti/device_manager/nvme/nvmeservice/src/  (standalone, 13 files)
  └── (identical content)

$ diff -rq backends/local/NVMeService/src/ tutti/device_manager/nvme/nvmeservice/src/
(no output — files identical)
```

### 6.2 风险

与 Round 10 S1 前的 libnvm 双树同类风险：
- 同一 `nvmeservice` target 名在 root build 和 standalone `tutti/` build 中各定义一次。
- root build 的 `nvmeservice` 从 `backends/local/NVMeService/src/` 编译；standalone `tutti/` 的从 `tutti/device_manager/nvme/nvmeservice/src/` 编译。
- 如果两边的源码漂移（一方修改未同步），行为不一致。
- `tutti_daemon` 在 root build 中链接 root `nvmeservice`；在 standalone `tutti/` build 中无法构建（standalone build 的 `tutti/CMakeLists.txt` 不含 `tutti_daemon` target）。

### 6.3 决议

**以 `tutti/device_manager/nvme/nvmeservice/` 为唯一源**（Session 1 决议）。
- S10 退役步骤中，root CMake 的 `nvmeservice` target 重定向到 `tutti/device_manager/nvme/nvmeservice/src/`。
- `backends/local/NVMeService/` 删除。
- `backends/local/NVMeService/examples/` 中的 `nvmeservice_daemon_example` 已有等价物在 `tutti/device_manager/nvme/nvmeservice/examples/`。

## 7. `tutti_daemon` 迁移分析

### 7.1 现状

```
examples/tutti_daemon.cpp  (root build target tutti_daemon)
  links: nvmeservice, libnvm, gRPC::grpc++, protobuf::libprotobuf, Threads, CUDA::cudart
  includes: nvmeservice_config.h, nvmeservice_server.h, nvmeservice_state.h
```

### 7.2 迁移目标

**建议迁移到 `tutti/device_manager/nvme/nvmeservice/examples/`**（已有 `nvmeservice_daemon_example` 在该路径）：
- `tutti_daemon.cpp` 本质是 nvmeservice 的 daemon wrapper
- 放在 nvmeservice 的 examples/ 目录最自然
- 该目录在 standalone `tutti/` build 中已有 CMake 接线（`tutti/device_manager/nvme/nvmeservice/examples/CMakeLists.txt`）

### 7.3 迁移影响

- root build 的 `tutti_daemon` target 从 `examples/` 移到 `tutti/device_manager/nvme/nvmeservice/examples/`
- root CMake 的 `add_subdirectory(examples)` 移除后，`tutti_daemon` 由 `tutti/` 侧的 CMake 定义
- 生产运行命令不变：`sudo ./build/bin/tutti_daemon --config sys_config.yaml`

## 8. root CMake 退役后预期状态

退役完成后，root `CMakeLists.txt` 的 legacy `add_subdirectory` 调用全部移除：

```
# Before (current):
add_subdirectory(tutti)          # ← 保留（standalone tutti/ build）
add_subdirectory(memory)         # ← 删除
add_subdirectory(backends/local/NVMeService/examples)  # ← 删除
add_subdirectory(device_manager) # ← 删除
add_subdirectory(nvme_storage)   # ← 删除
add_subdirectory(block_storage)  # ← 删除
add_subdirectory(io_engine)      # ← 删除
add_subdirectory(coordinator)    # ← 删除
add_subdirectory(adapters/kv_cache)  # ← 删除
add_subdirectory(examples)       # ← 删除
add_subdirectory(tests)           # ← 删除

# After (expected):
add_subdirectory(tutti)          # 唯一入口
# nvmeservice target 重定向到 tutti/device_manager/nvme/nvmeservice/src/
# tutti_daemon target 在 tutti/device_manager/nvme/nvmeservice/examples/
```

root `CMakeLists.txt` 保留的职责：
- libnvm target 定义（从 `tutti/device_manager/nvme/libnvm/` 编译）
- nvmeservice target 定义（重定向到 `tutti/device_manager/nvme/nvmeservice/src/`）
- kernel module 构建配置（从 `tutti/device_manager/nvme/kernel_modules/`）
- 全局编译选项、CUDA 检测、yaml-cpp/gRPC 查找

## 9. 已知风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| root `tutti_device_manager` target 与 standalone `tutti/` 的同名 target 在 root build 中共存 | CMake target namespace 全局冲突；当前 root build 先 add `tutti/`（含 `tutti_device_manager` from control/），再 add root `device_manager/`（也定义 `tutti_device_manager`） | S8 删除 root `device_manager/` 后冲突消失 |
| `tutti_daemon` 迁移后 root build 的 `tutti_daemon` target 路径变化 | 生产部署脚本可能引用 `examples/tutti_daemon.cpp` | S2 迁移时更新 root CMake target 路径 |
| nvmeservice 重定向后 root build 的 proto 生成路径变化 | `nvmeservice.pb.cc` 生成位置从 root binary dir 变为 `tutti/` binary dir | S10 重定向时确认 proto 生成路径一致 |
| legacy `tests/` 删除后，`layer3_integration_test` 的集成测试覆盖丢失 | 这些测试已被 `tutti/tests/` 的等价测试替代 | 确认 `tutti/tests/` 覆盖等价场景 |

## 10. 诊断

- 未修改任何源码与 CMake（纯审计+测量）
- 未执行 insmod/rmmod/daemon/mount 操作（环境由用户预先就绪）
- 未提交 Git
- `scripts/round13_baseline.sh` 可复跑

## 11. 结论

**审计完成，退役计划就绪。**

## 总指挥验收（2026-08-02）

**PASS。** 独立抽查与复跑：

- **无越权改动**：git status 确认 S1 未触碰任何源码/CMake（仅新增 `scripts/round13_baseline.sh` 与本结果文件）。
- **关键声明抽查**：standalone `tutti/` 对 root legacy 树零 `add_subdirectory` 引用（复核为空）；`tutti_device_manager` 同名 target 双处定义属实（root `device_manager/CMakeLists.txt:26` 与 tutti 侧 `control/CMakeLists.txt:105`；root build 当前因 `TUTTI_BUILD_HARDWARE_STACK=OFF` 不定义 tutti 侧 target 而暂未爆炸，S8 删除后风险消除——风险表记录准确）。
- **基线复跑**：`scripts/round13_baseline.sh` 独立复跑 735+115 全过，median 18265ms（vs 报告 18320ms，偏差 0.3%，可复现）。
- **退役顺序**：10 步 leaf-first 排序与依赖证据一致；S2（daemon 迁移）先于 S10（NVMeService 重定向）的关键约束成立。
- **与并行协议的映射**：worker 的 S3-S7 步 = 本 round session2 批次（无消费者树）；S1/S2/S8/S9/S10 步 = session3 批次（tests、daemon 迁移、device_manager、memory、NVMeService）。CMake 行所有权与 `PARALLEL.md` 分治一致，不冲突。

**S2 与 S3 可按 `chat/round13/PARALLEL.md` 并行启动。** 注意 S3 侧的 daemon 迁移（worker 步骤 S2）是 NVMeService 重定向（步骤 S10）的前置，同在 session3 批次内，顺序天然满足。

- 10 棵 legacy 树逐树审计完成，每棵树有 include/CMake/standalone/生产四维证据。
- 决议表：6 棵"迁移后删除"（memory, device_manager, NVMeService, examples + 附带 backends/local/nvme），4 棵"删除"（nvme_storage, block_storage, io_engine, coordinator, adapters/kv_cache, tests）。
- 退役顺序 10 步（S1-S10），按依赖反向排序，每步有验证命令。
- 性能基线：735+115 断言全过，wall-clock 中位 18320ms，`scripts/round13_baseline.sh` 可复跑。
- NVMeService 双树确认（`diff -rq` 无差异），以 `tutti/` 侧为唯一源。
- `tutti_daemon` 迁移目标确定：`tutti/device_manager/nvme/nvmeservice/examples/`。
- standalone `tutti/` 不依赖任何 root legacy 树，退役不影响 standalone build。
