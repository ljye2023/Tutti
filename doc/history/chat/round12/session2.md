# TASK T-065 — Round 12 Session 2：CUDA-like profile 契约完善与唯一 profile 证明

## 前置条件

- 阅读 `Roadmap.md` Phase 6 deliverable「CUDA-like profile contract covering allocation, pointer, stream/event, copy/context」与「`HOST`/`CUDA` profile proving exactly one `TUTTI_USE_<PROFILE>`; unselected SDK does not participate in build」。
- 现状：`tutti/cmake/accelerators/{HOST,CUDA}.cmake` 两 profile；`cuda_like_contract_test` 已存在（范围待核）；`TUTTI_ACCELERATOR` 单 profile 选择在 configure 期生效。

## 目标

把 profile 契约补齐到 Roadmap 点名范围，并以 configure/build 期证据链证明：恰好一个 profile 生效、未选中 SDK 零参与。

## 允许修改/创建

- `tests/`（profile 契约测试补充）
- `tutti/cmake/`（仅当唯一性证明需要 configure 期检查时）
- `tutti/CMakeLists.txt`（测试接线）
- `chat/round12/result2.md`

## 禁止范围

- 不新增 profile（MACA/MUSA 属未来）；不修改 accelerator 选择逻辑语义。
- 不修改 public 头与生产代码（除非发现契约缺口且有测试先行）。
- CUDA profile 的运行时检查可用本机 GPU，但不得碰模块/daemon/挂载；不提交 Git。

## 必须实现的行为

1. **契约覆盖审计**：`cuda_like_contract_test` 现有覆盖 vs 点名范围（allocation / pointer 属性查询 / stream+event / copy / device context），列出缺口并补齐；HOST 与 CUDA 两 profile 各自有对应断言（HOST 走 shim 语义）。
2. **唯一 profile 证明**：configure 期检查——`TUTTI_ACCELERATOR` 非法值/缺失时 configure 明确失败；恰好一个 `TUTTI_USE_<PROFILE>` 宏被定义（编译期 static_assert 或 configure 输出证据）。
3. **未选中 SDK 零参与**：`TUTTI_ACCELERATOR=HOST` 时 configure 输出不含 CUDA 查找、编译命令行无 CUDA include/link（compile_commands.json 证据）；反向 CUDA 亦然（不查找 HOST 无需证明，重点证明未选中的 vendor SDK 路径不出现在任何 target）。
4. **文档化**：profile 扩展指南（新增一个 profile 需要哪些文件 + 一行注册），写入 result 或既有 docs。

## 测试要求

- HOST ctest 全绿（含新契约断言）；CUDA profile build + 非硬件 ctest 全绿。
- 唯一性/零参与证据可脚本化复跑（命令+输出贴入 result）。

## 验收

- `chat/round12/result2.md`：覆盖审计表（点名项→断言位置）、唯一 profile 证据、未选中 SDK 零参与证据、扩展指南。
- 总指挥复核：抽查 compile_commands 与 configure 输出；契约断言与点名范围一一对应。

## 后续依赖

- 与 Session 1 可并行；Session 4 门禁依赖本 session。
