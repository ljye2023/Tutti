# TASK T-034b — Round 9 S4 Follow-up：补 S4 四项必修缺陷

## 背景

`chat/round9/result4.md` 的总指挥验收为 **PASS WITH REQUIRED FOLLOW-UP**，四项必修：

1. `d_inject` 每次 submit 泄漏设备内存（生产路径也泄漏）；
2. NVMe CQ error 被误分类为 CQ timeout（`result=3` 死代码），且缺 NVMe-error 注入测试；
3. `progress()` 把真实 CUDA 错误改写为 NotReady，op 可永久 IN_FLIGHT；
4. CQ timeout 后 `release()` 允许提前 unmap PRP-list DMA（quiesce 语义缺口）。

本任务只做这 4 项最小修复 + 回归，不做任何新设计。

## 允许修改/创建

- `tutti/data_paths/local_nvme/io/submit_one.cuh`
- `tutti/data_paths/local_nvme/io/submit_one.cu`
- `tutti/data_paths/local_nvme/local_nvme_data_path.h`
- `tutti/data_paths/local_nvme/local_nvme_data_path.cpp`
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`
- `chat/round9/result4b.md`

## 禁止范围

不改 public/SPI、Runtime、resolver、binding、libnvm、kernel、CMake；不做 reset/retry/abort 机制设计；不执行模块/daemon/mount/raw block IO；不提交 Git。

## 必修实现

### FIX 1：消除 d_inject 每次 submit 的设备内存泄漏

- 注入 flag 改为 kernel **标量参数**（`std::uint32_t inject_flag` 按值传入 `launch_submit_one` → kernel），删除每次 submit 的 `cudaMalloc(&d_inject)`、`cudaMemcpy` 与泄漏 TODO。
- kernel 内 `resolve_lba` 的 inject 参数从指针改为值（0=正常，bit0=resolve_lba fail，bit1=见 FIX 2）。
- 验证：注入开/关路径行为不变，且生产 submit 不再发生任何按 op 的小额设备分配（可用 accessor 或代码路径证明）。

### FIX 2：NVMe CQ error 正确分类为 result=3，并增加注入测试

- `poll_bounded`：timeout 才返回 `NVM_CQ_TIMEOUT`；NVMe error（dword3>>17 != 0）时返回真实 CQ loc，`status_dword3` 带出错误值。调用方判定顺序：`loc == NVM_CQ_TIMEOUT` → result=2；否则 `(status_dword3 >> 17) != 0` → result=3 并保留 dword3。
- 增加 inject flag bit1：当置位且该 entry 正常完成时，设备端**合成** dword3 错误位并走 result=3 路径（测试 seam，不伪造真实 CQ）。
- 新测试：注入 NVMe-error → op `state=FAILED`、status 含 `NVMe CQ error (dword3=...)`、`bytes_transferred` 只计成功 entry、per-entry result=3；再关注入做真实 SINGLE roundtrip 证明无回退。

### FIX 3：progress() 恢复真实 CUDA 错误 → FAILED

- `local_nvme_data_path.cpp` 的 `progress()`：删除“清除 sticky 并改写为 NotReady”的逻辑；恢复 `cudaEventQuery/cudaStreamQuery` 返回非 success、非 NotReady 错误时 op → FAILED（`DEVICE_ERROR`，携带 `cudaGetErrorString`），记录状态后再 `cudaGetLastError()` 清除 sticky 位以免污染后续调用。
- 新测试：用现有 STREAM_QUERY/event 失败注入 seam 或等价 seam 触发 query 返回持久错误，断言 op 终结为 FAILED 而非停在 IN_FLIGHT（Runtime 侧由 S2 语义消费，本测试只验 DataPath）。

### FIX 4：timeout op 的 PRP-list 资源不得提前 unmap

- `OpEntry` 增加 `bool has_timeout = false`；`aggregate_completion_status_` 发现任一 entry `result == 2` 时置位。
- `release()`：若 `has_timeout`，不 `nvm_dma_unmap(prp_list_dma)`、不 `cudaFree(prp_list_raw)`（保守泄漏，注释说明：timed-out command 可能仍在控制器队列，abort/reset 是未来工作）；event/d_entries/d_status 正常释放（kernel 已返回）。
- `shutdown()` 析构同样遵守该规则；CID 未归还导致的 queue slot 降级写入注释。
- 新测试：预算=1 强制 CQ timeout（真实 IO 下用极小时延或注入保证 timeout 触发）→ op FAILED；`test_op_has_prp_list_dma` 仍 true；release 后 PRP DMA 保留；shutdown 后无 crash；再验证正常 op 的 PRP 释放路径无回归。

## 验收

1. 既有 550 断言零回退 + 新增断言全通过；
2. FIX 1-4 各自有确定性测试证据；
3. 真实 SINGLE/DUAL/LIST/cross-extent roundtrip 无回归；
4. 直跑输出断言总数；`git diff --check` clean；linter 0；
5. 环境不变（daemon/mount/module/RAID），临时文件清空。

## 结果落盘

写 `chat/round9/result4b.md`：四项修复的实现点、测试证据、断言计数、环境边界、最终 `PASS`/`BLOCKED`。不写总指挥验收，不提交 Git。
