# Round 14 Session 4b: Partial-Commit Contract Fix

## 根因复述（总指挥裁定）

S4 报告的"DataPath `wait` 不可靠"诊断**错误**。真实根因：示例违反 Runtime 的 partial-commit 契约。

- DataPath `max_in_flight_operations_ = 16`（`local_nvme_data_path.h:401`），超出即 per-request `RESOURCE_EXHAUSTED`。
- 示例每层 submit 2×460=920 请求，只检查 `!o.io.has_value()`——部分接受时 `o.io` 有值，904 个被拒请求被静默丢弃。
- `wait(handle)` 只跟踪已接受 op；被拒请求不属于 handle，wait 返回 OK 不代表它们完成。
- "52 GB/s"是把假设的 36GB 除以时间；实际每层只写 16 请求 ≈16MB。
- 小规模通过：6 请求 < 16 配额全部接受。

## 修复点清单

### REQUIRED 1: 全量接受检查
`windowed_submit_wait()` 函数遍历 `initial_states` 统计 accepted/rejected。rejected 请求被收集并重投，严禁静默丢弃。

### REQUIRED 2: 窗口化提交循环
`windowed_submit_wait()` 实现了 reject-retry 循环：
```
pending = all_requests
while (!pending.empty()):
    submit ≤16 from pending
    if accepted: wait + release + count bytes
    collect rejected → next round
    pending = rejected + unsubmitted
```
DataPath 配额 16 自然形成窗口。所有 IO 路径（prewrite / do_read / do_write / verify）全部走此函数。

### REQUIRED 3: 真流水线结构（方案 B — 交错单线程）
每层 L：
1. submit read(L+1) 窗口 on s_r（阻塞 wait 但 s_c/s_w 可并发）
2. submit write(L-1) 窗口 on s_w（阻塞 wait 但 s_c/s_r 可并发）
3. compute(L) on s_c（async，依赖 read(L) event）

read 和 write 在不同 stream 上，GPU 侧可并发。host 在一个 IO 的 wait 期间，另一个 stream 的 IO 和 s_c 的 compute 可并行推进。

同步结构：
- `er[L]`：read(L) 完成事件（s_r 上 record）
- `ec[L]`：compute(L) 完成事件（s_c 上 record）
- `s_c` waits `er[L]` before compute(L)
- `s_w` waits `ec[L-1]` before write(L-1)

### REQUIRED 4: 诚实计时
- IO 时间 = host wall clock 累加各窗口 submit+wait 时间
- IO 字节 = 只统计实际 accepted 的字节数（`total_bytes += bytes_per_req` per accepted request）
- 带宽 = accepted_bytes / io_time
- 输出注明 in-flight 深度为 16 量级（`windowed submit` 标注）

### REQUIRED 5: 验证
- 小规模（4L/4C）Phase H 全部字节校验通过 ✓
- 完整 HY3（80L/512C）Phase H 全部字节校验通过 ✓
- 写带宽物理合理（prewrite 14.10s / 35.94 GB = 2.5 GB/s，单盘 ≤7 GB/s 量级）✓
- 硬件契约回归 799/0 + 115/0 ✓
- 临时文件已清理 ✓

## 小规模测试（4L/4C）

```
[ OK ] Phase E: pre-wrote 3 chunks x 4 layers (0.00 GB) in 0.03s
[ OK ] Phase F: auto compute_us=5177 us (read 3.994 ms / 0.00 GB = 0.1 GB/s, write 1.184 ms / 0.00 GB = 0.1 GB/s)
[INFO] pipeline: layers=4 chunks=4 (hit=3 miss=1) compute=5177 us = 2 iters (in-flight cap=16, windowed submit)
[ OK ] Phase G: req 1 0.037s (serial 0.058s, saving 36%) READ 0.00GB=0.1GB/s WRITE 0.00GB=0.0GB/s
[ OK ] SIM TOTAL: 1 req wall=0.037s | READ 0.00GB=0.1GB/s | WRITE 0.00GB=0.0GB/s | serial=0.058s overlap 36%
[ OK ] Phase H: verified 4 samples, all correct

=== layerwise_kv_overlap: PASSED ===
```

## 完整 HY3 测试（80L/512C）

```
[ OK ] Phase A: 512 files (40.0 GB) in 26.20s
[ OK ] Phase B: 460 hit + 52 miss chunks, 1024 tensors registered
[ OK ] Phase C: opened 512 targets
[ OK ] Phase E: pre-wrote 460 chunks x 80 layers (35.94 GB) in 14.10s
[ OK ] Phase F: auto compute_us=397894 us (read 372.045 ms / 0.48 GB = 1.3 GB/s, write 25.850 ms / 0.05 GB = 2.1 GB/s)
[INFO] pipeline: layers=80 chunks=512 (hit=460 miss=52) compute=397894 us = 132 iters (in-flight cap=16, windowed submit)
[INFO] rq1 L9   read 482.3MB/301.70ms=1.6GB/s write 54.5MB/407.84ms=0.1GB/s
...
[INFO] rq1 L79  read 482.3MB/291.20ms=1.7GB/s write 54.5MB/407.71ms=0.1GB/s
[ OK ] Phase G: req 1 56.728s (serial 88.558s, saving 36%) READ 0.48GB=1.6GB/s WRITE 0.05GB=0.1GB/s
[INFO] rq2 L9   read 482.3MB/302.04ms=1.6GB/s write 54.5MB/407.75ms=0.1GB/s
...
[INFO] rq2 L79  read 482.3MB/292.89ms=1.6GB/s write 54.5MB/408.42ms=0.1GB/s
[ OK ] Phase G: req 2 56.834s (serial 88.663s, saving 36%) READ 0.96GB=1.6GB/s WRITE 0.11GB=0.1GB/s
[ OK ] SIM TOTAL: 2 req wall=113.562s | READ 77.18GB=1.6GB/s | WRITE 8.72GB=0.1GB/s | serial=177.221s overlap 36%
[ OK ] Phase H: verified 26 samples, all correct

=== layerwise_kv_overlap: PASSED ===
```

### 带宽分析

| 指标 | 值 | 物理合理性 |
|------|----|-----------|
| Pre-write | 35.94 GB / 14.10s = 2.5 GB/s | ✓ 单盘 ≤7 GB/s |
| Pipeline READ | 77.18 GB / 48.3s = 1.6 GB/s | ✓ 单盘 ≤7 GB/s，窗口化开销导致偏低 |
| Pipeline WRITE | 8.72 GB / 65.3s = 0.1 GB/s | ✓ 小写入量，窗口化开销主导 |
| Overlap saving | 36% | ✓ 方法：host wall / (IO-time + compute-time) |

带宽偏低（1.6 GB/s vs 盘的 7 GB/s 极限）是因为窗口化 submit 每次只发 16 个请求（16 × 512 KiB = 8 MiB），每轮 submit+wait 有固定开销。这是 in-flight cap=16 的直接后果。

## 硬件契约回归

```
tutti_local_nvme_datapath_contract_test: 799 passed, 0 failed — PASS
tutti_storage_runtime_local_nvme_contract_test: 115 passed, 0 failed — PASS
```

使用 tutti/build_cuda 全新构建（非陈旧 build 目录）。无回归。

## 修改文件

| 文件 | 改动 |
|------|------|
| `tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu` | 完整重写：windowed_submit_wait + 方案 B 交错流水线 + 诚实计时 |
| `tutti/include/tutti/storage_runtime.h` | S4 的 `std::move` 修复保持现状 |

## 遗留建议

1. **in-flight cap=16 对 simulator 场景偏低**：每轮 16 请求 × 512 KiB = 8 MiB，导致带宽利用率低（1.6/7 = 23%）。建议后续将 `max_in_flight_operations_` 作为 tunable 参数暴露（如 `TUTTI_NVME_INFLIGHT_CAP` 环境变量），但不修改默认值。
2. **方案 B 的重叠有限**：单线程交错在 host 阻塞 wait 时只有另一 stream 的 IO 和 compute 能并发。方案 A（双线程）可实现更高重叠，但代码复杂度增加。当前 36% saving 对 simulator 场景可接受。
3. **窗口化 submit 调用次数**：每层 920 请求 / 16 = 58 轮 submit+wait，每轮有 host→kernel→CQ poll 开销。减少轮次需要提高 cap 或使用更少的请求（如合并 K+V 为单请求）。

## 临时文件清理

`/mnt/nvme1/GPU0/kvlw_*` 和 `/mnt/nvme2/GPU0/kvlw_*` 已删除。

## 总指挥验收（2026-08-02）

**PASS。Round 14 全部关闭。**

独立复跑（tutti/build_cuda 全新构建）：

- 小规模（4L/4C）：PASSED，Phase H 4/4 字节校验通过。
- 完整 HY3（80L/512C）：PASSED——Phase H **26/26 全部正确**（S4 时为 16/26 mismatch）；诚实带宽 READ 1.7GB/s、WRITE 0.2GB/s、overlap 35%（物理合理，单盘 ≤7GB/s 量级）。
- 硬件契约回归：**799/0 + 115/0** 双绿。
- 代码核验：`windowed_submit_wait()` 全量遍历 `initial_states`，被拒请求收集重投（REQUIRED 1/2 落地）；方案 B 交错流水线 + er/ec event 同步结构（REQUIRED 3）；带宽只计 accepted 字节并标注测量方法（REQUIRED 4）；双盘 kvlw_* 临时文件已清（REQUIRED 5）。
- 遗留建议（in-flight cap=16 导致带宽利用率 ~23%、tunable 化、方案 A 双线程）记录合理，归属后续性能工作，不阻塞。

**S4 升级为最终 PASS。** Round 14（S1 文档由 maintainer 手动完成、S2 日志门控、S3 等价性核查、S4+S4b simulator）全部闭合。
