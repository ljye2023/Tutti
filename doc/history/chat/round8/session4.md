# TASK T-029 — Round 8 Consolidation A

你是一名资深 CUDA/NVMe C++ 工程师。你的任务不是设计新数据面，而是对 Round 8 已搬入的 `LocalNvmeDataPath` 做一次**冻结 SPI 所必需的生命周期与硬契约收口**。

# 任务定位

**继续遵守：分层抽象 + 代码搬运。**

- main 数据面基线固定为：`4862157d50c8a7004cdeb166dda630ab1ef4561a`。
- 必读：
  - `MAIN_IO_PATH.md`
  - `MAIN_MEMORY_PRP_PATH.md`
  - `chat/round8/result2.md` 的「总指挥验收」
  - `chat/round8/result3.md` 的「总指挥验收」
- 当前真实基线：`build/round8-session3`，`236 passed / 0 failed`。
- 保留 S1-S3 已搬入的 queue、target、DMA、SQE/CQ、SINGLE/LIST、MDTS fan-out、owned PRP fallback、per-op workspace；**禁止推倒重写**。
- 本任务只修新 SPI 适配产生的资源预留、timeout、capacity、status 与 capability 缺口；不评审或改变 main 的 queue/SQE/CQ 策略，不引入 PRP tiered cache。

# 前置条件与执行约束

必须单独执行，不能与 Session 5 或其他使用 daemon/controller 的任务并发。

负责人保持：

```bashå
pgrep -af tutti_daemon | head -1
findmnt /mnt/nvme1 | tail -1
ls -l /dev/ssnvme0
nvidia-smi -L | head -1
```

若环境不就绪，报告 `BLOCKED`；禁止自行启停 daemon、bind、mount。

# 1. 修复不可逆 submit 的资源预留顺序

冻结 SPI 不变量：

```text
op == nullopt  => 零不可逆发出
op != nullopt  => 至少一项已发出且始终可观察、可安全回收
```

当前错误顺序是 kernel launch 后才创建 event；event create/record 失败会返回 `op=nullopt` 并释放 GPU/NVMe 仍可能使用的资源。

改为：

1. 完成所有 request validation、partial-commit 决策和 entry fan-out；
2. 在 launch 前预留：op table capacity、PRP raw/aligned/DMA、device entry、completion event、所有 owner metadata；
3. 检查每次 PRP-list copy 与 entry copy 的 CUDA 返回值；失败时尚未 launch，可安全清理并返回 `op=nullopt`；
4. `launch_submit_one` 按 main `launch_nvme_batch_xfer` 改为返回 `cudaError_t`，launch 后执行并返回 `cudaGetLastError()`；launch 失败且确认 kernel 未发出时安全清理；
5. launch 成功后必须有可观察 op。若后续 `cudaEventRecord` 失败，禁止伪装为 zero-issued：
   - 保留 op 与全部 workspace；
   - 使用安全 completion fallback（例如在 `OpEntry` 记录 `STREAM_QUERY` mode，由 `progress()` 用 borrowed stream 的 `cudaStreamQuery` 保守判断 completion）；
   - 或同步证明所有已发出工作完成后返回 terminal op；
   - 不能 free 仍可能被 kernel/controller 使用的 entry/PRP/target/queue/mapping。

不要新增 public/SPI 字段。completion mode 是 DataPath 私有 metadata。

## H2D staging 生命周期

main owned fallback 对 PRP pages 使用同步 H2D；main batch entry async H2D 的 host scratch 是长寿命 owner。当前本地 `std::vector` 是 submit 栈变量。

选择最小忠实方案之一并说明：

- PRP page/entry 使用同步 `cudaMemcpy`，然后在 caller stream launch；或
- 使用 per-op pinned host staging，持有到 completion。

禁止把栈/局部 vector 当作未完成 async H2D 的 source 后立即销毁。

# 2. 修复 `progress()` 的双 hard cap

冻结契约：`ProgressBudget.max_work_units` 与 `timeout_ns` 都是本次调用 hard cap。

本任务明确采用：

```text
max_work_units == 0 → 零 work，立即返回
timeout_ns == 0     → 零 wall-clock budget，零 work，立即返回
```

实现：

- `std::chrono::steady_clock` 记录 deadline；
- 每个 event/stream query 前检查 deadline；
- 一个 query = 一个 work unit；
- 永不 busy-poll；
- 达到任一 cap 立即返回，并正确填写 `more_work_likely`；
- 正常 event mode 与 event-record 失败后的 stream-query fallback 都遵守同一预算。

更新旧测试中把 `{16, 0}` 当作可执行 work 的错误用法。

# 3. 修复 `shutdown(timeout_ns)` 与析构安全

## `shutdown`

当前实现忽略 timeout 并 force-free in-flight entry/PRP/target/queue/DMA/controller，必须改掉。

规则：

1. `shutdown(0)` 遇到任何 IN_FLIGHT op → 返回 `StatusCode::TIMEOUT`，且**不释放任何可能被它引用的资源**；
2. `timeout_ns > 0` 时在 deadline 内有界查询/drain；禁止无限 busy-poll；
3. deadline 到仍有 IN_FLIGHT → `TIMEOUT`，DataPath 保持 initialized，op/target/memory/queue/controller 全部可继续 query/progress/release；
4. 全部 op terminal 后，按以下顺序释放：

```text
op event/entry/PRP DMA+raw
→ target device handle
→ queue group
→ data memory DMA
→ controller
```

5. shutdown 成功后幂等；timeout 后可再次 progress/shutdown。

## 析构

析构不能 force-free in-flight GPU/NVMe 资源。因为析构无法返回 TIMEOUT，采用安全且简单的规则：

- 若仍有 in-flight op，等待其 completion fence 后再 teardown；允许析构阻塞，但禁止 UAF；
- 若 CUDA completion 等待返回错误，必须采取不会释放仍可能被设备使用资源的保守策略，并在结果中说明；
- 不在本任务新增 CQ watchdog/error channel（main 当前没有 host error channel）。

# 4. 修复 partial commit status

至少一个 request ACCEPTED 且至少一个 REJECTED 时：

```text
op != nullopt
accepted request initial status = OK
rejected request initial status = 具体错误
outcome.status = 非 OK
```

整体 status 使用第一个 rejected request 的非 OK code，并加 `partial commit:` 前缀即可；不要新增 StatusCode。

补硬断言：

```cpp
CHECK(!outcome.status.ok(), ...);
```

# 5. 修复 controller MDTS、capacity 与 capabilities

`ioctl_get_dev_info()` 已返回 `disk.max_data_size`（bytes）。禁止继续无条件假定 128 KiB。

## MDTS

- `initialize()` 从 `dev_info.max_data_size` 取得 hardware MDTS；
- 构造参数 `mdts_bytes == 0` → 使用 hardware MDTS；
- 非 0 override → `effective_mdts = min(override, hardware_mdts)`；
- 校验 effective MDTS 非 0、是 block-size multiple；
- owned single-page PRP-list builder 只支持最多 `page_size/8 + 1` 个 data pages；若 effective MDTS 超过可表达范围，应结构化失败或进一步按该上限 fan-out，禁止 host buffer 越界。

## Capacity

分别定义并执行：

```text
max_batch_requests      // 输入 request 数
max_batch_entries       // fan-out 后 entry 数（private limit）
max_in_flight_operations
max_request_bytes       // 单 request 经 fan-out 可接受的上限
max_batch_bytes
```

最小实现允许复用现有 constructor 参数，但概念和检查必须分开：

- submit 前检查 request count；
- fan-out 时检查 entry 数；
- irreversible submit 前检查 `ops_` capacity；
- capability 字段与实际执行的 hard limit 逐一一致；
- `max_single_io_bytes` 按本项目 prompt 语义填 **DataPath 可接受的单 request 最大字节数**，不能填单个 SQE 的 MDTS；
- `max_batch_bytes` 必须在 submit 中执行，不只声明。

## 其他 capability/validation

- 恢复 `ctx.device_id == queue_group_->cuda_device()` 校验；
- HOST memory registration 可保留，但 HOST memory IO 未实现，因此 `supports_host_memory=false`；
- S5 完成双 stream 数据验证前，临时将 `supports_multi_stream=false`、`max_concurrent_streams=0`；
- `max_concurrent_operations` 与 enforced in-flight cap 一致；
- `supports_read/write/direct/device_execution/device_memory` 保持 true。

# 6. 清理过期/误导内容

只清理本任务触及范围内的内容：

- 三处 `skeleton: UNSUPPORTED` 过期注释；
- `submit_one.cuh` 的 `ioaddrs[i]` 注释改准确：GPU pin/map 底层粒度为 64 KiB，但 `nvm_dma_t::ioaddrs[]` 按 controller MPS（本机 4 KiB）展开；
- 删除 `submit()` 中 `Hack` / `I can't change the header now` 生成过程注释，改为准确的 `total_bytes` owner 说明；
- `launch_fill_pattern` 暂可保留，S5 再决定是否迁到测试 `.cu`。

# 7. 必测回归

保留当前 **236 个断言**，并新增至少：

1. **WRITE 防假阳性**：初始文件 0xAB → 先 READ 证实 → DMA WRITE 0x5A → 新 read buffer 读回 0x5A；旧 0xAB 必须消失。
2. **partial status**：一个合法、一个越界；op 存在且总体 status 非 OK。
3. **progress zero timeout**：已有 terminal-ready event 时，`ProgressBudget{N,0}` 仍消费 0 work、不改变 op；正 timeout 才推进。
4. **shutdown timeout 保留资源**：用 `cudaLaunchHostFunc` 在 stream 上插入受控 sleep，再 submit；立即 `shutdown(0)` 必须 TIMEOUT；target/memory/op 仍可查；之后 drain/release/shutdown 成功。
5. **event/owner 顺序**：至少通过 test accessor/counter 证明 timeout 后 PRP DMA/entry/event 未释放。
6. **in-flight cap**：在受控 delayed stream 上填满 cap；下一 submit 在零发出前 `RESOURCE_EXHAUSTED`；drain 后可再次 submit。
7. **MDTS**：输出 kernel-reported MDTS、effective MDTS、PRP-list capacity bound；验证 fan-out entry 不超过两者。
8. **device mismatch**：错误 `ctx.device_id` 在零发出前 REJECTED。
9. **capabilities**：逐项与真实 limits 比对；HOST memory submit capability false；multi-stream 暂 false。
10. **launch failure seam**：若真实 CUDA failure 难以稳定触发，允许增加 private test-only injection seam，证明 launch 前失败 → op null/零发出；不得污染 public/SPI。

测试临时文件只放 `/mnt/nvme1/GPU0/resolver_test/` 并清理。

# 8. 你只能修改/创建

- `tutti/data_paths/local_nvme/**`
- `tests/local_nvme_datapath_contract/CMakeLists.txt`
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cu`（仅若把 CUDA 测试 helper 搬出 production）
- `chat/round8/result4.md`

构建只能写 `build/round8-session4*`。

禁止修改：public/SPI、binding、resolver、main/旧 source、libnvm 源码、其他 tests/CMake、根目录两份参考文档。

# 9. 安全限制

禁止 sudo、模块操作、bind/unbind、mkfs/mount/umount、启停 daemon、打开 raw block device、固定 LBA IO。只允许 resolver 解析出的临时测试文件 LBA。禁止触碰 `/mnt/nvme4`。

# 10. 验收

```bash
rm -rf build/round8-session4
cmake -S tests/local_nvme_datapath_contract -B build/round8-session4 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round8-session4 -j8
ctest --test-dir build/round8-session4 --output-on-failure
```

成功标准：

1. 当前 236 断言无回退，新增测试全通过；
2. WRITE pattern 不再是假阳性；
3. shutdown timeout 不释放 in-flight 资源；
4. op-null/zero-issued 不变量在所有失败路径成立；
5. progress 双 hard cap 成立；
6. partial commit 总体非 OK；
7. MDTS/capacity/capabilities 与真实执行一致；
8. daemon/mount/module/RAID 不变；
9. 文件边界、whitespace、EOF、linter 正常。

# 结果落盘

写入 `chat/round8/result4.md`，至少包含：

- 每项 REQUIRED FOLLOW-UP 的 source→fix 对照；
- submit 可逆/不可逆边界；
- shutdown timeout 前后资源计数证据；
- progress budget 实测；
- hardware/effective MDTS 与所有 capability 值；
- 防假阳性 WRITE 输出；
- 当前+新增测试完整计数；
- 环境与文件边界；
- 最终 `PASS` / `BLOCKED`。

不要寒暄、不要提交 Git commit、不要写「总指挥验收」。
