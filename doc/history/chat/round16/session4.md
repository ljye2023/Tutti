# TASK — Round 16 Session 4：4 盘 striped HY3 KV cache 加载模拟 + 性能根因

**日期：** 2026-08-03（总指挥签发）
**性质：** 性能专项（maintainer 最高优先级）。不是回归任务——**不要一遍遍跑契约套件**，只在最后做一次门禁。
**模式：** 单机独占。构建目录 `build/r15base`。

---

## 目标（maintainer 原话转述）

做一个和 `third_pkgs/Tutti/examples/adapters/kv_cache_layerwise_overlap.cu`（legacy）一样的测试：**4 块盘、模拟 HY3 KV cache 加载、使用最上层接口**。性能达不到就找原因。

具体验收线：

1. `tutti_layerwise_kv_overlap` 新增 **striped 4 盘模式**（如 `--striped4`）：每层 K-cache/V-cache 各为**一个 striped 文件**（`striped://layerN_K?devs=0000:08:00.0,0000:4b:00.0,0000:57:00.0,0000:63:00.0&unit=...`，backing 文件一盘一个，经 `create_backing_file` 预建），512 chunk = 文件内偏移。**只走最上层 public API**：`rt->open(uri)` → `rt->submit` → `rt->wait`（test 87 已证 public 路径零 striped 感知）。每层的读 = 2 次 submit（K 一次、V 一次；StripedDataPath 当前单 submit 只接受单 striped target——P2-2 多 target batch 不在本 session 范围）。
2. **性能目标：聚合读带宽 ≥ 15 GB/s**（单盘实测 6.9 GB/s，4 盘理想 ~27 GB/s；≥15 为"明显打满"线，≥20 为优秀）。Phase H 字节校验 26/26 必须保持（正确性不可 trade）。
3. **达不到就根因定位**，定位后修复（允许改 StripedDataPath/kernel 生产代码——这正是本 session 的核心产出之一），修复后重测到达标。

## 已知的性能线索（起点，勿重复测量）

总指挥在 S3 验收时实测（64MiB striped READ，steady_clock 包裹 submit→wait）：

| N | 带宽 | 备注 |
|---|---|---|
| 1（LocalNvme） | 6.9 GB/s | simulator HY3 全量，盘饱和 |
| 1（striped N=1） | 5.10 GB/s | 64MiB 小样本 |
| 2 | 7.30 GB/s | 1.43× |
| 4 | **2.35 GB/s** | 非线性塌陷，比单盘还慢 |

**首要嫌疑（按优先级排查）：**

1. **fused kernel 的 entry→queue 映射与 SQ/CQ 共享语义**：1 thread = 1 entry，64MiB/64KiB = 1024 entries → 每设备 256 threads 打 16 队列 = 16 threads/queue 并发写同一 SQ、消费同一 CQ。单盘同模型能跑 6.9，但跨 4 设备时 CQ phase/门铃时序是否仍正确？查 `striped_local_nvme/` fused kernel 的 qid 分配与 `poll_bounded` 的完成消费。
2. **跨设备 doorbell/CQ 的 BAR 访问路径**：4 块盘 BAR 经 daemon 映射进 GPU，P2P 写 4 个不同 BAR 是否有路径退化（nsys 看 kernel 内 stall）。
3. **StripedArena/PRP**：16MiB/shard 走 PRP LIST 路径，per-submit 是否有隐藏的 per-entry H2D 或重建。
4. **host 侧序列化**：workspace H2D、event、submit_wait_all 的 window 循环在 striped 下是否被放大。

**方法学**：nsys 看 kernel 墙钟 vs host 墙钟占比；kernel 内 `clock64()` 直方图（issue 段/poll 段）；固定 per-shard 16MiB 扫 N=1/2/4 隔离每盘效率；必要时在 kernel 加 per-device 命令计数 debug 输出（临时手段，收尾删除）。队列深度（现 64）与队列数（现 16）是可调旋钮（构造参数/daemon 配置），但**先定位再调参**，禁止用调参掩盖根因。

## 硬约束

- **公共 API 语义零改动**；partial-commit 契约保持（被拒请求窗口化重投，simulator 已有 windowed_submit_wait）。
- **O_DIRECT 政策**：新增/触碰的一切 host 文件 open 必须 O_DIRECT + 4096 对齐 buffer。
- perf 输出统一 `[perf] 场景 bytes elapsed_ms GB/s`，`bytes/ms/1e6` 公式，计时只取真实 DMA 完成后；result 的 perf 样本必须来自**最终验证运行**（S3 报告贴旧样本的教训）。
- simulator 原有单盘模式保持可用（它是对照基线）；`--striped4` 为新增模式，不破坏默认行为。
- 临时 debug 手段（kernel 计数、额外 printf）收尾必须摘除。
- 环境注意：snvme0 admin IRQ 曾中风（IRQ 83 disable），若遇建队异常告知 operator reload 模块，不要在代码里绕。
- 测试文件变更清单写入 result（防缠结规则）；新测试编号从 95 起（如需要）。

## 验收

1. `--striped4` HY3 全量（80 层 × 512 chunk × K/V × 512KiB）：Phase H 26/26、聚合读带宽实测值（目标 ≥15 GB/s）、每层 2 submit/2 launch（instrumentation 计数佐证）。
2. 根因报告：瓶颈定位证据链（数据→结论），修复内容，修复前后带宽对比。
3. 门禁一次：三个硬件契约（842/137/66）+ 非硬件 15/15（最终态跑一遍即可）。
4. result：`doc/history/chat/round16/result4.md`，含改动清单、根因、前后带宽、perf 样本（最终运行）。
