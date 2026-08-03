# T-034b — Round 9 S4 Follow-up：补 S4 四项必修缺陷 — Result

## 0. 结论

**PASS**。616 passed / 0 failed（550 既有 + 66 新增），连跑两次稳定，无 segfault。
S4 四项必修缺陷全部修复并有确定性测试证据；真实 SINGLE/DUAL/LIST/cross-extent
roundtrip 无回归。

## 1. FIX 1：消除 d_inject 每次 submit 的设备内存泄漏

### 实现点

- `submit_one.cuh`：`launch_submit_one` 与 `submit_one_kernel` 的 inject 参数从
  `const std::uint32_t* d_inject_flag`（设备指针）改为 `std::uint32_t inject_flag`
  （标量按值传入）。`resolve_lba` 的 inject 参数同步改为 `std::uint32_t`，检查
  `inject_flag & 0x1u`（bit0 = resolve_lba fail）。
- `submit_one.cu`：launcher 直接将标量 `inject_flag` 传入 kernel launch，不再
  `cudaMalloc` / `cudaMemcpy`。
- `local_nvme_data_path.cpp` submit()：删除每次 submit 的
  `cudaMalloc(&d_inject, 4)` + `cudaMemcpy` + 泄漏 TODO。改为在 host 计算
  `std::uint32_t inject_flag`（bit0 = `test_inject_resolve_lba_failure_`，
  bit1 = `test_inject_nvme_error_`），按值传入 `launch_submit_one`。
  **生产路径 `inject_flag == 0`，零按-op 设备分配。**

### 测试证据（test 59）

- `test_get_inject_resolve_lba_failure()` / `test_get_inject_nvme_error()` 默认 false。
- 生产 submit（`inject_flag == 0`）→ COMPLETED（标量路径无设备分配）。
- `test_set_inject_resolve_lba_failure(true)` → submit → FAILED，per-entry result=1
  （证明标量 bit0 路径生效）。
- 关闭注入后 accessor 反映 false。

## 2. FIX 2：NVMe CQ error 正确分类为 result=3，并增加注入测试

### 实现点

- `submit_one.cuh` `poll_bounded`：**NVMe error 不再返回 `0xFFFFFFFF`**。
  timeout 才返回 `NVM_CQ_TIMEOUT`；NVMe error（`dword3 >> 17 != 0`）时返回**真实
  CQ loc**（命令已正常完成，dequeue + put_cid 正常执行），`status_dword3` 带出
  raw CQE dword[3] 错误值。调用方判定顺序：
  1. `loc == NVM_CQ_TIMEOUT` → `result = 2`（CQ timeout）
  2. `(status_dword3 >> 17) != 0` → `result = 3`（NVMe CQ error），保留 dword3
  3. `inject_flag & 0x2u` → 合成 dword3 错误位，`result = 3`（测试 seam，不伪造
     真实 CQ——命令已正常 dequeue）
  4. 否则 → `result = 0`（成功）

- `submit_read_one` / `submit_write_one` 均按上述顺序判定。
- `aggregate_completion_status_` 中 `result == 3` 的 error 文案为
  `"entry N: NVMe CQ error (dword3=0x...)"`。

### 测试证据（test 60）

```
inject: test_set_inject_nvme_error(true)  (bit1)
submit: op minted
query:  state = FAILED (2)
        status = DEVICE_ERROR, "entry 0: NVMe CQ error (dword3=0x30000)"
        bytes_transferred = 0
per-entry status: results[0] = 3 (NVMe CQ error)
```

再关注入关闭后真实 SINGLE WRITE → COMPLETED（无回退）。

## 3. FIX 3：progress() 恢复真实 CUDA 错误 → FAILED

### 实现点

- `local_nvme_data_path.cpp` `progress()`：**删除"清除 sticky 并改写为
  NotReady"的逻辑**（原 `ce = cudaErrorNotReady` 强制改写已移除）。恢复：
  `cudaEventQuery` / `cudaStreamQuery` 返回非 success、非 NotReady 错误时 →
  `op.state = FAILED`，`op.status = DEVICE_ERROR`（携带 `cudaGetErrorString`），
  记录状态后 `cudaGetLastError()` 清除 sticky 位以免污染后续查询。
- 新增 test seam `test_set_inject_query_error(bool)`：置位时 progress() 将 query
  结果视为 `cudaErrorUnknown`（持久错误，非 NotReady）。

### 测试证据（test 61）

```
submit → sync stream → op IN_FLIGHT (event 未查询)
inject: test_set_inject_query_error(true)
progress → query: state = FAILED (2)
              status = DEVICE_ERROR, "cudaEventQuery error: unknown error"
inject off → release OK
```

op 终结为 FAILED 而非停在 IN_FLIGHT（S2 语义可消费终态）。

## 4. FIX 4：timeout op 的 PRP-list 资源不得提前 unmap

### 实现点

- `local_nvme_data_path.h` `OpEntry`：新增 `bool has_timeout = false`。
- `aggregate_completion_status_`：发现任一 entry `result == 2` 时置位
  `op.has_timeout = true`（主路径 + entries D2H 失败的 fallback 路径均已覆盖）。
- `release()`：若 `has_timeout`，**不** `nvm_dma_unmap(prp_list_dma)`、**不**
  `cudaFree(prp_list_raw)`（保守泄漏）。`event` / `d_entries` / `d_status` 正常
  释放（kernel 已返回，安全）。
- `shutdown()` 与析构函数：同样遵守该规则。
- 注释说明：timed-out command 可能仍在控制器 SQ/CQ；CID 未归还导致该 SQ slot
  降级；abort/reset 为未来工作。

### 测试证据（test 62）

```
DataPath cq_poll_budget=1 → 真实 LIST 1MiB WRITE
submit → sync → drain → FAILED
  status = "entry 0: CQ poll timeout"
  bytes_transferred = 0
  has_timeout == true
  test_op_has_prp_list_dma == true (pre-release)
  per-entry: 8 entries, all result==2 (CQ timeout)
release → OK (PRP retained, no crash)
shutdown → OK (no crash)
```

回归（正常 budget DataPath）：
```
LIST 1MiB WRITE → COMPLETED
  has_timeout == false
  test_op_has_prp_list_dma == true (pre-release)
release → OK (normal PRP freed)
```

## 5. 真实 SINGLE/DUAL/LIST/cross-extent roundtrip 回归

既有 tests 26/33/34/46/47/48/49/50/54/56/57 全部通过（零回退）。
test 52（LIST shutdown timeout）+ test 55（resolve_lba injection）也通过
（标量 inject_flag 路径无回退）。

## 6. 最终 assertions 计数

```
Session 4 既有 (tests 1-58):     550 assertions（零回退）
test 59 FIX 1 scalar inject_flag: +13
test 60 FIX 2 NVMe CQ error:     +16
test 61 FIX 3 query error:       +12
test 62 FIX 4 timeout PRP:       +25

Total: 616 passed / 0 failed (连跑两次稳定)
```

## 7. 环境与文件边界

```
pgrep tutti_daemon: (not running — not required)
findmnt /mnt/nvme1: /mnt/nvme1 /dev/snvme0n1 ext4 rw,relatime (不变)
ls /dev/ssnvme0: crw-rw-rw- 1 root root 507, 0 (不变)
nvidia-smi -L: GPU 0: NVIDIA H20 (不变)
dmesg: no segfault (only normal snvme create/destroy/destroy_qgroup)
ls /mnt/nvme1/GPU0/resolver_test/: (empty — all test files cleaned)
git diff --check: clean (0 issues)
read_lints: 0 diagnostics on all modified files
```

修改文件（T-034b 范围）：
- `tutti/data_paths/local_nvme/io/submit_one.cuh`（scalar inject_flag, poll_bounded NVMe error 真实 loc, bit1 合成）
- `tutti/data_paths/local_nvme/io/submit_one.cu`（launch signature 标量）
- `tutti/data_paths/local_nvme/local_nvme_data_path.h`（has_timeout, inject_nvme_error_/inject_query_error_ members + accessors）
- `tutti/data_paths/local_nvme/local_nvme_data_path.cpp`（删除 d_inject alloc, progress 真实错误, release/shutdown/析构 has_timeout 保护）
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`（tests 59-62）
- `chat/round9/result4b.md`（new）

未触碰：public/SPI header、binding、resolver、StorageRuntime、libnvm（nvm_cmd.h /
nvm_parallel_queue.h）、kernel、CMake。未做 reset/retry/abort 机制设计。未提交 Git。

## 8. public/SPI 未变的证明

`tutti/include/` 与 `tutti/spi/` 在本任务范围无改动（S4 已建立的不变量保持）。

## PASS

## 总指挥验收（2026-08-01）

**PASS。** Round 9 随本验收正式关闭。

独立复跑（daemon 由 operator 启动于 127.0.0.1:50051，`/dev/snvme0n1` → `/mnt/nvme1` 已挂载）：

- 直跑两连跑：**616 passed / 0 failed**（550 既有 + 66 新增，与报告一致），CTest `1/1 Passed`（10.83s）。
- tests 59–62 均在日志中出现并全 PASS：标量 `inject_flag` 无按-op 设备分配、NVMe CQ error → `result=3` FAILED、`progress()` query error → FAILED、timeout op 保留 PRP-list DMA + 正常路径回归。
- 代码抽查与报告一致：`poll_bounded` 对 NVMe error 返回真实 loc 并正常 dequeue/put_cid（`submit_one.cuh:144-168`）；`release()` 以 `has_timeout` 保护 PRP-list unmap/free（`local_nvme_data_path.cpp:1428-1440`）；`progress()` 改写 NotReady 的逻辑已删除，FAILED 分支恢复可达。
- S4b 修改了 `release()` 语义，按 S5 验收条件复跑 S5 门禁：**115 passed / 0 failed**，无回退。
- 诊断 0、`git diff --check` clean、测试临时目录已清空（含清理此前模块卸载中断遗留的 `round8_t41.bin`）。

四项 REQUIRED 全部闭合，S4 升级为最终 PASS；S5 生效条件满足。Round 10（Phase 3）prompts 已就绪于 `chat/round10/`，可启动。
