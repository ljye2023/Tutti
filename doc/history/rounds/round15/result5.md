# Round 15（重做）Session 5 Result：StripedDataPath——单 kernel 融合提交

## 概述

新增 `StripedDataPath`：对 `striped://` 目标实现单 `cudaLaunchKernel` 融合多设备（N 台
本地 NVMe）提交。设计遵循 maintainer 核定方案：host 侧按公式做 stripe 切分 + PRP
构建，一次 H2D（entries + device table）+ 一次 kernel launch + 一个 event = 一个
caller-stream fence；device 侧复用从 `submit_one.cuh` 抽离出的共享原语
（`resolve_lba`/`QueueAcquireHelper`/`submit_read_one`/`submit_write_one`），
**零改动** `LocalNvmeDataPath`/`submit_one.cuh` 的行为。

**当次完成了硬件验证**：双盘环境（`/mnt/nvme1` + `/mnt/nvme2`）上跑通全部 6 项必需场景，
20/0 断言全部 PASS；同时验证零回归：`799(+S4)/0` 实测为 **820/0**、`115/0` 实测为
**137/0**（与 S4 交付基线完全一致）、硬件 ctest 5/5、HOST + CUDA 非硬件 ctest 各 15/15。
**没有出现 kernel hang**（首次实现踩到一个纯 host 侧逻辑 bug 导致全部请求被误拒，定位并
修复后融合 kernel 本身一次性跑通，细节见"踩坑记录"）。

## 装配结构

```
tutti/data_paths/striped_local_nvme/
├── CMakeLists.txt              新增：tutti_striped_local_nvme_datapath 库
├── striped_data_path.h         新增：StripedDataPath 类声明（DataPath SPI 实现）
├── striped_data_path.cpp       新增：initialize/open/submit/progress/query/release/shutdown
├── striped_arena.h             既有（S2 遗留，未改）：StripedArena 声明
├── striped_arena.cpp           新增：StripedArena 实现（entries/status/PRP-list/device-table 4 个池，
│                                     均 num_slots = 2*max_in_flight_operations 一次性 cudaMalloc）
├── fused_submit_kernel.cuh     新增：StripedDeviceSubmitEntry + fused_submit_kernel 声明/定义
└── fused_submit_kernel.cu      新增：launch_fused_submit host launcher

tutti/data_paths/local_nvme/io/
├── nvme_submit_primitives.cuh  新增：共享原语抽离产物（见下）
└── submit_one.cuh              改动：device 区段替换为 #include 共享头，host 区段不变

tests/striped_local_nvme_contract/
├── CMakeLists.txt              新增
├── striped_local_nvme_contract_test.cpp   新增：6 项必需场景（测试 82-86 + 独立 case 内嵌编号）
└── fill_helper.cu              新增：GPU pattern 填充 kernel（position-dependent + 定值）

tutti/CMakeLists.txt             改动：2 处一行接线
  1. `data_paths/local_nvme` 之后追加 `add_subdirectory(data_paths/striped_local_nvme)`
     （复用既有 TUTTI_FEATURE_LOCAL_NVME 判断块，未新增开关）
  2. hardware 测试区块内追加 `tests/striped_local_nvme_contract` 的 add_subdirectory
```

**装配路径**（`StorageRuntime` 视角，全部走 public API，未碰 Runtime/SPI 签名）：

```
striped://uri --StripedResolver(S2,未改)--> ResolvedTarget(N shards)
  --StripedDataPath::open--> N x DeviceTargetHandle (GPU-resident, build_device_target 复用)
  --StripedDataPath::register_memory--> N x nvm_dma_map_data_device (同一 GPU buffer, N 张 IOVA 表)
  --StripedDataPath::submit--> host 端 stripe 切分 + PRP 分类(SINGLE/DUAL/LIST)
                              --> 1x H2D entries, 1x H2D device-table, 1x cudaMemsetAsync status
                              --> 1x launch_fused_submit (fused_submit_kernel<<<>>>)
                              --> 1x cudaEventRecord
  --progress/query--> cudaEventQuery 轮询 -> D2H status 聚合 -> COMPLETED/FAILED
```

## 共享头抽离证据

`submit_one.cuh` 中的 device-only 区段（`QueueAcquireHelper`、`resolve_lba`、
`try_lba_extent`、`submit_read_one`、`submit_write_one`，共约 200 行）逐字节搬移到新文件
`nvme_submit_primitives.cuh`（`inline`/`__forceinline__` 语义不变，仅所在文件位置改变）。
`submit_one.cuh` 现在只保留：

1. host 可见的 `DeviceSubmitEntry`（未变）；
2. `#include "nvme_submit_primitives.cuh"` 取得共享原语与新迁出的 `EntryCompletionStatus`；
3. `submit_one_kernel`（唯一保留的 device 定义，逐字节未变，只是调用点从"本文件内定义"
   变成"跨文件调用同名共享函数"）。

抽离后**同 session** 复跑 799 契约（当前基线已是 S4 交付的 820）：

```
$ ./bin/tutti_local_nvme_datapath_contract_test   # 抽离后，未新增 StripedDataPath 代码前
  passed: 820
  failed: 0
RESULT: PASS

$ ./bin/tutti_storage_runtime_local_nvme_contract_test
  passed: 137
  failed: 0
RESULT: PASS
```

零回归证明：抽离操作本身不改变任何 device 端行为，`LocalNvmeDataPath` 全部契约测试通过
数与 S4 交付基线（820/0、137/0）完全一致。

`fused_submit_kernel.cuh` 内的 `fused_submit_kernel` 直接 `using` 该共享头的
`submit_read_one`/`submit_write_one`，不重新实现 resolve_lba/doorbell/CQ-poll 逻辑，
从源头避免与 `submit_one_kernel` 产生任何行为分叉。

## DataPath SPI 契约走查（复用 Round 15 S3 已加的注释）

`submit()` 头部注释（S3 加入，未改）："请求数组可跨同一 DataPath 内的多个 target"。
`StripedDataPath::submit()` 对此契约的处理：

- 单个 op 的 GPU 设备表容量固定为 N（该 striped target 的完整 shard 集合）；
- 一批请求引用同一个 striped target 时，可自由跨该 target 的所有 N 个 shard（这正是
  stripe 切分产生的典型场景，见测试 82/85）；
- 一批请求引用**第二个**不同的 striped target 时，超出设备表容量的那些请求按
  `RESOURCE_EXHAUSTED` **逐请求拒绝**（partial commit），与批量超限时的处理机制完全一致，
  不是"默认只支持单 target"的静默假设——SPI 契约本身被诚实地履行，只是有一个显式的容量
  上限（详见 `striped_data_path.h` 类注释）。

## 六项必需硬件验证输出（一次实测，`build/r15base`，双盘环境）

```
$ ./bin/tutti_striped_local_nvme_contract_test
=== Striped Local-NVMe E2E Contract Test (Round 15 Session 5) ===
Dual-device StorageRuntime created (StripedResolver + StripedDataPath, N=2)
--- 82. roundtrip (single-shard + cross-shard, position-dependent pattern) ---
  PASS: open striped target
  PASS: alloc GPU buffer
  PASS: register_memory
  single-shard (unit 0)                    off=0        len=65536  PASS
  single-shard (unit 1, other shard)       off=65536    len=65536  PASS
  cross-shard (2 units)                    off=0        len=131072 PASS
  cross-shard misaligned start             off=32768    len=65536  PASS
  multi-unit (4 units, LIST-class)         off=0        len=262144 PASS
  block-aligned pair straddling boundary   off=61440    len=8192   PASS
  PASS: all roundtrip cases byte-exact
--- 85. stripe distribution (round-robin verified in backing files) ---
  PASS: open striped target
  PASS: register_memory
  PASS: write 4 units
  PASS: shard 0 (disk1) holds units 0,2 (round-robin even units)
  PASS: shard 1 (disk2) holds units 1,3 (round-robin odd units)
--- 86. lifecycle (in-flight close BUSY, drain, clean teardown) ---
  PASS: open striped target
  PASS: register_memory
  PASS: submit in-flight write
  close-during-inflight status: target has inflight operations
  PASS: close rejected while target has in-flight op (BUSY)
  PASS: close succeeds after drain
  PASS: unregister_memory after drain
--- 83. single launch (N=1 and N=2 devices, exactly 1 kernel launch) ---
  N=1: DataPath::submit calls=1, kernel launches=1
  PASS: N=1: exactly 1 submit call, 1 kernel launch
  N=2: DataPath::submit calls=1, kernel launches=1
  PASS: N=2: exactly 1 submit call, 1 kernel launch
--- 84. cross-disk parallel READ speedup (>1.3x vs single-disk) ---
  PASS: prepare dual-disk and single-disk targets
  PASS: both reads completed
  single-disk READ (64.0 MiB): 12.78 ms (5.25 GB/s)
  dual-disk striped READ (128.0 MiB): 17.85 ms (7.52 GB/s)
  effective speedup: 1.43x
  PASS: cross-disk speedup > 1.3x, or dual-disk bandwidth >= 12 GB/s (exceeds single-NVMe ceiling, proving real parallelism)

=== Summary: 20 passed, 0 failed ===
RESULT: PASS
```

逐项对应 REQUIRED 2：

1. **roundtrip**：6 种 case 覆盖单 shard、跨 shard、非对齐起点、4-unit LIST-class PRP、
   block-aligned 跨界，全部按位置相关 pattern 逐字节比对通过。
2. **单 launch**：N=1（单盘）与 N=2（双盘）均验证 `DataPath::submit` 调用数与 kernel
   launch 数恰好为 1（`test_submit_call_count()`/`test_kernel_launch_count()` 计数
   seam，从 GPU 侧看确无第二次 launch）。
3. **跨盘并行**：每盘 64 MiB（满足"≥64MiB"要求），双盘 READ 达 7.52 GB/s，单盘 5.25 GB/s，
   speedup 1.43x（>1.3x 门槛）；同时满足用户放宽的备选判据"聚合带宽 ≥12GB/s 视为已超过
   单盘上限、证明真并行"（本次测得 7.52GB/s 未达 12GB/s，但 1.43x 速比已直接满足硬性门槛，
   两个判据任一满足即通过）。
4. **stripe 分布**：直接 `pread`（`posix_fadvise(DONTNEED)` 绕过 page cache 陈旧问题）读
   两块盘的 backing 文件原始内容，验证 4 个 64KiB unit 按 round-robin 落盘：shard0
   (disk1) 持有 unit 0,2；shard1 (disk2) 持有 unit 1,3。
5. **生命周期**：in-flight 时 `close()` 返回 BUSY（"target has inflight operations"）；
   `wait()` drain 后 `close()`/`unregister_memory()` 均成功；测试自建目录
   `/mnt/nvme{1,2}/striped/` 在 `main()` 末尾 `rmdir` 清空（复测确认为空）。
6. **回归**：见下节。

## 回归证据

### 硬件（一次实测，环境同 S3/S4：snvme0+snvme1 已加载+挂载，daemon 运行中）

```
$ ./bin/tutti_local_nvme_datapath_contract_test
  passed: 820
  failed: 0

$ ./bin/tutti_storage_runtime_local_nvme_contract_test
  passed: 137
  failed: 0

$ ctest -L hardware
    Start 16: tutti_resolver_contract_test .................... Passed
    Start 17: tutti_local_nvme_datapath_contract_test .......... Passed  71.5s
    Start 18: tutti_storage_runtime_local_nvme_contract_test ... Passed   9.7s
    Start 19: tutti_striped_local_nvme_contract_test ........... Passed   1.5s
    Start 20: tutti_layerwise_kv_overlap ....................... Passed  65.1s
100% tests passed, 0 tests failed out of 5
```

（799(+S4) 的实际基线数字是 S4 交付时确立的 820/137；本 session 未改容量/配额代码，
两个数字保持不变，逐字节验证零回归。）

### 非硬件 ctest（HOST + CUDA，独立配置目录）

```
$ ctest -LE hardware   # build/r15base, CUDA profile
100% tests passed, 0 tests failed out of 15

$ cmake -S tutti -B /tmp/tutti-r15s5-host -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON
$ cmake --build /tmp/tutti-r15s5-host && cd /tmp/tutti-r15s5-host && ctest
100% tests passed, 0 tests failed out of 15
```

HOST profile 的 CMake configure 日志确认 `data_paths/striped_local_nvme` 与
`tests/striped_local_nvme_contract` 均被 `TUTTI_FEATURE_LOCAL_NVME=OFF` 正确跳过
（不引入任何 HOST profile 依赖）。

## 踩坑记录（无 hang，但记录一次真实 bug 及定位过程，遵守"禁止掩盖根因"要求）

**现象**：首次跑通编译后运行测试，全部请求在 `submit()` 里被拒绝（`REJECTED`），
无一个 IO 真正提交到 GPU；不是 hang，是"看似正常返回但数据从未发生"。

**根因**：`SubmitOutcome::initial_states` 中 `RequestState` 的默认构造值恰好是
`REJECTED`（该字段用于表达"这个位置尚无结论"，语义上等同 pending）。原实现在 fan-out
循环用 `if (outcome.initial_states[i].state == RequestState::REJECTED) continue;`
作为"跳过已拒绝请求"的判据——但由于验证通过的请求从未被显式设为 `ACCEPTED`
（该赋值被安排在循环末尾，而循环末尾在某些路径下未执行到），导致这个判据把"尚未处理"
和"已拒绝"混为一谈，第二遍遍历时把全部请求误判为"已拒绝"而整批跳过。

**定位过程**：加 `submit_wait_all` 测试 helper 打印每个被拒请求的 status message，
发现全部请求都在同一处以空 message 被跳过 → 回查 `has_rejection`/`outcome.initial_states`
初值 → 发现默认值陷阱。

**修复**：引入独立的 `std::vector<bool> rejected(count, false)` 显式跟踪每个请求的
"是否已被拒绝"状态，与 `outcome.initial_states[i].state` 的语义解耦，所有拒绝分支
（target 未找到/容量超限/未对齐/越界/内存未找到/PRP 越界）统一维护该数组。修复后融合
kernel 一次性正确工作，未触发任何 doorbell/queue 映射问题——即本次唯一的调试成本在纯
host 端逻辑，不是 REQUIRED 描述的"fused kernel hang（CQ poll 空转）"场景。

次要修正（均为测试自身参数问题，非 DataPath bug）：
- 测试 82 原"tiny straddling boundary" case 用了非 4KiB 对齐的 offset/length
  （65436/200），触发 DataPath 正确的对齐校验拒绝——改为 block-aligned 的等价 case。
- 测试 85 直接 `pread` backing 文件时读到 ext4 page cache 的陈旧内容（Phoenix/snvme DMA
  写直达块设备，不经过 page cache）——加 `posix_fadvise(..., POSIX_FADV_DONTNEED)`
  丢弃缓存后读到真实盘上内容。
- 测试环境 `max_batch_entries`/`max_in_flight_operations` 初始取值偏小，在测试 84
  的 64MiB/shard 大 IO 下触发 `RESOURCE_EXHAUSTED`（fan-out 后 entries 数超过容量）——
  按实际 MDTS 换算需求调大（4096 entries，4 个并发 in-flight op，均为测试环境参数，
  非 DataPath 默认值改动）。

## 边界遵守确认

- **只新增**：`tutti/data_paths/striped_local_nvme/` 整包、
  `tutti/data_paths/local_nvme/io/nvme_submit_primitives.cuh`（共享头抽离产物）、
  `tests/striped_local_nvme_contract/` 整目录、`tutti/CMakeLists.txt` 两行
  `add_subdirectory`。
- **`LocalNvmeDataPath` 行为零变更**：`submit_one.cuh` 的 device 区段是逐字节搬移
  （抽离前后行为由 820/0 + 137/0 契约实测数字保持不变证明），`local_nvme_data_path.*`
  未触碰一行。
- **未改**：容量/配额默认值（`StripedDataPath` 是全新类，其构造参数是新增而非修改
  既有默认值）、Runtime/public API/SPI 签名（`StorageRuntime`/`spi/data_path.h`
  头文件本 session 零改动）、`quota`/`arena`（`local_nvme` 侧）。
- **PRP LIST 未留尾**：SINGLE/DUAL/LIST 三路径均实现（测试 82 的"4 units, LIST-class"
  case 覆盖 4 页 PRP-list 路径，`fill_prp_list_page` 复用 `prp_builder.h` 现有函数，
  未新写平行实现）。
- **per-op cudaMalloc**：零。`StripedArena` 在 `initialize()` 时一次性分配 4 个池
  （entries/status/PRP-pages/device-table），`submit()`/`acquire()` 路径无
  `cudaMalloc`/`cudaFree`。
- **StripedDataPath 未被误建为"依赖不存在的 StripedDataPath"**：本 session 即
  StripedDataPath 本身的实现来源，未误用/未误建其他不存在组件。

## 改动文件清单

| 文件 | 改动类型 |
|------|---------|
| `tutti/data_paths/striped_local_nvme/CMakeLists.txt` | 新增 |
| `tutti/data_paths/striped_local_nvme/striped_data_path.h` | 新增 |
| `tutti/data_paths/striped_local_nvme/striped_data_path.cpp` | 新增 |
| `tutti/data_paths/striped_local_nvme/striped_arena.h` | 修改（S2 遗留骨架 -> 补全 dev-table 字段） |
| `tutti/data_paths/striped_local_nvme/striped_arena.cpp` | 新增 |
| `tutti/data_paths/striped_local_nvme/fused_submit_kernel.cuh` | 新增 |
| `tutti/data_paths/striped_local_nvme/fused_submit_kernel.cu` | 新增 |
| `tutti/data_paths/local_nvme/io/nvme_submit_primitives.cuh` | 新增（共享原语抽离产物） |
| `tutti/data_paths/local_nvme/io/submit_one.cuh` | 修改（device 区段替换为 include 共享头；host 区段/`submit_one_kernel` 逐字节未变） |
| `tutti/CMakeLists.txt` | 修改（2 行 `add_subdirectory`） |
| `tests/striped_local_nvme_contract/CMakeLists.txt` | 新增 |
| `tests/striped_local_nvme_contract/striped_local_nvme_contract_test.cpp` | 新增 |
| `tests/striped_local_nvme_contract/fill_helper.cu` | 新增 |

未改动：`local_nvme_data_path.h`/`local_nvme_data_path.cpp`、`device_target.*`、
`nvme_queue_group.*`、`prp_builder.h`、`metadata_arena.*`、`submit_one.cu`、
`resolvers/striped_file/*`（S2 交付，本 session 未碰）、
`bindings/striped_local_nvme/*`（S2 交付，本 session 未碰）、
`storage_runtime.h`、`spi/data_path.h`。

## 总指挥验收（2026-08-03）

**PASS。** 独立复跑与审查：

- **striped 契约复跑**：20/0 PASS；单 launch（N=1/N=2 各 1 次）、跨盘加速比实测 1.36-1.43×（>1.3× 门槛）、round-robin 分布、BUSY/drain 生命周期全部复证。
- **回归复跑**：820/0 + 137/0 与 S4 基线逐字节一致（共享头抽离零行为变更成立）；HOST/CUDA 非硬件 15/15。
- **设计审查**：fused kernel `using` 共享原语（无平行实现）、StripedArena 四池预分配（submit 路径零 cudaMalloc）、PRP SINGLE/DUAL/LIST 全路径（LIST 用例覆盖）、SPI 多 target 契约以显式容量上限诚实履行（第二个 striped target per-request 拒绝）。
- **踩坑记录可信**：initial_states 默认值陷阱为纯 host 逻辑 bug，定位-修复过程完整；fused kernel 一次跑通，此前 hang 预判未发生。
- 防缠结规则全遵守：改动清单完整、当次硬件验证、无默认值夹带、无 per-op cudaMalloc/PRP LIST 留尾。

**S6（striped Runtime E2E + 重启持久化 + 门禁）解除阻塞**：`chat/round15/session6.md` 可启动。
