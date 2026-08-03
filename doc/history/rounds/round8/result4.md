# T-029 Round 8 Consolidation A — Result

## 0. 结论

**PASS**。338 passed / 0 failed（236 既有 + 102 新增），连跑两次稳定。

## 1. REQUIRED FOLLOW-UP 逐项对照

### REQUIRED 1：不可逆 submit 的资源预留顺序

**Source**: `local_nvme_data_path.cpp::submit()` 旧顺序：H2D → launch → event create → event record。

**Fix**: 新顺序（`submit()` 实现）：

1. 全部 request validation + fan-out + partial-commit 决策（可逆）
2. 检查 `ops_.size() >= max_in_flight_operations_`（可逆）
3. `cudaEventCreateWithFlags` 创建 event（**launch 前预留**）
4. `cudaMalloc(d_entries)` 分配 device entry（**launch 前预留**）
5. PRP-list raw/aligned/DMA 分配（**launch 前预留**）
6. PRP-list page 填充 + **同步** `cudaMemcpy` H2D（检查返回值）
7. Entry flatten + **同步** `cudaMemcpy` H2D（检查返回值）
8. `launch_submit_one` 返回 `cudaError_t`（搬自 main `launch_nvme_batch_xfer` 的 `cudaGetLastError()`）；失败时 kernel 未发出，安全清理
9. `cudaEventRecord` 在 launch 成功后执行；失败时**不返回 op=nullopt**，改为同步 `cudaStreamSynchronize` 证明完成状态后存储 terminal op

**H2D staging 选择**：同步 `cudaMemcpy`（非 `cudaMemcpyAsync`）。PRP page 和 entry 的 host buffer 是栈/局部变量，同步拷贝在函数返回前完成，不存在 dangling async source。选择理由：最小忠实方案，不需要 per-op pinned host staging。

**Test 证据**：test 45（launch failure seam）注入 `test_inject_launch_failure_=true`，证明 launch 前失败 → `op=nullopt` + `REJECTED` + `test_in_flight_count()==0`。

### REQUIRED 2：`progress()` 双 hard cap

**Source**: 旧 `progress()` 只使用 `max_work_units`，完全忽略 `timeout_ns`。

**Fix**（`progress()` 实现）：

```text
max_work_units == 0 || timeout_ns == 0 → 零 work，立即返回（仍报告 more_work_likely）
```

- `std::chrono::steady_clock` 计算 deadline
- 每个 event/stream query 前检查 deadline 和 work cap
- 一个 query = 一个 work unit
- 不 busy-poll（shutdown drain 中使用 `std::this_thread::sleep_for(50us)`）

**旧测试修复**：test 26 READ-ONLY 的 `ProgressBudget{16, 0}` → `{16, 1000000000}`。

**Test 证据**：test 39 — terminal-ready op 时 `{16, 0}` 消费 0 work、op 仍 IN_FLIGHT；`{16, 1000000000}` 消费 >0 work、op → COMPLETED。

### REQUIRED 3：`shutdown(timeout_ns)` 与析构安全

**Source**: 旧 `shutdown()` 忽略 timeout，无条件 force-free 全部资源。

**Fix**（`shutdown()` 实现）：

1. `shutdown(0)` 遇 IN_FLIGHT op → 返回 `TIMEOUT`，**不释放任何资源**
2. `timeout_ns > 0` 在 deadline 内有界 drain（progress + `sleep_for(50us)`）
3. deadline 到仍 IN_FLIGHT → `TIMEOUT`，DataPath 保持 initialized
4. 全部 op terminal 后按序释放：op event/entry/PRP → target → queue → memory DMA → controller
5. 成功后幂等

**析构**（`~LocalNvmeDataPath()` 实现）：

- 遇 IN_FLIGHT op → 收集唯一 stream → `cudaStreamSynchronize` 每个
- sync 成功 → 标记 terminal → 正常 teardown
- sync 失败 → **保守不释放**（leak 但不 UAF）

**Test 证据**：test 40 — `cudaLaunchHostFunc` 插入 0.3s 延迟 → submit → `shutdown(0)` 返回 `TIMEOUT` → op/target/memory/DMA 仍可查 → `test_op_has_resources()==true` → drain → `shutdown(0)` 返回 OK。

### REQUIRED 4：partial commit status

**Source**: 旧实现 `Status(StatusCode::OK, "partial commit")` → `ok()` 返回 true。

**Fix**（`submit()` 实现）：

```text
has_rejection → outcome.status = Status(first_rejected_code, "partial commit: " + first_rejected_msg)
```

- 使用第一个 rejected request 的非 OK code
- 加 `partial commit:` 前缀
- 不新增 StatusCode

**Test 证据**：test 36 — `CHECK(!outcome.status.ok(), "partial commit: overall status non-OK")` + `CHECK(outcome.initial_states[0].status.ok(), ...)` + `CHECK(!outcome.initial_states[1].status.ok(), ...)`。

### REQUIRED 5：MDTS、capacity 与 capabilities

**Source**: 旧实现硬编码 128 KiB MDTS，不使用 `ioctl_get_dev_info()` 返回的 `dev_info.max_data_size`；capacity 概念混为一谈。

**Fix**（`initialize()` + `submit()` 实现）：

**MDTS**:
- `initialize()` 从 `dev_info.max_data_size` 取得 `hardware_mdts_bytes_`
- `mdts_bytes_ == 0` → `effective = hardware`
- 非 0 → `effective = min(override, hardware)`
- 校验 effective 非 0、是 block-size multiple
- PRP-list page capacity: `page_size/8 + 1` data pages；effective MDTS 不超过此范围

**Capacity（概念分离）**:
- `max_batch_requests_` = max 输入 request 数（submit 前检查）
- `max_batch_entries_` = max fan-out entry 数（fan-out 后检查）
- `max_in_flight_operations_` = enforced cap on `ops_.size()`（irreversible submit 前检查）
- `max_request_bytes_` = `max_batch_entries_ * effective_mdts`（per-request 检查）
- `max_batch_bytes` = same（batch 总字节检查）

**Capability 字段**:
- `max_single_io_bytes` = `max_batch_entries_ * effective_mdts`（DataPath 可接受的单 request 最大字节数，**不是**单个 SQE 的 MDTS）
- `max_batch_bytes` = same
- `max_in_flight_operations` = `max_in_flight_operations_`
- `max_concurrent_operations` = `max_in_flight_operations_`
- `supports_host_memory = false`（HOST memory IO 未实现）
- `supports_multi_stream = false`、`max_concurrent_streams = 0`（S5 前）
- `supports_read/write/direct/device_execution/device_memory = true`
- 恢复 `ctx.device_id == queue_group_->cuda_device()` 校验

**Test 证据**:
- test 42: hardware MDTS=131072 (128 KiB), effective=131072, PRP-list page capacity=513, max_single_io=33554432 (32 MiB), fan-out entries=8 (1 MiB / 128 KiB)
- test 43: wrong device_id → REJECTED
- test 44: 逐项 capability 比对
- test 41: in-flight cap (16) 填满 → RESOURCE_EXHAUSTED → drain → submit 成功

## 2. submit 可逆/不可逆边界

```text
可逆（op == nullopt）：
  - not initialized / null requests / HOST_EXECUTION / null stream
  - no queue group / device_id mismatch
  - count > max_batch_requests
  - ops_.size() >= max_in_flight_operations
  - per-request validation (target/memory/bounds/alignment)
  - all rejected / no accepted
  - entry count > max_batch_entries
  - batch bytes > max_batch_bytes
  - event create failed
  - cudaMalloc(d_entries) failed
  - cudaMalloc(PRP-list) failed
  - nvm_dma_map_data_device(PRP-list) failed
  - cudaMemcpy H2D PRP page failed
  - cudaMemcpy H2D entries failed
  - launch_submit_one returned error (kernel NOT issued)

不可逆（op != nullopt）：
  - launch succeeded → event record succeeded → IN_FLIGHT op
  - launch succeeded → event record FAILED → sync → terminal op (COMPLETED or FAILED)
```

## 3. shutdown timeout 前后资源计数证据

Test 40 输出：

```text
op is IN_FLIGHT during delay          ← op exists
shutdown(0) → TIMEOUT                 ← resources NOT freed
op still queryable after TIMEOUT      ← op table intact
op still IN_FLIGHT                    ← state unchanged
target still exists                   ← targets_ map intact
dev_handle still exists               ← GPU target handle intact
DMA handle still exists               ← mem_regs_ intact
op resources (d_entries, event) still allocated  ← entry+event not freed
--- drain ---
shutdown after drain → OK             ← all released in order
```

## 4. progress budget 实测

Test 39 输出：

```text
zero-timeout: 0 work consumed        ← ProgressBudget{16, 0} → zero work
zero-timeout: op still IN_FLIGHT      ← op state unchanged
positive-timeout: work consumed       ← ProgressBudget{16, 1s} → work done
positive-timeout: op COMPLETED        ← op advanced to terminal
```

## 5. hardware/effective MDTS 与所有 capability 值

```
hardware MDTS:           131072 bytes (128 KiB)
effective MDTS:           131072 bytes (128 KiB)
PRP-list page capacity:  513 data pages
max_single_io_bytes:     33554432 (32 MiB = 256 * 128 KiB)
max_batch_requests:      256
max_batch_bytes:         33554432 (32 MiB)
max_in_flight_operations: 16
max_concurrent_operations: 16
max_concurrent_streams:  0
supports_host_memory:    false
supports_multi_stream:   false
supports_device_memory:  true
supports_read:           true
supports_write:          true
supports_direct:         true
supports_device_execution: true
device_completion_fence_on_caller_stream: true
device_execution_autonomous: true
target/memory/length_alignment: 4096
```

## 6. 防假阳性 WRITE 输出

Test 26（修复后）：

```text
WRITE pattern: 0x5A (initial file: 0xAB)
READ-after-WRITE: D2H 0x5A match (WRITE actually changed content)
```

Test 38（专门验证）：

```text
READ proves file is 0xAB              ← 4096/4096 bytes are 0xAB
WRITE 0x5A                            ← different pattern from initial
READ-after-WRITE: 4096/4096 are 0x5A  ← WRITE landed
                       0/4096 are 0xAB ← old content gone
```

## 7. 当前+新增测试完整计数

```
Session 2 (tests 1-10):     57 assertions (unchanged, test 1 fixed)
Session 3 (tests 11-25):    48 assertions (unchanged)
Session 2 IO (tests 26-32): 45 assertions (test 26 fixed)
Session 3 batch (tests 33-37): 86 assertions (tests 35,36,37 fixed)
  + test 35: +2 assertions (file 2 read-back + different pattern)
  + test 36: +3 assertions (partial status non-OK + per-request status)
  + test 37: +2 assertions (stream 1/2 data verified)
New tests (38-45):           102 assertions
  38: WRITE anti-false-positive    (12)
  39: progress zero timeout        (9)
  40: shutdown timeout resources  (13)
  41: in-flight cap                (13)
  42: MDTS and capacity            (18)
  43: device mismatch               (7)
  44: capabilities match           (19)
  45: launch failure seam          (11)

Total: 338 passed / 0 failed
```

## 8. 环境与文件边界

```
pgrep tutti_daemon: 3386944 ./build/bin/tutti_daemon --config sys_config.yaml (不变)
findmnt /mnt/nvme1: /mnt/nvme1 /dev/snvme0n1 ext4 rw,relatime (不变)
ls /dev/ssnvme0: crw-rw-rw- 1 root root 507, 0 (不变)
nvidia-smi -L: GPU 0: NVIDIA H20 (不变)
ls /mnt/nvme1/GPU0/resolver_test/: (empty — all test files cleaned up)
```

修改/创建文件（T-029 范围）：
- `tutti/data_paths/local_nvme/local_nvme_data_path.h` (modified)
- `tutti/data_paths/local_nvme/local_nvme_data_path.cpp` (modified)
- `tutti/data_paths/local_nvme/io/submit_one.cuh` (modified)
- `tutti/data_paths/local_nvme/io/submit_one.cu` (modified)
- `tutti/data_paths/local_nvme/io/prp_builder.h` (unchanged)
- `tests/local_nvme_datapath_contract/CMakeLists.txt` (unchanged)
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp` (modified)
- `chat/round8/result4.md` (new)

未触碰：public/SPI header、binding、resolver、main/旧 source、libnvm 源码、根目录两份参考文档。

构建目录：`build/round8-session4`。

## 9. 清理项

- 三处 `skeleton: UNSUPPORTED` 过期注释 → 已删除
- `submit_one.cuh` 的 `ioaddrs[i]` 注释 → 修正为 "GPU pin/map 底层粒度为 64 KiB，但 `nvm_dma_t::ioaddrs[]` 按 controller MPS（4 KiB）展开"
- `submit()` 中 `Hack` / `I can't change the header now` 生成过程注释 → 替换为准确的 `total_bytes` owner 说明
- `launch_fill_pattern` 保留（S5 再决定是否迁到测试 `.cu`）

## 10. 验收对照

1. ✅ 236 既有断言无回退 + 新增测试全通过（338 total）
2. ✅ WRITE pattern 不再假阳性（test 26 + test 38: 0xAB→0x5A, old gone）
3. ✅ shutdown timeout 不释放 in-flight 资源（test 40）
4. ✅ op-null/zero-issued 不变量在所有失败路径成立（test 43 device mismatch, test 45 launch failure, test 41 cap exceeded）
5. ✅ progress 双 hard cap 成立（test 39）
6. ✅ partial commit 总体非 OK（test 36: `!outcome.status.ok()` + `!initial_states[1].status.ok()`）
7. ✅ MDTS/capacity/capabilities 与真实执行一致（test 42 + test 44）
8. ✅ daemon/mount/module/RAID 不变
9. ✅ 文件边界、whitespace、EOF、linter 正常（0 diagnostics）

## PASS

## 总指挥验收

验收结论：`PASS WITH REQUIRED FOLLOW-UP`。S2 的五项核心生命周期/capability 缺口已实质闭合，S4 可在其定义范围内通过；但提交边界上仍有一个明确的 in-flight capacity 语义 bug，且 event-record 失败路径与 LIST PRP timeout 资源证据缺失，必须在进入 Session 5 前补齐。

### 已独立核验通过

- **最终构建与测试稳定。** 我独立重跑官方 CTest：`1/1 Passed`，`338 passed / 0 failed`，用时约 10.26s。`LastTest.log` 与源码/二进制时间一致，不再是 worker 报告之外的旧日志。
- **WRITE 不再是假阳性。** test 38 先经 NVMe READ 证明文件为 0xAB，再 DMA WRITE 0x5A，最终 4096/4096 为 0x5A 且 0/4096 为 0xAB。旧内容真正消失，证明 WRITE 落盘。
- **不可逆 submit 边界已重构。** 我读了 `submit()`：所有 validation、partial decision、request count、entry count、batch bytes、`ops_.size()` capacity、event、entry、PRP raw/aligned/DMA、同步 H2D 都在 kernel launch 前完成；PRP/entry H2D 的 CUDA 返回值全部检查；`launch_submit_one()` 已按 main 返回 `cudaError_t` 并调用 `cudaGetLastError()`。
- **event-record 失败不再伪装 zero-issued。** launch 成功后 `cudaEventRecord` 失败会同步 caller stream，并存储可观察 terminal op，不再返回 `op=nullopt`。这符合冻结 SPI。
- **`progress()` 双 hard cap 已成立。** 任一预算为 0 立即返回且消费 0；正常 event 查询与 STREAM_QUERY fallback 均受 deadline/work cap 限制。test 39 有明确状态对照。
- **shutdown timeout 不再拆资源。** `shutdown(0)` 遇 IN_FLIGHT 返回 `TIMEOUT` 且 DataPath 保持 initialized；超时后可 query op、target、DMA，drain 后再次 shutdown OK。test 40 用 `cudaLaunchHostFunc` 形成确定性 delay。
- **析构安全策略符合 prompt。** IN_FLIGHT 时按唯一 caller stream 等待；等待失败保守不释放，避免 UAF。
- **partial commit status 已修正并硬断言。** `outcome.status` 使用第一个 rejected request 的非 OK code；test 36 显式检查 `!outcome.status.ok()` 和 per-request status。
- **MDTS/capability 已从 controller 事实源取得。** 日志确认 hardware/effective MDTS 均为 131072，PRP-list page capacity 为 513，`max_single_io_bytes` 为 32 MiB（fan-out 可接受上限），1 MiB 按 8 entries fan-out。
- **S3 的 mixed-target 与 dual-stream 数据验证已被补齐。** test 35 两个 target 使用不同 memory offset/pattern 并双读回；test 37 0x37/0x73 均读回验证。
- **边界与环境。** 允许范围内的 DataPath/test 文件 whitespace/EOF 正常，linter 0 diagnostics；public/SPI、binding、resolver、旧 source 未新增改动（libnvm 现有 diff 仍为 R6-S1 已验收内容）。daemon、挂载、模块、生产 RAID 保持，临时文件零残留。

### REQUIRED 1：`ops_.size()` 不是 in-flight 数

`submit()` 在 `local_nvme_data_path.cpp:793` 用：

```cpp
ops_.size() >= max_in_flight_operations_
```

作为 hard capacity。`ops_` 在 op 变成 terminal 后仍保留到 release，因此 terminal 未 release 的 op 也被计入。

这会把：

```text
max_in_flight_operations = 16
```

错误地变成：

```text
最多 16 个尚未 release 的 op（包括已完成者）
```

假设当前循环 16 个 submit → terminal → 但尚未 release，第 17 个会 `RESOURCE_EXHAUSTED`，即使实际没有任何 IN_FLIGHT。这是明确 hard capability 违约。

必须改为在不可逆 submit 前统计：

```cpp
count_if(op.state == OpState::IN_FLIGHT)
```

并新增测试：

```text
连续填满 cap → progress 全部 terminal（不 release）→ 第 17 个 submit 必须成功
```

release 仅是 owner cleanup，不应占 in-flight quota。

### REQUIRED 2：event-record 失败路径没有测试

实现已有正确 fallback（launch success → record failure → stream sync → terminal op），但没有任何 injection/test 触发。若将来重构，这条最关键 SPI 分支可能无声回退。

补一个 private test-only event-record failure seam：

```text
launch 成功 → record 强制失败 → submit 返回 op != nullopt
→ query 得到 COMPLETED 或 FAILED
→ release 安全释放
```

并断言它不返回 `op=nullopt`。

### REQUIRED 3：timeout 时 LIST PRP-list 资源证据缺失

`test_op_has_resources()` 只检查 `d_entries != nullptr && event != nullptr`，没有检查 `prp_list_dma`、`prp_list_raw` 或 `prp_list_aligned`。而 timeout 风险最大的正是 controller 仍在读取的 PRP-list DMA mapping。

必须：

1. 把 accessor 扩展为在 op 拥有 PRP list 时验证：
   - `prp_list_dma != nullptr`
   - `prp_list_raw != nullptr`
   - `prp_list_aligned != nullptr`
2. 做一个 LIST delayed-stream shutdown timeout 测试：
   - submit 1 MiB LIST
   - `shutdown(0)` → TIMEOUT
   - 确认 PRP DMA/raw/aligned 与 event/entry 仍全部保留
   - drain → release → shutdown OK

### REQUIRED 4：`submit_one.cuh` 与 `submit_one.cu` 注释仍描述旧 4 KiB slice

当前 entry/kernel 已支持 variable-length batch，但注释仍写：

```text
One entry = one 4 KiB NVMe read or write
simplified ... batch count = 1
```

按 prompt 的过期注释纪律顺手更正，避免 S5/后续 session 误判范围。

### 非阻塞观察

- launch failure seam 是 private `test_set_inject_launch_failure()`，没有污染 public/SPI；方向正确。
- `cudaGetLastError()` 在 launch 后才调用，这与 main 一致；它覆盖 launch-config 类错误，不是设备运行期 CQ error channel。当前任务明确不新增该 channel，记录即可。
- `supports_multi_stream` 保持 false 是保守且正确的。S4 已证明双 stream 数据隔离，但 prompt 原约定由 S5 在完成更严格 multi-stream capability 检查后开启；这不是本 follow-up 阻塞项。

### 后续决定

T-029 的核心整改保留。以下 S4 follow-up 已完成独立复核，并取代本节此前的阻塞结论。

### Follow-up closure（2026-07-31）

1. **in-flight 配额：** `submit()` 现在只统计 `OpState::IN_FLIGHT`，不会让终态但未 `release()` 的可观察 op 占用 `max_in_flight_operations`。测试 41 先填满 16 个 in-flight op，drain 至 terminal 且不 release，再次提交成功。
2. **event-record 失败：** 新增私有注入 seam；测试 51 模拟 kernel 已成功 launch 后 `cudaEventRecord` 失败，验证 `op.has_value()`、请求仍为 `ACCEPTED`、查询得到 `COMPLETED` 且可安全 `release()`。没有把已发出 IO 伪装为 `op == nullopt`。
3. **LIST timeout 资源：** `test_op_has_resources()` 对拥有 PRP-list 的 op 额外要求 `prp_list_dma`、`prp_list_raw`、`prp_list_aligned` 全部存在。测试 52 对 1 MiB LIST IO 注入 stream 延迟，在 `shutdown(0)` 返回 `TIMEOUT` 前后均验证这些资源以及 entry/event 仍被保留，drain 后正常 release/shutdown。
4. **注释：** `submit_one.cuh/.cu` 已更正为可变长度、每 op 批量 entry 的实际语义。

独立构建与验证：

```text
cmake -S tests/local_nvme_datapath_contract -B build/round8-session4-followup -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round8-session4-followup -j8
ctest --test-dir build/round8-session4-followup --output-on-failure
```

`ctest` 为 `1/1` 通过；随后直接重跑二进制得到 **484 passed / 0 failed**。受影响文件的 IDE diagnostics 为 0。构建过程中仅保留既有 `libnvm` 头的 signedness 和 CUDA `ulonglong4` deprecation warnings，未由本次改动引入。

## Final commander acceptance

**PASS。** T-029 / S4 的生命周期、硬能力与资源所有权验收条件均已闭合；原 REQUIRED FOLLOW-UP 1–4 已由可重复的硬件契约测试覆盖。S5 的数据面范围应按其独立结果和验收清单复核，不再受 S4 阻塞。
