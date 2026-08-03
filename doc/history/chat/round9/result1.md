# Round 9 Session 1 Result — Standalone 构建图与测试分层

## 新 CMake target 与依赖方向

```
tutti_cuda_like (INTERFACE, profile shim)
  └── tutti_api (INTERFACE, public include root)
        └── tutti_spi (INTERFACE, SPI include root)
              └── tutti_local_file_binding (INTERFACE, header-only)
                    │   include: ${PROJECT_SOURCE_DIR}/..  (workspace root)
                    │   links: tutti_spi
                    │
                    └── tutti_local_file_resolver (INTERFACE, header-only)
                          │   include: ${PROJECT_SOURCE_DIR}/..
                          │   links: tutti_local_file_binding, tutti_spi
                          │
                          └── tutti_local_nvme_datapath (STATIC, CUDA)
                                │   sources: local_nvme_data_path.cpp + io/*.cu
                                │   include (PRIVATE): ${PROJECT_SOURCE_DIR}/..
                                │   links (PUBLIC): tutti_local_file_binding,
                                │                    tutti_local_file_resolver,
                                │                    libnvm
                                │   links (PRIVATE): CUDA::cudart
                                │
                                └── (consumers: local_nvme_datapath_contract_test)
```

依赖方向关键属性：
- `tutti_api` / `tutti_spi` 不链接任何 private target — libnvm/CUDA/PRP/kernel include 不会传播到 public headers。
- `tutti_local_file_binding` 和 `tutti_local_file_resolver` 是 hardware-free header-only target（HOST profile 可创建）。
- `tutti_local_nvme_datapath` 仅在 `TUTTI_BUILD_HARDWARE_STACK=ON`（CUDA profile）时创建，复用 standalone `libnvm` target（来自 `tutti/device_manager/nvme/libnvm/`）。

## 创建/修改的文件

| 文件 | 操作 |
|---|---|
| `tutti/bindings/ext4_local_nvme/CMakeLists.txt` | 新建 — `tutti_local_file_binding` INTERFACE target |
| `tutti/resolvers/local_file/CMakeLists.txt` | 新建 — `tutti_local_file_resolver` INTERFACE target |
| `tutti/data_paths/local_nvme/CMakeLists.txt` | 新建 — `tutti_local_nvme_datapath` STATIC CUDA target |
| `tutti/CMakeLists.txt` | 修改 — 添加 private package 子目录、`TUTTI_BUILD_HARDWARE_TESTS` option、hardware-free/hardware test 接线、CCCL include 修复 |
| `tests/status_contract/CMakeLists.txt` | 重写 — 链接 `tutti_api`，去硬编码路径 |
| `tests/memory_types_contract/CMakeLists.txt` | 重写 — 链接 `tutti_api` |
| `tests/io_types_contract/CMakeLists.txt` | 重写 — 链接 `tutti_api` |
| `tests/storage_target_resolver_contract/CMakeLists.txt` | 重写 — 链接 `tutti_spi` |
| `tests/binding_contract/CMakeLists.txt` | 重写 — 链接 `tutti_local_file_binding` |
| `tests/data_path_contract/CMakeLists.txt` | 重写 — 链接 `tutti_spi`，去硬编码路径 |
| `tests/storage_runtime_contract/CMakeLists.txt` | 重写 — 链接 `tutti_spi`，去硬编码路径 |
| `tests/resolver_contract/CMakeLists.txt` | 重写 — 链接 `tutti_local_file_resolver`，加 `LABELS "hardware;resolver"` |
| `tests/local_nvme_datapath_contract/CMakeLists.txt` | 重写 — 链接 `tutti_local_nvme_datapath`，加 `LABELS "hardware;local_nvme"` |
| `Roadmap.md` | 修改 — 更新 Known Bugs Snapshot 和 Current Implementation Status |

## HOST profile (TUTTI_ACCELERATOR=HOST)

### Configure

```
cmake -S tutti -B build/round9-session1-host \
  -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON \
  -DTUTTI_BUILD_HARDWARE_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

- **PASS** — configure 完成 (0.5s)
- **CUDA/libnvm/gRPC/yaml 查找**：0 出现（grep 验证）
- 无 `find_package(CUDA)`、`find_package(gRPC)`、`find_package(yaml-cpp)`、`libnvm` 调用

### Build

```
cmake --build build/round9-session1-host -j8
```

- **PASS** — 10 个测试可执行文件全部编译/链接成功

### CTest

```
ctest --test-dir build/round9-session1-host --output-on-failure
```

- **PASS** — 10/10 tests passed, 0 failed

| # | Test | 结果 |
|---|---|---|
| 1 | cuda_like_contract_test | Passed |
| 2 | tutti_public_api_usage_test | Passed |
| 3 | tutti_spi_consumer_test | Passed |
| 4 | tutti_data_path_contract_test | Passed |
| 5 | tutti_storage_runtime_contract_test | Passed |
| 6 | tutti_status_contract_test | Passed |
| 7 | tutti_memory_types_contract_test | Passed |
| 8 | tutti_io_types_contract_test | Passed |
| 9 | tutti_storage_target_resolver_contract_test | Passed |
| 10 | tutti_binding_contract_test | Passed |

### ctest -N

- 10 tests listed — 不含 resolver/local-NVMe hardware test ✓

## CUDA profile (TUTTI_ACCELERATOR=CUDA)

### Configure

```
cmake -S tutti -B build/round9-session1-cuda \
  -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON \
  -DTUTTI_BUILD_HARDWARE_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

- **PASS** — configure 完成 (5.5s)
- CCCL include 自动检测：`/usr/local/cuda-13.0/targets/x86_64-linux/include/cccl`
- gRPC not found → NVMeService disabled（符合预期）
- `tutti_local_nvme_datapath` target 已创建

### Build (compile/link gate)

```
cmake --build build/round9-session1-cuda -j8 --target tutti_local_nvme_datapath
```

- **PASS** — libnvm.a + libtutti_local_nvme_datapath.a 编译/链接成功
- 仅有 1 个 deprecation warning（`ulonglong4` deprecated，来自 libnvm `nvm_parallel_queue.h`，pre-existing，非本次改动引入）

### CCCL include 修复

CUDA 13.0 将 `cuda/atomic` 等 CCCL 头文件移至 `targets/<arch>/include/cccl/` 子目录，该路径不在 `CUDAToolkit_INCLUDE_DIRS` 中。在 `tutti/CMakeLists.txt` 的 hardware stack guard 内自动检测并添加该路径，确保 libnvm `queue.cpp` 的 `#include <cuda/atomic>` 可解析。

## Hardware test 开关、labels 与默认行为

| 开关 | 默认 | 效果 |
|---|---|---|
| `TUTTI_BUILD_HARDWARE_TESTS` | `OFF` | OFF: 不注册 hardware CTest；ON: 注册 resolver + local-NVMe |

Hardware test labels:

| Test | Labels |
|---|---|
| `tutti_resolver_contract_test` | `hardware;resolver` |
| `tutti_local_nvme_datapath_contract_test` | `hardware;local_nvme` |

验证（`TUTTI_BUILD_HARDWARE_TESTS=ON`）：
- `ctest -N` 含 Test #11 `tutti_resolver_contract_test`、Test #12 `tutti_local_nvme_datapath_contract_test`
- 默认 CTest 不访问 `/dev/ssnvme*`、GPU、`/mnt/nvme1` ✓

## 硬编码路径审查

- `grep "/data/home/ryeqiu/Tutti" tests/*/CMakeLists.txt` → 0 匹配 ✓
- `grep "build/lib" tests/*/CMakeLists.txt` → 0 匹配 ✓
- `grep "backends/local/nvme/libnvm" tests/*/CMakeLists.txt` → 0 匹配 ✓
- 所有测试使用 target usage requirements（`tutti_api`/`tutti_spi`/`tutti_local_file_binding`/`tutti_local_file_resolver`/`tutti_local_nvme_datapath`）获取 include 路径和编译定义

## git diff --check

- **PASS** — 无空白错误

## 仍未解决的 root/standalone 双 source owner 边界

1. 根 `CMakeLists.txt` 的 legacy production graph 与 standalone `tutti/CMakeLists.txt` 仍共存，未合并。
2. `backends/local/nvme/libnvm/`（根树）与 `tutti/device_manager/nvme/libnvm/`（standalone 树）仍是两份源码，standalone CUDA profile 仅使用后者。
3. 根 `tests/CMakeLists.txt`（layer1/layer3 smoke tests）仍硬编码路径并引用 legacy targets，不在 standalone CTest 图中。
4. Root `CMakeLists.txt` 的 `set(TUTTI_BUILD_HARDWARE_STACK OFF ... FORCE)` 确保 root include tutti/ 时不会触发 hardware stack，但 root 自身的 hardware stack 仍独立存在。

## Roadmap.md 已更新的事实项

### Known Bugs Snapshot（CLOSED 标记）

- **[CLOSED]** PRP-list 现在写入正确的 DMA IOVA 到 `prp2`
- **[CLOSED]** NVMe SQE 零初始化；per-op `DeviceSubmitEntry[]` 在 launch 前 memset 清零
- **[CLOSED]** Mixed-target batches：per-request target/memory lookup + bounds/alignment 校验
- **[CLOSED]** Async/concurrent scratch overwrite：每个 op 独立 `DeviceSubmitEntry[]` + `cudaEvent_t` + PRP-list DMA
- **[CLOSED]** File target resolver：`LocalFileResolver`（FIEMAP）已实现并通过 binding 连接到 `LocalNvmeDataPath`

### Known Bugs Snapshot（保留未闭合）

- Public `MemoryHandle` → DMA address 映射通过 Runtime 的完整路径仍在进行中
- CQ polling 无 timeout/cancellation；NVMe status 字段未完全 surfacing
- Resolver fail-closed policy 对 edge-case extent flags 未最终确定
- `StorageRuntime` 线程安全：当前单线程，多线程并发不安全
- 双 source owner：root legacy 树与 standalone `tutti/` 共存
- GPU persistence 契约未稳定

### Current Implementation Status（新增）

- Standalone `tutti/CMakeLists.txt` 为新架构 build owner
- HOST profile 无 CUDA/libnvm/gRPC 依赖
- CUDA profile 完整 hardware stack + `tutti_local_nvme_datapath` 编译/链接通过
- `TUTTI_BUILD_HARDWARE_TESTS` 默认 OFF，hardware CTest 有 labels
- 501 contract assertions 在 real hardware 上通过（test 53 端到端 4 KiB WRITE/READ）

## 诊断

- 改动文件 linter diagnostics: 0
- `git diff --check`: 0 errors

## 结论

**PASS**

- HOST profile: clean configure (0 CUDA/libnvm/gRPC/yaml) + build + 10/10 CTest pass
- CUDA profile: configure + `tutti_local_nvme_datapath` compile/link gate pass
- Hardware tests gated by `TUTTI_BUILD_HARDWARE_TESTS=OFF` default, labels applied
- No hardcoded workspace paths in test CMakeLists.txt
- `git diff --check` clean
- Roadmap.md fact snapshot updated with CLOSED/retained items
