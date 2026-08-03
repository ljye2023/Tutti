> **状态：已暂缓（2026-08-01，maintainer 决定）。** 当前版本不实施；kernel strategy 基准与替代策略实验待后续版本再启动。本文件保留作为未来 prompt 基线。

# TASK T-053 — Round 11 Session 4：kernel strategy 基准测试

## 前置条件

- Session 1-3 完成（arena/cache/异步均稳定）；阅读 `Roadmap.md` Phase 4 gate 的测量要求：「GPU warp/SM occupancy, metadata H2D, doorbell/atomic/CQ scan, IOPS/BW, p99, and compute overlap measured」。
- 当前 kernel 形状：one-thread-per-entry、每 entry 独立 doorbell+CQ bounded poll（`submit_one.cu/.cuh`）。

## 目标

建立可重复的硬件基准 harness，量化当前策略并对比至少一种替代策略（如 warp-per-QP 或 doorbell batching），产出采用/不采用的决策依据。不改 public API；替代策略仅作为 harness 内实验，默认不替换生产 kernel。

## 允许修改/创建

- `tests/local_nvme_bench/**`（新建，hardware label）
- `tutti/data_paths/local_nvme/io/`（仅当实验 kernel 需要与生产 kernel 共存时，以独立 `.cu` 存在，生产路径不引用）
- `chat/round11/result4.md`

## 禁止范围

- 不改 public/SPI；不改变生产 kernel 默认行为（替代策略实验隔离在 bench harness）。
- 不做耐久/满载压测（bench 时长受控，单场景 ≤60s）；不执行模块/daemon/mount 操作。
- 不提交 Git。

## 必须实现的行为

1. 基准 harness：可配置 {IO 大小（4K/64K/1M）、batch 深度、并发 stream 数、读写比}；预热 + 多轮取样；输出 IOPS、带宽、p50/p99 延迟。
2. 分解测量：metadata H2D 时间（arena 后应≈0）、doorbell 写次数/成本、CQ poll 迭代分布、kernel occupancy（`cudaOccupancyMaxActiveBlocksPerMultiprocessor` 或 nsys 摘要）。
3. compute overlap：同 stream 与跨 stream 下 IO 与 compute kernel 的重叠度（IO 期间 SM 是否空转）。
4. 至少一种替代策略实验（warp-per-QP 或 doorbell batching 或 split submit/completion 三选一）：同 harness 对比，输出相对当前策略的倍数。
5. 决策记录：采用/不采用替代策略，依据（收益 vs 复杂度 vs 风险）写入 result；若收益 ≥2× 且语义等价，给出后续采用 plan（不在本 session 实施）。

## 测试要求

- harness 自身正确性：小样本 run 与契约测试数据一致（一次 4K WRITE/READ roundtrip 校验）。
- 结果可复现：连续两次 run 关键指标偏差 <20%（注明环境变量）。
- 既有 616+115 断言无回归（bench 为独立 target，不影响契约）。

## 验收

- `chat/round11/result4.md`：harness 设计、完整测量数据表（含环境：GPU 型号/队列深度/块大小）、分解成本、对比实验、决策与理由。
- 总指挥复核数据合理性（数量级、与既有实测 16-22 GiB/s 经验值对照）并重跑一次关键场景。
