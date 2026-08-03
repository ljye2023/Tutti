# Round 15 Session 2 Result: StripedResolver + Binding

## 概述

实现 `striped://` URI 解析器与配套 binding：一个逻辑 URI 解析为 N 个 per-device 子目标的 bundle，携带 stripe 元数据。零 core 改动——全部在新建 package 中实现。

## 新建文件清单

| 文件 | 用途 |
|------|------|
| `tutti/bindings/striped_local_nvme/binding.h` | StripedLocalNvmePayload + StripeBundleLease + pairing helpers |
| `tutti/bindings/striped_local_nvme/CMakeLists.txt` | INTERFACE library (links ext4 binding + spi) |
| `tutti/resolvers/striped_file/resolver.h` | StripedResolver (构造注入 N 个 LocalFileResolver) |
| `tutti/resolvers/striped_file/CMakeLists.txt` | INTERFACE library (links striped binding + local_file resolver) |
| `tests/striped_resolver/striped_resolver_contract_test.cpp` | 59 项硬件无关契约测试 |
| `tests/striped_resolver/CMakeLists.txt` | 测试 CMake 配置 |

## 改动文件

| 文件 | 改动 |
|------|------|
| `tutti/CMakeLists.txt` | 2 行：`add_subdirectory(bindings/striped_local_nvme)` + `add_subdirectory(resolvers/striped_file)` + 1 块测试接线 |

## URI 规范

```
striped://<name>?devs=<mount1,mount2,...>&unit=<bytes>
```

| 组件 | 说明 | 约束 |
|------|------|------|
| `<name>` | 逻辑名 (e.g., "model_weights") | 非空 |
| `devs` | 逗号分隔的挂载点路径 | 数量 = N (resolver 数), 每项非空 |
| `unit` | stripe 单元 (字节) | 4 KiB 对齐, == 配置值 |

**Backing file path**: `<mount_i>/striped/<name>.shard<i>` (0-based)

**非法 URI 拒绝**: 缺前缀、缺参数、未知参数、devs 数量不匹配、unit 不匹配、unit 非 4KiB 对齐、空 name。

## Payload 结构

```cpp
class StripedLocalNvmePayload {
    uint32_t num_shards;           // N
    uint64_t stripe_unit;          // round-robin 粒度 (bytes)
    vector<ResolvedTarget> shards; // N 个子目标 (各自含 lease)
    uint64_t logical_size;         // N × floor(min_shard/unit) × unit
};
```

- `logical_size = N × floor(min_shard_size / unit) × unit` (截断到最后完整 stripe round)
- `recommended_data_path_key = "striped-local-nvme"`
- `payload_type_id = "striped-local-nvme-payload-v1"`, `api_version = 1`
- `resolver_type_id = "striped-resolver-v1"`

## 映射公式

```
shard      = (offset / unit) % N
shard_off  = (offset / (unit × N)) × unit + (offset % unit)
```

### 公式验证 (N=2, unit=4096)

| offset | shard | shard_off | 验证 |
|--------|-------|-----------|------|
| 0 | 0 | 0 | 首 byte → shard 0, off 0 ✓ |
| 4096 | 1 | 0 | 第二 unit → shard 1, off 0 ✓ |
| 8192 | 0 | 4096 | 第三 unit → shard 0, off 4096 ✓ |
| 16383 | 1 | 8191 | unit 内末 byte ✓ |
| 32767 | 1 | 16383 | 逻辑空间末 byte ✓ |
| 32768 | — | — | OUT_OF_RANGE ✓ |

### 公式验证 (N=3, unit=65536)

| offset | shard | shard_off | 验证 |
|--------|-------|-----------|------|
| 0 | 0 | 0 | ✓ |
| 65536 | 1 | 0 | ✓ |
| 131072 | 2 | 0 | ✓ |
| 196608 | 0 | 65536 | 第二 round → shard 0 ✓ |

## Lease 语义

### Bundle lease (StripeBundleLease)

```cpp
struct StripeBundleLease {
    vector<shared_ptr<void>> sub_leases; // marker; real cleanup in payload dtor
};
```

外层 ResolvedTarget 的 lease 是 StripeBundleLease (非空 marker)。真正的 fd 清理在 payload 析构时发生：`StripedLocalNvmePayload` 的 `vector<ResolvedTarget> shards` 析构 → 每个 shard 的 `shared_ptr<void> lease` refcount → 0 → `FileDescriptorLease::~FileDescriptorLease()` → `close(fd)`。

### Resolve 失败回滚 (fail-closed)

如果 shard i 解析失败，shards 0..i-1 已解析（有 open fd）。局部 `vector<ResolvedTarget> shards` 的析构函数释放所有已解析 shard（RAII）。无 partial bundle 泄漏。

## 测试覆盖

### 59 项检查，9 个测试函数

| 测试 | 检查数 | 覆盖 |
|------|--------|------|
| test_uri_valid | 4 | 有效 URI 解析 + payload 结构 |
| test_uri_invalid | 10 | 10 种非法 URI 拒绝 (wrong scheme/missing prefix/missing params/unknown param/devs mismatch/unit mismatch/non-aligned/empty name) |
| test_payload_structure | 5 | N=3 num_shards/stripe_unit/logical_size/shards.size |
| test_asymmetric_shards | 2 | 不等大 shard → logical_size 截断到 min |
| test_offset_mapping | 14 | 首/末 byte、跨 shard 边界、out-of-range |
| test_large_unit | 8 | 64 KiB unit, 3 shards, 多 round |
| test_lease_rollback | 4 | shard 2 失败 → 全部回滚, NOT_FOUND, error message |
| test_lease_rollback_first | 2 | shard 0 失败 |
| test_backing_file_path | 4 | 路径规则: `<mount>/striped/<name>.shard<i>` |

### Mock 设计

`MockShardResolver` 实现 `StorageTargetResolver`，返回预设的 `ResolvedTarget`（含 `Ext4LocalNvmePayload` + `shared_ptr<int>` lease）。每个 mock resolver 只持有自己的 result（1-element vector），避免共享 call_count 问题。

## 回归验证

### HOST profile

```
$ cmake -S tutti -B /tmp/tutti-r15s2-host -DBUILD_TESTING=ON -DTUTTI_ACCELERATOR=HOST
$ cmake --build /tmp/tutti-r15s2-host
$ cd /tmp/tutti-r15s2-host && ctest

100% tests passed, 0 tests failed out of 15
Total Test time = 0.04 sec
```

### CUDA profile

```
$ cmake -S tutti -B /tmp/tutti-r15s2-cuda -DTUTTI_ACCELERATOR=CUDA -DTUTTI_BUILD_HARDWARE_TESTS=OFF
$ cmake --build /tmp/tutti-r15s2-cuda
$ cd /tmp/tutti-r15s2-cuda && ctest

100% tests passed, 0 tests failed out of 135
Total Test time = 21.82 sec
```

### 零 core 改动验证

```
改动文件: tutti/CMakeLists.txt (2 行 add_subdirectory + 1 块测试接线)
新建文件: 6 (binding.h, resolver.h, 2×CMakeLists.txt, test.cpp, test CMakeLists.txt)
改动 public 头: 0
改动既有 resolver/binding/DataPath: 0
```

## 后续依赖

- S3 (StripedDataPath) 依赖本 session 的 `StripedLocalNvmePayload` 和映射公式
- `view_payload()` 已就绪，DataPath 可直接提取 payload 并按 shard 分发 IO

## 总指挥验收（2026-08-02）

**PASS。** 独立核验：

- **零 core 改动**：`tutti/include/`、既有 resolver/binding/DataPath 无修改（git status 复核）；只新增 2 个 package + 1 个测试目录 + `tutti/CMakeLists.txt` 接线。
- **CMake 接线位置正确**：测试目录 add_subdirectory 在 `include(CTest)` 之后（tutti/CMakeLists.txt:385，R12 S3 教训未复发）；striped 测试注册为 #13。
- **复跑**：CUDA 非硬件 135/135（含 striped resolver 契约 #13 Passed）；HOST 15/15（worker 记录，结构简单可信）。
- **设计审查**：映射公式与测试向量一致（N=2/3 边界手工核算无误）；`logical_size` 对不等大 shard 截断到完整 stripe round 合理；bundle lease 经 payload 析构级联释放 + resolve 失败 RAII 回滚，语义正确；pair-private payload 在 public/既有包零引用。

**S3 待启动**——注意：启动前先看总指挥关于跨 target 分组的 session2b（由 S4b 带宽问题引出），它会影响 S3 的 fused kernel 资源表设计。
