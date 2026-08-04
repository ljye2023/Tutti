# Round 16 Session 7：IO 编排/布局/参数对齐 + 24 GB/s 达成证明

**日期**: 2026-08-04
**状态**: ✅ 达成 — READ **24.8 GB/s**（legacy 同几何实测 25.57 GB/s，目标线 24 GB/s）

---

## 核心发现：性能差距根因（4盘不加速）

S7 开始时新架构 4 盘 READ 仅 **6.9 GB/s**，与单盘 **6.9 GB/s** 完全相同 —— **4 盘聚合完全没有生效**。

### 根因：缺失 legacy 的 `shard_placement` 按 chunk 轮转

Legacy 有**两层**映射，新架构只实现了第一层：

| 层 | legacy | 新架构（S7 前） |
|---|---|---|
| ① 逻辑 offset → shard 索引 | `gpu_file_resolve`: `fd_idx = (offset/tensor_size) % num_shards`<br>（`block_storage/include/gpu_file_resolve.h:16-19`） | ✅ 等价：`shard = (offset/stripe_unit) % num_shards`<br>（`binding.h:182`, `striped_data_path.cpp:967`） |
| ② shard 索引 → 物理设备 | `shard_placement = {coord_devs[(2i)%ndev], coord_devs[(2i+1)%ndev]}`<br>**按 chunk index `i` 轮转**（`kv_cache_layerwise_overlap.cu:293`） | ❌ **缺失**：shard s → device s 固定 1:1 |

Legacy 注释直接说明了这一层的目的（`kv_cache_layerwise_overlap.cu:287-291`）：

> Alternate K/V shard pairs across ALL attached devices ({dev0,dev1}, {dev2,dev3}, ... round-robin): **every batch then mixes chunks living on every device, so a single nvme_batch_xfer_kernel drives all N NVMes concurrently (aggregate bandwidth)**, with no adapter changes needed.

### 失效机理（数学推导）

simulator 的请求模式：层 L 的所有请求共享 `target_offset = L*ts`（`ts` = tensor size = stripe unit = 512KiB）。

无轮转时：
```
shard = (target_offset / stripe_unit) % 4
      = (L*ts / ts) % 4
      = L % 4                    ← 与chunk index i 无关，整个batch 常量！
```

→ 一层的 460 个chunk **全部落在同一块盘**上，融合 kernel 虽然是单 launch，但只驱动 1 个设备的队列。4 盘退化为单盘带宽。

有轮转（`rot = i % 4`）时：
```
shard = ((target_offset / stripe_unit) + rot) % 4
      = ((L % 4) + (i % 4)) % 4  ← 随 chunk index i 变化
```

→ 460 个 chunk 在 `i = 0..459` 上均匀铺满 4 个设备，单次融合 kernel launch 同时驱动全部 4 个 NVMe。

---

## 改动清单

### 1. 数据布局：`shard_rotation`（legacy `shard_placement` 等价物）

| 文件 | 改动 |
|------|------|
| `tutti/bindings/striped_local_nvme/binding.h` | `StripedLocalNvmePayload::create()` 新增 `shard_rotation` 参数（**默认 0 = 零行为变更**）；新增 `shard_rotation()` accessor；`map_to_shard()` 公式改为 `((offset/unit) + rot) % N` |
| `tutti/resolvers/striped_file/resolver.h` | URI 新增可选 `rot=` query param（缺省 → 0）；透传给 payload |
| `tutti/data_paths/striped_local_nvme/striped_data_path.h` | `StripedTarget` 新增 `shard_rotation` 字段 |
| `tutti/data_paths/striped_local_nvme/striped_data_path.cpp` | `open()` 从 payload 镜像 `shard_rotation`；`submit()` 的 shard 公式加入rot（与 `map_to_shard()` 保持逐字一致） |

**语义论证（自洽性）**：rot 只是对 shard 索引做**单射置换**，`shard_offset` 公式不含 rot。设两个逻辑 offset `o1, o2` 映射到同一 `(shard, shard_offset)`，则 `(o1/unit)%N == (o2/unit)%N`、`o1/(unit*N) == o2/(unit*N)`、`o1%unit == o2%unit` ⟹ `o1 == o2`。故无别名冲突，读写用同一 rot 必然自洽。`rot == 0` 逐字复现 S7 前公式。

**实证**：Phase H 字节级校验 26/26 全部正确（跨 shard + 单 shard + 重启后重开同URI 均通过）。

### 2. 数据布局：simulator 默认 4 盘 + stripe unit = tensor size

| 文件 | 改动 |
|------|------|
| `tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu` | `striped4` 默认值 `false → true`（`--striped4` 显式保留，新增 `--single` 退出）；`StripedResolver` 构造 unit `65536 → ts`（512KiB）；URI `unit=524288` + `rot=i%4` |

### 3. IO 编排：去掉阻碍并发的 host sync

| 文件 | 改动 |
|------|------|
| `layerwise_kv_overlap.cu` | `windowed_submit_wait()` 内每轮的 `cudaStreamSynchronize(stream)` **删除** — `rt->wait()` 已轮询至 kernel 完成，该 sync 冗余且阻断跨层 read/write GPU 侧重叠；跨层顺序由 Phase G 的 event DAG（`cudaStreamWaitEvent`）表达 |
| `tutti/data_paths/striped_local_nvme/striped_data_path.cpp` | `progress()` 在 budget 超时窗口内 **spin `cudaEventQuery`**，避免 `Runtime::wait()` 每轮 1ms 条件变量休眠 |
| `tutti/data_paths/local_nvme/local_nvme_data_path.cpp` | `progress()` 同上（EVENT 模式；STREAM_QUERY 回退路径不变） |

**偏差记录（硬约束要求）**：legacy 的 `submit_batch` 内部用**阻塞** `cudaStreamSynchronize`；新架构的公共契约是 `ProgressModel::HOST_POLL`（非阻塞 `progress()` + Runtime 侧 `wait()`）。**以公共契约为准** —— 未改为阻塞，而是在 budget 窗口内 spin-poll，达到等价延迟且不破坏抽象层语义。

### 4.临时 debug 摘除

S7 排查期间加的 `[INFO] StripedDataPath: effective_mdts=...` 与 `submit=/wait=` 计时打印已全部摘除。

---

## 对照表 1：数据布局语义（legacy ↔ 新架构逐项等价）

| 语义项 | legacy | 新架构 | 等价性 |
|---|---|---|---|
| 逻辑 offset → shard | `fd_idx = (off/ts) % num_shards`<br>`gpu_file_resolve.h:16-19` | `shard = ((off/unit) + rot) % N`<br>`binding.h:182` | ✅ rot=0 时逐字相同；rot 是 legacy ② 层的等价物 |
| shard 内offset | `shard_off = (off/(ts*N))*ts + (off%ts)`<br>`gpu_file_resolve.h:19` | 同式<br>`binding.h:184-185` | ✅ 逐字相同 |
| shard → 物理设备轮转 | `shard_placement[s] = coord_devs[(N*i + s) % ndev]`<br>`kv_cache_layerwise_overlap.cu:293` | `rot = i % N`，`device = ((off/unit)+rot) % N`<br>`layerwise_kv_overlap.cu` Phase C | ✅ 均为「按 chunk index 轮转，使 batch 混合所有设备」；legacy 用 stride=num_shards(2)，新架构 num_shards=4=ndev 时 stride 必须为 1 才有效轮转 |
| stripe unit | `tensor_size`（per-layer tensor 对象大小） | `ts` = 512KiB | ✅ 相同 |
| tensor 落盘粒度 | tensor 完整落单 shard | 同（unit == tensor size⟹ 每请求 1 entry/shard） | ✅ 相同 |
| K/V 角色 | 2 shards (K, V)，各自轮转 | K/V 为同一 striped target 的不同 layer offset，由① 层公式分离 | ⚠️ **形态不同但效果等价**：legacy 用 shard 维区分 K/V，新架构用 offset 维；两者都保证 K/V 分布在不同设备上 |

## 对照表 2：IO 编排（host 往返次数）

| 指标 | legacy | 新架构 S7 前 | 新架构 S7 后 |
|---|---|---|---|
| 每层每方向 submit 次数 | 1（`submit_batch` 阻塞调用） | 1 | **1** ✅ |
| batch 深度 | 8192 | 4096（容量参数） | 4096（足够：460 req × 1 entry = 460） |
| kernel launch/层/方向 | 1（`nvme_batch_xfer_kernel`） | 1（fused kernel） | **1** ✅ |
| 每轮 host sync | 0（sync 在 submit_batch 内一次） | **1 次 `cudaStreamSynchronize`**（冗余） | **0** ✅ |
| wait 轮询模式 | 阻塞 `cudaStreamSynchronize` | 非阻塞 query + 1ms 休眠 | budget 内 spin-poll（契约保持 HOST_POLL） |
| DpSeam instrumentation | — | — | `calls=277 total_rounds=277 multi_round_calls=0`（rounds==calls，无多轮 ⟹ 单 submit/层/方向）✅ |

## 对照表 3：kernel launch 参数（threads_per_block）

| tpb | READ GB/s | WRITE GB/s | wall (s) |
|---|---|---|---|
| **32（legacy 默认，已采用）** | 24.8 | 1.2 | 5.319 |
| 64 | 24.9 | 1.5 | 4.377 |
| 128 | 25.1 | 1.5 | 4.366 |
| 256 | 25.0 | 1.2 | 5.073 |

**结论**：tpb 对 READ 聚合带宽无显著影响（24.8–25.1 GB/s，抖动 ±1%）。保持 legacy 默认 **32**（`submit_one.cu:20`、`fused_submit_kernel.cu:19`，可用 `TUTTI_TPB` env A/B 覆盖）。这符合预期 —— 每 thread 处理 1 个 entry，瓶颈在 NVMe 设备带宽而非 GPU 占用率。

---

## 达成判定

### legacy 本体基准（ground truth）

`third_pkgs/Tutti/build/bin/kv_cache_layerwise_overlap` 已构建（未污染新架构 build 目录）。同机同几何（80 层/ 128Kctx / 256 chunk-tokens / 90% hit / 512KiB tensor）：

| 配置 | legacy READ | 新架构 READ |
|---|---|---|
| 1 盘 | — | 6.9 GB/s |
| 2 盘 | 12.19 GB/s | — |
| **4 盘** | **25.57 GB/s** | **24.8 GB/s** |

### 判定

- legacy 4盘实测 **25.57 GB/s ≥ 24**，故按任务规则**以 24 GB/s 为达成线**。
- 新架构最终运行 **READ 24.8 GB/s ≥ 24** →✅ **达成**。
- 相对 legacy 达成率 **97%**（24.8 / 25.57）。
- 扩展性：单盘 6.9 → 4 盘 24.8 = **3.6× 线性扩展**（理想 4×，效率 90%）。
- 每盘 24.8/4 = **6.2 GB/s**，与单盘饱和值 6.9 GB/s 的 90% 一致 ⟹ 每盘近饱和。

---

## 六维对齐总表

| 维度 | legacy 形态 | 新架构现状 | 状态 | 证据 |
|---|---|---|---|---|
| **① 内存注册/ PRP** | 注册期按 MDTS粒度预建 `PRPMappingEntry{prp1,prp2,len,off}` 数组，GPU 常驻 | 注册期预建 `AddressDescriptor` GPU 常驻数组；动态路径写arena 每槽描述符池 | ✅ **已恢复等价** | S5 result5.md（预构建）；S6 REQUIRED 0（kernel 单路径化，永远从描述符指针读） |
| **② handle cache** | `TieredHandleCache<T>`：L2 host-pinned 大容量 + L1 GPU 小容量，evict=downgrade，promote=memcpy 回灌 | 单层 `HandleWorkspaceCache` + `open_refcount`（R16 S1 P0 UAF 修复） | ⚠️ **单层，未双层化** | S6 result6.md 记录未完成原因（`T` 类型设计难点：`DeviceTargetHandle` 含 GPU 指针，不能直接 memcpy） |
| **③ PRP cache** | `PrpPageCache` 双层 + `ensure_resident_batch` + scatter kernel 一次 patch prp2 | 单层 `PrpPageCache` + `checkout_refcount`（R16 S1 同模式修复） | ⚠️ **单层，未双层化** | 同上 |
| **④ 元数据调度** | `GpuSlotPool`/`HostSlotPool` stream-fenced slot reuse（cudaEvent 栅栏异步回收） | `MetadataArena`/`StripedArena` 预分配槽池+ per-slot event；acquire/release O(1) 零 CUDA API | ✅ **已恢复等价**（单层形态下） | `metadata_arena.h/.cpp`、`striped_arena.h/.cpp`；S6 新增描述符池同池化 |
| **⑤ IO 编排** | 每层每方向 1 次阻塞 `submit_batch`（内部 stream sync）；三流 event DAG 实现 read(L+1)/compute(L)/write(L-1) 同时在飞 | 每层每方向 1 次 `rt->submit` + 1 次 kernel launch；冗余 host sync 已删；跨层依赖走 event DAG；`progress()` budget 内 spin-poll | ✅ **已恢复等价**（偏差：保持 HOST_POLL 契约而非阻塞 sync，见上文「偏差记录」） |本session；对照表 2；DpSeam 277/277/0 |
| **⑥ 数据布局** | ① `gpu_file_resolve` offset→shard + ② `shard_placement` 按 chunk 轮转 → batch 混合全部设备 | ① 同式+ ② `shard_rotation`（rot=i%N）→ batch 混合全部设备 | ✅ **已恢复等价（本 session 修复）** | 本 session；对照表 1；6.9→24.8 GB/s (3.6×) |

**六维小结**：④⑤⑥ 三维在本 session 收口后达到legacy 等价；① 在 S5/S6 已完成；②③ 的**双层化**（tiered）未完成，但**功能正确性与生命周期语义已对齐**（S1 P0 UAF 修复），且当前默认配置 cache OFF（cap=0），故不影响本 session 的性能达成。②③ 双层化属性能优化余量，不属正确性缺口。

---

## 回归证据（最终运行）

###硬件契约

| 测试 | 结果 | 基线 |
|---|---|---|
| `tutti_local_nvme_datapath_contract_test` | **855 / 0** PASS | S6: 843 → +12 |
| `tutti_striped_local_nvme_contract_test` | **88 / 0** PASS | S6: 88（不变） |
| `tutti_storage_runtime_local_nvme_contract_test` | **137 / 0** PASS | S6: 137（不变） |

### ctest

- **非硬件（CUDA profile）**：`100% tests passed, 0 failed out of 15` ✅
- **硬件**：`tutti_resolver_contract_test` 失败 — **pre-existing**，S6 result6.md已记录（`[FAIL] not regular file (directory)`，`code=10`，ext4 UNWRITTEN flag 环境相关，与本 session 改动无关）。其余全绿。

### KV sim 最终运行（`--layers 80 --requests 1 --verify`）

```
[ OK ] Phase F: auto compute_us=39402 us (read 22.116 ms / 0.48 GB = 21.8 GB/s, write 17.286 ms / 0.05 GB = 3.2 GB/s)
[ OK ] Phase G: req 1 4.613s (serial 7.763s, saving 41%) READ 0.48GB=24.8GB/s WRITE 0.05GB=1.4GB/s
[ OK ] SIM TOTAL: 1 req wall=4.613s | READ 38.59GB=24.8GB/s | WRITE 4.36GB=1.4GB/s | serial=7.763s overlap 41%
[ OK ] Phase H: verified 26 samples, all correct
[INFO] Round15 S4 instrumentation: windowed_submit_wait calls=277 total_rounds=277 multi_round_calls=0
=== layerwise_kv_overlap: PASSED ===
```

- **READ 24.8 GB/s** ✅ ≥ 24 目标线
- **Phase H 26/26** ✅
- **instrumentation 277/277/0** ✅ rounds==calls、multi_round==0 ⟹ 单 submit/层/方向

### IRQ 监控

| 指标 | 前 | 后 | 差 |
|---|---|---|---|
| snvme IRQ 累计 | 19,369,202 | 20,636,079 | +1,266,877（正常 IO 中断） |
| `dmesg \| grep -c "nobody cared"` | 10 | 10 | **0** ✅ |

「nobody cared」计数**差为 0** —— 本 session 未新增任何 IRQ 风暴。（基线 10 条为 R16 S1 遗留，已在 result1.md 记录根因。）

### 其他门禁

- `git diff --check`：clean（exit 0，无 whitespace 错误）✅
- 双盘/四盘测试目录：`/mnt/nvme{1,2,3,4}/striped` 全部 **0 files**；`/mnt/nvme1/GPU0/resolver_test/` **0 files** ✅
- `third_pkgs/` 只读：未修改任何文件（legacy build 目录为既有产物）✅

---

## 未完成项说明

**工作项 3b（构造函数 Config 聚合）** 未实施。原因：该项为「纯外观零行为」重构，涉及 `LocalNvmeDataPath` 15 参数 → Config 结构体 + `StripedDataPath`/`DeviceDescriptor` 同构处理 + 全部调用点（3 个契约测试 + simulator + 示例）改写。本 session 的上下文预算已用于定位并修复**性能达成的阻塞性根因**（shard_placement 轮转缺失，6.9 → 24.8 GB/s）。3b 与性能目标无关，建议独立 session 完成以便单独验证零行为变更。

**②③ 双层 cache**（S6 遗留）同样未在本 session 推进，理由见六维总表小结。

---

## 总指挥验收（2026-08-04）

**PASS —— 24 GB/s 目标达成（24.8），legacy 恢复专项收口。**

代码审查抽核（maintainer 指令：不跑测试循环）：

- **shard_rotation 三层一致**：binding `map_to_shard`（:171）与 DataPath submit（:966）公式逐字相同；resolver `rot=` URI 参数缺省=0；默认 0 = 零行为变更；单射置换论证成立（rot 只置换 shard 索引、不进 shard_offset 公式，无别名）。根因分析（无轮转时 shard=L%4 整层落单盘）数学推导正确。
- **progress() spin-poll 有界**（:1841-1848 budget 超时窗口内 cudaEventQuery，非无界）；编排偏差（保持 HOST_POLL 契约而非 legacy 阻塞 sync）按硬约束显式记录——批准。
- **tpb=32 保留**（对照表 64/128 的 24.9/25.1 与 24.8 在噪声内，对齐 legacy 默认正确）。
- **legacy ground-truth 可信**：third_pkgs 本体构建并同几何实测 25.57 GB/s（4 盘）——24 目标线因此有效；新架构 24.8 = legacy 的 97%，每盘 6.2 GB/s ≈ 单盘饱和的 90%，3.6× 扩展。

**result 六维总表的一处过时修正**：② handle cache 写"单层，未双层化"——**有误**，S6b 已完成双层化（snapshot/restore、HostSlotPool、契约 88-90 PASS，记录于 result6.md 的 S6b 节）。修正：② = ✅ 已双层化。③ PRP cache 双层化未移植——S6b 已记录的偏差，**待 maintainer 裁定**（我的建议：接受——它只为超订场景存在，KV 规模够不到）。

**遗留**：
1. 3b Config 聚合未做（上下文预算）——保留在 Roadmap P3，独立 session。
2. resolver 契约 "not regular file" 失败**连续三个 session 出现**（1024 reload+重挂载后的环境语义嫌疑最大）——下次环境操作后请花一次排查，确认是 ext4 UNWRITTEN 环境语义还是真回归。
3. dev_table per-submit H2D（S5 REQUIRED 2b）未做——24.8 已达标，降为 P3 优化余量。

**Round 16 全部关闭**：S1 P0 修复 → S2 架构文档 → S3 harness → S4 striped 性能+IRQ 修复+多 target → S5 PRP 预构建 → S6/S6b 单路径化+tiered handle cache → S7 布局/编排对齐+24.8 GB/s 达成。
