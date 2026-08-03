# TASK T-070 — Round 13 Session 3：第二批退役（NVMeService 双树收敛与生产资产迁移）

> **并行执行**：本 session 可与 Session 2 并行，必须遵守 `chat/round13/PARALLEL.md` 的行级分治与联合验证协议。编辑期不得自行构建验证；daemon 重启由 operator 在联合验证通过后执行。

## 前置条件

- Session 2 验收通过；阅读 S1 决议表中本批条目。
- 已核实现状：`backends/local/NVMeService/` 与 `tutti/device_manager/nvme/nvmeservice/`（src+examples）内容相同；`tutti_daemon` 源码在根 `examples/tutti_daemon.cpp`，root build 的 daemon 运行依赖根 `nvmeservice` 库（`backends/local/NVMeService/src`）；`memory/`、`device_manager/` 为 legacy L1/L2。

## 目标

NVMeService 收敛为 tutti 侧唯一事实源；`tutti_daemon` 等生产资产迁移到 tutti 侧；删除 `backends/local/NVMeService/`、`memory/`、`device_manager/`、`examples/` 中已迁移或无消费者的部分。daemon 功能全程可用。

## 允许修改/创建

- 删除/迁移本批树与文件；根 `CMakeLists.txt`、`tutti/CMakeLists.txt`（daemon target 接线）
- `tutti/device_manager/nvme/nvmeservice/**`（daemon target 落位）
- `doc/history/`（归档）
- `chat/round13/result3.md`

## 禁止范围

- 不改变 daemon 行为、协议、配置格式（`sys_config.yaml` 兼容）；不改 nvmeservice 库逻辑。
- 不执行模块/daemon/mount 操作（daemon 重启验证由 operator 执行）；不提交 Git。
- 未列入 S1 决议的树不动。

## 必须实现的行为

1. **NVMeService 唯一源**：tutti 侧为唯一事实源；root build 的 `nvmeservice`/`tutti_daemon` 从 tutti 侧源码编译（路径重定向，同 Round 10 S1 手法）；`backends/local/NVMeService/` 删除。
2. **daemon target 迁移**：`tutti_daemon` target 在 tutti 侧定义（root build 经 add_subdirectory 或直接引用 tutti 源码）；构建产物路径与启动命令（`build/bin/tutti_daemon --config sys_config.yaml`）保持兼容或提供显式迁移说明。
3. **memory/、device_manager/ 退役**：standalone 与 root build 均不再引用后删除；`examples/` 中无消费者的示例删除，`tutti_daemon.cpp` 随迁移移除。
4. **功能验证**：迁移后 daemon 二进制构建通过；operator 重启 daemon 后两硬件契约复跑通过（735/115 基线）。

## 测试要求

- 三端构建（root、HOST、CUDA）通过；ctest 基线全绿；
- operator 重启 daemon（新二进制）后：两硬件契约 735/0 + 115/0；
- `git grep` 证明无第二份 NVMeService 实现、无 `backends/local` 残留引用。

## 验收

- `chat/round13/result3.md`：迁移/删除清单、唯一源证据（find+grep）、daemon 兼容验证、operator 重启后硬件回归。
- 总指挥复跑三端构建；operator 重启 daemon 后复跑两硬件契约确认。

## 后续依赖

- S4（Phase 7 门禁）依赖本批完成。
