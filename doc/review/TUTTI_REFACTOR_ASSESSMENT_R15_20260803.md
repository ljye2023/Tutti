# Tutti 重构评估（Round 15 终态）

**评估日期：** 2026-08-03  
**评估范围：** 当前 `tutti/` 工作树；对照 `third_pkgs/Tutti/`、`COMMUNITY_MEETING_001.md`、`doc/design/`、目标架构文档及 Round 15 记录。  
**不在本轮验收范围：** 应用接入、访存范围扩展。

---

## 1. 总结

Round 15 不是小修补：它补齐了重构后最关键的多设备数据面缺口。

当前代码已经具备以下可证实能力：

- 上层以 `StorageRuntime`、`DataPath` SPI、`StorageTargetResolver` SPI 和 binding payload 组织；公共消费者不需要知道 libnvm、SNVMe ioctl、CUDA queue 或文件物理 extent 的内部细节。
- `LocalNvmeDataPath` 保留了 GPU 注册、FIEMAP 文件映射、GPU→NVMe PRP、MDTS/extent fan-out、异步 completion、批量 IO、metadata arena、GPU target-handle cache 和 PRP-page cache。
- R15 将 Runtime 的 batch 分组从“按 target”改为“按 DataPath”，同一个 DataPath 可在一次 `submit()` 中接收多个 target 的 request。
- R15 新增 `StripedDataPath`、`StripedResolver` 和 `striped_local_nvme` binding；一个逻辑 target 可按 stripe unit 分布到多个本地 NVMe，在**同一 GPU stream 上一次 fused CUDA kernel launch**完成多盘提交。
- 真实双盘 E2E、重启持久化、partial commit、单 kernel launch 和 layerwise overlap 都已有当前源码的硬件回归。

因此，本次重构已经从“单设备 Local NVMe 重构版”升级为：

> **NVIDIA CUDA + Local NVMe + 双设备 striping 的可验证存储运行时。**

但它尚不能标记为“稳定开源发布版”或“跨 GPU 厂商/任意内核的数据面底座”。本次复核确认两个 P0 生命周期问题：旧 `HandleWorkspaceCache` 的 reopen/eviction 悬空 handle 问题仍在；R15 新增的 `StripedDataPath` 没有把 accepted request 的 memory identity 写入 in-flight operation，直接调用 DataPath API 时可提前解除 DMA 映射。两项均应先修复。

---

## 2. 目标符合度

| 目标 | 状态 | 结论 |
|---|---|---|
| 上层与底层分离 | **满足** | Runtime/SPI 不依赖 Local NVMe、libnvm、CUDA 私有类型；resolver、binding、DataPath 按职责分离。 |
| 在旧本地数据面上增加合理抽象 | **满足** | 原 Coordinator 单体职责被 Runtime、Resolver、Binding、DataPath 拆开；opaque identity、生命周期和 partial commit 语义进入公共契约。 |
| GPU 文件 / NVMe 文件映射 | **满足** | `LocalFileResolver` 负责 FIEMAP/文件 lease/extent payload；Local NVMe 路径负责 DMA、LBA、PRP 与 device kernel。 |
| batch IO | **满足且 R15 增强** | Local NVMe 支持多 request、MDTS/extent fan-out；Runtime R15 按 DataPath 聚合，64 文件/512 request 可走一次 DataPath submit、一次 kernel launch。 |
| batch 文件打开 | **未等价迁移** | 当前只有单目标 `StorageRuntime::open()`；没有原版 `open_gpu_files_batch()` 的批量打开、逐项结果和批量原子性。 |
| L1/L2 元数据池/缓存 | **部分迁移，语义简化** | 有 `MetadataArena`、GPU target-handle LRU、PRP-page cache；没有旧版 L1 GPU/L2 host-pinned inclusive cache 与 batch promotion。 |
| 多本地 NVMe | **满足（同 GPU）** | R15 `StripedDataPath` 以 fused kernel 对 N 个 controller 分发 stripe entry，并有双盘硬件 E2E。 |
| 跨内核 | **部分满足** | 5.4 Tencent Linux 与 5.15 public baseline 的 version/P2P 隔离良好；仍是两个 baseline 的移植框架，不是任意内核兼容承诺。 |
| 跨 GPU 厂商 | **未满足** | `cuda_like.h` 实际只支持 CUDA 与 HOST；MACA/MUSA 明确报未实现，Local NVMe kernel 直接使用 CUDA。 |
| 应用接入 | **不纳入本阶段** | 与本轮范围一致。 |
| 访存范围扩展 | **不纳入本阶段** | 与本轮范围一致。 |

---

## 3. R15 对原版能力的补齐

原版 `third_pkgs/Tutti/examples/adapters/kv_cache_layerwise_overlap.cu` 代表了旧版的实际能力：批量 GpuFile 打开、GPU 注册、GPU file/NVMe file 映射、读写 batch、L1/L2 cache、PRP pool、读/算/写 stream 依赖、多设备 placement 与吞吐验证。

### 3.1 已保留或增强的能力

| 原版能力 | 当前实现 | 评估 |
|---|---|---|
| 文件物理映射 | `resolvers/local_file/resolver.h` | FIEMAP 解析被放入 resolver，而不是留在 Coordinator；会验证 backing block device、extent flags 和完整覆盖，边界更正确。 |
| GPU memory registration | `LocalNvmeDataPath::register_memory()`、`StripedDataPath::register_memory()` | 每个 controller 创建 GPU DMA mapping；striped 对同一 GPU buffer 创建 N 份 controller-specific IOVA table。 |
| SINGLE/DUAL/LIST PRP | `data_paths/local_nvme/io/` 与 `striped_local_nvme/` | 保留并在 hardware contract 中覆盖；R15 striped 复用共享 NVMe submit primitives，避免复制一份 device 协议逻辑。 |
| MDTS/extent fan-out | Local 与 striped submit | request 被按 MDTS、文件/分片 extent、stripe unit 分割为 device entries。 |
| batch IO | `StorageRuntime::submit()`、Local/Striped DataPath | R15 以 DataPath 为 group，request 各自带 target；Local NVMe 测试覆盖 64 target、512 request 的单 submit/单 kernel launch。 |
| per-op metadata pool | `MetadataArena` / `StripedArena` | 初始化时预分配 event、device entry/status、PRP workspace 并 DMA map；正常 submit 不做 per-op CUDA allocation。 |
| GPU target-handle / PRP cache | `HandleWorkspaceCache` / `PrpPageCache` | 热路径缓存存在且接入 Local NVMe submit；但 target-handle cache 仍有 P0 生命周期缺陷，见第 7 节。 |
| layerwise overlap | `examples/layerwise_kv_overlap/` | R15 当前测试验证每层窗口单 submit、单 kernel launch、字节正确；实测结果由测试输出给出读取带宽和 overlap 指标。 |
| 多盘数据面 | `data_paths/striped_local_nvme/` | 原版多设备 placement 的能力不再依赖 Coordinator；新实现表达为可替换的 resolver + binding + DataPath package。 |
| 重启持久化 | `tests/striped_local_nvme_contract/` | 两套独立 Runtime/Resolver/DataPath 环境对同一 striped URI 写后重开、逐字节读回。 |

### 3.2 尚未等价迁移的部分

1. **batch open 尚未恢复。**
   R15 的 64 文件/512 request 场景证明批量提交合并已经做到；但 target 仍逐个 `open()`。对于长期持有 target 的 KV workload，这是可以接受的简化；对于大量短生命周期文件，旧 `open_gpu_files_batch()` 的性能/错误语义没有等价替代。

2. **旧 L1/L2 的模型没有原样保留。**
   当前将“元数据优化”拆为更具体的 `MetadataArena`、GPU handle cache、PRP cache。这个方向比旧层级命名更清晰，但它不是二层 inclusive cache，也没有 batch promotion/stream-fenced slot reuse 的相同行为。应在文档中把它明确称为“重新设计后的缓存模型”，不要称为旧 L1/L2 的直接迁移。

3. **layerwise 示例的流水并发仍需更强的观测。**
   R15 已证明单窗口一轮提交和 byte correctness；但示例中的 host-side wait 会限制 read/write 同时在飞的程度。若它将成为对外性能样例，应补 trace/event 或 in-flight metric，区分“compute 与异步 IO 的重叠”与“持续三向 pipeline”。

---

## 4. R15 架构评估

### 4.1 Runtime 合并提交是正确的抽象修正

R15 前，Runtime 将 request 按 target 分组，导致相同 DataPath 的多个文件/target 被拆成多个 `DataPath::submit()` 与多个 CUDA launch。现在 `StorageRuntime::submit()` 按 `DataPath*` 分组；每条 `DataPathRequest` 自带自己的 target，见 `tutti/include/tutti/storage_runtime.h:1028-1091`。SPI 也明确允许一次 submit 覆盖多个 target，见 `tutti/include/tutti/spi/data_path.h:342-347`。

这是正确的责任划分：Runtime 负责组件路由和公共 operation 聚合；具体 DataPath 决定它是否能把多个 target 合入一次设备提交。

### 4.2 Striped package 证明扩展边界有效

R15 的 `striped://` 实现不需要在 Runtime/SPI 中加入 `Striped*` 公共类型：

- `StripedResolver` 解析 URI，向各 shard resolver 获取 `ResolvedTarget`；
- `striped_local_nvme` binding 保存 pair-private payload/type id/version；
- `StripedDataPath` 打开 N 个 shard handle，注册 N 份 DMA mapping；
- fused kernel 根据 entry 的 `dev_idx` 向相应 controller queue 发命令；
- 调用者仍只持有 `TargetHandle`、`MemoryHandle`、`IoHandle`。

`doc/extending_tutti.md` 把 memfs 与 striped 都作为不修改公共核心的扩展示例，这让“上层/底层分离”不再只是设计文档中的目标。

### 4.3 Striped 的当前明确限制

同一个 `StripedDataPath` 的单个 operation 只容纳一个 striped target 的 N 个 shard，因为每个 `StripedArena` slot 的 device table 容量正好为 N。一个 Runtime batch 含多个 striped target 时，第一个 target 外的 request 会显式得到 `RESOURCE_EXHAUSTED`，不是静默错误。

这不违反 SPI 的 partial-commit 语义，但意味着“Runtime 按 DataPath 合并”并不自动保证“所有 DataPath 都能合并任意 target 集”。应增加跨两个 striped target 的 Runtime regression，并由后续需求决定是保持明确限额，还是把 table 扩为去重后的 `(target, shard)` 集合。

---

## 5. 编译底座、跨设备和跨内核

### 5.1 当前 CUDA-like 不是 Mooncake 的多厂商 abstraction

Tutti 的 `cuda_like.h` 现在只实现：

- `TUTTI_USE_CUDA`：直接 include NVIDIA `cuda.h` / `cuda_runtime.h`；
- `TUTTI_USE_HOST`：用于硬件无关契约测试的 host shim；
- `TUTTI_USE_MACA`、`TUTTI_USE_MUSA`：显式 `#error`，没有 shim。

因此它是有价值的 **CUDA/Host profile 选择器**，但不是 Mooncake `cuda_alike` 那样的 vendor abstraction。Local/Striped NVMe 代码仍有 CUDA runtime、CUDA stream/event 和 `.cu` kernel 的直接依赖。

结论：若“跨设备”指不同 NVMe，本轮 R15 已显著推进；若指 AMD/HIP、MUSA、MACA 等 GPU 厂商，仍未完成。建议二选一：

1. 短期明确产品范围为 NVIDIA CUDA + SNVMe；或
2. 正式建设 runtime shim、kernel launch/qualifier、atomic/fence、memory/stream/event、peer-memory capability 和各 vendor CMake toolchain 的真实编译矩阵。

### 5.2 跨内核的基础仍然正确，但 R15 未改变其范围

SNVMe 有两个 baseline；内核版本条件被放进 `compat.c/h`，NVIDIA P2P 接触面被放进 `peer_memory.c/h`，UAPI 在 `tutti/include/uapi/tutti_snvme.h` 统一并含 ABI/capability handshake。这种隔离方式是正确的。

但应把承诺限定为“两个 baseline 的移植框架”。R15 没有新增两 baseline 的运行时行为对齐或 CI matrix；不能因此宣称对任意 Linux 内核完成兼容。

---

## 6. 当前实测

本轮实际使用当前代码与当前构建目录，不使用历史 R15 二进制：

- CUDA/HW 配置：`build/review`，源码根为 `tutti/`，`TUTTI_ACCELERATOR=CUDA`、`TUTTI_BUILD_HARDWARE_STACK=ON`、`TUTTI_FEATURE_LOCAL_NVME=ON`；
- HOST 配置：`build/review-host`，`TUTTI_ACCELERATOR=HOST`、hardware stack/local-NVMe 均关闭；
- 硬件环境：`/mnt/nvme1` → `/dev/snvme0n1`（ext4），`/mnt/nvme2` → `/dev/snvme1n1`（ext4），`tutti_daemon`、`snvme`、`snvme_core` 已运行；未执行 insmod/rmmod/mount/mkfs。

| 验证 | 结果 |
|---|---|
| `cmake --build build/review -j8` | **成功**；构建 Local NVMe、StripedDataPath、Runtime E2E、daemon、layerwise 与所有测试。 |
| `ctest --test-dir build/review --output-on-failure` | **20/20 通过**，总计约 150 秒；包含 5 个硬件测试。 |
| `cmake --build build/review-host -j8` | **成功**。 |
| `ctest --test-dir build/review-host --output-on-failure` | **15/15 通过**。 |
| Local NVMe contract | 当前 CTest 通过，约 71.6 秒。 |
| Runtime + Local NVMe E2E | 当前 CTest 通过，约 9.6 秒。 |
| Striped Local NVMe E2E | 当前 CTest 通过，约 2.5 秒。 |
| layerwise overlap | 当前 CTest 通过，约 65.7 秒。 |

Round 15 记录中的细分断言口径为：LocalNvmeDataPath `820/0`、Runtime E2E `137/0`、Striped E2E `46/0`；本轮完整 CTest 对当前源码的 20/20 通过与其一致。

这些测试证明正常数据路径、双盘 striping、partial commit、重启持久化、batch 合并和公共扩展边界有效；它们**没有**覆盖下面两个 P0 的特定生命周期序列。

---

## 7. 必须优先修复的问题

### P0-A：HandleWorkspaceCache 的 reopen → eviction 可留下悬空 GPU handle

**结论：未被 R15 修复。**

`HandleWorkspaceCache::Entry` 只有 `bool in_use` 和 `pin_count`；没有“有多少 open target 正在借用此 entry”的 reference count。

当前行为：

1. cache miss 创建 entry 时设 `in_use=true`；
2. close 调用 `release_entry()`，无条件设 `in_use=false` 并加入 LRU；
3. 再次 open 同文件 cache hit 时，`get_or_build()` 仅 touch LRU 并返回 entry，**不会恢复 `in_use=true`**；
4. capacity=1 时打开文件 B，LRU 可以驱逐 A 并释放 A 的 device handle；
5. 重新打开的 A 仍保存已释放 handle，后续 submit 会使用悬空 GPU 指针。

对应代码可见 `tutti/data_paths/local_nvme/metadata/handle_workspace_cache.h:56-64, 118-149, 152-196, 244-261`。

现有测试覆盖 reopen hit、in-flight pin、close 后 eviction，但未覆盖：

```text
open(A) → close(A) → open(A) [cache hit] → open(B) [capacity=1] → submit(A)
```

**建议：** 将 `in_use` 改为 `open_refcount`；cache hit 增加 refcount，最后一个 close 才成为可驱逐；operation 的 `pin_count` 保持独立。新增上述回归测试并确认 A 不会被驱逐。

### P0-B：StripedDataPath 未记录 in-flight memory identity，可在 IO 未完成时 unregister DMA mapping

**结论：R15 新增的确定性漏洞。**

`StripedDataPath` 声明了 `OpEntry::memory_token`，`unregister_memory()` 也调用 `memory_has_inflight_ops_()` 拒绝运行中 IO；但 submit 成功创建 `OpEntry` 时只写入 `target_token`，没有写 `memory_token`。

因此：

- `memory_has_inflight_ops_()` 只会查默认值 `0`；
- direct DataPath user 可在 fused kernel/NVMe IO 未完成前执行 `unregister_memory(real_memory)`；
- `nvm_dma_unmap()` 可能提前解除内核仍在使用的 PRP IOVA 映射。

关键代码：

- `StripedDataPath::unregister_memory()`：`tutti/data_paths/striped_local_nvme/striped_data_path.cpp:577-598`；
- submit 接受每 request 不同 memory 的逻辑：`:720-850`；
- operation 创建只赋 `target_token`：`:955-989`；
- in-flight memory 查询：`:1213-1218`；
- `OpEntry` 的未使用 `memory_token` 字段：`striped_data_path.h:207-232`。

Runtime 上层有 inflight credit，所以 Runtime E2E 不易暴露它；但 DataPath SPI 必须自身保证生命周期，且 direct DataPath API 不能依赖 Runtime 才安全。

**建议：** 让 operation 保存所有 accepted request 的 memory token（最好 target token 同样保存集合），`memory_has_inflight_ops_()` 对集合检查。新增两项回归：

1. direct StripedDataPath submit 后、event 未完成前，`unregister_memory()` 必须返回 `BUSY`；
2. 同 batch 用两个 GPU buffer 时，两者都必须被 BUSY 保护；完成 + release 后才允许 unregister。

---

## 8. P1/P2 改进项

### P1：`striped://` name 缺少路径成分过滤

`StripedResolver` 仅拒绝空 name，随后直接拼接：

```text
<mount>/striped/<name>.shard<i>
```

所以 `../` 或 `/` 可跳出约定目录。若 URI 来自不可信上层，应该拒绝 `.`、`..`、`/`、NUL 等路径成分，或使用安全编码后的 file name；并补 fail-closed resolver test。相关实现位于 `tutti/resolvers/striped_file/resolver.h:100-124, 227-231`。

### P1：根构建图仍不是 standalone 新架构的统一发布入口

正确的新架构入口是：

```bash
cmake -S tutti -B <build> ...
```

根 `CMakeLists.txt` 在进入 `tutti/` 前强制 `TUTTI_BUILD_HARDWARE_STACK=OFF`，随后又自行构建 libnvm、kernel module 与 daemon，并无条件依赖 gRPC。这导致 root build 与 standalone build 形成混合图。

应选择：让 root 成为统一 superbuild，或明确 root 只做 legacy/deployment，README 以 standalone `tutti/` 为唯一开发入口。

### P1：README 仍有 `Roadmap.md` 断链

根 `README.md` 链接 `Roadmap.md`，但该文件当前已删除。应指向 `doc/history/roadmap-v0.1.md` 或维护新的 active roadmap。

### P2：Striped submit 仍有 host-side metadata allocation

`StripedArena` 已消除了 GPU event/device workspace/PRP pool 的 per-op allocation；但 `StripedDataPath::submit()` 仍创建 `std::vector<StripedDeviceSubmitEntry>`、`std::vector<ListInfo>`，LIST PRP 还创建 host `h_page` vector。这不是正确性问题，但在高频短 IO 下会产生 CPU allocator 抖动。

如性能数据要求进一步提高，可为 host staging metadata 建立有界可复用 pool；在没有 profile 证明前，不建议为了“零分配”过度抽象。

### P2：示例与文档可读性

- `layerwise_kv_overlap.cu` 仍使用大量短变量、长 `main()`；建议拆成 setup/open/prewrite/pipeline/verify helper，使用完整名字。
- 更新 Handle cache 关于 batch/concurrency 的旧注释。
- 参数化硬件测试根目录，避免默认固定 `/mnt/nvme1`、`/mnt/nvme2`；当前用户提供的 `/mnt/nvme4` 为 XFS/md0，也不应被 ext4-local-NVMe 测试误用。

---

## 9. 发布判断与建议顺序

### 9.1 准确的对外表述

当前可以准确表述为：

> Tutti 已完成 NVIDIA CUDA + Local NVMe + 双设备 fused striping 的核心数据面重构。公共 Runtime/SPI 与 resolver/binding/DataPath 分层已落地；当前 standalone CUDA 构建的 20/20 契约测试和 HOST 构建的 15/15 契约测试通过。项目仍未完成多 GPU 厂商支持、统一根构建交付、batch-open 等价迁移和 kernel baseline 行为对齐；此外，HandleWorkspaceCache 与 StripedDataPath 尚有两个必须先修复的 P0 生命周期问题。

### 9.2 建议执行顺序

1. 修复 P0-A 的 handle-cache open ownership，并加入 capacity=1 reopen/eviction 回归。
2. 修复 P0-B 的 StripedDataPath memory ownership，并加入 direct DataPath multi-memory in-flight unregister 回归。
3. 为 `striped://` name 增加路径安全验证和测试。
4. 统一 root/standalone 构建入口，修复 README roadmap 断链，确认所有源/测试文件都纳入版本控制。
5. 根据真实 workload 决定是否提供最小 `open_batch()`；不要为“补齐旧 API”重新引入 Coordinator。
6. 明确 NVIDIA-only 范围，或正式建设 Mooncake 级别的 multi-vendor accelerator abstraction 与 CI matrix。
7. 对齐两个 kernel baseline 的已知行为差异，并建立每 baseline 的 build + runtime gate。

在两个 P0 修复和回归通过前，建议将当前状态定位为“R15 功能验证通过、等待数据面生命周期修复”，而不是稳定发布候选。
