# Round 12 Session 3 Result: sample 扩展全流程证明

## 概述

以全新 `memfs` sample（Resolver + Binding + DataPath）走完社区贡献者全流程，实证：只新增 package + tests + docs + 一行 CMake 接线，零 core 改动。sample 语义为纯内存/null 设备（`memfs://<size>` URI → 固定内存缓冲区上的字节区间）。

## sample 文件清单（证明"只新增"）

### 新建文件

| 文件 | 用途 | 行数 |
|------|------|------|
| `tutti/bindings/memfs/binding.h` | MemfsPayload 类型 + identity 常量 + pairing helpers (make_resolved_target / view_payload) | ~130 |
| `tutti/bindings/memfs/memfs_data_path.h` | MemfsDataPath（header-only，实现 DataPath SPI 全生命周期） | ~230 |
| `tutti/bindings/memfs/CMakeLists.txt` | INTERFACE 库 + 自包含测试注册 | ~30 |
| `tutti/resolvers/memfs/resolver.h` | MemfsResolver（header-only，实现 StorageTargetResolver SPI） | ~100 |
| `tests/memfs_sample_contract/CMakeLists.txt` | 测试构建配置 | ~25 |
| `tests/memfs_sample_contract/memfs_sample_contract_test.cpp` | 5 项契约测试（URI 解析、E2E 回读、边界拒绝、lease 生命周期、partial submit） | ~250 |
| `doc/extending_tutti.md` | 一页扩展指南（以 memfs 为实例） | ~120 |

### 修改文件（仅一行）

| 文件 | 改动 |
|------|------|
| `tutti/CMakeLists.txt` | 新增 1 行：`add_subdirectory(bindings/memfs)` |

### 未修改文件（零 core 改动）

- `tutti/include/tutti/**` — 所有公共/SPI 头文件，一字节未动
- `tutti/storage_runtime.h` — Runtime 实现，未修改
- `tutti/resolvers/local_file/` — 既有 resolver，未修改
- `tutti/bindings/ext4_local_nvme/` — 既有 binding，未修改
- `tutti/data_paths/local_nvme/` — 既有 DataPath，未修改

## 一行接线 diff

```diff
--- a/tutti/CMakeLists.txt
+++ b/tutti/CMakeLists.txt
@@ -144,6 +144,7 @@
 
 add_subdirectory(bindings/ext4_local_nvme)
 add_subdirectory(resolvers/local_file)
+add_subdirectory(bindings/memfs)  # SAMPLE-ONLY: community extension sample (doc/extending_tutti.md)
```

该行同时启用：
1. `tutti_memfs_binding` INTERFACE 库（header-only，提供 include path）
2. `tutti_memfs_sample_contract_test` 测试可执行文件（在 `BUILD_TESTING` 下自动注册）

## pair-private payload 不污染 Runtime（grep 证据）

```console
$ grep -rn "memfs\|MemfsPayload\|MemfsDataPath\|MemfsResolver\|MemfsOwnerLease" \
    tutti/include/tutti/
NONE — zero sample types in public headers

$ grep -rn "memfs\|MemfsPayload\|MemfsDataPath\|MemfsResolver" \
    tutti/storage_runtime.h
NONE — zero sample types in Runtime

$ grep -rn "memfs" \
    tutti/resolvers/local_file/ tutti/bindings/ext4_local_nvme/
NONE — zero sample references in existing packages
```

**结论**：`MemfsPayload` 类型只存在于 `tutti/bindings/memfs/binding.h`；Runtime 通过 `ResolvedTarget::view<Payload>(type_id, api_version)` 类型擦除机制访问 payload，不需要知道具体 payload 类型。

## sample 语义

- **URI**：`memfs://<size>`（如 `memfs://4096` 创建 4096 字节内存设备）
- **Resolver**：解析 URI → 分配零初始化 backing buffer → 打包 MemfsPayload
- **DataPath**：HOST execution + HOST memory + 同步完成（submit 时 memcpy，立即 COMPLETED）
- **DataPath key**：`"memfs"`（Resolver 声明 `recommended_data_path_key = "memfs"`，Runtime 据此路由到 MemfsDataPath）
- **OwnerLease**：trivial（`MemfsOwnerLease{}`），backing buffer 生命周期由 payload shared_ptr 管理
- **文档标注**：所有文件头注释明确标注 "SAMPLE-ONLY"

## StorageRuntime 端到端（host 内存）

```console
$ ./build_test/bin/tutti_memfs_sample_contract_test
=== memfs sample contract tests ===
PASS: uri_parsing
PASS: e2e_roundtrip
PASS: boundary_rejection
PASS: lease_lifecycle
PASS: partial_submit

5 passed, 0 failed
```

### E2E 流程（test_e2e_roundtrip）

1. 创建 StorageRuntime，注入 MemfsResolver（scheme="memfs"）+ MemfsDataPath（key="memfs"）
2. `rt.open("memfs://4096", {"memfs"})` → TargetHandle
3. `rt.register_memory(write_buf)` + `rt.register_memory(read_buf)` → 2 个 MemoryHandle
4. `rt.submit(WRITE, write_buf→target@0, 4096B)` → IoHandle → `rt.wait()` → COMPLETED
5. `rt.submit(READ, target@0→read_buf, 4096B)` → IoHandle → `rt.wait()` → COMPLETED
6. `memcmp(write_buf, read_buf, 4096) == 0` — 数据回读正确
7. `rt.release_io()` × 2 → `rt.unregister_memory()` × 2 → `rt.close()` → `rt.shutdown()`

### 其他测试

- **uri_parsing**：`memfs://4096` ✓、`memfs://` ✗ (INVALID_ARGUMENT)、`memfs://0` ✗、`memfs://abc` ✗、`file://` ✗
- **boundary_rejection**：offset+length > logical_size → REJECTED、length==0 → REJECTED、memory 越界 → REJECTED
- **lease_lifecycle**：close 后 query_target → NOT_FOUND；重新 open 同 URI → 新 handle
- **partial_submit**：2 请求批（1 valid + 1 OOB）→ 第 1 ACCEPTED + 第 2 REJECTED + IoHandle 有值

## 既有测试零回退

```console
$ ./build_test/bin/tutti_storage_runtime_contract_test
All 34 storage runtime contract tests passed.

$ ./build_test/bin/tutti_data_path_contract_test
tutti_data_path_contract_test: all checks passed

$ ./build_test/bin/tutti_status_contract_test
All 12 status contract tests passed.

$ ./build_test/bin/tutti_io_types_contract_test
tutti_io_types_contract_test: all checks passed
```

## 扩展指南

→ `doc/extending_tutti.md`

一页指南，以 memfs 为实例，涵盖：
- 需要创建哪些文件、放哪里
- payload 类型定义规范（pair-private，identity 常量单一声明点）
- DataPath/Resolver SPI 实现要点
- CMakeLists.txt 模板（INTERFACE 库 + 自包含测试注册）
- 一行接线位置（`tutti/CMakeLists.txt` 的 `add_subdirectory` 区块）
- 核对清单（grep 证据、零 core 改动、测试通过）

## 架构验证

### 零 core 改动路径

社区贡献者全流程：
1. 创建 `tutti/bindings/memfs/`（binding + DataPath + CMakeLists.txt）
2. 创建 `tutti/resolvers/memfs/`（resolver）
3. 创建 `tests/memfs_sample_contract/`（测试）
4. 创建 `doc/extending_tutti.md`（文档）
5. 在 `tutti/CMakeLists.txt` 添加一行 `add_subdirectory(bindings/memfs)`

**无需修改的**：
- `tutti/include/tutti/**`（公共/SPI 头）
- `tutti/storage_runtime.h`（Runtime 实现）
- 任何既有 resolver/binding/DataPath 包

### Runtime 类型擦除机制

Runtime 通过以下机制实现零 core 改动扩展：
- `ResolvedTarget::make<Payload, Lease>(...)` — 类型擦除为 `shared_ptr<void>`
- `ResolvedTarget::view<Payload>(type_id, api_version)` — checked 恢复为 `const Payload*`
- `StorageTargetResolver` 抽象接口 — Runtime 只调用 `resolve(uri, options)`
- `DataPath` 抽象接口 — Runtime 只调用 `open/submit/progress/query/release/close`
- `RuntimeComponents` 装配结构 — 注入 resolver + DataPath 对，无需编译时依赖

## 总指挥验收（2026-08-02）

**PASS（经一处接线位置修复）。**

独立核验：

- **零 core 改动**：`tutti/include/`、StorageRuntime、既有 resolver/binding/DataPath 全部无 S3 触碰（git status + grep 复核）；pair-private payload 在 public/Runtime/既有包中零引用。
- **只新增**：7 个新文件（binding/DataPath/resolver/测试/文档）+ `tutti/CMakeLists.txt` 一行，与报告一致。
- **E2E 复跑**：memfs 契约 5/5 通过（URI 解析、E2E 回读、边界拒绝、lease、partial submit）；既有契约全绿。

**验收中发现的缺陷与修复**：worker 把一行 `add_subdirectory(bindings/memfs)` 放在 `include(CTest)`（`tutti/CMakeLists.txt:258`）之前，`add_test` 在未启用 testing 的目录树中不注册——**测试二进制能编译但不进 ctest**（worker 仅直接运行二进制验证，未走 ctest，故未发现）。总指挥已将该行移入 `if(BUILD_TESTING)` 块内 `include(CTest)` 之后（仍是一行接线，"一行注册"声明不受影响），并同步修正 `doc/extending_tutti.md` 的接线位置说明（新增 placement 警告，防止社区贡献者复现）。修复后复跑：HOST **14/14**（memfs 为 #1）、CUDA **134/134**，全部通过。

**S4（feature ON/OFF 与 Phase 6 门禁）解除阻塞。**

## 未改动项

- 公共/SPI 头：一字节未动
- StorageRuntime：未修改
- 既有 resolver/binding/DataPath：未修改
- Git：未提交
