# TASK T-075 — Round 14 Session 4：layerwise KV-cache overlap simulator 移植到 StorageRuntime

## 前置条件

- 阅读 main commit `b83adfc`（`third_pkgs/Tutti`，legacy examples）：HY3 形状 128K-context 请求（80 layers、512×256-token chunks、90% prefix hit）、3-stream layerwise pipeline（read(L+1) ∥ SGEMM compute(L) ∥ write(L-1)）、SM-budgeted grid-stride SGEMM、高优先级 IO stream、GPU-event 计时、per-10-layer 报告。
- 新架构事实：StorageRuntime public API + LocalNvmeDataPath（Round 8-11 验收）；stream-ordered 异步已证明（Round 11 S3）；示例必须只调用 public API。

## 目标

把 layerwise KV-cache overlap simulator 重写为基于 StorageRuntime 的示例，在真实硬件上演示 IO∥compute 重叠，输出与 legacy 版同级别的可读报告。

## 允许修改/创建

- `tutti/examples/`（新建示例目录；CMake 接线一行风格，参考 memfs sample）
- `chat/round14/result4.md`

## 禁止范围

- 不修改 core/DataPath/resolver/binding；示例只用 public `tutti_api`。
- 不依赖 legacy 头/库；不进默认构建的必跑测试（hardware label，显式运行）。
- 不执行模块/daemon/mount 操作（环境由 operator 预置）；不提交 Git。

## 必须实现的行为

1. **同等形状**：80 layers、chunk 化、90% prefix hit 的读写混合负载；3-stream pipeline（read-ahead ∥ compute ∥ write-behind）；SGEMM compute kernel 带 SM 预算（不占满 SM，给 IO kernel 留资源）。
2. **只经 public API**：KV 池一次 `register_memory`；layer/chunk 切片经 `memory_offset`；`submit/query/wait/release_io` 生命周期完整。
3. **计时与报告**：GPU event 计时 IO 接口时间；per-10-layer 进度；末尾汇总（总时长、IO 暴露时间、重叠率估计）。输出格式参照 legacy 版级别。
4. **正确性校验**：pipeline 结束后抽样（或全量）校验 write→read 数据一致（不得只做性能不验证数据）。
5. **接线**：`tutti/CMakeLists.txt` 一行注册（放在 `include(CTest)` 之后的 BUILD_TESTING 块——吸取 Round 12 S3 教训），hardware label。

## 测试要求

- 真实硬件运行通过；数据校验全对；报告输出可读。
- 两硬件契约基线（735/115）无回归。

## 验收

- `chat/round14/result4.md`：示例结构、运行输出（含重叠率）、正确性校验证据、回归输出。
- 总指挥复跑示例与两硬件契约；审查是否仅用 public API。

## 后续依赖

- 与 Session 1/2/3 可并行开发；硬件运行错峰。
