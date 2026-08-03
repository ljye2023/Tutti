# TASK T-077b — Round 15 Session 2b：跨 target 合并提交 + 大批次容量（复现 legacy 单 launch 效果）

> ## 进度基线（2026-08-03 总指挥核定——已落地，勿重复、勿回滚）
>
> 1. **REQUIRED 1 已完成**：`storage_runtime.h:1028` 起 PendingGroup 已按 DataPath 分组（注释 "groups requests by DataPath (not by target)"）。验收：`tutti_storage_runtime_local_nvme_contract_test` 115/0、`tutti_local_nvme_datapath_contract_test` 799/0 均在此改动下通过。
> 2. **REQUIRED 2 进行中**：`local_nvme_data_path.h:137` 构造函数已新增 `max_in_flight_operations` 参数（默认 0=512）。继续完成其余容量参数化与 arena 内存账。
> 3. **测试编号（当前真实值，旧文档中的 72-75 已作废）**：arena=70-73、defense=76/77、**多设备=78-81**；不存在 74/75。验证多设备用 `grep -E '^--- (7[8-9]|8[01])\.'`。
> 4. **当前 quota/arena**：`max_in_flight_operations_` 默认 512（maintainer 认可，不得改小），arena = 2×in-flight 槽。
> 5. **构建目录**：`build/r15s3`（含 S3 合入的共享头/StripedDataPath）。二进制自带 rpath，**不需要** LD_LIBRARY_PATH。
> 6. **禁止**重复调查已核实事实（见下）；禁止再跑 120s 超时全量契约作为常规验证——增量编译后用 `-R` 跑目标测试即可。
>
> 剩余工作 = REQUIRED 2 收尾 + REQUIRED 3 + REQUIRED 4（见下）。

## 目标效果（maintainer 定义，对照 `third_pkgs/Tutti/examples/adapters/kv_cache_layerwise_overlap.cu`）

simulator 每层 IO：**一次 `rt->submit`（整层 920 请求/460 文件）→ 一次 `DataPath::submit` → 一次 kernel launch**，kernel 以 NVMe 队列背压流式消费全部 entries（legacy `submit_batch` 效果）；IO 带宽接近单盘物理极限（≥5GB/s），IO/compute 经 stream/event 图重叠。新架构允许 read/write 也并发（legacy 因共享 scratch 不能），允许超越 legacy。

## 已核实事实（不要重复调查）

- `LocalNvmeDataPath::submit` 数据面已 per-entry 多 target 就绪：`local_nvme_data_path.cpp:965`（每请求 `find_`）、`:1040`（`entry.target = tstate->dev_handle`）、`submit_one.cuh:124/127`（kernel 按 `e.target` resolve）。
- 原唯一串行化点（`storage_runtime.h` PendingGroup 按 `(data_path, target)` 分组）**已由本 session 修复**（现按 DataPath 分组，见进度基线 1）。
- 批次容量限制：`max_batch_entries_`（默认 256）、`max_batch_requests_`、`max_request_bytes_`、`max_in_flight_operations_`（现默认 512，maintainer 认可，不得改小）。
- arena 容量 = 2 × max_in_flight，每 slot entries 缓冲随 max_batch_entries 定容——配置联动必须算清内存账。

## REQUIRED 1：Runtime 分组改为按 DataPath —— ✅ 已完成（见进度基线 1，勿重复）

剩余仅需：SPI 契约注释明确"submit 请求数组可跨 target"（DataPath SPI 头文件），并对 MockDataPath/memfs 做一次单 target 假设走查（预计零改动，结论写入 result）。

## REQUIRED 2：批次容量参数化

- `max_in_flight_operations`、`max_batch_entries`、`max_batch_requests`、单请求字节上限全部改为构造可配（现有构造函数参数保留，缺省值保持当前默认）；arena 槽位数与每槽 entries 缓冲随配置计算，注释写明内存账（例：in-flight=8 × 2 槽 × entries=4096 × entry 大小）。
- 大 batch 时 kernel 一 thread 一 entry 自然形成 QD 背压流式消费（现有 acquire_queue/poll_bounded 语义），确认无额外 host 侧分批。
- 配额拒绝语义不变（超出仍 per-request `RESOURCE_EXHAUSTED`），fail-closed。

## REQUIRED 3：契约测试

- **合并计数（mock）**：一次 `rt->submit` 跨 K 个 target（K≤配额）→ `DataPath::submit` 恰好 1 次，且全部请求在同一调用数组中。
- **单 launch 大批次（硬件）**：一次 `rt->submit` 携带 ≥512 请求跨 ≥64 文件（DataPath 配置大容量），断言 DataPath submit 调用次数==1、kernel launch==1，逐文件逐字节校验（位置相关 pattern）。
- **既有零回归**：799+115 全绿；HOST/CUDA 非硬件 ctest 全绿。

## REQUIRED 4：simulator 复现 legacy 效果

- 示例改为每层每方向**一次 `rt->submit`**（`windowed_submit_wait` 保留为拒收安全网，正常路径一轮完成）；示例自建 DataPath 时配置大容量（如 in-flight≥4、batch_entries≥4096），注释更新（删掉 cap=16 的过时叙述）。
- 完整 HY3（80L/512C）验收标准：
  - 每层每方向 DataPath submit==1、kernel launch==1（instrumentation 证据）；
  - Phase H 26/26 字节校验通过；
  - 读带宽 ≥5GB/s（单盘；达不到则如实分析，禁止虚报）；
  - overlap saving 重新测量并标注方法；
  - 清理双盘 `kvlw_*`。
- 对比表：修复前（窗口化/464 launches/1.7GB/s）vs 修复后（1 launch/层/实测带宽）。

## 边界

- 禁止改 public API、SPI 签名、默认配额值（512 保持）。
- 本 session 不动 StripedDataPath（S3 已另行实现单 launch；其 arena/cache 简化项后续收敛）。
- 禁止 insmod/rmmod/daemon 操作；硬件用既有双盘环境。

## 结果文件

`chat/round15/result2b.md`：分组与容量 diff 摘要、SPI 注释、走查结论、合并计数与单 launch 测试输出、simulator 前后对比（launch 数/带宽/overlap/Phase H）、回归证据、arena 内存账。
