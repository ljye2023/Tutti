# TASK T-088 — Round 16 Session 3：测试 harness 升级（4 盘 + 16 队列 + 实测性能输出）

> 前置：R16 S1（P0 修复）已验收。环境已就绪：4 盘挂载（`/mnt/nvme1`-`/mnt/nvme4` ↔ `/dev/ssnvme0`-`/dev/ssnvme3`，daemon 已按 4 条目 sys_config 启动）。**本 session 只改测试与示例，不改生产代码。**

## REQUIRED 1：队列数 2 → 16

- 全部硬件测试与示例的 DataPath 构造：`num_user_queues` 统一改为 **16**（`tests/local_nvme_datapath_contract/` 的 `kNumQueues`、`tests/storage_runtime_local_nvme_contract/`、`tests/striped_local_nvme_contract/`、`tutti/examples/layerwise_kv_overlap/`）。queue_depth 保持 64。
- daemon 约束核对：单组上限 16（`NVM_MAX_QUEUES_PER_GROUP=16`）、`queue_pool.default_per_client=16`——16 队列单组可达，无需 daemon 变更；在 result 中记录核对结论。
- 注意测试 70（arena exhaustion）等依赖 in-flight 配额的场景：配额语义与队列数无关，不应受影响；若有断言隐含队列数假设，修正并说明。

## REQUIRED 2：4 盘测试

- **多设备测试（78-81）**：设备数从 2 扩展为 **4**（挂载点 `/mnt/nvme1`-`/mnt/nvme4` ↔ `/dev/ssnvme0`-`/dev/ssnvme3`，建议常量化集中定义）。79（跨设备 batch）覆盖 4 设备混合分组；80（双 stream）改为至少 2 设备并发（保持流语义验证）；81（故障隔离）至少 3 设备（1 坏 2 好）。
- **striped 契约（82-90）**：新增 **N=4** 场景（编号续排 91+）：roundtrip + round-robin 分布验证（4 个 backing 文件）+ 单 launch 计数；既有 N=2 场景保留。
- 环境自检：测试开头检查 4 个 `/dev/ssnvme{0-3}` 可打开、4 个挂载点可写，不满足则明确报 environment 错误退出（沿用既有 fail-closed 风格）。

## REQUIRED 3：实测性能输出

- IO 密集型场景输出实测带宽，格式统一：`[perf] <场景> <bytes> <elapsed_ms> <GB/s>`：
  - datapath 契约：test 8/86（单 launch 大批次）、大批量读写场景；
  - striped 契约：N=2/N=4 roundtrip 与跨盘并行（单盘 vs N 盘对比 + 加速比）；
  - simulator 已有 perf 输出，核对格式一致即可。
- 计时用 submit→wait 墙钟（chrono）或 cudaEventElapsedTime 包裹真实 GPU 工作；**禁止打印假数字**（必须来自真实 DMA 完成后的时间）。性能数字只作展示，不设硬阈值断言（避免噪声 flaky），但跨盘加速比断言保留（>1.3×）。

## REQUIRED 4：GPU 选择参数化

- 测试 cuda device 从硬编码 0 改为 env `TUTTI_TEST_GPU`（默认 0）；全部硬件测试与 simulator 统一。
- 多 GPU（GPU_i ↔ device_i 一一对应，拓扑 06:00/49:00/56:00/62:00 ↔ 08:00/4b:00/57:00/63:00）**本 session 只留 seam**（device→GPU 映射表参数化）；要实际跑 GPU 1-3 需 sys_config `allowed_gpus` 扩展 + daemon 重启——记录为 operator 待办，不在本 session 验证。

## 回归

- 全量：datapath（含 78-81 四盘版）、runtime、striped（含 91+ N=4）、非硬件 ctest——全绿；
- `git diff --check` clean；4 盘测试目录清空；
- result 中附一轮完整性能输出样本（每场景 GB/s）。

## 结果文件

`doc/history/chat/round16/result3.md`：改动清单（队列数/设备数/性能输出/GPU seam）、4 盘+16 队列全量验证证据、性能输出样本、多 GPU operator 待办、**改动文件清单**。
