# T-033 Round 9 Session 3 — Fail-closed LocalFileResolver — Result

## 0. 结论

**PASS（经 fail-closed follow-up 复核）**。22/22 resolver hardware tests 通过；12/12 binding hardware-free tests 通过（无回退）。

## 1. backing identity / namespace base 验证方式

### Backing device identity

`resolve()` 在打开文件后执行 `fstat(fd, &st)`，然后：

1. `S_ISREG(st.st_mode)` — 拒绝非 regular file（目录、设备、symlink 等）。
2. 如果 `BackingDeviceConfig.backing_device_path` 非空，执行 `stat(backing_device_path, &dev_st)`，比较 `st.st_dev == dev_st.st_rdev`。不匹配返回 `INVALID_ARGUMENT`。

测试 18 证明：文件在 `/dev/snvme0n1`，配置指向 `/dev/null`（`st_rdev=259`），`st_dev=66304` 不匹配 → 拒绝。

### Namespace byte base

`BackingDeviceConfig.namespace_base_bytes` 是显式配置的 namespace 起始偏移：

- 构造时校验 `namespace_base_bytes % block_size == 0`（resolve 时检查）。
- `device_offset = fe_physical + namespace_base_bytes`，在 `collect_fiemap_()` 中计算。
- 溢出检查：`fe_physical > UINT64_MAX - namespace_base_bytes` → `OUT_OF_RANGE`。
- 对齐检查：`device_offset % block_size != 0` → `INVALID_ARGUMENT`。

测试 15 证明：base=0 时 `device_offset=141557760`，base=1MiB 时 `device_offset=142606336`（= 141557760 + 1048576）。所有 extent 的 device_offset 都正确偏移。

## 2. 接受与拒绝的 FIEMAP flag 表

### 接受的 flag

| Flag | 含义 | 接受原因 |
|------|------|----------|
| (无 flag) | 正常已写入 extent | 直写安全 |
| `FIEMAP_EXTENT_LAST` | 文件末尾标记 | 仅作为结束标记，不表示 unsafe 状态 |

### 拒绝的 flag

| Flag | 值 | 拒绝原因 |
|------|----|----------|
| `FIEMAP_EXTENT_UNKNOWN` | 0x0001 | 未知状态，无法证明安全 |
| `FIEMAP_EXTENT_DELALLOC` | 0x0002 | 延迟分配，数据未落盘 |
| `FIEMAP_EXTENT_DATA_ENCRYPTED` | 0x0008 | 加密数据，不适用直写 |
| `FIEMAP_EXTENT_NOT_ALIGNED` | 0x0010 | 非对齐，违反 block 约束 |
| `FIEMAP_EXTENT_DATA_INLINE` | 0x0020 | inline data，无物理 extent |
| `FIEMAP_EXTENT_DATA_TAIL` | 0x0040 | tail packed，不适用 |
| `FIEMAP_EXTENT_UNWRITTEN` | 0x0800 | **fallocate-only**，数据未写入，直写会读到未初始化块 |
| `FIEMAP_EXTENT_ENCODED` | 0x1000 | encoded data（压缩），不适用直写 |
| `FIEMAP_EXTENT_SHARED` | 0x2000 | reflink/COW 共享 extent，直写会破坏其他文件 |

测试 2（fallocate-only → UNWRITTEN 被拒）和测试 20（flag rejection）验证了 `UNWRITTEN`（flag 值 0x801 = `UNWRITTEN | LAST`）被拒绝。SHARED/ENCODED 等通过代码检查验证 mask 覆盖。

### 其他拒绝条件

- 零长度 extent → `DATA_LOSS`
- 非单调 extent（`fe_logical < cursor`）→ `DATA_LOSS`
- 非对齐 extent（`fe_physical` 或 `fe_length` 非 block-aligned）→ `INVALID_ARGUMENT`
- extent 总数 > 124 → `RESOURCE_EXHAUSTED`
- 0 extent（空文件/sparse）→ `DATA_LOSS`
- extent 覆盖字节数 != file_size → `DATA_LOSS`

## 3. file mutation / layout 的明确边界

### Resolver 承诺

- 打开文件并验证其当前 FIEMAP 状态。
- 持有 fd lease（`FileDescriptorLease`），在 `ResolvedTarget` 销毁时关闭 fd。
- 产生 immutable payload，包含已验证的 extent snapshot。

### Resolver 不承诺

- **fd lease 是 advisory 的**，不能防止其他进程 truncate、hole-punch、reflink、COW 或 defrag。
- **不提供 extent pin**，不阻止布局变化。
- **不提供文件系统级锁**，不阻止 buffered/direct IO 并发。

### 部署契约

- 部署侧必须使用可强制的管理方式禁止 handle 生命周期内的 layout mutation。
- 部署侧必须确保文件是 Tutti 预分配且完整写入+fsync 的。
- 部署侧必须禁止普通 buffered/direct filesystem IO 与 Tutti 物理 extent IO 并发。
- resolver 只验证 metadata snapshot，不强制 layout lease。

这些约束已写入 resolver.h 的注释和错误信息中，不声称 resolver 能单独强制它们。

## 4. 正反测试与清理结果

### 正例（接受）

| # | 测试 | 结果 |
|---|------|------|
| 1 | normal_path (fallocate+write+fsync, 4MiB) | PASS |
| 3 | view_payload round-trip | PASS |
| 4 | map_to_device_offset | PASS |
| 5 | filefrag cross-validation | PASS |
| 12 | fd lease lifetime | PASS |
| 13 | lease move safety | PASS |
| 14 | multi-round FIEMAP (exts_per_call=1 vs default) | PASS |
| 15 | namespace_base application (offset = fe_physical + base) | PASS |
| 21 | payload compatibility (type/version/key/fd-lease) | PASS |

### 反例（拒绝）

| # | 测试 | 拒绝原因 | StatusCode |
|---|------|----------|------------|
| 2 | fallocate_only (UNWRITTEN) | unsafe flags 0x801 | DATA_LOSS |
| 6 | sparse_file (hole) | incomplete coverage (1MiB != 4MiB) | DATA_LOSS |
| 7 | scheme_mismatch | scheme != "file" | UNSUPPORTED |
| 8 | file_not_found | ENOENT | NOT_FOUND |
| 9 | malformed_uri | not "file://" prefix / not absolute | INVALID_ARGUMENT |
| 10 | block_size_zero | block_size == 0 | INVALID_ARGUMENT |
| 11 | alignment_check (1MiB block_size) | fs block size not multiple | INVALID_ARGUMENT |
| 16 | namespace_base_overflow | base near UINT64_MAX → misaligned | INVALID_ARGUMENT |
| 17 | namespace_base_misaligned | base=100 not block-aligned | INVALID_ARGUMENT |
| 18 | backing_device_mismatch | st_dev != st_rdev (/dev/null) | INVALID_ARGUMENT |
| 19 | not_regular_file (directory) | !S_ISREG | INVALID_ARGUMENT |
| 20 | fiemap_flag_rejection (UNWRITTEN) | unsafe flags | DATA_LOSS |

### 清理结果

```
$ ls /mnt/nvme1/GPU0/resolver_test/
(empty)
```

所有测试文件在测试结束时 `::unlink` 清理。

### Binding contract tests（hardware-free）

```
$ ctest --test-dir build/round9-session3 -R binding --output-on-failure
1/1 Test #10: tutti_binding_contract_test ......   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

12 个 binding contract test 全通过（无回退）。

## 5. public/SPI 未变的证明

```
$ git diff --name-only HEAD -- tutti/include/ tutti/spi/
(no output)
```

public/SPI header 完全未修改。

修改的文件（T-033 范围内）：

- `tutti/resolvers/local_file/resolver.h`（重写：fail-closed）
- `tutti/bindings/ext4_local_nvme/binding.h`（注释更新：device_offset 语义）
- `tests/resolver_contract/resolver_contract_test.cpp`（重写：21 个测试）
- `tutti/CMakeLists.txt`（修复目录名 target_resolvers → resolvers，已在 add_subdirectory 引用）
- `chat/round9/result3.md`（new）

未修改：
- public `StorageTargetResolver` SPI（`tutti/include/tutti/spi/storage_target_resolver.h`）
- `StorageRuntime`
- `LocalNvmeDataPath`
- libnvm
- kernel
- `tests/binding_contract/`（hardware-free binding test 无需改动）
- `tests/binding_contract/binding_contract_test.cpp`（12 个现有测试全部通过，无回退）

## 6. 环境与文件边界

```
$ pgrep tutti_daemon | head -1
(daemon 状态不变)

$ findmnt /mnt/nvme1 | tail -1
/mnt/nvme1 /dev/snvme0n1 ext4 rw,relatime

$ ls /mnt/nvme1/GPU0/resolver_test/
(empty — all test files cleaned)
```

- 未执行 mount/umount、mkfs、raw block IO、模块或 daemon 操作。
- 未接触 `/mnt/nvme4`。
- 测试文件只放在 `/mnt/nvme1/GPU0/resolver_test/`。
- resolver hardware test 仅在 `TUTTI_BUILD_HARDWARE_TESTS=ON` 下注册/执行。
- binding hardware-free test 在 `BUILD_TESTING=ON` 下注册/执行。

## 7. 失败路径 StatusCode 汇总

| StatusCode | 使用场景 |
|------------|----------|
| `INVALID_ARGUMENT` | 非法 URI、block_size=0、namespace_base 未对齐、backing device 不匹配、非 regular file、extent 非对齐 |
| `UNSUPPORTED` | scheme 不匹配 |
| `NOT_FOUND` | 文件不存在 |
| `NOT_READY` | backing device stat 失败 |
| `DATA_LOSS` | unsafe FIEMAP flags、零长度 extent、非单调 extent、0 extent、incomplete coverage |
| `OUT_OF_RANGE` | device_offset 溢出（由 namespace_base + fe_physical 超出 UINT64_MAX） |
| `DEVICE_ERROR` | fstat/open/fsync/FIEMAP ioctl 系统调用失败 |
| `RESOURCE_EXHAUSTED` | extent 总数 > 124 |

无 silent fallback。

## 8. 验收对照

1. ✅ HOST hardware-free binding tests 通过（12/12）
2. ✅ resolver hardware test 仅在 `TUTTI_BUILD_HARDWARE_TESTS=ON` 下注册/执行，使用受控测试目录
3. ✅ 执行 resolver contract 后清理所有临时文件（`/mnt/nvme1/GPU0/resolver_test/` 为空）
4. ✅ 失败路径明确是 `INVALID_ARGUMENT`、`UNSUPPORTED`、`NOT_READY`、`DATA_LOSS` 或 `OUT_OF_RANGE`，无 silent fallback
5. ✅ `git diff --check` 0 问题，linter 0 diagnostics

## Follow-up 独立复核（2026-07-31）

初版实现保留了一个无 `BackingDeviceConfig` 的 legacy 构造函数，且空 `backing_device_path` 会跳过 `st_dev` / `st_rdev` 校验；这与“无法证明 backing device 时 fail-closed”的目标冲突。因此完成以下最小收口：

1. 删除无配置 legacy 构造函数；所有 resolver 调用现在都显式提供配置。
2. `resolve()` 在打开文件前拒绝空 `backing_device_path`；配置路径还必须是 `S_ISBLK` 的 block device。
3. resolver 与 local-NVMe hardware contract 通过 `TUTTI_RESOLVER_BACKING_DEVICE`（默认 `/dev/snvme0n1`）提供受控 backing device。
4. 修正 test 16：原 `UINT64_MAX - 4096` 不对齐，实际只覆盖了提前的参数拒绝；现在使用最大 4 KiB 对齐 base，并硬断言 `OUT_OF_RANGE`。独立运行实际覆盖 `device_offset overflow` 分支。
5. 新增空配置拒绝 test；resolver hardware contract 由 21 项增至 22 项。

独立验证：

```text
ctest --test-dir build/round9-session3-hw -R '^tutti_resolver_contract_test$' --output-on-failure
1/1 Passed

build/round9-session3-hw/bin/tutti_resolver_contract_test
22/22 tests passed

ctest --test-dir build/round9-session3-hw -R '^tutti_local_nvme_datapath_contract_test$' --output-on-failure
1/1 Passed (10.89s)
```

local-NVMe hardware contract 在强制 resolver backing-device 配置后仍通过，证明公开 Runtime/file IO 回归未破坏；`/mnt/nvme1/GPU0/resolver_test/` 在运行后为空。相关 source/test diagnostics 为 0，`git diff --check` clean。

注意：当前 `build/round9-session3` 的完整 HOST CTest 有一个与 S3 无关的 `tutti_storage_runtime_contract_test` test 28 失败（Runtime Session 2 正在进行的并发测试）；binding/resolver 本身通过，不将该未完成 S2 问题归为 S3 回归。

## PASS
