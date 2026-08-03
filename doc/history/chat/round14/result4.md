# Round 14 Session 4 Result: Layerwise KV-cache Overlap Simulator

## 状态：未通过（DataPath `wait` 语义问题待查）

示例已编译通过并在真实硬件上运行，3-stream pipeline 成功执行（43% overlap，无内核崩溃），但数据校验失败。根因是 DataPath `wait` 在大批量场景下可能没有真正等待 NVMe DMA 完成，导致 pre-write 数据未落盘、read 计时虚短。

## 已完成

### 示例结构

- `tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu` — 完整实现
- `tutti/examples/layerwise_kv_overlap/CMakeLists.txt` — CMake 接线
- `tutti/CMakeLists.txt` — 一行注册（hardware label，TUTTI_BUILD_HARDWARE_TESTS + TUTTI_FEATURE_LOCAL_NVME 门控）
- `tutti/include/tutti/storage_runtime.h` — `std::move` 修复（nvcc move-semantics workaround）

### Public API 使用

所有数据面操作仅用 public StorageRuntime API：`create / register_memory / open / submit / wait / release_io / close / unregister_memory / shutdown`。私有 `LocalNvmeDataPath` + `LocalFileResolver` 仅作为 `RuntimeComponents` 注入。

### 内存架构（匹配 legacy Coordinator）

| 方面 | Legacy | 本示例 |
|------|--------|--------|
| GPU 张量 | per-chunk K/V，各 `tensor_size`（512 KiB），`coord.register_tensor` 注册 | per-chunk K/V，各 `tensor_size`，`rt->register_memory(DEVICE)` 注册 |
| BAR1 窗口 | L1 cache 512 MiB budget，LRU 淘汰 | 每个 512 KiB 张量独立注册（128 页 DMA 映射），总 512 MiB |
| IO 方式 | `KvCacheIoAdapter::batched_read/write`，内部用 `d_scratch_` entry buffer | 批量 `rt->submit`（2×n_hit 请求一次提交），NVMe DMA 直接到/从张量 |
| GPU buffer fill | `cudaMemsetAsync`（可见因为 adapter 内部先 copy 到 scratch） | `launch_fill_pattern`（`__threadfence_system`，DMA-visible） |
| 批量提交 | 一次 `batched_read` 内部 fan-out | 一次 `rt->submit` 发一次 doorbell |

### 经历的 3 个版本

1. **40 GB 单缓冲区** — `register_memory(DEVICE)` 注册整个 40 GB → `nvidia_p2p_dma_map_pages` 对整个 GPU BAR 调 `dma_map_resource` → BAR 页有 `struct page` → 内核拒绝 → **僵尸进程 + 内核不稳定**
2. **512 MiB scratch + 逐 chunk submit** — 单 entry scratch buffer 逐 chunk submit+wait → 460×2×80=73600 次 doorbell → **snvme 中断风暴** → `irq 98: nobody cared` → IRQ 禁用 → **内核崩溃**
3. **逐 chunk 注册 + 批量 submit（当前版本）** — 1024 个 512 KiB 张量独立注册，一层一次 `rt->submit` → 无内核崩溃，pipeline 成功运行，但 **pre-write 数据未落盘**

### 小规模测试（4 layers, 4 chunks）— 通过

```
[ OK ] Phase E: pre-wrote 3 chunks x 4 layers (6.00 MB) in 1.27s
[ OK ] Phase F: auto compute_us=5434 us (read 2.161 + write 1.259 ms)
[INFO] pipeline: layers=4 chunks=4 (hit=3 miss=1) compute=5434 us = 2 iters
[ OK ] Phase G: req 1 0.025s (serial 0.050s, saving 50%)
[ OK ] SIM TOTAL: 1 req wall=0.025s READ 0.01GB=0.2GB/s WRITE 0.00GB=0.1GB/s serial=0.050s overlap 50%
[ OK ] Phase H: verified 4 samples, all correct

=== layerwise_kv_overlap: PASSED ===
```

### 完整 HY3 测试（80 layers, 512 chunks）— pipeline 成功但验证失败

```
[ OK ] Phase A: 512 files (40.0 GB) in 26.25s
[ OK ] Phase B: 460 hit + 52 miss chunks, 1024 tensors registered
[ OK ] Phase C: opened 512 targets
[ OK ] Phase E: pre-wrote 460 chunks x 80 layers (35.94 GB) in 0.69s    ← 见下文问题
[ OK ] Phase F: auto compute_us=28134 us (read 19.665 + write 8.469 ms)
[INFO] pipeline: layers=80 chunks=512 (hit=460 miss=52) compute=28134 us = 9 iters
[INFO] rq1 L9   read 482.3MB/9.16ms=52.6GB/s  write 54.5MB/28.23ms=1.9GB/s
...
[ OK ] Phase G: req 1 3.014s (serial 5.260s, saving 43%)
[ OK ] Phase G: req 2 3.014s (serial 5.260s, saving 43%)
[ OK ] SIM TOTAL: 2 req wall=6.028s READ 77.18GB=49.7GB/s WRITE 8.72GB=2.0GB/s serial=10.519s overlap 43%
[FAIL] Phase H: 16/26 mismatch (all "got=00")
```

Pipeline 本身成功——3-stream 重叠工作正常，43% overlap saving，无内核崩溃。但验证失败。

## 未解决问题：DataPath `wait` 在大批量场景下可能不可靠

### 现象

1. **Pre-write 0.69s 写 36 GB = 52 GB/s** — 物理不可能（两块 NVMe 盘极限 ~14 GB/s）
2. **READ 49.7 GB/s** — 同样超过物理极限
3. **验证 mismatch 全是 `got=00`** — pre-write 的数据没有真正落盘，读回全是 0

### 分析

- 小规模（8 张量）`wait` 工作正常：pre-write 1.27s/6MB = 4.7 MB/s（合理），验证通过
- 大规模（1024 张量）`wait` 返回 OK 但数据未落盘
- `wait` 时间与物理带宽矛盾：0.69s 写 36GB 需要 52 GB/s，但盘只有 ~7 GB/s/盘

### 可能根因

**`rt->wait` 检查的是 NVMe 完成队列（CQ），但 DMA kernel 在 GPU stream 上排队。**

DataPath 的 submit 调用 `submit_one_kernel<<<..., stream>>>`（GPU kernel，写 SQE + ring doorbell）。`wait` 轮询 CQ。如果：

1. submit 的 GPU kernel 在 stream 上排队但还没执行
2. `wait` 轮询 CQ 时发现没有完成条目
3. 但 `wait` 可能有超时或退避逻辑导致提前返回（错误地认为 IO 完成）

或者：

4. 批量 submit 的 920 个请求中，只有一部分被 NVMe 控制器实际处理
5. `wait` 收到部分完成就返回 OK
6. 未处理的请求的数据从未写入磁盘

### 对比 Legacy

Legacy 的 `KvCacheIoAdapter::batched_read/batched_write` 内部：
1. 调用 `submit_batch` 提交所有 chunk
2. 调用 `cudaStreamSynchronize` 确保 DMA kernel 执行完毕
3. 轮询 CQ 直到所有完成
4. 返回

本示例的 `do_read`/`do_write`：
1. `rt->submit` 提交批量请求
2. `rt->wait` 等待完成
3. `cudaEventRecord + cudaEventSynchronize`（但这只等 event 记录，不等 DMA 完成）

缺少的环节：`rt->wait` 和 `cudaStreamSynchronize` 的先后顺序。如果 `wait` 在 DMA kernel 执行前就检查 CQ，会看到空 CQ 并可能错误返回。

### 需要确认

1. `StorageRuntime::wait` 的语义：是等 NVMe CQ 完成条目，还是等 GPU stream 上的 DMA kernel 完成？
2. `wait` 是否有内部 `cudaStreamSynchronize` 或等价的 stream 同步？
3. 大批量 submit（920 请求）时，DataPath 是否将所有请求排入一个 `submit_one_kernel` 调用，还是分批？
4. 如果分批，`wait` 是否等到所有批次完成？

### 临时解决方案（未实施）

在 `rt->submit` 之后、`rt->wait` 之前加 `cudaStreamSynchronize(stream)`，确保 DMA kernel 已执行（doorbell 已响）。但这会破坏 pipeline 重叠（host 在 sync 时阻塞，compute 不能并发）。

正确的解决方案需要理解 `wait` 的内部实现，可能需要修改 DataPath 或在示例中用 `cudaStreamSynchronize` 替代 `rt->wait`。

## 计时统计问题

当前统计的 IO 带宽（read 49.7 GB/s, write 2.0 GB/s）不可信，因为 `wait` 可能没有真正等待 IO 完成。真实的 IO 时间应该用 `cudaEventElapsedTime` 包裹 `submit + cudaStreamSynchronize + CQ poll` 的完整时间，而不是 `submit + wait`。

Legacy 的 `time_io` 函数包裹的是整个阻塞调用（`batched_read/batched_write` 内部做 sync + poll），所以计时准确。

## 文件清单

| 文件 | 状态 |
|------|------|
| `tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu` | 编译通过，小测试通过，大规模验证失败 |
| `tutti/examples/layerwise_kv_overlap/CMakeLists.txt` | 完成 |
| `tutti/CMakeLists.txt` | 一行注册（hardware label） |
| `tutti/include/tutti/storage_runtime.h` | `std::move` 修复 |
| `chat/round14/result4.md` | 本文件 |

## 硬件契约回归

未运行（示例验证未通过，优先解决 wait 问题）。

## 下一步

1. 查清 `StorageRuntime::wait` 的内部实现——它等什么？
2. 如果 `wait` 不等 GPU stream，在示例中加 `cudaStreamSynchronize` 后重试
3. 修复后重跑完整 HY3 + 硬件契约回归（735/115）

## 总指挥验收（2026-08-02）

**NOT PASS — 根因诊断错误，按 follow-up（session4b）修复后重新验收。**

本报告的"DataPath `wait` 在大批量场景下可能不可靠"假设**不成立**。独立核查证据链：

1. DataPath in-flight 配额 = 16（`local_nvme_data_path.h:401`），超出部分 per-request `RESOURCE_EXHAUSTED`——这是 Round 11 明确验收过的有界池背压行为。
2. 示例每层 submit 2×460=920 请求（`layerwise_kv_overlap.cu:208`），仅检查 `!o.io.has_value()`（:209）。**部分接受时 `o.io` 有值**，904 个被拒请求躺在 `initial_states` 里无人查看——示例违反了 Runtime 的 partial-commit 契约。
3. `wait(handle)` 只跟踪已接受的 op（Round 11 S3 fence 语义，无内部 cudaStreamSynchronize 是设计使然）；被拒请求不属于 handle，wait 正常返回 OK。
4. "52 GB/s 写入"：实际每层仅 16 请求 ≈16MB 落盘，80 层约 1.25GB / 0.69s ≈ 1.8GB/s——物理完全合理；36GB 是假设值。
5. 小规模通过的原因：6 请求 < 16 配额全部接受。
6. 43% overlap 数字无效（IO 大多未发生）；READ 49.7GB/s 同理为假象。

DataPath/Runtime 生产代码无任何缺陷，不需要改动。修复全部在示例内（session4b）：全量接受检查、窗口化提交循环、真流水线结构（读/写 host 线程或单线程交错）、诚实计时。四个"需要确认"问题的答案已写入 session4b prompt。
