# Round 15（重做）Session 3 Result：Runtime 跨 target 合并提交

## 概述

`StorageRuntime::submit` 内部的 `PendingGroup` 分组 key 由 `(DataPath, target)` 改为仅 `DataPath`。
一次跨多个 target（同一 DataPath）的公有 batch，现在合并为**一次** `DataPath::submit()` 调用，
消除了此前"每个 target 一次串行 submit"造成的 kernel launch 串行化。容量/配额/`LocalNvmeDataPath`/
`submit_one.cuh`/StripedDataPath 均未触碰。

## 改动文件清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `tutti/include/tutti/storage_runtime.h` | 修改 | `PendingGroup` 删除 `target` 字段；分组匹配条件由 `data_path == && target ==` 改为仅 `data_path ==` |
| `tutti/include/tutti/spi/data_path.h` | 修改 | 在 `submit()` 声明上方补充 SPI 契约注释：请求数组可跨同一 DataPath 内的多个 target |
| `tests/storage_runtime_contract/storage_runtime_contract_test.cpp` | 修改 | ① 更新既有 test 26（原 `test_component_runtime_groups_by_target`，重命名为 `test_component_runtime_groups_by_datapath`）的断言以反映合并后行为；② 新增 `TwoWayFakeResolver`（路由到两个不同 DataPath key）；③ 新增测试 82、83（合并计数） |

未改动：`local_nvme_data_path.*`、`submit_one.cuh`、任何容量/配额/arena 代码、MockDataPath（已有的 `submit_calls`/`last_requests`/`release_calls` 计数 seam 足够，无需新增）、memfs DataPath（走查确认无假设，未改）。

## REQUIRED 1：分组改为按 DataPath

`storage_runtime.h` diff（核心逻辑）：

```cpp
// Before
struct PendingGroup {
    DataPath* data_path = nullptr;
    DataPathTarget target;
    std::vector<std::size_t> indices;
    std::vector<DataPathRequest> requests;
};
...
if (candidate.data_path == target.data_path &&
    candidate.target == target.data_path_target) { ... }
...
group->target = target.data_path_target;

// After
struct PendingGroup {
    DataPath* data_path = nullptr;
    std::vector<std::size_t> indices;
    std::vector<DataPathRequest> requests;
};
...
if (candidate.data_path == target.data_path) { ... }
// (group->target assignment removed)
```

每个 `DataPathRequest` 仍携带自己的 `target`（`DataPathRequest{request, registration.value(), target.data_path_target}` 不变），
因此 per-request 路由语义未变——只是把"何时开新分组"的判据从 `(data_path,target)` 放宽为 `data_path`。
per-request 错误隔离（`initial_states`）、partial-commit、结果聚合、`release_io`（每个 `PendingGroup` 仍对应一个
`DataPathOp`，调用一次 `release`）语义均不变，只是现在同一 DataPath 下的多 target 请求落在同一个
`DataPathOp` / 同一次 `release` 调用里。

## REQUIRED 2：SPI 契约注释 + 走查

### 注释（`tutti/include/tutti/spi/data_path.h`）

```cpp
// submit() contract: the `requests` array MAY span multiple distinct
// `target` identities within this same DataPath (each DataPathRequest
// carries its own `target`, resolved independently by the
// implementation). Callers (the runtime) group requests by DataPath
// only, NOT by (DataPath, target); a DataPath implementation must not
// assume all requests in one submit() call share a single target.
```

### 走查结论

- **MockDataPath**（`tutti/testing/mock_data_path.h`）：`submit()` 纯按索引遍历 `requests[0..count)`，
  对 `request.target` 只是转交/记录（`last_requests.assign(...)`），不存在"取 `requests[0].target` 广播到全部"
  之类的单 target 假设。**无需修复**。已有的 `submit_calls`（调用次数）、`last_requests`（最近一次调用的完整请求数组）、
  `release_calls` 计数 seam 已满足本 session 合并计数测试所需，未新增字段。
- **memfs DataPath**（`tutti/bindings/memfs/memfs_data_path.h`）：`submit()` 循环内对每个请求独立调用
  `req.target.token()` / `req.memory.token()` 做查找，无跨请求共享单一 target 的逻辑。**无需修复**。

两者均已是多 target 安全的，走查未发现需修复的假设。

## REQUIRED 3：测试

### 合并计数测试（新增，编号 82/83，均为 hardware-free mock 测试）

- **测试 82** `test_cross_target_batch_merges_single_submit`：单 DataPath 上开 3 个 target（K=3），一次
  `rt->submit` 提交 3 个跨 target 请求 → 断言 `data_path.submit_calls == 1`、`last_requests.size() == 3`
  且 3 个请求的 target 两两不同（证明未被拆分）。
- **测试 83** `test_cross_datapath_batch_not_merged`：两个独立 DataPath（各 1 个 target），一次
  `rt->submit` 提交跨两个 DataPath 的 2 个请求 → 断言 `data_path_a.submit_calls == 1` 且
  `data_path_b.submit_calls == 1`（合计恰好 2 次），且各自 `last_requests.size() == 1`（验证按
  DataPath 分组不会误并不同 DataPath 的请求）。

两测试均加入 `tests/storage_runtime_contract/storage_runtime_contract_test.cpp` 的本地测试数组（编译期
第 35、36 项），运行输出：

```
$ ./bin/tutti_storage_runtime_contract_test
All 36 storage runtime contract tests passed.
```

（原 34 项 + 新增 2 项 = 36，全部 PASS。）

### 既有测试同步更新

`test_component_runtime_groups_by_target`（原 test 26）验证的正是旧的 `(DataPath,target)` 分组行为
（2 target 同 DataPath → 断言 `submit_calls == 2`、`release_calls == 2`）。分组策略改变后该断言不再成立，
已重命名为 `test_component_runtime_groups_by_datapath` 并更新断言为 `submit_calls == 1`、
`release_calls == 1`、`last_requests.size() == 2`，用以文档化新的合并行为。

## 回归证据

构建目录：`build/r15base`（已有 CUDA + 硬件测试配置，复用自 S2/S2b）。

### 硬件（一次实测，环境：snvme0/snvme1 已加载+挂载，daemon 运行中）

```
$ ./bin/tutti_local_nvme_datapath_contract_test
  passed: 799
  failed: 0
RESULT: PASS

$ ./bin/tutti_storage_runtime_local_nvme_contract_test
=== Summary ===
  passed: 115
  failed: 0
RESULT: PASS

$ ctest -L hardware
100% tests passed, 0 tests failed out of 4
(tutti_resolver_contract_test, tutti_local_nvme_datapath_contract_test,
 tutti_storage_runtime_local_nvme_contract_test, tutti_layerwise_kv_overlap)
```

### 非硬件 ctest（HOST + CUDA，独立全新配置目录）

```
$ cmake -S tutti -B /tmp/tutti-r15s3-host -DBUILD_TESTING=ON -DTUTTI_ACCELERATOR=HOST
$ cmake --build /tmp/tutti-r15s3-host && cd /tmp/tutti-r15s3-host && ctest
100% tests passed, 0 tests failed out of 15

$ cmake -S tutti -B /tmp/tutti-r15s3-cuda -DBUILD_TESTING=ON -DTUTTI_ACCELERATOR=CUDA -DTUTTI_BUILD_HARDWARE_TESTS=OFF
$ cmake --build /tmp/tutti-r15s3-cuda && cd /tmp/tutti-r15s3-cuda && ctest
100% tests passed, 0 tests failed out of 15
```

同时在 `build/r15base` 内 `ctest -LE hardware` 复核：15/15 全绿。

测试临时目录 `/mnt/nvme1/GPU0/resolver_test/` 运行后为空，无残留。

## 边界遵守确认

- 未改动：容量/配额/arena 任何代码、`local_nvme_data_path.*`、`submit_one.cuh`、StripedDataPath（未重建）。
- 只改动：`storage_runtime.h`（分组）、SPI 头注释（`spi/data_path.h`）、`storage_runtime_contract_test.cpp`
  （新增计数 seam 走查确认无需改 MockDataPath 本体 + 新增测试 + 更新既有测试断言）。memfs 未改（走查确认
  无需修复假设）。
- 新测试编号从 82 起（82、83），未与既有 70-73/76-77/78-81 冲突。

## 未提交 Git

`storage_runtime.h`、`spi/data_path.h` 当前为 git untracked（`??`，符合既有备忘：该重构分支下这两个文件
尚未纳入版本控制）；`tests/` 整目录被 `.gitignore` 忽略（`.gitignore:84`）。本 session 未执行任何 git 操作。

## 总指挥验收（2026-08-03）

**PASS。** 独立核验：

- **改动抽查**：`storage_runtime.h:1078` 分组条件已为 data_path-only；`DataPathTarget target;` 成员计数 0（已删除）；SPI 契约注释在 `spi/data_path.h:342`；改动范围与报告清单一致（3 文件），容量/配额/DataPath/submit_one 零触碰。
- **复跑**：storage_runtime 契约 **36/36**（含新增合并计数 82/83 与更新后的 test 26）；datapath 硬件契约 **799/0**、runtime E2E **115/0**、非硬件 ctest **15/15**；双盘 resolver_test 空；`git diff --check` clean。
- **既有测试更新正确**：test 26 从"按 target 分组（2 次 submit）"改为"按 DataPath 合并（1 次）"，正是本 session 语义变更的文档化。
- 防缠结规则遵守良好：改动文件清单完整、测试编号 82/83、无默认值夹带。

**S4（容量参数化 + simulator legacy 效果）解除阻塞**：`chat/round15/session4.md` 可启动。注意 simulator 是本 round 的 main 目标（1 launch/层、≥5GB/s）。
