# Round 15 Session 4 结果：批次容量参数化 + simulator 复现 legacy 效果

状态：**完成**。前置 Session 3（Runtime 按 DataPath 分组，已验收）未改动。

构建目录：复用 `build/r15base`（cmake -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON
-DTUTTI_BUILD_HARDWARE_TESTS=ON，S2/S3 沿用的基线目录）。硬件环境：daemon 中途曾掉线，
用户手动重启后（`sudo ./build/bin/tutti_daemon --config sys_config.yaml`，双盘已挂载
`/mnt/nvme1`→`/dev/snvme0n1`、`/mnt/nvme2`→`/dev/snvme1n1`）恢复正常，本 session 全部
硬件验证均在重启后完成。

---

## REQUIRED 1：批次容量参数化

### 改动摘要

`LocalNvmeDataPath` 生产构造函数追加 3 个尾随可选参数（保持位置兼容，旧调用点无需改动）：

```cpp
LocalNvmeDataPath(std::string snvme_dev_path, std::uint32_t bar0_size,
                   std::uint32_t cuda_device, std::uint32_t num_user_queues,
                   std::uint32_t queue_depth, std::uint32_t namespace_id,
                   std::uint32_t block_size,
                   std::uint64_t mdts_bytes = 0,
                   std::uint32_t max_batch_entries = 0,
                   std::uint32_t cq_poll_budget = 0,
                   std::uint32_t handle_cache_capacity = 0,
                   std::uint32_t prp_cache_capacity = 0,
                   std::uint64_t max_in_flight_operations = 0,   // 新增，0=16
                   std::uint64_t max_batch_requests = 0,          // 新增，0=跟随 max_batch_entries
                   std::uint64_t max_request_bytes_override = 0); // 新增，0=entries*effective_mdts
```

- `max_in_flight_operations`：0 → 16（原硬编码值，现改为可配）。
- `max_batch_requests`：0 → 跟随 `max_batch_entries_`（与 S4 之前"两个上限共用一个旋钮"的行为完全一致）；非 0 时可独立于 entries 设置。
- `max_request_bytes_override`：0 → `initialize()` 内按原公式 `max_batch_entries_ * effective_mdts_bytes_` 计算；非 0 时直接作为 `caps_.max_single_io_bytes` / `caps_.max_batch_bytes`。
- 默认值数值（16 / 256）未改动，任何未传新参数的现有调用点（生产代码内未发现其他调用点；`tests/`、`tutti/examples/` 中的调用点已核实）行为逐位一致。

### 内存账（写入构造函数注释）

```
arena_slots = 2 * max_in_flight_operations
bytes/slot  = max_batch_entries *
              (sizeof(DeviceSubmitEntry)[48B] + sizeof(EntryCompletionStatus)[8B]
               + page_size[PRP-list pool，典型 4096B])
total       = arena_slots * bytes/slot
```

- 默认（16, 256）：32 slots × 256 × 4152B ≈ **32.4 MiB**（不变）。
- 大容量示例（in-flight=8, batch_entries=4096）：16 slots × 4096 × 4152B ≈ **259.5 MiB**。
- simulator 用（in-flight=4, batch_entries=4096）：8 slots × 4096 × 4152B ≈ **129.8 MiB**。

拒绝语义不变：`submit()` 入口仍先做 `count > max_batch_requests_` 早退检查，超出即
per-request `RESOURCE_EXHAUSTED`、`op` 保持 `nullopt`（fail-closed），见测试 85/9。

### 新增测试 seam（用于 REQUIRED 2/3 的硬证据）

`LocalNvmeDataPath` 新增只读计数器（不改变任何 IO 行为）：

- `test_submit_call_count()` / `test_kernel_launch_count()` / `test_reset_submit_counters()`
- `test_submit_call_count_` 在 `submit()` 入口无条件 `++`（含被拒绝的调用）。
- `test_kernel_launch_count_` 仅在 `launch_submit_one` 成功返回后 `++`（早退拒绝路径不计入）。

### 改动文件（REQUIRED 1）

- `tutti/data_paths/local_nvme/local_nvme_data_path.h`
- `tutti/data_paths/local_nvme/local_nvme_data_path.cpp`

---

## REQUIRED 2：单 launch 大批次硬件测试

全局 Round 15 测试编号说明：**82/83 已被 Session 3**（`storage_runtime_contract_test.cpp`
的跨 target 合并 mock 测试）占用，本 session 新测试从 **84** 起（已在代码注释中说明，
与 session4.md 原文"编号 82 起"的偏差已记录）。

### 84/85（`tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`，直连 DataPath 层）

- **84. capacity parameterization: constructor knobs** — 构造一个 `in-flight=4 /
  batch_entries=512 / batch_requests=1024(独立于entries) /
  request_bytes_override=16MiB` 的实例，核对 `test_arena_capacity()==8`、
  `capabilities()` 四项均等于配置值；再构造一个全 0（默认）实例，核对
  `test_arena_capacity()==32`、`max_in_flight_operations==16`、
  `max_batch_requests==256`、`max_single_io_bytes==256*effective_mdts`（证明默认公式不变）。13 项断言。
- **85. default capacity regression: oversized batch fail-closed** — 默认容量实例，
  提交 257 个请求（>256），断言 `status.code()==RESOURCE_EXHAUSTED`、`op==nullopt`、
  257 个 `initial_states` 全部 `REJECTED`。8 项断言。

### 8/9（`tests/storage_runtime_local_nvme_contract/storage_runtime_local_nvme_contract_test.cpp`，经 `rt->submit`）

- **8. capacity-configured single-launch big batch**（全局编号 86）：
  - DataPath：`max_in_flight_operations=8`、`max_batch_entries=4096`。
  - 64 个文件 × 8 个 4KiB 请求/文件 = **512 请求跨 64 文件**，位置相关字节 pattern
    `byte(i,p) = (i*131 + p*7 + 13) & 0xFF`（复用 test 76 手法，覆盖全部
    64×32768=2,097,152 字节）。
  - WRITE 与 READ 各一次 `rt->submit`：均断言
    `dp_big.test_submit_call_count() 增量 == 1`、`test_kernel_launch_count() 增量 == 1`。
  - 逐字节回读校验：**0 / 2,097,152 mismatches**。
- **9. default capacity regression via Runtime**：默认容量 DataPath 接入 Runtime，
  257 个（同 target/memory）请求经 `rt->submit` 一次提交，断言
  `status.code()==RESOURCE_EXHAUSTED`、`io` 未置位（fail-closed）。

### 实测输出

```
$ ./bin/tutti_local_nvme_datapath_contract_test
  ...
  PASS
  PASS
  passed: 820
  failed: 0
RESULT: PASS

$ ./bin/tutti_storage_runtime_local_nvme_contract_test
--- 8. capacity-configured single-launch big batch (512 reqs / 64 files) ---
  byte mismatches: 0 / 2097152 (64 files x 32768 bytes)
--- 9. default capacity regression: oversized batch fail-closed ---
=== Summary ===
  passed: 137
  failed: 0
RESULT: PASS
```

（799→820，+21；115→137，+22）

### 改动文件（REQUIRED 2）

- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`（新增 84/85）
- `tests/storage_runtime_local_nvme_contract/storage_runtime_local_nvme_contract_test.cpp`（新增 8/9，顶部注释补充 test-seam 只读访问说明）

---

## REQUIRED 3：simulator 复现 legacy 效果

`tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu`：

- DataPath 构造显式传入 `max_in_flight_operations=4`（原先隐式默认 16），
  `max_batch_entries=4096` 沿用既有值；`max_batch_requests` /
  `max_request_bytes_override` 留 0（跟随 entries / entries×MDTS）。
- 删除"S4b fix：cap=16，必须窗口循环"的过时注释，改为说明当前容量下
  `windowed_submit_wait` 正常路径一轮完成，循环体保留仅作拒收安全网。
- `windowed_submit_wait` 新增 `LocalNvmeDataPath&` 参数与逐轮硬断言：
  每轮 `dp.test_submit_call_count()` 增量必须 `==1`（否则 `STEP_FAIL`），
  accepted 时 `dp.test_kernel_launch_count()` 增量必须 `==1`（否则 `STEP_FAIL`）。
  全局累加 `g_windowed_calls` / `g_windowed_rounds_total` / `g_windowed_multi_round_calls`。
- 程序末尾（Cleanup 前）打印并硬校验 instrumentation 汇总：
  `calls==rounds` 且 `multi_round_calls==0`，否则 `STEP_FAIL`。

### HY3（80L/512C，全部默认参数即 HY3 形状）实测

```
[ OK ] Phase A: 512 files (40.0 GB) in 26.13s
[ OK ] Phase B: 460 hit + 52 miss chunks, 1024 tensors registered
[ OK ] Phase C: opened 512 targets
[ OK ] Phase E: pre-wrote 460 chunks x 80 layers (35.94 GB) in 9.62s
[ OK ] Phase F: auto compute_us=87451 us (read 69.552 ms / 0.48 GB = 6.9 GB/s, write 17.900 ms / 0.05 GB = 3.0 GB/s)
[INFO] pipeline: layers=80 chunks=512 (hit=460 miss=52) compute=87451 us = 29 iters
       (DataPath in-flight=4/batch_entries=4096, 1 submit/layer/direction expected)
[ OK ] Phase G: req 1 12.421s (serial 19.415s, saving 36%) READ 0.48GB=6.9GB/s WRITE 0.05GB=0.6GB/s
[ OK ] Phase G: req 2 12.406s (serial 19.399s, saving 36%) READ 0.96GB=6.9GB/s WRITE 0.11GB=0.6GB/s
[ OK ] SIM TOTAL: 2 req wall=24.827s | READ 77.18GB=6.9GB/s | WRITE 8.72GB=0.6GB/s
       | serial=38.815s overlap 36% (measured: host wall / accepted-bytes IO-time + compute)
[ OK ] Phase H: verified 26 samples, all correct
[INFO] Round15 S4 instrumentation: windowed_submit_wait calls=437 total_rounds=437
       multi_round_calls=0 (expect rounds==calls, multi_round==0 at this capacity)

=== layerwise_kv_overlap: PASSED ===
real 1m5.057s
```

验收对照：

| 项目 | 结果 |
|---|---|
| 每层每方向 DataPath submit==1、kernel launch==1 | **是**（437 次 `windowed_submit_wait` 调用，每次都在 `submit()` 内部逐轮硬断言通过；汇总 `calls(437)==rounds(437)`，`multi_round_calls==0`，即全部单轮/单 submit/单 launch，覆盖 Phase E 预写、Phase F 校准、Phase G 主循环读写、Phase H 校验） |
| Phase H 字节校验 | 26/26 全部正确 |
| 读带宽 | **6.9 GB/s**（SIM TOTAL 汇总 77.18GB/6.9GB/s），≥5GB/s 要求达标 |
| 写带宽 | 0.6 GB/s。**如实说明**：写工作量本身很小（每层仅 52/512=10.2% miss chunks 需写回，每层 54.5MB vs 读 482.3MB），且 write 走独立小批次（52 请求/层，远低于 460 请求/层的读批次），带宽被小 IO 数量与固定开销摊薄；不属于本 session 范围内的"多 launch"问题（write 同样验证过 1 submit/1 launch），是工作负载形状本身决定的，未虚报。 |
| overlap saving | 36%（方法：`(serial_ms - wall_ms) / serial_ms`；`serial_ms` = 该请求 IO 总耗时(read+write windowed_submit_wait 累计 ms) + compute 耗时；`wall_ms` = 该请求实际墙钟耗时。两次请求（req1/req2）均为 36%，同一累计口径下的 SIM TOTAL 亦为 36%） |
| 双盘 `kvlw_*` 清理 | 是（跑后 `find /mnt/nvme1 /mnt/nvme2 -iname "kvlw_*"` 为空；simulator 自带 Cleanup 阶段） |

### 对比表：修复前 vs 修复后

| | 修复前（Round 15 重做前基线） | 修复后（本 session 实测） |
|---|---|---|
| Runtime 分组 | 按 (DataPath,target) 分组，跨文件不合并 | 按 DataPath 分组（S3 已验收），跨 64+ 文件合并进单个 op |
| DataPath 容量 | `max_in_flight_operations` 硬编码 16，不可配 | 三项新增旋钮可配置，默认值不变 |
| 每层每方向 submit/launch 次数 | ~464 次 launches（每个 target 一次） | **1 次**（437 次窗口调用全部单轮/单 submit/单 launch，硬断言通过） |
| 读带宽（简报口径，量级） | ~1.7 GB/s | **6.9 GB/s**（本次 HY3 实测，SIM TOTAL） |

（"修复前"464 launches / 1.7GB/s 为既往会话记录的量级参考，非本 session 重新采集；本
session 的"修复后"数字为当次 HY3 实测。）

### 改动文件（REQUIRED 3）

- `tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu`

---

## REQUIRED 4：回归

- `ctest`（`build/r15base`，daemon 重启后全量重跑）：**19/19 全绿**（含 4 项 hardware
  label：`tutti_local_nvme_datapath_contract_test`、`tutti_storage_runtime_local_nvme_contract_test`、
  `tutti_layerwise_kv_overlap`、`tutti_resolver_contract_test`）。
- `tutti_local_nvme_datapath_contract_test`：**820/0**（799 基线 + 新增 84/85 共 21 项）。
- `tutti_storage_runtime_local_nvme_contract_test`：**137/0**（115 基线 + 新增 8/9 共 22 项）。
- 其余 HOST/CUDA 非硬件 ctest（`tutti_public_api_usage_test` 等 15 项）全部 Passed。
- `git diff --check`：clean（exit 0，无空白字符问题）。
- 双盘 `resolver_test` 目录：跑完后为空（`/mnt/nvme1/GPU0/resolver_test`、
  `/mnt/nvme2/GPU0/resolver_test` 均为空目录）。

---

## 改动文件清单（全部）

- `tutti/data_paths/local_nvme/local_nvme_data_path.h` — 构造函数 3 新参数 + 内存账注释 + test seam 声明。
- `tutti/data_paths/local_nvme/local_nvme_data_path.cpp` — 构造函数实现、`initialize()` 公式改为尊重 override、`submit()` 计数器埋点、accessor 实现。
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp` — 新增 TEST_CASE 84/85。
- `tests/storage_runtime_local_nvme_contract/storage_runtime_local_nvme_contract_test.cpp` — 新增 section 8/9，顶部注释补充 test-seam 说明。
- `tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu` — DataPath 构造显式大容量、`windowed_submit_wait` instrumentation、过时注释清理、末尾汇总校验。
- `chat/round15/result4.md`（本文件）。

未改动：`storage_runtime.h`、`StripedDataPath`（未创建）、任何默认值数值（16/256）。

## 总指挥验收（2026-08-03）

**PASS。legacy 单 launch 效果达成。**

独立复跑与抽查：

- **容量参数化抽查**：构造函数 3 尾随参数在位（`local_nvme_data_path.h:149-151`），0→默认转换语义正确（`:87` ternary、`:84` 跟随 entries、`:89` override 直通）；默认值 16/256 未动；计数 seam 埋点位置正确（submit 入口 `:865` 含拒绝、launch 成功后 `:1328`）。
- **硬件契约复跑**：datapath **820/0**（+21：容量构造 84、默认容量 fail-closed 85）；runtime **137/0**（+22：单 launch 大批次 8=全局86、Runtime 默认容量拒绝 9）——512 请求跨 64 文件单次 submit/launch、2,097,152 字节零 mismatch 复证。
- **simulator 独立复跑（HY3 完整）**：PASSED——每层读 ~6.9-7.0GB/s（单盘饱和，≥5GB/s 达标）、Phase H **26/26**、instrumentation `calls(437)==rounds(437)、multi_round==0`（每次窗口调用均单 submit 单 launch 硬断言）、overlap 37%（测量口径已标注）；写带宽 0.6GB/s 的低值归因（miss 仅 10.2% 工作负载形状）如实合理。
- **环境**：双盘 kvlw_* 与 resolver_test 均空、`git diff --check` clean。
- 编号说明记录规范（84 起，避开 S3 的 82/83）；改动文件清单完整；未触碰 storage_runtime.h 与任何默认值——防缠结规则遵守良好。

**对比基线固化**：修复前 ~464 launches/层、读 ~1.7GB/s → 修复后 **1 launch/层、读 6.9GB/s**（~4×）。

**S5（StripedDataPath fused 单 launch）解除阻塞**：`chat/round15/session5.md` 可启动。S5 可直接复用本 session 的容量旋钮（striped workspace 定容）与计数 seam 模式（单 launch 证据）。
