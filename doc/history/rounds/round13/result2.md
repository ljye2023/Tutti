# Round 13 Session 2 Result: 第一批退役（无生产消费者的 legacy 树）

## 概述

按 S1 决议删除 5 棵无生产消费者的 legacy 树：`nvme_storage/`、`block_storage/`、`io_engine/`、`coordinator/`、`adapters/kv_cache/`。同步移除 root CMake 接线、清理 legacy tests/、归档设计文档。root build configure + 核心目标 build 通过；standalone HOST 14/14、CUDA 134/134 零回归。

## 删除清单

### git rm 输出

```
$ git rm -rf nvme_storage/ block_storage/ io_engine/ coordinator/ adapters/kv_cache/

rm 'nvme_storage/src/host_fs_backed_nvme_storage_device.cu'
rm 'nvme_storage/src/persistent_file_log.cpp'
rm 'nvme_storage/test/nvme_storage_smoke.cu'
rm 'nvme_storage/test/nvme_storage_gpu_smoke.cu'
rm 'nvme_storage/test/nvme_storage_bulk_smoke.cu'
...
(共删除 5 棵树下全部文件)
```

| 树 | 文件数 | 状态 |
|----|--------|------|
| `nvme_storage/` | 18 | 已删除 |
| `block_storage/` | 11 | 已删除 |
| `io_engine/` | 18 | 已删除 |
| `coordinator/` | 7 | 已删除 |
| `adapters/kv_cache/` | 3 (1 .txt + 1 .h + 1 .cpp) | 已删除 |

## CMake 接线移除

### Root CMakeLists.txt

移除 5 个 `add_subdirectory` 调用（原行 429/434/438/445/452），替换为注释说明。

验证：
```
$ grep -n 'add_subdirectory.*nvme_storage\|add_subdirectory.*block_storage\|add_subdirectory.*io_engine\|add_subdirectory.*coordinator\|add_subdirectory.*kv_cache' CMakeLists.txt
(none)

$ grep -n 'tutti_nvme_storage\|tutti_block_storage\|tutti_io_engine\|tutti_coordinator\|tutti_kv_adapter' CMakeLists.txt
(none)
```

### examples/CMakeLists.txt

注释掉 13 个引用被删树目标的 example target（nvme_storage_smoke ×4、block_storage_* ×4、io_engine_* ×2、coordinator/e2e_* ×2、kv_cache_* ×2），保留 5 个仍可用的 target（registry_smoke、memory_smoke、tiered_handle_cache_smoke、phoenix_conflict_diag、tutti_daemon 已移至 root CMakeLists.txt）。

移除重复的 `tutti_daemon` 定义（root CMakeLists.txt 已有定义，examples/CMakeLists.txt 中的是遗留重复）。

### tests/CMakeLists.txt

移除全部 4 个 legacy 测试目标：
- `layer1_smoke_test` — pre-existing link failure（链接 `tutti_accel` standalone target，root build 不可用）
- `layer3_smoke_test` — 引用已删除的 `accel/` 路径
- `layer3_integration_test` — 同上
- `layer3_integration_simple` — 同上

tests/CMakeLists.txt 替换为状态消息，源 .cu 文件保留在磁盘但不编译（S3 决定命运）。

## 归档清单

| 原路径 | 归档路径 | 说明 |
|--------|---------|------|
| `doc/layer5-implementation-summary.md` | `doc/history/layer5-implementation-summary.md` | block_storage 设计文档 |
| `doc/layered/layer4-io-engine.md` | `doc/history/layer4-io-engine.md` | io_engine 设计文档 |
| `doc/layered/layer5-storage-interfaces.md` | `doc/history/layer5-storage-interfaces.md` | block_storage 接口文档 |
| `doc/layered/layer6-coordinator.md` | `doc/history/layer6-coordinator.md` | coordinator 设计文档 |

使用 `git mv` 保留历史。

## 每步验证记录

### Step 1: 删除 5 棵树 + 移除 CMake 接线

```
$ cmake -S . -B build 2>&1 | grep -iE 'error|fatal'
(no output — configure clean)
```

### Step 2: 核心目标 build

```
$ cmake --build build --target tutti_daemon nvmeservice libnvm
[100%] Built target tutti_daemon
[100%] Built target nvmeservice
[100%] Built target libnvm
```

### Step 3: Standalone HOST build + ctest

```
$ cmake -S tutti -B /tmp/tutti-r13s2-standalone-host -DBUILD_TESTING=ON -DTUTTI_ACCELERATOR=HOST
$ cmake --build /tmp/tutti-r13s2-standalone-host
$ cd /tmp/tutti-r13s2-standalone-host && ctest

100% tests passed, 0 tests failed out of 14
Total Test time = 0.03 sec
```

### Step 4: Standalone CUDA build + ctest

```
$ cmake -S tutti -B tutti/build-profile-cuda -DTUTTI_ACCELERATOR=CUDA -DTUTTI_BUILD_HARDWARE_TESTS=OFF
$ cmake --build tutti/build-profile-cuda
$ cd tutti/build-profile-cuda && ctest

100% tests passed, 0 tests failed out of 134
Total Test time = 19.44 sec
```

## git grep 证明：standalone 无被删树引用

```
$ git grep -n 'nvme_storage/\|block_storage/\|io_engine/\|coordinator/\|kv_cache/' -- 'tutti/*.cpp' 'tutti/*.h' 'tutti/*.cu' 'tutti/*.cuh'
```

结果：standalone `tutti/` 中存在 `tutti/block_storage/`、`tutti/coordinator/`、`tutti/io_engine/` 子目录——这些是 **standalone 内部副本**，非被删的 root 树。standalone build 134/134 PASS 证明这些内部副本自包含，不依赖 root 被删树。

`tutti/nvme_storage/` 不存在（standalone 无此目录）。

## 已知 gap

1. **layer1_smoke_test pre-existing failure**：链接 `tutti_accel`（standalone target），root build 不可用。按 S1 决议移除，记录在此。
2. **examples/ 中 13 个注释掉的 target**：引用被删树目标（`tutti_nvme_storage` 等），待 S3 决定是迁移到 standalone API 还是删除源文件。
3. **tutti/README.md 引用被删树名称**：文档引用，不影响编译，不动 standalone 源码（S3 处理）。
4. **硬件契约测试**（735/0 + 115/0、memfs 5/5）：需用户手动执行（snvme module + daemon + mount），agent 不代跑。

## 全量回归汇总

| 维度 | 结果 |
|------|------|
| Root build configure | PASS |
| Root build core targets (tutti_daemon, nvmeservice, libnvm) | PASS |
| Standalone HOST ctest | 14/14 PASS |
| Standalone CUDA ctest | 134/134 PASS |
| Root CMakeLists.txt 残留引用 | 0 |
| Root tests/CMakeLists.txt 残留引用 | 0 |
| Standalone tutti/ 依赖被删 root 树 | 否（自包含） |
| 设计文档归档 | 4 份 (git mv) |

## 总指挥验收（2026-08-02）

**PASS（联合验证）。**

- **删除完整性**：5 棵树物理消失（目录核查属实）；根 CMake 对应 5 行 `add_subdirectory` 移除；`backends/local/nvme` 仅剩 test/。
- **standalone 副本澄清正确**：`tutti/block_storage|coordinator|io_engine` 为新架构骨架包，与退役根树不同层，保留无误。
- **联合验证**（与 S3 编辑合并后）：root configure 0 错 + `libnvm/nvmeservice/tutti_daemon/modules` 全 PASS；standalone HOST 14/14、CUDA 134/134。
- **硬件回归**（daemon 重启后）：735/0 + 115/0，环境清洁。
- legacy `tests/` 三个孤儿 `.cu` 源码已记录待 S4 处理，归属明确。
