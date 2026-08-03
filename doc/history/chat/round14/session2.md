# TASK T-073 — Round 14 Session 2：TUTTI_VERBOSE 日志门控移植

## 前置条件

- 阅读 main commit `6d9a8e1`（`third_pkgs/Tutti`）：info 级 bring-up 日志门控模式（`TUTTI_VERBOSE` env，error 路径不变）。
- 已核实：重构树 nvmeservice/control/data_paths/daemon 区域有 43 处 `fprintf(stderr/printf` 输出点；其中 error 路径（RPC failed/rejected 等）必须保持常开。

## 目标

把 `TUTTI_VERBOSE` 门控模式移植到重构树：bring-up/info 级日志默认静默，`TUTTI_VERBOSE=1` 恢复；error 路径不受门控。

## 允许修改/创建

- `tutti/device_manager/nvme/nvmeservice/**`（client/daemon bring-up 日志）
- `tutti/data_paths/local_nvme/**`（init/open/arena/cache 的 info 日志）
- `examples/tutti_daemon.cpp`（若已迁移到 tutti 侧则为对应新位置）
- `chat/round14/result2.md`

## 禁止范围

- 不改变任何 error 路径输出；不改变日志内容文本（只加门控）。
- 不动测试文件的输出（契约测试的 PASS/FAIL/统计输出不门控）。
- 不引入新日志框架；模式与 main 一致（`static bool tutti_verbose()` + `TUTTI_INFO` 宏，放在合适的最小公共头）。
- 不执行模块/daemon/mount 操作；不提交 Git。

## 必须实现的行为

1. **分类审计**：43 处输出点逐条分类 info/error（清单入 result）；error 零改动。
2. **门控实现**：统一的 `tutti_verbose()` 判定（一次 getenv 缓存）；info 输出经 `TUTTI_INFO` 宏；默认运行（无 env）下 stderr 干净。
3. **daemon 启动横幅**：`nvmeservice: device=...`/`listening on...`/`Owned devices` 这类 bring-up 横幅属 info 级，纳入门控（daemon 作为运维工具，建议保留一行 listening 提示常开——裁决并记录理由）。
4. **kernel 侧不动**：`pr_info` 等内核日志不在本 session 范围。

## 测试要求

- HOST/CUDA ctest 全绿（门控不得影响断言）。
- 硬件契约两连跑：默认 env 与 `TUTTI_VERBOSE=1` 各一次，断言一致（735/115），默认 env 下 stderr 无 info 级输出（给出前后 stderr 行数对比证据）。

## 验收

- `chat/round14/result2.md`：分类清单、门控位置、两 env 对比证据、回归输出。
- 总指挥复核：error 路径零改动（diff 抽查）；默认静默实测。

## 后续依赖

- 与 Session 1 可并行；Session 3 依赖本 session 完成（同 package）。
