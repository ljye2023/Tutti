# TASK T-073b — Round 14 Session 4b：修复 layerwise KV overlap simulator 的 partial-commit 契约违反

## 背景（总指挥根因裁定，推翻 result4.md 的 wait 语义假设）

S4 报告的"DataPath `wait` 在大批量场景下不可靠"诊断**错误**。真实根因：示例违反 Runtime 的 partial-commit 契约。

证据链：
1. DataPath in-flight 配额 = 16（`local_nvme_data_path.h:401`），超出即 per-request `RESOURCE_EXHAUSTED`——Round 11 验收过的有界池背压。
2. 示例每层 submit 2×460=920 请求（`layerwise_kv_overlap.cu:208`），只检查 `!o.io.has_value()`（:209）——部分接受时 `o.io` 有值，904 个被拒请求被静默丢弃。
3. `wait(handle)` 只跟踪**已接受**的 op（Round 11 S3 fence 语义）；被拒请求不属于 handle，wait 返回 OK 不代表它们完成。
4. "52 GB/s 写入"是把假设的 36GB 除以时间；实际每层只写 16 请求 ≈16MB（≈1.8GB/s，物理合理）。
5. 小规模通过：6 请求 < 16 配额全部接受。

result4.md 四个"需要确认"问题的答案（不要再重新调查）：
1. `wait` 语义：等 IoHandle 内全部**已接受** op 到达 terminal（device fence event）。
2. `wait` 无内部 `cudaStreamSynchronize`（Round 11 S3 设计；唯一同步点是 event-record 失败回退路径）。
3. 大批量 submit：Runtime 按 (DataPath,target) 分组下发；DataPath 每次调用最多接受配额内请求，其余 per-request 拒绝并写入 `initial_states`。
4. `wait` 覆盖 handle 内全部 op；被拒请求 terminal 于 submit 时刻，状态在 `initial_states`。

## 修复要求（只允许改 `tutti/examples/layerwise_kv_overlap/` 下文件，生产代码零改动）

### REQUIRED 1：全量接受检查
每次 submit 后遍历 `initial_states` 统计 accepted/rejected。rejected 是背压信号而非致命错误，但**必须被处理**，严禁静默丢弃。

### REQUIRED 2：窗口化提交循环
do_read / do_write / Phase E pre-write / Phase H 验证读 全部改为窗口化循环：

```cpp
remaining = all_requests;
while (!remaining.empty()) {
    auto o = rt->submit(remaining.data(), remaining.size(), ctx);
    if (o.io.has_value()) { rt->wait(o.io.value(), timeout); rt->release_io(o.io.value()); }
    remaining = collect_rejected(o.initial_states, remaining);  // 下一轮重投
}
```

（配额 16 自然形成窗口；也可显式按 ≤16 切片以减少 submit 调用次数。）

### REQUIRED 3：真流水线结构
当前单 host 线程 + 阻塞 wait 无法让 read(L+1) 与 write(L-1) 真正重叠（S4 的 43% 数字因此无效）。二选一：

- **方案 A（推荐）**：读 host 线程（s_r）+ 写 host 线程（s_w）+ 主线程 compute，层间用 event/条件变量同步；
- **方案 B**：单线程交错——每轮先 submit 读窗口再 submit 写窗口（两路 IO 已在各自 stream 上并发），再依次 wait 两个 handle。

任一时刻读、写、compute 三者必须能真正并发推进，并在 result 中说明所选方案与同步结构。

### REQUIRED 4：诚实计时
- 带宽只统计实际 accepted 且完成的字节；
- IO 时间用 `cudaEventElapsedTime` 包裹真实 GPU 工作（或累加各窗口 wait 时间）；
- 输出注明窗口化后的实际 in-flight 深度（应为 16 量级，不是 920）。

### REQUIRED 5：验证
- 小规模（4L/4C）与完整 HY3（80L/512C）Phase H 全部字节校验通过；
- 完整规模下写带宽必须物理合理（单盘 ≤7GB/s 量级）；
- overlap saving 百分比重新计算并标注测量方法；
- 硬件契约回归 **799/0 + 115/0** 不劣化（二进制用全新构建，不要用陈旧 build 目录）；
- 运行后清理 `/mnt/nvme1` 与 `/mnt/nvme2` 下的测试临时文件。

## 边界

- 若发现配额 16 对 simulator 场景确实过低，**不得**在本 session 调整生产默认值——记录为后续 tunable 建议写入 result。
- 不得改动 `local_nvme_data_path` / `storage_runtime` / 任何生产 header（S4 的 header 改动已由总指挥在验收时修正，保持现状）。
- 禁止 insmod/rmmod/daemon 操作。

## 结果文件

`chat/round14/result4b.md`：根因理解复述、修复点清单、小规模+完整规模输出（含真实带宽与 overlap）、回归证据、遗留建议。
