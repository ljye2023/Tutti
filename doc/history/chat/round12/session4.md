# TASK T-067 — Round 12 Session 4：feature ON/OFF 开关与 Phase 6 门禁

## 前置条件

- Session 1-3 全部验收通过；阅读 `Roadmap.md` Phase 6 deliverable「Feature ON/OFF CI entries for every profile and DataPath」。

## 目标

为每个 profile 与 DataPath 建立 feature 开关（OFF=零依赖参与构建），并以一键门禁关闭 Phase 6。

## 允许修改/创建

- `tutti/CMakeLists.txt`、`tutti/cmake/**`（feature 选项定义与接线）
- `scripts/phase6_gate.sh`（新建）
- `Roadmap.md`（Phase 6 标记）
- `chat/round12/result4.md`

## 禁止范围

- 不改变默认构建行为（全部 feature 默认 ON，现状不变）；不删除任何现有测试。
- 不执行模块/daemon/mount 操作；不提交 Git。

## 必须实现的行为

1. **feature 开关**：`TUTTI_FEATURE_LOCAL_NVME=ON|OFF`（及 sample 扩展的开关）——OFF 时对应 package 的源码/依赖（libnvm、CUDA、daemon 头）完全退出 configure/build，且其余 target 与测试不受影响。
2. **OFF=零依赖证据**：`TUTTI_ACCELERATOR=HOST -DTUTTI_FEATURE_LOCAL_NVME=OFF` configure 输出无 libnvm/CUDA 查找、compile_commands 无对应 TU、无悬空引用；组合矩阵（HOST×{ON,OFF}、CUDA×{ON,OFF}）全部 configure+build 通过。
3. **一键门禁脚本**（`scripts/phase6_gate.sh`）：
   - HOST/CUDA 两 profile build+ctest（hardware-free 部分）；
   - feature ON/OFF 组合矩阵；
   - MockDataPath kit 契约、profile 唯一性、sample 扩展契约；
   - header hygiene + 既有硬件契约提示（有环境时显式运行）；
   - 汇总表输出。
4. **Roadmap 更新**：Phase 6 各 deliverable 标记完成状态；「不做项」（raw/GDS/RDMA/新 fs/新 GPU vendor/device-initiated IO 未主动实现）保持如实记录。

## 测试要求

- 门禁脚本全绿；矩阵各组合 ctest 断言数符合预期（OFF 时相应测试不存在而非失败）。
- 既有 735+115 硬件基线不受影响（ON 配置下重跑一次确认）。

## 验收

- `chat/round12/result4.md`：开关定义清单、组合矩阵证据、门禁输出、Roadmap diff。
- 总指挥重跑门禁与组合矩阵；全过后宣布 Phase 6 关闭。
