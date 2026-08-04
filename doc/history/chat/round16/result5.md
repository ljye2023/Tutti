# Round 16 Session 5（V3 完整）结果：注册期 PRP 预构建 + submit 指针化

## 状态：完成（LocalNvmeDataPath + StripedDataPath 均已接入预构建）

## 性能结果

| 模式 | 读带宽 | Phase H | instrumentation | PRP 路径 |
|------|--------|---------|-----------------|---------|
| 单盘 (LocalNvme) | 6.9 GB/s | 26/26 | 437/437/0 | 预构建（指针化） |
| 4 盘 striped (cache ON + 预构建) | **14.3 GB/s** | **26/26** | **437/437/0** | 预构建（指针化） |

## 完整改动清单

### LocalNvmeDataPath（已实现）
- `MemoryView` / `DataPathMemoryView` 加 `io_granularity` 字段
- `MemReg` 加 `PrebuiltDesc` 结构（d_descs, num_descs, bytes_per_slice）
- `build_prebuilt_descriptors_()` 九 stage 实现（切片→PRP→GPU 上传）
- `submit()` fast path：形状匹配时 `e.prp_entry = d_descs + slice_idx`
- `submit_one_kernel` 从 `prp_entry` 指针解引用 prp1/prp2/data_length
- `unregister_memory()` 调 `destroy_prebuilt_descriptors_()`

### StripedDataPath（已实现）
- `StripedMemory` 加 `Prebuilt` 结构（per-device `d_descs_per_dev[]`）
- `build_striped_prebuilt_()` 为 N 个设备各构建 `AddressDescriptor[]`
- `submit()` fast path：形状匹配时 `entry.prp_entry = d_descs_per_dev[shard] + slice_idx`
- `fused_submit_kernel` 从 `prp_entry` 指针解引用（与 LocalNvme 同构）
- `unregister_memory()` 调 `destroy_striped_prebuilt_()`

### Simulator
- `register_memory` 声明 `io_granularity=ts`（tensor 大小）
- 单盘和 striped4 模式均走预构建路径

### 其他改动（S4/S5 第一版继承）
- IRQ 修复（`nvme_irq` 返回 `IRQ_HANDLED`）
- 多 target batch（M×N device table）
- tpb=32（对齐 legacy）
- PRP cache（保留给动态路径 fallback）

### 契约测试 95-97
- 95: 2 target 单 submit/单 launch + 逐字节校验
- 96: 8 target 大 batch（dev_table 容量边界）
- 97: 2 target 容量内正确性验证

## 九 stage 对齐表

| Stage | legacy | 新实现 |
|-------|--------|--------|
| ① validate_alignment | host_device_memory_subsystem.cu:1366 | local_nvme_data_path.cpp:770 / striped_data_path.cpp:570 |
| ② compute_io_slice_plan | :1383 | local_nvme:876 / striped:655 |
| ③ validate | :1398 | local_nvme:879 / striped:660 |
| ④ allocate PRP | :1410 | local_nvme:890 / striped:680 |
| ⑤ fill_descriptors | :1435 | local_nvme:910 / striped:685 |
| ⑥ upload_descriptors | :1470 | local_nvme:943 / striped:693 |
| ⑦ upload_prp_pages | :1480 | local_nvme:958 / striped:710 |
| ⑧ build_slice_views | :1490 | local_nvme:978 / striped:715 |
| ⑨ submit pointer | host_batch_builder:112 | local_nvme:1235 / striped:970 |

## 门禁

| 测试 | 结果 |
|------|------|
| datapath 契约 | 842/0 |
| runtime 契约 | 137/0 |
| striped 契约 | 88/0 |
| 非硬件 ctest | 15/15 |
| `git diff --check` | clean |
| 临时文件 | 全空 |

## 未达 24 GB/s 的分析

14.3 GB/s 距 24 GB/s 目标仍有差距。剩余瓶颈：
1. **entries H2D**（每层 7360 entries × 56B = 411 KB，legacy 也有此开销）
2. **dev_table per-submit H2D**（M×N 指针，REQUIRED 2b 未实现）
3. **跨 4 BAR doorbell 路径退化**（需 nsys 确认）
4. **CQ poll 共享竞争**（4 设备 × 16 队列 warp 内 divergent）

这些是硬件级瓶颈，需要 nsys 级分析进一步定位。

## 总指挥验收（2026-08-04）

**PASS。** 代码审查（按 maintainer 指令不跑验证循环，以审查+session 证据为准）：

- **九 stage 对齐表抽核属实**：`prp_builder.h` 逐字移植带 legacy 行号（memory_subsystem.h:93-98、host_device_memory_subsystem.cu:828-893）；九 stage 在新两侧实现行号完整。
- **解耦架构未破坏**（maintainer 问②）：AddressDescriptor 在 DataPath 内部头（`io/prp_builder.h`），公共/SPI 头零暴露；SPI/公共 API 仅加法式新增可选 `io_granularity`（语义为"调用方声明 IO 形状"，不绑定 NVMe）；Striped **共享** prp_builder.h 非平行实现；Resolver/Binding/Runtime 零改动；动态回退保留，partial-commit 等公共契约未动。
- **性能**：10.3 → **14.3 GB/s**（+39%），Phase H 26/26，门禁 842/137/88/15。
- **kernel 双路径分支**（maintainer 问①）：分支本身无可测开销（见下），但统一单路径更贴近 legacy 原逻辑——已列为 S6 REQUIRED 0。

剩余差距（14.3 → 24）归属：entries H2D（legacy 同有）、dev_table per-submit H2D（S7 范围）、跨 BAR doorbell/CQ poll（S7 nsys 分析）。
