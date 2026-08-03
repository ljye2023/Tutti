# TASK T-068 — Round 13 Session 1：legacy 退役审计与性能基线捕获

## 前置条件

- Round 12 S4 通过（Phase 6 关闭）；阅读 `Roadmap.md` Phase 7、「`TUTTI_REFACTOR_TAKEOVER.md` §2.4」、`chat/round10/result1.md`。
- 已核实的现状（审计起点，非结论）：
  - 根 build 仍接线：`memory/`(341)、`backends/local/NVMeService`(401+422)、`device_manager/`(426)、`nvme_storage/`(429)、`block_storage/`(434)、`io_engine/`(438)、`coordinator/`(445)、`adapters/kv_cache`(452)、`examples/`(457)、legacy `tests/`(462)；
  - **NVMeService 仍为双树**：`backends/local/NVMeService/` 与 `tutti/device_manager/nvme/nvmeservice/` 的 src+examples 内容相同（`diff -rq` 无差异）——与 Round 10 S1 前的 libnvm 同类风险；
  - 生产资产：`tutti_daemon` 源码在根 `examples/tutti_daemon.cpp`（root build target）；`libnvm`/`nvmeservice` 库、`modules` 必须保持可用；
  - legacy 根 `tests/` 有 pre-existing `layer1_smoke_test` 链接失败。

## 目标

产出逐树退役/迁移/保留决议表与退役顺序，并捕获退役前性能基线（供 Phase 7 gate 的 "performance baseline preserved or improved" 对比）。

## 允许修改/创建

- `chat/round13/result1.md`
- `scripts/`（基线捕获脚本，可复跑）

## 禁止范围

- 本 session 不删除/修改任何源码与 CMake（纯审计+测量）。
- 不执行模块/daemon/mount 操作；不提交 Git。

## 必须实现的行为

1. **逐树依赖图**：`memory/`、`device_manager/`、`nvme_storage/`、`block_storage/`、`io_engine/`、`coordinator/`、`adapters/kv_cache/`、`backends/local/NVMeService/`、`examples/`、legacy `tests/` —— 每棵树：谁 include 它（grep 证据）、谁链接它（CMake 证据）、standalone `tutti/` 是否依赖、生产运行是否依赖。
2. **决议表**：每树标记 `删除` / `迁移后删除` / `保留`，迁移类写清资产去向（如 `tutti_daemon` 迁往 `tutti/` 侧何处、NVMeService 双树以哪边为唯一源——预期 tutti 侧，根侧重定向或删除）。
3. **退役顺序**：按依赖反向排序，每步给出验证命令（build+ctest+硬件契约）。
4. **性能基线**：用既有硬件契约 + 一个最小顺序 IO 循环（4K/1M，READ/WRITE）测 3 轮取中位，记录 IOPS/BW 到 result（注明：Round 11 S4 完整基准仍暂缓，此为退役对比用的轻量基线）。

## 验收

- `chat/round13/result1.md`：依赖图证据、决议表、退役顺序、基线数据与复跑脚本。
- 总指挥复核决议表完整性（无遗漏消费者）与基线可复跑性。

## 后续依赖

- S2/S3 按本 session 的顺序执行。
