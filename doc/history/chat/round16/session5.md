# TASK — Round 16 Session 5：完整恢复 legacy 逻辑（一）：注册期 PRP 预构建 + submit 指针化

**日期：** 2026-08-03（总指挥签发，V3 全量重写）
**maintainer 最高指令：** 在现有架构上**完整恢复历史逻辑**，**不要修改原本的逻辑**。legacy（`third_pkgs/Tutti/`）是只读参考源，逐字移植其设计；禁止"改进""简化""重新设计"。总指挥不再做测试验证循环——session 自行验证并在 result 报告，验收以代码审查 + session 证据为准。

---

## 目标

把 legacy 性能链路的根基——**注册期预构建 GPU 常驻描述符，submit 热路径纯指针算术**——完整恢复到新架构的 `LocalNvmeDataPath` 与 `StripedDataPath`。

## legacy 原逻辑（移植源，逐字对齐）

`third_pkgs/Tutti/memory/src/host_device_memory_subsystem.cu` `build_io_slice_table` 九 stage（文件头注释 636-696 行）：

```text
register_tensor(spec{ptr, size, granularity})
  ① validate_alignment
  ② compute_io_slice_plan        粒度 = min(spec.granularity, MDTS)
  ③ validate dma
  ④ allocate + dma_map PRP buf
  ⑤ validate
  ⑥ fill_address_descriptors     每个 sub-IO 的 {data_length, prp1, prp2}；
                                  LIST 路径把 ioaddrs[start+1..+pages-1] 打包进 PRP-list 页内容
  ⑦ upload_descriptors_to_gpu    一次 cudaMalloc + cudaMemcpy，AddressDescriptor[] GPU 常驻
  ⑧ upload_prp_list_pages        PRP-list 页内容一次上传 GPU
  ⑨ build_slice_views            host 侧 slice 索引
```

IO 时（`io_engine/src/local_nvme/host_batch_builder.cpp`）：`e.prp_entry = v.d_ios + sub` —— **纯指针算术**，`NvmeBatchEntry`（40B）存 GPU 描述符**指针**而非值。kernel（`nvme_batch_xfer_kernel.cu`）解引用 `e.prp_entry->prp1/prp2/data_length`。

## 恢复规格（新架构落点）

1. **注册**：`DataPathMemoryView`（SPI）与 public `MemoryView` 增加可选 `io_granularity`（0 = 未声明）。声明时，`register_memory` 执行九 stage 等价流程：按 `min(io_granularity, MDTS)` 切片、预构建全部 `AddressDescriptor` 与 PRP-list 页内容、一次上传 GPU 常驻；host 侧保存 slice 索引视图。未声明（0）走现有动态路径（保留通用性）。
2. **submit**：请求形状匹配声明粒度（offset/len 对齐）时——entry 构建 = 描述符基址 + 下标的**指针算术**，零 PRP 计算、零 LIST 页 H2D、零 classify；entry 携带描述符 GPU 指针。形状不匹配 → 回退现有动态路径（不拒绝，保留通用性）。
3. **kernel**：与 legacy 同构——从描述符指针读 `prp1/prp2/data_length`，`resolve_lba` + queue acquire + issue + poll 原语不变（已同源）。
4. **unregister**：释放描述符 GPU 内存与 PRP 页（对称于 legacy 的 erase/free 路径）。
5. StripedDataPath 同样接入（entry = dev_idx + 描述符指针）；其 dev_table 构建维持 S4 形态，本项只改 PRP 侧。
6. 现有 `PrpPageCache` 保留给动态路径；KV（声明粒度）路径不经过它。

## 验收（session 自行验证并报告）

1. simulator（单盘 + --striped4）KV 路径走声明粒度注册：Phase H 26/26；instrumentation 437/437/0；**每次 submit 的 PRP 相关 H2D = 0**（计数证据）；host 侧每 entry 成本对比（前/后 ns/entry）。
2. READ 带宽报告（对照 V2 基线 10.3 GB/s）；本项是达到 24 GB/s 目标的必要条件而非充分条件。
3. 契约一次：842/137/66 + 非硬件 15；新增契约（编号 95 起）：声明粒度注册的 roundtrip + 不匹配形状回退路径 + unregister 对称释放。
4. result：改动清单（文件级）、九 stage 对齐表（legacy 行号 ↔ 新实现行号）、证据。

## 硬约束

- `third_pkgs/` 只读；公共 API 语义不破（partial-commit/Status/lease 保留——这是抽象层，不在"核心逻辑"范围）；O_DIRECT；防缠结（改动清单）；临时 debug 收尾摘除；PRP/handle cache 默认值不动。
