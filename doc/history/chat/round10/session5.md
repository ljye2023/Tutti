# TASK T-044 — Round 10 Session 5：头文件卫生与 Phase 3 验收门禁

## 前置条件

- Session 1-4 全部完成并通过总指挥验收。
- 阅读 `Roadmap.md` Phase 3 gate 原文与 Phase 3 最后一条 deliverable「Local-NVMe private headers, CUDA kernel, and libnvm do not propagate from public targets」。

## 目标

以可重复、可 CI 化的检查关闭 Phase 3：public target 不传播 local-NVMe 私有头；根 build 与 standalone build 引用同一事实源；UAPI 断言与两 baseline compile-only 构成正式门禁。

## 允许修改/创建

- `tutti/CMakeLists.txt`、根 `CMakeLists.txt`（仅卫生修正：INTERFACE include 收缩）
- `tests/` 下新增头文件卫生契约测试（consumer 编译测试）
- `scripts/`（Phase 3 gate 一键脚本）
- `chat/round10/result5.md`

## 禁止范围

- 不改变任何运行时行为；不重构 Session 1-4 已定型的结构。
- 不执行模块/daemon/mount/bind/unbind/format/raw LBA IO；硬件契约测试只按既有方式显式运行。
- 不提交 Git。

## 必须实现的行为

1. 私有头不传播：`tutti_api` 及 public header 的 consumer TU 在只有 public include path 时无法 `#include` 到 libnvm/nvmeservice/snvme/CUDA kernel 任何头；新增 consumer 编译测试同时给出正例（public API 可用）与负例（私有头不可达，可用 `__has_include` 断言）。
2. CMake 审计：每个 target 的 INTERFACE_INCLUDE_DIRECTORIES 不含 local-NVMe 私有路径（脚本化检查输出清单）。
3. Phase 3 gate 一键脚本按顺序执行并输出汇总：
   - 根 build configure+build；
   - standalone HOST clean configure+build+ctest；
   - standalone CUDA clean configure+build（硬件测试仅在有 GPU+snvme 环境时显式运行）；
   - UAPI static-assert 契约测试；
   - 两个 kernel baseline 的 module compile-only；
   - `git grep` 级双事实源检查（libnvm/snvme 只剩一份实现）。
4. 更新 `Roadmap.md`：Phase 3 各项 deliverable 标记完成状态，Known Bugs Snapshot 同步（若发现新问题）。

## 测试要求

- 门禁脚本全绿；硬件两契约（local_nvme_datapath、storage_runtime_local_nvme）显式运行一次，断言数不低于 Round 9 基线且全通过。
- consumer 卫生测试纳入默认 `BUILD_TESTING`（hardware-free）。

## 验收

- `chat/round10/result5.md` 含：门禁脚本完整输出、私有头不可达证据、target include 审计清单、Roadmap 更新 diff。
- 总指挥复核：重跑门禁脚本与硬件两契约，抽查 include 图；全部通过后宣布 Phase 3 关闭、Phase 4（Metadata Pools and True Async IO）可启动。
