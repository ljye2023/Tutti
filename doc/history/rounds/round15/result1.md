# Round 15 Session 1 Result: 多设备底座实测（双 LocalNvmeDataPath 经 Runtime）

## 1. 执行摘要

**结论：PASS。**

- 双真实 NVMe（snvme0 `0000:08:00.0` + snvme1 `0000:4b:00.0`）经同一 `StorageRuntime` 实测通过。
- 两个 `LocalNvmeDataPath` 实例 + 两个 `LocalFileResolver`（各自 backing device），通过 `RuntimeComponents` 注入同一 Runtime，使用设备特定的 DataPath key 路由。
- 4 个多设备测试用例（72-75）全部通过：跨设备 WRITE/READ、跨设备 batch group-by-target、双 stream 双设备并发、故障隔离。
- 既有断言零回归：datapath 799/0（735 + 64 新增），runtime 115/0。
- resolver_test 双盘目录均清空。
- 环境检测：第二盘不可用时测试 SKIP（不 fail）。

## 2. 环境前置确认

```
/dev/ssnvme0          crw-rw-rw- 507, 0   (snvme0, 0000:08:00.0)
/dev/ssnvme1          crw-rw-rw- 507, 1   (snvme1, 0000:4b:00.0)
/dev/snvme0n1 → /mnt/nvme1  (ext4, rw,relatime)
/dev/snvme1n1 → /mnt/nvme2  (ext4, rw,relatime)
snvme module: loaded (77824)
snvme_core module: loaded (77824)
tutti_daemon: pid 3329984, listening 50051
```

第二盘 `/mnt/nvme2` 已挂载（ext4），环境就绪。

## 3. 架构方案

### 3.1 问题

`ext4_local_nvme` binding 硬编码 `kRecommendedDataPathKey = "local-nvme-ext4"`。所有 `LocalFileResolver` 产出的 `ResolvedTarget` 都使用此 key，导致 `StorageRuntime` 只能路由到一个 DataPath。

### 3.2 解决方案

测试专用 `MultiDeviceResolverWrapper`：
1. 委托内部 `LocalFileResolver` 解析文件（FIEMAP + backing device 验证）
2. 重写 URI：`file0:///path` → `file:///path`（内部 resolver 期望 `file://` 前缀）
3. 从内部 `ResolvedTarget` 提取 `Ext4LocalNvmePayload`（via `view_payload()`）
4. 用设备特定的 `recommended_data_path_key` 重建 `ResolvedTarget`（null-deleter shared_ptr 借用 payload，dummy lease fd=-1）
5. 内部 `ResolvedTarget` 存储在 wrapper 的 vector 中，保持 payload 生命周期

### 3.3 装配

```
RuntimeComponents:
  resolvers:
    - {scheme: "file0", resolver: &resolver0}  → wraps LocalFileResolver(08:00.0, /dev/snvme0n1)
    - {scheme: "file1", resolver: &resolver1}  → wraps LocalFileResolver(4b:00.0, /dev/snvme1n1)
  data_paths:
    - {key: "local-nvme-ext4-dev0", data_path: &dp0}  → LocalNvmeDataPath(/dev/ssnvme0)
    - {key: "local-nvme-ext4-dev1", data_path: &dp1}  → LocalNvmeDataPath(/dev/ssnvme1)

URI routing:
  "file0:///mnt/nvme1/..." → resolver0 → key "local-nvme-ext4-dev0" → dp0
  "file1:///mnt/nvme2/..." → resolver1 → key "local-nvme-ext4-dev1" → dp1
```

## 4. 用例输出

### 4.1 Test 72: 双设备 WRITE/READ verify

```
--- 72. dual device WRITE/READ verify ---
  PASS: resolve files on both devices
  PASS: open target on device 0
  PASS: open target on device 1
  PASS: register memory for both devices
  PASS: WRITE device 0
  PASS: WRITE device 1
  PASS: READ device 0
  PASS: device 0 read-back 0x72
  PASS: READ device 1
  PASS: device 1 read-back 0x27
```

设备 0 写入 0x72、设备 1 写入 0x27，各自读回校验通过。无串扰。

### 4.2 Test 73: 跨设备 batch group-by-target

```
--- 73. cross-device batch group-by-target ---
  PASS: resolve files
  PASS: open targets
  PASS: register memory
  PASS: cross-device batch submit OK
  PASS: batch has IoHandle
  PASS: 2 initial states
  PASS: req 0 accepted
  PASS: req 1 accepted
  PASS: cross-device batch READ
  PASS: dev0 read-back 0x73
  PASS: dev1 read-back 0x37
```

一个 `submit()` 调用包含 2 个请求（不同 target、不同 DataPath），Runtime 按 `(DataPath, target)` 分组下发，两设备各自写入正确模式。

### 4.3 Test 74: 双 stream 双设备并发

```
--- 74. dual stream dual device concurrency ---
  PASS: resolve files
  PASS: open targets
  PASS: concurrent submits OK
  PASS: both have IoHandle
  PASS: dev0 no cross-talk (0x74)
  PASS: dev1 no cross-talk (0x47)
```

设备 0 在 stream 0、设备 1 在 stream 1 并发 submit，同步后各自读回校验通过。无串扰。

### 4.4 Test 75: 故障隔离

```
--- 75. fault isolation across devices ---
  PASS: resolve files
  PASS: open targets
  PASS: register memory
  PASS: batch has IoHandle (partial commit)
  PASS: device 0 request accepted
  PASS: device 1 request rejected (length=0)
  PASS: device 0 IO succeeded despite device 1 rejection
```

设备 1 的非法请求（length=0）被拒绝，设备 0 的正常 IO 不受影响。partial commit 语义正确。

### 4.5 总计

```
--- 72-75. Multi-device tests ---
  (双盘环境就绪，全部执行)
  passed: 799 (735 existing + 64 new)
  failed: 0
  RESULT: PASS
```

## 5. 回归输出

### 5.1 既有 datapath 契约测试

```
passed: 799  (Round 13 baseline: 735 → +64 from tests 72-75)
failed: 0
RESULT: PASS
```

零回归。既有测试 1-71 全部通过，新增测试 72-75 全部通过。

### 5.2 runtime 契约测试

```
passed: 115
failed: 0
RESULT: PASS
```

零回归。

### 5.3 HOST profile

HOST profile 存在 pre-existing 的 `-Werror=redundant-move` 编译失败（`storage_runtime.h:217`，生产代码中的 `return std::move(runtime)` 被 GCC 标记为冗余 move）。此问题同样影响 Round 12 的 HOST/CUDA build，非本 session 引入。硬件契约测试（实际验收标准）通过 `-Wno-redundant-move` 编译选项正常编译运行。

### 5.4 resolver_test 残留检查

```
/mnt/nvme1/GPU0/resolver_test/  → 空（无残留文件）
/mnt/nvme2/GPU0/resolver_test/  → 空（无残留文件）
```

## 6. 文件变更

### 修改

| 文件 | 变更 |
|---|---|
| `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp` | 新增 `MultiDeviceResolverWrapper` 类、`make_resolved_file_on_device()` / `make_resolved_file_dev0()` / `make_resolved_file_dev1()` 辅助函数、`second_device_available()` 环境检测、tests 72-75（4 个多设备用例） |
| `tests/local_nvme_datapath_contract/CMakeLists.txt` | 加 `-Wno-redundant-move`（抑制 `storage_runtime.h:217` 的 pre-existing warning） |

### 新建

| 文件 | 用途 |
|---|---|
| `chat/round15/result1.md` | 本结果文件 |

## 7. MultiDeviceResolverWrapper 设计说明

### 7.1 为什么需要 wrapper

`ext4_local_nvme` binding 硬编码 `kRecommendedDataPathKey = "local-nvme-ext4"`，所有 `LocalFileResolver` 产出的 `ResolvedTarget` 都使用此 key。要在同一 Runtime 中路由到不同 DataPath，需要设备特定的 key。

### 7.2 实现机制

```cpp
class MultiDeviceResolverWrapper : public StorageTargetResolver {
    LocalFileResolver inner_;                    // 委托真正的 FIEMAP 解析
    std::string scheme_;                         // "file0" / "file1"
    std::string override_key_;                   // "local-nvme-ext4-dev0" / "dev1"
    std::vector<ResolvedTarget> inner_results_; // 保持内部 RT 的 payload 存活
    
    resolve(uri, opts):
        1. 重写 URI: "file0:///path" → "file:///path"
        2. inner_.resolve(inner_uri, {scheme: "file"})
        3. 存储内部 RT 到 inner_results_（保持 payload 存活）
        4. view_payload(inner_rt) → 提取 Ext4LocalNvmePayload*
        5. null-deleter shared_ptr 借用 payload
        6. dummy lease (fd=-1, 不 close 任何 fd)
        7. ResolvedTarget::make(..., override_key_, borrowed_payload, dummy_lease)
};
```

### 7.3 生命周期安全

- 内部 `ResolvedTarget`（拥有真实 fd lease + payload）存储在 wrapper 的 `inner_results_` vector 中
- wrapper 的生命周期 ≥ Runtime 的生命周期（测试代码持有 wrapper 直到 `runtime->shutdown()` 后）
- 新 `ResolvedTarget` 的 null-deleter shared_ptr 借用 payload，不拥有它
- dummy lease（fd=-1）在析构时检查 `fd >= 0` 为 false，不调用 `close()`
- `StorageRuntime::open()` 将新 RT 存储在 `TargetEntry::resolved_target` 中，直到 target 被 close 或 runtime 被 shutdown

### 7.4 生产代码影响

**零改动**。wrapper 是纯测试设施，位于 test 文件中，不进入任何生产 target。

## 8. 环境检测

```cpp
static bool second_device_available() {
    struct stat st{};
    if (::stat("/mnt/nvme2", &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    struct stat parent_st{};
    if (::stat("/mnt", &parent_st) != 0) return false;
    return st.st_dev != parent_st.st_dev;  // 不同的 device = 挂载点
}
```

不可用时：
```
--- 72-75. Multi-device (SKIP: /mnt/nvme2 not mounted) ---
  SKIP: second NVMe device not available
  To enable: sudo mkfs.ext4 /dev/snvme1n1 && sudo mount -t ext4 /dev/snvme1n1 /mnt/nvme2
```

## 9. 已知事项

| 项 | 说明 | 归属 |
|---|---|---|
| `MultiDeviceResolverWrapper` 使用 null-deleter shared_ptr | 测试专用模式，借用 payload 不拥有。生产代码不应使用此模式——正确方案是让 binding 支持配置 `recommended_data_path_key` | Follow-up: 生产 binding 增加可配置 key |
| HOST profile pre-existing `-Werror=redundant-move` | `storage_runtime.h:217` 的 `return std::move(runtime)` 被 GCC 标记为冗余。影响所有含 `-Werror` 且 include `storage_runtime.h` 的非硬件测试 | Follow-up: 修复生产代码（`return runtime;` 去掉 `std::move`） |
| 测试编号重复（67-71 有两组） | Round 11 和 Round 14 各自添加了 67-71，编号冲突但断言独立 | Follow-up: 统一编号 |

## 10. 结论

**PASS。**

- 双真实 NVMe 设备经同一 StorageRuntime 多设备路径实测通过。
- 4 个多设备用例（72-75）：跨设备 WRITE/READ、跨设备 batch group-by-target、双 stream 并发、故障隔离——全部通过。
- 既有 735+115 断言零回归（799+115）。
- resolver_test 双盘目录均清空。
- 环境检测正确（不可用时 SKIP）。
- 生产代码零改动。

## 总指挥验收（2026-08-02）

**PASS。** 独立核验：

- **全量复跑**（全新 build/r15check）：datapath 契约 **799/0**（tests 76-81 全部出现并通过）、runtime E2E **115/0**、双盘 resolver_test 目录均空。
- **架构方案合理**：MultiDeviceResolverWrapper 为纯测试设施（生产零改动），payload 借用生命周期分析正确；设备特定 key 路由是 Runtime 原生能力，无需 core 改动。
- **总指挥顺手修复三处**：
  1. 测试编号冲突（R14 S3 defense 与 R15 多设备复用了 70-75 段）——defense 重编号 76/77、多设备重编号 78-81；
  2. `storage_runtime.h:217` redundant-move——在源头修复为显式构造 `Result<...>(std::move(runtime))`（GCC -Werror 与 nvcc 双兼容），撤掉 R15 S1 在测试 CMake 加的 `-Wno-redundant-move` 补丁；
  3. 提醒：`tutti/include/tutti/storage_runtime.h` 当前为 git untracked 状态（`??`），提交时需 `git add`。
- **S2（StripedResolver+Binding）解除阻塞。**
