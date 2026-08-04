# TASK — Round 20 Session 1：config/ 目录 + 多 GPU 接线

**日期：** 2026-08-03（预生成，启动前总指挥复核）
**前置依赖：** 无硬依赖（新文件为主）；daemon 默认配置路径切换需与 R17（daemon 生命周期）协调——若 R17 未启动，本 session 只做兼容期（双路径都认，旧路径 deprecation 警告）。
**模式：** 可与 S4 并行（纯主机侧新文件，不碰 daemon/kernel 运行时）。

---

## 工作项

### 1. config/ 目录
- `config/sys_config.yaml`：从仓库根迁入（daemon 配置）。daemon 查找顺序：`--config` 显式参数 > `config/sys_config.yaml` > `./sys_config.yaml`（旧路径保留兼容 + deprecation 警告）。
- `config/tutti_config.yaml`：用户配置。键位至少覆盖：
  - GPU 厂商、存储后端（local-nvme / RDMA 占位）、默认 stripe unit；
  - **DataPath 缓存旋钮**（maintainer 2026-08-03 指定）：`local_nvme.handle_cache_capacity`、`local_nvme.prp_cache_capacity`（默认均 0=OFF，KV 型负载建议显式开启；这是 GPU file/PRP 复用机制的用户入口）；
  - **容量旋钮**：`max_in_flight_operations`、`max_batch_entries`、`num_user_queues`（默认保持现状 16/256/16 不动）。**注意：`queue_depth` 不是键位**——R16 S6b 起 DataPath 构造参数已删除该字段，建队自动跟随内核 `ctrl->q_depth`；深度由内核模块参数 `io_queue_depth` 控制（生产 1024），属部署/sys_config 文档范畴，不进 tutti_config；
  - **IO 形状声明**：KV 型负载的默认 `io_granularity`（R16 S5 注册期预构建的声明入口；默认 0=动态路径）。
- **tutti_config loader**：一个小库把 yaml 翻译为 `RuntimeComponents`（含 DataPath 构造参数）再调现有 `StorageRuntime::create()`。**程序化注入仍是唯一事实源**——优先级：程序化注入 > 配置文件 > 内置默认；`TUTTI_HANDLE_CACHE_CAP`/`TUTTI_PRP_CACHE_CAP` 两个 env 定位为测试专用后门（loader 传显式值时 env 不生效），文档写清。RDMA 键先占位（解析接受、明确报 unimplemented），不承诺实现。
- sys_config 现有 4 盘条目的 S3 遗留注释复制错误顺手修正（若 R17 未先做）。

### 2. 多 GPU 接线（S3 留的 seam 启用）
- sys_config 各 nvme 条目 `allowed_gpus` 按拓扑扩展（NVMe_i ↔ GPU_i，拓扑：GPU0↔08:00.0、GPU1↔4b:00.0、GPU2↔57:00.0、GPU3↔63:00.0；注意 S3 加的两条目现为 `[0]`）。
- daemon 重启后：`TUTTI_TEST_GPU=1/2/3` 跑契约冒烟（各 GPU 至少跑通 datapath 契约的 device-open 子集），simulator 的 `kGpuNvmeMap[]` 实际启用验证。
- 4 对 GPU↔NVMe 同 PCIe switch 的拓扑验证写入 result（`nvidia-smi topo -m` 证据）。

### 3. 文档
- README 配置章节更新（两配置文件职责：daemon 的 sys_config vs 应用的 tutti_config；两构建入口按 maintainer 已定的 daemon-only + 两用途写清楚）。

## 验收

1. loader 单测：程序化 > 配置 > 默认的优先级链、RDMA 占位 unimplemented 报错、坏 yaml fail-closed。
2. 双路径兼容：旧路径启动有 deprecation 警告，新路径正常。
3. 多 GPU：GPU 1-3 各跑通一次 datapath 契约冒烟（`TUTTI_TEST_GPU=i`），拓扑证据入 result。
4. 回归一次：842/137/66 + 非硬件 15。
5. result：改动清单 + 验证证据。

## 硬约束

- O_DIRECT 政策；防缠结（改动清单）。
- tutti_config 不直接驱动 DataPath 创建（只经 RuntimeComponents 注入）——防止"配置与代码两套真源"。
- 启动前总指挥复核：R17 状态（daemon 路径切换的归属）、S4 是否已改 simulator 配置入口。
