# TASK T-083 — Round 15（重做）Session 4：批次容量参数化 + simulator 复现 legacy 效果

> **先读 `chat/round15/BASELINE.md`**。前置：Session 3（分组）已验收。目标效果对照 `third_pkgs/Tutti/examples/adapters/kv_cache_layerwise_overlap.cu`：每层 IO 一次 submit → 一次 kernel launch，kernel 以 QD 背压流式消费全部 entries，IO 带宽接近单盘极限。

## REQUIRED 1：批次容量参数化

- `LocalNvmeDataPath` 构造函数增加可配参数（带默认值，**默认值与当前行为完全一致**：in-flight=16、batch_entries=256、batch_requests=跟随 entries、request_bytes=entries×MDTS）：
  - `max_in_flight_operations`、`max_batch_requests`、`max_request_bytes_override`（max_batch_entries 已有参数）。
- arena 槽位 = 2 × in-flight，每槽 entries 缓冲随 batch_entries 定容；在构造函数注释中写明内存账（槽数 × 每槽字节）。
- 拒绝语义不变：超出容量 per-request `RESOURCE_EXHAUSTED`，fail-closed。

## REQUIRED 2：单 launch 大批次硬件测试（编号 82 起）

- DataPath 配置大容量（如 in-flight=8、batch_entries≥4096），一次 `rt->submit` 携带 ≥512 请求跨 ≥64 文件：
  - 断言 DataPath `submit` 调用次数==1（test seam 计数）、kernel launch==1；
  - 逐文件逐字节校验（位置相关 pattern，复用 test 76 手法）；
  - 默认容量实例回归：超出默认容量的同形状 batch 仍按 per-request `RESOURCE_EXHAUSTED` 拒绝（fail-closed 证据）。

## REQUIRED 3：simulator 复现 legacy 效果

- `tutti/examples/layerwise_kv_overlap/`：每层每方向**一次 `rt->submit`**（`windowed_submit_wait` 保留为拒收安全网，正常路径一轮完成）；示例自建 DataPath 配置大容量（in-flight≥4、batch_entries≥4096）；删除 cap=16 时代的过时注释。
- 完整 HY3（80L/512C）验收：
  - 每层每方向 DataPath submit==1、kernel launch==1（instrumentation 证据）；
  - Phase H 26/26 字节校验通过；
  - 读带宽 ≥5GB/s（达不到如实分析，禁止虚报）；
  - overlap saving 重测并标注方法；
  - 清理双盘 `kvlw_*`。
- 对比表：修复前（窗口化/464 launches/1.7GB/s）vs 修复后（1 launch/层/实测）。

## REQUIRED 4：回归

799(+新增)/0 + 115/0 + HOST/CUDA 非硬件 ctest 全绿；`git diff --check` clean；双盘 resolver_test 目录空。

## 边界

- 只准改：`local_nvme_data_path.{h,cpp}`（容量参数化）、契约测试文件、simulator 目录、必要注释。
- **禁止**：改任何默认值数值（16/256 保持）；动 `storage_runtime.h`（S3 已验收，如需微调先报告）；StripedDataPath（S5 才建）。
- 禁止 insmod/rmmod/daemon 操作。

## 结果文件

`chat/round15/result4.md`：容量 diff 摘要 + 内存账、单 launch 测试输出、simulator 前后对比、回归证据、**改动文件清单**。
