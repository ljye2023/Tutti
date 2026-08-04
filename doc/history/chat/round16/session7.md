# TASK — Round 16 Session 7：完整恢复 legacy 逻辑（三）：IO 编排/布局/参数对齐 + 24 GB/s 达成证明

**日期：** 2026-08-03（总指挥签发）
**前置：** S5、S6 落地。这是收口 session。
**maintainer 最高指令：** 完整恢复历史逻辑、不修改原本逻辑；`third_pkgs/` 只读；总指挥不做测试循环，session 自行验证。**性能目标：聚合读 ≥24 GB/s（4 盘 × ~6 GB/s，每盘近饱和）。**

---

## 目标

IO 编排、数据布局、kernel 参数全面对齐 legacy，并用 legacy 本体在同一几何下的实测作为"最优"基准，证明新架构达到（或超过）之。

## 工作项

### 1. 数据布局：默认多盘聚合（对齐 legacy `shard_placement` round-robin）

- simulator **默认启用 4 盘 striped**（`--striped4` 成为默认模式；单盘保留为 `--single` 对照）。
- stripe unit = **chunk/tensor 大小（512KiB）**（KV 条带化设计定案：tensor 完整落单盘、idx%N 轮转；新 striped resolver 公式已核实与 legacy `gpu_file_resolve` 等价）。
- legacy 布局语义对照表（K/V 角色分盘、shard_placement 轮转）写入 result，证明新布局逐项等价。

### 2. IO 编排对齐

- legacy：每层每方向**一次阻塞调用**（submit_batch 内部 stream sync）= 一次 kernel 驱动全部 N 盘、batch 深度 8192；三流 **event DAG** 实现 read(L+1)/compute(L)/write(L-1) 真正同时在飞。
- 新 sim 对齐：每层每方向一次 submit（深度 8192 已由容量参数支撑）；**去掉 windowed 循环里阻碍 read/write 并发的每轮显式 sync**，改为 event DAG（读/写各自 event，跨层依赖用 event wait 表达，非 host sync）。host 往返次数对照（DpSeam 计数）写入 result。

### 3. kernel launch 参数对齐

- `threads_per_block=32`（legacy）vs 新 256：先以 32 为默认（对齐 legacy），附 32/64/128/256 对照数据；fused kernel（striped）同。

### 3b. 构造函数 Config 聚合（maintainer 指定，纯外观零行为）

- `LocalNvmeDataPath` 构造函数 15 个位置参数 → `LocalNvmeDataPath::Config` 聚合结构体（默认值内嵌：`queue_depth=1024`、`cq_poll_budget=0`、capacity 参数等保持现默认）；构造函数改为 `(snvme_dev_path, bar0_size, Config)` 或等价形态。`StripedDataPath`/`DeviceDescriptor` 同构处理。
- 全部调用点（三个契约测试 + simulator + 示例）改用 Config；`TUTTI_TEST_QDEPTH` env 读取并入（若 S6b 已做则对齐其形态）。
- **零行为变更**：默认值逐项与现签名一致；契约全绿为证。

### 4. legacy 本体基准（ground truth）

- 构建 `third_pkgs/Tutti`（独立 build 目录，不污染），同机同几何（80 层/128K ctx/512 chunk/90% hit/4 盘）跑 `kv_cache_layerwise_overlap`，记录 READ/WRITE GB/s 作为"最优"基准。
- 新 sim 同几何对照。**判定标准：新架构 READ ≥ legacy 实测值**；若 legacy 实测 <24 GB/s，以其值为达成线并在 result 说明；若 ≥24，以 24 为线。

### 5. 六维对齐总表（对齐 `doc/review/PERF_REGRESSION_ANALYSIS_20260803.md` §3）

在 result 中给出最终六维表：内存注册/PRP、handle cache、PRP cache、元数据调度、IO 编排、数据布局——每维标注"已恢复等价（证据链接）"。

## 验收（session 自行验证并报告）

1. **READ 带宽 ≥ legacy 同几何实测值（目标 24 GB/s 量级）**，Phase H 26/26，instrumentation 单 launch/层。
2. legacy 基准数据 + 布局/编排/参数三对照表 + 六维对齐总表。
3. 契约一次：全量门禁（842/137/66+新增/15）。
4. IRQ 监控前后差为 0。
5. result：改动清单、全部对照数据（最终运行）、达成判定。

## 硬约束

同 S5/S6。另：**若某项"恢复"与既有公共契约（partial-commit/fail-closed）冲突，以公共契约为准并在 result 显式记录偏差**——抽象层语义不动，核心逻辑按 legacy。
