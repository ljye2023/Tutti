# TASK T-040 — Round 10 Session 1：libnvm/snvme 双树事实源审计与唯一 source owner 收敛

## 前置条件

- Round 9 已全部收口（含 `session4b` 总指挥验收 PASS）；阅读 `Roadmap.md` Phase 3、`TUTTI_TARGET_ARCHITECTURE.md` 第 2143 行迁移表与 R14、`chat/round9/result1.md`。
- 已知事实：根树 `backends/local/nvme/libnvm/`（+`backends/local/kernel_modules/snvme-*/`）与 `tutti/device_manager/nvme/{libnvm,kernel_modules,nvmeservice}` 并存；`git status` 显示两棵树的 `src/ctrl.cpp`、`src/linux/device.cpp` 同时被修改——同一份修复被打了两遍，dual-tree 危害是活的事实。
- Round 9 已确认 standalone CUDA target 只消费 `tutti/device_manager/nvme/libnvm/`；根 build（project `libnvm`）仍 glob 根树源码。

## 目标

为 libnvm/snvme 选定唯一 source owner（预期 `tutti/device_manager/nvme/`），让根 build 与 standalone build 引用同一份源码，且本 session 不改变任何运行时行为。nvmeservice 不在本 session 收敛（Session 2 处理其归属）。

## 允许修改/创建

- 根 `CMakeLists.txt`（仅将 libnvm source/include 路径重定向到唯一 owner，或加保护性 `message(FATAL_ERROR)` 防止并行分叉）
- `tutti/CMakeLists.txt`、`tutti/device_manager/CMakeLists.txt`（如需要导出统一变量）
- 两棵树中任何一棵的**删除或替换为转发**（须先在 result 中记录审计结论再动手）
- `chat/round10/result1.md`

## 禁止范围

- 不改变 libnvm/snvme 的任何源码行为（不改 `.cpp/.cu/.h` 逻辑；仅移动/去重/审计）。
- 不删除或重写根树 legacy 其他组件（`memory/`、`io_engine/`、`nvme_storage/`、`backends/local/` 其余部分属 Phase 7）。
- 不覆盖用户未提交改动：两棵树当前均有未提交修改，动手前必须先 `git diff` 审计这些修改在两树间是否等价，把差异逐条写入 result1.md，再决定归并方向。
- 不执行模块/daemon/mount/bind/unbind/format/raw LBA IO；不 insmod/rmmod。
- 不提交 Git。

## 必须实现的行为

1. 审计两树 libnvm 全部源码差异（含未提交改动），输出逐文件结论：完全相同/仅一方修复/逻辑分叉。对每条分叉给出归并决议与理由。
2. 归并后磁盘上 libnvm 实现只剩一份事实源；另一处要么删除，要么显式失败/转发，且任何后续“只改一边”在 configure 期就会被发现。
3. snvme kernel module 源码同样只剩一份事实源（预期 `tutti/device_manager/nvme/kernel_modules/`）；根 `CMakeLists.txt` 的 module 编译路径同步重定向或移除。
4. 根 build 与 standalone build 都能 configure+build；二者编译出的 libnvm 目标来自同一批源文件（在 result 中给出两边 compile_commands 或 verbose 证据）。

## 测试要求

- 根 build：`cmake` configure + build 通过；既有根树测试不回归（若根树测试目标引用了被删除的旧路径，允许最小重定向，不许改测试语义）。
- standalone：`TUTTI_ACCELERATOR=HOST` 与 `=CUDA` 各自 clean configure+build+`ctest` 通过（HOST 至少 10/10 现状；CUDA 硬件测试只显式运行一次确认无回归）。
- 不需要新硬件测试；本 session 是纯构建/源码归并。

## 验收

- `chat/round10/result1.md` 含：双树逐文件差异审计表、归并决议、两 build 引用同一事实源的命令级证据、前后 `git status` 对比。
- 总指挥复跑：根 build + HOST/CUDA standalone ctest；并抽查 `git grep` 确认不再存在第二份 libnvm/snvme 实现。

## 后续依赖

- Session 2（control plane 归位）与 Session 3（UAPI）都依赖本 session 的唯一事实源结论。
