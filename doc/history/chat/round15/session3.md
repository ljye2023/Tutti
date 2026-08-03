# TASK T-082 — Round 15（重做）Session 3：Runtime 跨 target 合并提交

> **先读 `chat/round15/BASELINE.md`**（当前树状态与防缠结规则）。本 session 是小步：只改分组策略 + 测试，**禁止**动容量/配额/LocalNvmeDataPath/submit_one.cuh。

## 背景（已核实事实，勿重复调查）

- Runtime 按 `(DataPath, target)` 分组（`storage_runtime.h` PendingGroup），导致一次跨文件的 `rt->submit` 变成 N 次 `DataPath::submit`（每次 3 H2D + 1 memset + 1 kernel launch 在 caller stream 串行）。
- `LocalNvmeDataPath::submit` 数据面已 per-entry 多 target 就绪：`local_nvme_data_path.cpp:965`（每请求 `find_(req.target)`）、`:1040`（`entry.target = tstate->dev_handle`）、`submit_one.cuh` kernel 按 `e.target` resolve。
- 本 session 只解除分组这一层；容量不足由下一 session 解决（超出容量时按既有语义 per-request `RESOURCE_EXHAUSTED`，行为不变）。

## REQUIRED 1：分组改为按 DataPath

`storage_runtime.h` 内部（非 public API）：PendingGroup key 从 `(data_path, target)` 改为 `data_path`（PendingGroup 的 `target` 成员可删除）。per-request 错误隔离、partial-commit、聚合、`release_io` 语义不变。

## REQUIRED 2：SPI 契约注释 + 走查

- DataPath SPI 头文件契约注释明确：`submit()` 请求数组**可跨多个 target**（同一 DataPath 内）。
- 走查 MockDataPath（testing kit）与 memfs DataPath 的 submit 实现，确认无单 target 假设；结论写入 result（有假设则修复）。

## REQUIRED 3：测试

- **合并计数（mock，hardware-free）**：MockDataPath 增加 submit 调用计数 seam；一次 `rt->submit` 跨 K 个 target（K=3，同 DataPath）→ 断言 `DataPath::submit` 恰好 **1 次**且 K 个请求全在同一调用数组中；两 DataPath 各 1 target 的混合 batch → 恰好 2 次（验证按 DataPath 分组不误并）。
- **零回归**：799/0 + 115/0（硬件，一次）+ HOST/CUDA 非硬件 ctest 全绿。

## 边界

- **只准改**：`storage_runtime.h`（分组）、SPI 头注释、MockDataPath（计数 seam + 必要时修假设）、memfs（仅必要时）、测试文件。
- **禁止**：容量/配额/arena 任何变更；`local_nvme_data_path.*`、`submit_one.cuh`；StripedDataPath 相关（不存在，勿重建）。
- 新测试编号从 **82** 起。

## 结果文件

`chat/round15/result3.md`：diff 摘要、SPI 注释、走查结论、合并计数测试输出、回归证据、**改动文件清单**。
