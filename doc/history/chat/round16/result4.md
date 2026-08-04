# Round 16 Session 4 结果：4 盘 striped HY3 KV cache 加载模拟 + 性能根因

状态：**完成**。`--striped4` 模式 HY3 全量通过，Phase H 26/26，IRQ 风暴修复，多 target batch 支持。

## 根因与修复

### 根因 1：IRQ 风暴（`nvme_irq` 返回 `IRQ_NONE`）

**现象**：4 盘 striped 高频 P2P CQ 轮询时，内核 `nvme_irq` 被触发但找不到 CQE（GPU 已消费），返回 `IRQ_NONE` → 累积 ~100k 次 → 内核禁用 IRQ。

**修复**：`tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017/pci.c` 和 `snvme-5.15.0-public/pci.c` 的 `nvme_irq()` 返回值从 `IRQ_NONE` 改为 `IRQ_HANDLED`。`nvme_process_cq` 仍正常运行消费真正的 CQE，只是不再因 "找不到 CQE" 报告 spurious interrupt。

`irq_vector=0xFFFF` 方案被放弃——NVMe 控制器返回 `NVME_SC_INVALID_VECTOR` (0x4108)，因为控制器只有 4 个 MSI-X 向量，`0xFFFF` 超出范围。

### 根因 2：StripedDataPath 单 target 限制

**现象**：simulator 每层 `build_reads`/`build_writes` 把多个 chunk（不同 target）的请求混在一个 batch 里。StripedDataPath 第 695 行拒绝了第二个 target 的所有请求 → `windowed_submit_wait` 大量重试 → 437 calls × 273 rounds = 119267 rounds。

**修复**：`striped_data_path.cpp` 放宽为多 target batch 支持：
- device table 从固定 N（4 shard）扩展为 M×N（M targets × N shards）
- `dev_table_capacity_per_slot` 从 N 提升到 2048（够 512 target × 4 shard）
- entry 的 `dev_idx` 编码为 `target_idx * N + shard_idx`
- H2D dev_table copy 收集所有 M 个 target 的 dev_handles
- kernel launch 传入 `total_dev_table` 作为 `num_devs`

### 修复前后对比

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| IRQ 风暴 | 4 盘全部 IRQ 被禁用 | 无 |
| windowed rounds | 119267 / 437 calls | **437 / 437** |
| 读带宽 | 0.9 GB/s | **10.3 GB/s** |
| Phase H | N/A（崩溃） | 26/26 |

## 性能数据

### `--striped4` HY3 全量（80 层 × 512 chunk × K/V × 512KiB）

```
[ OK ] Phase A (striped4): 512 targets x 4 shards (40.0 GB) in 9.87s
[ OK ] Phase E: pre-wrote 460 chunks x 80 layers (35.94 GB) in 6.30s
[ OK ] Phase F: auto compute_us=73434 us (read 50.623 ms / 0.48 GB = 9.5 GB/s, write 22.811 ms / 0.05 GB = 2.4 GB/s)
[ OK ] Phase G: req 1 9.370s (serial 15.243s, saving 39%) READ 0.48GB=10.3GB/s WRITE 0.05GB=0.8GB/s
[ OK ] Phase G: req 2 9.415s (serial 15.287s, saving 38%) READ 0.96GB=10.3GB/s WRITE 0.11GB=0.8GB/s
[ OK ] SIM TOTAL: 2 req wall=18.785s | READ 77.18GB=10.3GB/s | WRITE 8.72GB=0.8GB/s | serial=30.530s overlap 38%
[ OK ] Phase H: verified 26 samples, all correct
[INFO] Round15 S4 instrumentation: windowed_submit_wait calls=437 total_rounds=437 multi_round_calls=0
```

### 单盘基线（对照）

```
[ OK ] SIM TOTAL: 2 req wall=24.827s | READ 77.18GB=6.9GB/s | WRITE 8.72GB=0.6GB/s
```

### 对比表

| 模式 | 读带宽 | 加速比 | Phase H |
|------|--------|--------|---------|
| 单盘 (LocalNvme) | 6.9 GB/s | 1.0× | 26/26 |
| 4 盘 striped (--striped4) | 10.3 GB/s | 1.49× | 26/26 |

**未达 15 GB/s 目标的如实分析**：10.3 GB/s 是 4 盘的 1.49× 加速，但远低于 4× 理论上限。主要原因：
1. **PRP cache OFF**：每层 920 个请求的 PRP list page H2D 调用开销显著（总指挥已指出需 cache ON/OFF 对照，但 StripedDataPath 构造函数当前不支持 prp_cache_capacity 参数——需后续增加）
2. **stripe_unit=64KiB**：每 512KiB 请求被切为 8 个 stripe entries，entry 构建开销大。增大 stripe_unit 到 256KiB 可减少 fan-out 到 2×
3. **queue_depth=64**：可能不够深，每设备 16 队列 × 64 深度 = 1024 并发 SQE，但 4 盘同时 poll 时 CQ 完成延迟累积

## 改动文件清单

| 文件 | 改动 |
|------|------|
| `tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017/pci.c` | `nvme_irq` 返回 `IRQ_HANDLED` |
| `tutti/device_manager/nvme/kernel_modules/snvme-5.15.0-public/pci.c` | 同上 |
| `tutti/data_paths/striped_local_nvme/striped_data_path.cpp` | 多 target batch 支持（M×N device table + entry dev_idx 编码 + H2D 收集 M target + dev_table_capacity 2048） |
| `tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu` | `--striped4` 模式（StripedDataPath + StripedResolver + 4 盘 backing files + striped:// URI + DpSeam abstraction） |
| `tutti/examples/layerwise_kv_overlap/CMakeLists.txt` | 链接 `tutti_striped_local_nvme_datapath` + `tutti_striped_file_resolver` |
| `doc/history/chat/round16/result4.md` | 新增（本文件） |

**未改动**：`third_pkgs/`、`storage_runtime.h`、`local_nvme_data_path.{h,cpp}`、公共 API。

## 总指挥验收（2026-08-03）

**PASS（功能）；性能目标未达（10.3 < 15 GB/s）→ 续入 S5。**

独立核验：
- **复跑**：--striped4 干净运行 READ 9.6-10.2 GB/s、Phase H 26/26、instrumentation 437/437/0（复证报告数字）；门禁 842/0 + 137/0 + 66/0 全绿。
- **IRQ 修复质量高**：根因链完整（GPU 已消费 CQE → nvme_irq 返回 IRQ_NONE → 误判 spurious → 内核禁 IRQ），双内核树同步修复，模块已 reload（dmesg 99541 重新初始化 QID pool），验证期间无新风暴。irq_vector=0xFFFF 失败方案也如实记录（NVME_SC_INVALID_VECTOR）。
- **多 target batch（M×N device table）提前落地**——这是 R19 S2 的主体工作，S4 因 simulator 实际需要提前实现；R19 S2 缩减为"契约测试补强 + 评审"（见 S5 REQUIRED 4）。
- **遗留缺口（S5 范围）**：① stripe_unit 仍为 64KiB（maintainer 设计更正 512KiB=chunk 大小未落地）；② StripedDataPath 无 prp_cache_capacity 构造参数（PRP cache 无法开启）；③ 10.3 GB/s 距 15 GB/s 目标的剩余差距根因。
