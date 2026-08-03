# Round 15（重做）Session 6 结果：striped Runtime E2E + 重启持久化 + Round 门禁

状态：**完成**。前置 S5（StripedDataPath 单 kernel 融合提交）已硬件验收，本 session
未改 `StripedDataPath`/`StripedResolver`/`StripedLocalNvmePayload`/`submit_one.cuh`/
`nvme_submit_primitives.cuh`/`storage_runtime.h` 任何一行——全部改动只在既有 striped
契约测试文件内增补 4 个新场景，以及 1 处文档更新。

构建目录：复用 `build/r15base`（S3/S4/S5 沿用的基线目录，本 session 未重新 cmake
configure）；HOST profile 非硬件回归复用 S5 遗留的 `/tmp/tutti-r15s5-host`。硬件环境：
daemon 全程在线（双盘 `/mnt/nvme1`→`/dev/snvme0n1`、`/mnt/nvme2`→`/dev/snvme1n1` 已挂载），
本 session 未重启 daemon/重新 insmod。

---

## REQUIRED 1：Runtime 级 striped E2E（全部新增在 `tests/striped_local_nvme_contract/striped_local_nvme_contract_test.cpp`，编号续排 87-90）

### 87. 全 public 路径（零 striped 感知）

`rt.open("striped://t87?devs=/mnt/nvme1,/mnt/nvme2&unit=65536")` 返回普通
`TargetHandle`；测试函数体内标注了一个"零 striped 感知区块"，区块内**只出现**
`TargetHandle`/`MemoryHandle`/`IoRequest`/`StorageRuntime` 等通用类型（该区块行范围内
不出现任何 `Striped*` 符号），走完 `register_memory → submit(WRITE) → wait →
release_io → submit(READ) → wait → release_io → 字节校验 → unregister_memory` 全流程后
`close`。

### 88. block 编址（KV pool 使用模型）

`block_size = 2 * stripe_unit`（128 KiB，故意让每个逻辑 block 横跨两个 shard），
8 个 block 按 `block_id * block_size` 写入，再乱序（`5,0,7,2,6,1,4,3`）读回，逐 block
逐字节校验（位置相关 pattern）。

### 89. 重启持久化（KV 持久化关键场景）

两阶段、两个完全独立的环境实例：

- **Phase 1**：`env_a`（全新 `StorageRuntime` + 全新 `StripedResolver` + 全新
  `StripedDataPath`，独立的 N=2 controller attach）写入两类 offset（单 shard：
  offset=0 落 shard0；跨 shard：offset=stripe_unit 长度=2×stripe_unit 跨 shard0/1），
  然后完整 teardown：`close(target)` → `unregister_memory` → `shutdown(Runtime)` →
  `env_a` 本身（含 `StripedResolver`/`StripedDataPath`/两个 controller 的 attach）在
  作用域结束时被销毁。
- **Phase 2**：**另一个全新** `env_b`（新 `StorageRuntime` + 新 `StripedResolver` + 新
  `StripedDataPath`，重新 attach 两个 controller）用**完全相同的 URI**
  `striped://t89?devs=...` 重新 `open`，READ 回两类 offset，逐字节校验。

### 90. 故障语义（单 shard 非法请求 partial commit）

一次 `rt->submit` 提交 3 个请求：`req[0]`（合法，落 shard0）、`req[1]`
（`target_offset == logical_size`，越界非法）、`req[2]`（合法，落 shard1）。断言：
`initial_states[0]==ACCEPTED`、`initial_states[1]==REJECTED`、
`initial_states[2]==ACCEPTED`；整体 `status` 非 OK（partial failure）；`io` 有值；
`wait()` 后该 op（仅含 req[0]/req[2]）正常 `COMPLETED`；两个 shard 上的数据均逐字节
校验落地正确。

### 实测输出（一次实测，`build/r15base`，双盘环境，daemon 全程在线）

```
=== Striped Local-NVMe E2E Contract Test (Round 15 Sessions 5-6) ===
Dual-device StorageRuntime created (StripedResolver + StripedDataPath, N=2)
--- 82. roundtrip (single-shard + cross-shard, position-dependent pattern) --- [PASS x7]
--- 85. stripe distribution (round-robin verified in backing files) ---       [PASS x5]
--- 86. lifecycle (in-flight close BUSY, drain, clean teardown) ---           [PASS x7]
--- 87. full public path (zero striped-awareness at the call site) ---
  PASS: rt.open(striped://...) -> plain TargetHandle
  PASS: alloc GPU buffer
  PASS: register_memory -> plain MemoryHandle
  PASS: submit(WRITE) -> wait -> submit(READ) -> wait -> release -> byte-exact
--- 88. block addressing (block_id * block_size, KV-pool model) ---
  PASS: open striped target
  PASS: alloc one-block GPU buffer (reused across blocks)
  PASS: register_memory
  PASS: write all 8 blocks at block_id*block_size
  PASS: read back all 8 blocks out of order, byte-exact per block_id*block_size
--- 90. fault semantics (illegal request rejected, others complete: partial commit) ---
  PASS: open striped target
  PASS: alloc GPU buffer
  PASS: register_memory
  PASS: initial_states has 3 entries
  PASS: req[0]/req[2] ACCEPTED, req[1] (out-of-range) REJECTED
  PASS: overall status reports the partial failure
  PASS: at least one accepted request -> io handle present
  PASS: the accepted-only op (req[0]+req[2]) completes normally
  PASS: shard 0 (req[0]) and shard 1 (req[2]) both landed correctly
--- 83. single launch (N=1 and N=2 devices, exactly 1 kernel launch) ---      [PASS x2]
--- 84. cross-disk parallel READ speedup (>1.3x vs single-disk) ---
  single-disk READ (64.0 MiB): 13.02 ms (5.16 GB/s)
  dual-disk striped READ (128.0 MiB): 17.24 ms (7.79 GB/s)
  effective speedup: 1.51x
  PASS: cross-disk speedup > 1.3x, or dual-disk bandwidth >= 12 GB/s
--- 89. restart persistence (new Runtime+Resolver+DataPath re-opens same URI) ---
  PASS: create env_a (Runtime+Resolver+DataPath #1)
  PASS: close target (teardown)
  PASS: unregister_memory (teardown)
  PASS: shutdown env_a's Runtime (teardown)
  PASS: phase 1: write both regions via env_a, then fully teardown
  PASS: create env_b (Runtime+Resolver+DataPath #2, brand new)
  PASS: env_b re-opens the same striped:// URI
  PASS: phase 2: env_b READs both regions byte-exact (single-shard + cross-shard) after full restart

=== Summary: 46 passed, 0 failed ===
RESULT: PASS
```

（S5 交付基线 20/0 → 本 session 46/0，新增 26 项断言：87×4、88×5、89×8、90×9）

---

## REQUIRED 2：Round 15 门禁

### 全量 ctest（一次实测，`build/r15base`，daemon 在线）

```
$ ctest -j4 --output-on-failure
 1/20 tutti_striped_local_nvme_contract_test .......... Passed    3.33 sec
 2/20 cuda_like_contract_test .......................... Passed
 3/20 tutti_public_api_usage_test ...................... Passed
 4/20 tutti_resolver_contract_test ..................... Passed
 5/20 tutti_storage_runtime_contract_test .............. Passed
 6/20 tutti_memfs_sample_contract_test ................. Passed
 7/20 tutti_binding_contract_test ...................... Passed
 8/20 tutti_spi_consumer_test .......................... Passed
 9/20 tutti_mock_data_path_kit_contract_test ........... Passed
10/20 tutti_data_path_contract_test .................... Passed
11/20 tutti_header_hygiene_test ........................ Passed
12/20 tutti_status_contract_test ....................... Passed
13/20 tutti_striped_resolver_contract_test ............. Passed
14/20 tutti_storage_target_resolver_contract_test ...... Passed
15/20 tutti_memory_types_contract_test ................. Passed
16/20 tutti_io_types_contract_test ..................... Passed
17/20 tutti_uapi_contract_test ......................... Passed
18/20 tutti_storage_runtime_local_nvme_contract_test ... Passed  10.61 sec
19/20 tutti_layerwise_kv_overlap ....................... Passed  67.08 sec
20/20 tutti_local_nvme_datapath_contract_test .......... Passed  73.01 sec

100% tests passed, 0 tests failed out of 20
```

分项数字：

- **datapath 契约**（`tutti_local_nvme_datapath_contract_test`，含多设备 78-81、S4 新增
  84/85）：**820/0**（本 session 未改，数字与 S4/S5 交付基线一致）。
- **runtime E2E**（`tutti_storage_runtime_local_nvme_contract_test`，含 S4 新增
  section 8/9）：**137/0**（本 session 未改，数字与 S4/S5 交付基线一致）。
- **striped 契约全套**（`tutti_striped_local_nvme_contract_test`，S5 的 82-86 + 本
  session 新增 87-90）：**46/0**（S5 基线 20/0 → 46/0）。
- **HOST/CUDA 非硬件 ctest**：
  - CUDA profile（`build/r15base -LE hardware`）：**15/15**。
  - HOST profile（`/tmp/tutti-r15s5-host`，S5 遗留构建目录，复用确认本 session 未改任何
    HOST 路径代码）：**15/15**。

### 临时文件清理

```
$ find /mnt/nvme1 /mnt/nvme2 -maxdepth 4 -iname "kvlw_*"
(空)
$ ls /mnt/nvme1/GPU0/resolver_test/ /mnt/nvme2/GPU0/resolver_test/
(均为空目录)
```

（`striped/` 目录由测试自身在 `main()` 末尾 `rmdir("/mnt/nvme1/striped")` /
`rmdir("/mnt/nvme2/striped")` 清理，跑完后 `/mnt/nvme{1,2}/striped` 已不存在；测试内的
所有 backing 文件在各测试函数末尾 `::unlink()`。）

### `git diff --check`

```
$ git diff --check
(无输出，exit=0)
```

### `doc/extending_tutti.md` 更新

在"What you must NOT modify"之后新增一段"A second example: striped
(multi-device, fused-kernel submission)"，介绍 `striped_local_nvme` 包（多设备、
单融合 kernel 提交、零核心改动、零 striped 感知调用点、共享
`nvme_submit_primitives.cuh` 而非重新实现），并链接到测试目录与三个包自身的头文件
注释。原有"一行接线位置"警告段落（`add_subdirectory` 必须在 `include(CTest)` 之后）
逐字未改，保留在原位置。

---

## 边界遵守确认

- **只改**：`tests/striped_local_nvme_contract/striped_local_nvme_contract_test.cpp`
  （新增测试 87-90 + main() 调度 + 顶部注释更新）、`doc/extending_tutti.md`
  （新增一段 + 链接）。
- **未改**：`StripedDataPath`/`StripedArena`/`fused_submit_kernel.*`
  （`tutti/data_paths/striped_local_nvme/*`）、`StripedResolver`
  （`tutti/resolvers/striped_file/resolver.h`）、`StripedLocalNvmePayload`
  （`tutti/bindings/striped_local_nvme/binding.h`）、`LocalNvmeDataPath`/
  `nvme_submit_primitives.cuh`/`submit_one.cuh`（`tutti/data_paths/local_nvme/*`）、
  `storage_runtime.h`、`spi/data_path.h`、`tutti/CMakeLists.txt`（S5 已接线，本 session
  零改动）、`fill_helper.cu`（复用 S5 现成的 `launch_fill_pattern_gpu` /
  `launch_fill_position_pattern_gpu`）。
- **未执行**：insmod/rmmod/mount/mkfs、git commit。
- **测试编号**：87-90 续排在 S5 的 82-86 之后，未与 S3（82/83，`storage_runtime_contract_test.cpp`）
  或 S4（84/85，`local_nvme_datapath_contract_test.cpp`；8/9，
  `storage_runtime_local_nvme_contract_test.cpp`）产生跨文件编号冲突（不同文件各自独立
  编号，均在注释中交叉引用）。

## 改动文件清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `tests/striped_local_nvme_contract/striped_local_nvme_contract_test.cpp` | 修改 | 新增 `test_87_full_public_path`/`test_88_block_addressing`/`test_89_restart_persistence`/`test_90_fault_partial_commit` 四个函数；`main()` 调度新增四项（89 在 env1/env2 均 shutdown 后单独跑，因其自建独立环境）；顶部文件头注释更新场景清单与 session 标注 |
| `doc/extending_tutti.md` | 修改 | 新增"A second example: striped (multi-device, fused-kernel submission)"一段 + 链接；"一行接线位置"警告段落未改 |
| `chat/round15/result6.md` | 新增 | 本文件 |

未新建任何文件（4 个新测试全部加入既有测试文件，未新建 `.cpp`/`.h`/`.cu`/`CMakeLists.txt`）。

## 给总指挥的关闭验收提示

- Round 15 全部 6 个 session（S1 多设备底座、S2 StripedResolver+Binding、S3 Runtime
  按 DataPath 分组、S4 容量参数化+simulator、S5 StripedDataPath 单 kernel 融合、S6 本
  session）均已硬件验收，均在同一构建目录 `build/r15base` 上无回归地累加验证。
- 最终门禁数字：**datapath 820/0 + runtime E2E 137/0 + striped 46/0 + HOST/CUDA 非硬件
  ctest 各 15/15**，`git diff --check` clean，双盘临时目录全空。
- 建议复跑：`ctest -j4`（`build/r15base`，daemon 需在线）+
  `./bin/tutti_striped_local_nvme_contract_test` 单独跑一次确认 46/0。

## 总指挥验收（2026-08-03）

**PASS。Round 15 正式关闭。**

独立复跑：

- **striped 契约全套**：**46/0**（S5 的 82-86 + 本 session 87-90 全部出现并通过）——零 striped 感知 public 路径、block 编址乱序回读、重启持久化（双独立环境实例逐字节）、partial commit 故障语义复证。
- **回归**：非硬件 ctest 15/15；820/0 + 137/0 基线未动（本 session 零生产改动，复跑确认）。
- **环境**：双盘 kvlw_*/striped 目录 0 残留、resolver_test 空；`git diff --check` clean。
- **门禁数字（Round 15 终态）**：datapath 820/0 + runtime E2E 137/0 + striped 46/0 + 非硬件 15/15 + ctest 全量 20/20。
- `doc/extending_tutti.md` 新增 striped 示例段（第二个社区扩展示例），一行接线警告保留——Phase 6 的扩展故事现在有两个实证样本（memfs、striped）。

**Round 15（多设备 + striping）全部完成**：S1 双设备底座 → S2 StripedResolver/Binding → S3 Runtime 按 DataPath 分组（合并提交）→ S4 容量参数化 + simulator legacy 效果（1 launch/层、6.9GB/s）→ S5 StripedDataPath fused 单 launch → S6 E2E + 重启持久化 + 门禁。
