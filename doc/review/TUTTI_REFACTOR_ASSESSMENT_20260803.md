# Tutti 重构评估报告

**评估日期：** 2026-08-03  
**评估对象：** 当前工作树 `tutti/`，并与 `third_pkgs/Tutti/` 原始实现、`COMMUNITY_MEETING_001.md`、`doc/design/`、`TUTTI_REFACTOR_TAKEOVER.md`、`TUTTI_TARGET_ARCHITECTURE.md` 及 `chat/` 历史记录对照。  
**范围约束：** 本报告不把“应用接入”或“访存范围扩展”列为未完成项；这两项明确不属于当前阶段。

---

## 1. 结论摘要

### 1.1 总体结论

当前重构**已经完成了最重要的一次架构转向**：原来的 `Coordinator → memory → block_storage/nvme_storage → io_engine` 单体调用链，已经被拆为稳定的公共类型、`StorageRuntime`、`DataPath` SPI、`StorageTargetResolver` SPI、binding payload 和具体 `LocalNvmeDataPath`。公共上层不再包含 libnvm、CUDA Queue、SNVMe ioctl 或内核细节；Local NVMe 的具体实现也已收敛到私有目录。这一部分符合“在本地原有代码基础上增加合理抽象、上层与底层分离”的目标。

关键数据面能力没有被简单删除：GPU 注册、FIEMAP 文件物理映射、GPU→NVMe PRP IO、MDTS/extent fan-out、SINGLE/DUAL/LIST PRP、批量 IO、异步完成、stream 顺序、跨 DataPath 的聚合、元数据 arena 和 PRP/handle cache 都已有生产实现与硬件契约测试。实际硬件测试通过 `115/0` 与 `799/0` 两组断言，说明本次重构不是只停留在接口层。

但项目**还不适合以“跨设备、跨内核、可作为成熟开源底座”完成验收**。主要阻塞项是：

1. **P0：HandleWorkspaceCache 有已确认的 cache-hit 生命周期漏洞。** 已关闭再重新打开的 target 命中缓存后，entry 不会恢复 `in_use`；后续打开另一个文件可驱逐该 entry，释放仍被已打开 target 借用的 GPU handle，形成悬空设备指针。
2. **跨 GPU 厂商编译尚未实现。** 当前 `cuda_like.h` 实际只有 CUDA 与 HOST；Local NVMe kernel 和 CMake 都直接绑定 NVIDIA CUDA。它不是 Mooncake 那种已落地的多 vendor `cuda_alike` 层。
3. **构建入口分裂。** `cmake -S tutti` 是新架构的正确入口；仓库根 `cmake -S .` 为兼容旧 root target 强制关闭 nested `tutti/` 硬件栈，却仍自行构建 libnvm/daemon 并强依赖 gRPC，得到的是混合构建图，不能作为新数据路径的统一发布入口。
4. **跨内核是“两个 baseline 的移植框架”，不是任意内核兼容。** 5.4 Tencent Linux 与 5.15 public 的版本分支/NVIDIA P2P 已被很好地隔离；但 `PORTING.md` 仍明确记录 5.15 baseline 缺少部分 5.4 已修复的生命周期与失败回滚修复，因此不能宣称两个 baseline 行为等价。
5. **旧版批量文件打开和 L1/L2 缓存语义没有完全等价迁移。** 批量 IO 已迁移，批量 `open` 没有；旧两层 L1 GPU/L2 host-pinned cache 被有意收缩为 `targets_` 中的 host template、单层 GPU handle cache、PRP page cache 和 per-op arena。这是合理的简化方向，但应明确写成新的语义与性能假设，而不是默认等价。

**建议发布判断：** 可视为“CUDA + Local NVMe 的重构验证版本”，不应在修复 P0 前标记为稳定数据面或对外承诺多厂商 GPU 支持。P0 修复、根构建图统一、GPU portability 的范围澄清后，才适合作为可持续维护的开源基础。

### 1.2 目标符合度

| 目标 | 评估 | 依据与说明 |
|---|---|---|
| 上层与底层分离 | **基本满足** | 公共 Runtime/SPI 没有 Local NVMe、CUDA、libnvm 私有类型；具体设备路径位于 `data_paths/local_nvme/`。 |
| 可替换的数据路径抽象 | **满足** | `DataPath` 提供 target、registration domain、memory、submit/progress/query/release 生命周期；resolver 和 binding 将文件系统解析与传输路径解耦。 |
| GPU 文件/NVMe 文件映射 | **满足且增强** | Resolver 使用 FIEMAP、验证 backing block device/extent 覆盖，并将 extents 封装为 `ext4_local_nvme` payload；DataPath 负责转为 LBA 与 GPU device handle。 |
| 批量 IO、PRP、异步 stream 顺序 | **满足** | LocalNVMe 生产路径对多个 request 做验证、MDTS/extent fan-out、PRP SINGLE/DUAL/LIST、同 stream/cross stream 顺序与 completion 收割。 |
| 原版 batch 文件打开 | **未等价迁移** | 新 Runtime 只有单目标 `open()`；示例逐个打开 target。它有 batch submit，但没有 old `open_gpu_files_batch()` 的批量打开/原子结果语义。 |
| L1/L2 元数据池/缓存 | **部分满足，且语义有意收缩** | 有 `MetadataArena`、GPU handle LRU、PRP page cache；没有旧版 L1 GPU/L2 host-pinned inclusive cache 与 batch promotion。 |
| 跨内核编译 | **部分满足** | 两个 baseline、compat/P2P/UAPI 隔离良好；不是通用 Linux 兼容层，且 baseline 仍存在行为差异。 |
| 跨 GPU 厂商编译 | **未满足** | 实际支持 CUDA 与 HOST；MACA/MUSA 分支明确报未实现，Local NVMe 直接使用 CUDA runtime 和 CUDA kernel。 |
| 应用接入 | **不纳入本阶段** | 符合本轮范围。 |
| 访存范围扩展 | **不纳入本阶段** | 符合本轮范围。 |

---

## 2. 当前架构是否合理

### 2.1 分层结果

当前最合理的部分是将“文件如何解析”和“如何访问该文件”分开：

- `tutti/include/tutti/`：稳定的 value types、状态类型、Runtime 与 SPI。
- `StorageTargetResolver`：将 URI/文件系统对象解析为不透明 `ResolvedTarget` payload。
- `bindings/ext4_local_nvme/`：定义 ext4 + local-NVMe payload 的 type id/version 和读写视图。
- `LocalNvmeDataPath`：拥有 controller、queue group、DMA registration、GPU target handle、PRP 和 completion 细节。
- `StorageRuntime`：按 URI scheme 找 resolver、按 binding 推荐的 DataPath key 路由打开、统一 memory/IO handle 生命周期。
- `device_manager/nvme/`：libnvm、daemon 与 SNVMe 内核模块，位于数据路径的下方，不泄漏到公共头。

这是比旧版 Coordinator 汇聚所有职责更适合扩展的边界。尤其是：

- `DataPath` SPI 只暴露不透明身份、byte range、能力和操作状态，明确不允许 transport/device/kernel 私有类型进入接口，见 `tutti/include/tutti/spi/data_path.h:5-15, 238-242, 302-354`。
- `StorageRuntime` 通过 resolver URI scheme 与 `recommended_data_path_key` 组装，不直接依赖 Local NVMe，见 `tutti/include/tutti/storage_runtime.h:460-516`。
- `LocalNvmeDataPath` 私有头才引入 metadata、`nvm_ctrl.h`、`nvm_dma.h` 等依赖，见 `tutti/data_paths/local_nvme/local_nvme_data_path.h:21-33`。
- 已有 header hygiene 的负向测试，证明只链接公共 API 的消费者不能意外 include Local NVMe 私有头。

因此，这不是“把旧类改名”的抽象，而是实际的依赖方向修正。

### 2.2 仍需承认的边界

这套隔离是**公共消费者隔离**，不是“示例也完全不知道本地设备”。硬件示例仍显式构造 `LocalNvmeDataPath` 与 `LocalFileResolver`，并写死 `Local NVMe` 配置。这对于设备 bring-up/benchmark 是合理的，但不应把该示例当成完全 provider-neutral 的应用接入范式。

同时，`StorageRuntime` 是单头实现且内容很大；`LocalNvmeDataPath::submit()` 也承担了验证、fan-out、PRP 分配、H2D、kernel launch、缓存 pinning、操作建账等多种职责。功能正确性已有较强测试覆盖，但后续维护成本会随着下一个 DataPath 增加而上升。

---

## 3. 原版 `kv_cache_layerwise_overlap.cu` 能力迁移评估

原版示例是很好的回归基线：它同时覆盖 GpuFile 生命周期、多设备 placement、GPU tensor 注册、批量读写、handle cache、三 stream 事件依赖、吞吐统计和结果校验。本次审计按能力而不是按目录名比对。

| 原版能力 | 当前状态 | 证据与评价 |
|---|---|---|
| 批量创建/打开 GpuFile | **未等价迁移** | 原版调用 `Coordinator::open_gpu_files_batch()`，见 `third_pkgs/Tutti/examples/adapters/kv_cache_layerwise_overlap.cu:261-274, 296-301`。新版 `StorageRuntime::open()` 是单目标 API，示例逐个 `open`，见 `tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu:261-268`。cache 能降低重复 open 的 GPU handle 构造成本，但不提供批量 API、批量错误结果或批量原子性。 |
| 批量读写 | **已迁移** | Runtime/SPI 可向同一 DataPath 一次提交多个 request；Local NVMe 将所有接受的 request 展平为 device entries，见 `local_nvme_data_path.cpp:930-1113, 1243-1295`。硬件测试已验证 mixed batch、partial commit、跨设备 group-by-target。 |
| FIEMAP → LBA 映射 | **已迁移且增强** | 职责从旧 `nvme_storage` 移到 `LocalFileResolver`。resolver 验证 backing block device、unsafe extent flags 与完整覆盖，将物理偏移加 namespace base 后产出 payload；关键逻辑位于 `tutti/resolvers/local_file/resolver.h:248-308, 393-518`。这比旧路径把 `fe_physical` 直接转 LBA 更适合成为跨 DataPath 的解析层。 |
| GPU memory → NVMe PRP | **已迁移** | `register_memory()` 使用 `nvm_dma_map_data_device` 形成设备 DMA 映射；submit 根据 DMA IOVA 构造 PRP1/PRP2/PRP list，见 `local_nvme_data_path.cpp:735-785, 1029-1057`。 |
| MDTS 与跨 extent fan-out | **已迁移** | request 由 `min(remaining, effective_mdts)` 和 extent end 共同切分，见 `local_nvme_data_path.cpp:1005-1064`；实测覆盖 256 KiB fan-out、跨 segment、SINGLE/DUAL/LIST。 |
| L1 GPU + L2 host-pinned handle cache | **有意简化，不等价** | 原版配置 L1/L2 budget，见旧示例 `:235-237`。新版以 `targets_` 保留 host side template、`HandleWorkspaceCache` 保留 GPU handle、`PrpPageCache` 复用 LIST page，并以 `MetadataArena` 复用 event/entries/status/PRP workspace。它保留了热路径复用，但没有二层 inclusive LRU、batch promotion 和 stream-fenced slot reuse。对于当前每个文件可长驻 target 的模型是可接受简化；对于“海量短生命周期文件批量打开”不是等价替代。 |
| per-op 内存池 | **已迁移且改进** | `MetadataArena` 初始化时预分配 per-op event、entry/status GPU arrays 和 DMA-mapped PRP pool；submit 仅 acquire/release lease，见 `local_nvme_data_path.cpp:349-368, 1121-1143`。硬件测试覆盖 arena exhaustion、recovery、hot-path zero allocation 与 LIST PRP pool。 |
| handle/PRP 缓存 | **已实现，但 handle cache 有 P0 生命周期问题** | handle hit、pin/unpin、LRU eviction 的设计存在，PRP cache 在 LIST page 上也已接入生产 submit。详见第 7 节 P0。 |
| 读/算/写三 stream overlap | **能力保留，但示例并非完全三向在飞流水** | 新示例保留 `s_r/s_c/s_w` 和 event DAG，见 `layerwise_kv_overlap.cu:270-275, 419-456`。但每个 `windowed_submit_wait()` 都在进入下一阶段前 `wait()`，并显式说明 read/write 不是同时在飞、重叠主要来自 compute 与 host 阻塞 IO 等待，见 `:103-170, 390-398`。它是有效的 stream-order 演示和端到端 benchmark，但不等同于持续同时在飞的 read/compute/write pipeline。 |
| 多 NVMe、多 GPU placement | **系统级可组合，示例功能收缩** | 原版示例要求至少两设备并 round-robin shard，见旧示例 `:245-246, 287-294`。新版 `LocalNvmeDataPath` 单实例声明 `supports_multi_gpu=false`，但 Runtime 契约测试已验证 dual-device IO、cross-device batch grouping、fault isolation；说明系统可通过多个单设备 DataPath 组合。新版 layerwise 示例本身硬编码单 CUDA device 与单 `/dev/ssnvme0`，不再展示旧版 aggregate placement 能力。 |

### 3.1 对“关键优化是否放进来”的直接回答

- **batch 文件打开：没有完整放进来。** 已有 target handle cache，但没有 public `open_batch()`；这是功能/效率语义上的明确缺口。
- **batch IO：已经放进来。** 而且比旧上层 API 更明确地表达 partial commit、每 request 初始状态、operation 生命周期和资源限额。
- **L1/L2 元数据内存池：没有原样照搬，但核心热路径优化已放进来。** `MetadataArena`、GPU handle cache、PRP cache 是更小、更贴近 Local NVMe 的拆法；应将其定位为“简化后的新设计”，而不是“旧 cache 的同名迁移”。
- **GPU file-NVMe file 映射：已放进来，且架构位置更正确。** 文件物理映射在 resolver，GPU DMA/NVMe command 在 DataPath，避免上层 Coordinator 同时掌控文件系统与设备协议。

---

## 4. 编译底座与可移植性

### 4.1 与 Mooncake 的 CUDA-like 方式相比

结论非常明确：**当前 Tutti 的 `cuda_like` 只是“CUDA/Host profile selector”，不是 Mooncake 的多厂商 device abstraction。**

Tutti：

- CMake profile 只接受 `CUDA` 和 `HOST`，见 `tutti/CMakeLists.txt:16-32`。
- CUDA profile 直接要求 NVIDIA `CUDAToolkit`，直接链接 `CUDA::cudart` 与 `CUDA::cuda_driver`，见 `tutti/cmake/accelerators/CUDA.cmake:20-32`。
- `cuda_like.h` 只允许 `TUTTI_USE_CUDA` 或 `TUTTI_USE_HOST` 二选一；MACA/MUSA 分支明确报“not implemented”，见 `tutti/include/tutti/cuda_like.h:5-37`。
- Local NVMe 实现直接 include `<cuda_runtime.h>`，使用 CUDA event/stream/memcpy，以及 CUDA `.cu` kernel，见 `tutti/data_paths/local_nvme/local_nvme_data_path.cpp:5-12` 与 `io/` 子目录。
- HOST profile 是硬件无关 contract test 的同步 shim，不是一个可执行 GPU 后端，见 `tutti/cmake/accelerators/HOST.cmake:8-31`。

Mooncake：

- `third_pkgs/Mooncake/mooncake-transfer-engine/include/cuda_alike.h` 是真实 vendor selector，覆盖 CUDA、HIP、MUSA、MLU、MACA、SUNRISE 等，并由多个 vendor shim 提供 API 映射。

因此，若目标中的“跨设备编译”指 NVIDIA、AMD、MUSA/MACA 等不同 GPU 厂商，当前结论只能是**不满足**。仅增加几个宏名称没有意义；必须同时抽象：host runtime API、kernel qualifier/launch、stream/event、atomic/fence、device memory、peer-memory/driver capability 和 CMake toolchain profile。

### 4.2 跨内核工作

这一部分的工程方法是本次重构的优点：

- 两套 baseline 位于 `tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017/` 与 `snvme-5.15.0-public/`。
- 内核版本条件集中在 `compat.c`；`compat.h` 明确要求其它单元调用稳定 wrapper，见 `compat.h:2-17`、`compat.c:1-31`。
- NVIDIA P2P 接触面集中在 `peer_memory.c`；其它模块只通过 opaque type 与 `peer_memory_ops` 调用，见 `peer_memory.h:2-18, 37-99`、`peer_memory.c:1-46, 279-288`。
- kernel/userspace 共用固定宽度 UAPI，带 ABI version/capabilities fail-closed handshake 与 layout assertions，见 `tutti/include/uapi/tutti_snvme.h:4-23, 64-108, 231-290`。

但这个结果应描述为“**针对两个 NVMe baseline 的可移植框架**”，不能描述为“跨 Linux 内核自动兼容”。`PORTING.md` 自己也提示 5.15 baseline 仍有部分 5.4 baseline 已修复但未回填的生命周期/失败回滚问题，见 `PORTING.md:1192-1202, 1224-1252`。这意味着当前两条 baseline 的编译结构更统一，但行为质量并未完全对齐。

---

## 5. 构建图评估

### 5.1 正确的新架构入口

`cmake -S tutti -B <build>` 是当前新架构的合理入口：它始终构建 `tutti_api`/`tutti_spi`，并在 CUDA + hardware stack 条件满足时构建 `device_manager` 和 `data_paths/local_nvme`，见 `tutti/CMakeLists.txt:120-158, 261-269`。

这个入口可以：

- 用 `TUTTI_ACCELERATOR=HOST` 建立硬件无关 API/SPI contract 基座；
- 用 `TUTTI_ACCELERATOR=CUDA` 建立 Local NVMe 生产路径；
- 避免将 libnvm 私有 include 泄漏到公共头。

### 5.2 根 CMake 的问题

根 `CMakeLists.txt` 同时保留旧兼容职责，导致它不是上述 standalone 构建的正确 superbuild：

- root 在 add `tutti/` 前强制 `TUTTI_BUILD_HARDWARE_STACK=OFF`，见 `CMakeLists.txt:147-162`；
- root 随后又自行构建 libnvm、SNVMe module、nvmeservice、daemon，见 `:195-285, 315-432`；
- root 无条件 `find_package(gRPC CONFIG REQUIRED)`，即使只希望编译 HOST 公共 API，也会引入 daemon 依赖，见 `:353-380`。

这会造成“根构建是 public API + 旧式硬件库/daemon 混合图；standalone 构建才是新 LocalNvmeDataPath 图”的认知和维护负担。应在下一个阶段二选一：

1. 让 root 真正成为 standalone `tutti/` 的 superbuild，并通过选项控制 daemon/module/examples；或
2. 明确 root 只负责部署工具与兼容构建，README 将 `cmake -S tutti` 标为唯一开发入口。

在没有决策前，不宜对外只给出一条“根目录 cmake”命令。

---

## 6. 实测与覆盖范围

### 6.1 本轮实际执行结果

本次评估未安装/卸载内核模块，也没有改动设备或 daemon。复用现有运行中的 SNVMe、daemon 和已挂载测试环境，完成了以下验证：

| 验证 | 结果 | 覆盖内容 |
|---|---|---|
| standalone HOST profile | **15/15 通过** | `cmake -S tutti -B build/audit-tutti-host -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON` 后完整 build + CTest；覆盖 public API、SPI consumer、header hygiene、status/types、mock DataPath、resolver/binding contract。 |
| 当前 CUDA 构建 | **成功** | `cmake --build build/r15base -j8` 成功完成当前配置的 CUDA/Local NVMe 目标构建。 |
| CUDA 硬件无关 contract | **12 项通过** | API、SPI、Runtime、binding、striped resolver、UAPI、header hygiene 等。 |
| StorageRuntime + Local NVMe 端到端 | **115/0 通过** | 装配/open、lazy registration、4 KiB SINGLE、8 KiB DUAL、1 MiB LIST、跨 extent、mixed batch、partial commit、同 stream、双 stream、双 host thread、timeout、shutdown retry、重复 lifecycle。 |
| LocalNvmeDataPath 硬件 contract | **799/0 通过** | capabilities、target/memory identity、host/device registration、PRP、completion error、arena exhaustion/reuse/zero-alloc、handle cache/PRP cache、同/跨 stream、fan-out、dual device、cross-device group-by-target 与 fault isolation。 |

现有硬件 contract 测试实际使用的是 `/mnt/nvme1`、`/mnt/nvme2` 下的 `GPU0/resolver_test` 临时目录；它们在成功后清理。用户提供的 `/mnt/nvme4` 可用，但现有测试没有配置为使用该挂载点，因此本轮没有为它另造一套改变挂载假设的测试。

### 6.2 测试已经证明什么、尚未证明什么

已证明：

- 真实 GPU→NVMe write/read 与字节校验；
- PRP SINGLE/DUAL/LIST、MDTS fan-out、cross extent；
- Runtime 路由、partial commit、双 stream、双 host thread；
- arena/cache 的若干正常与故障路径；
- 单设备 DataPath 组合成多设备 Runtime 路径。

尚未充分证明：

- Handle cache 的“关闭后命中重开，再触发 eviction”的生命周期序列；
- batch `open`，因为公共 API 不存在；
- layerwise 示例的真正三向 read/write/compute 同时在飞程度；
- MACA/MUSA/HIP 等非 CUDA 编译；
- 两个 kernel baseline 的运行时行为一致性、reset/coexistence/stress matrix；
- root CMake 作为统一 release build 的正确性。

---

## 7. 代码质量与开源可维护性

### 7.1 正面评价

1. **接口命名与所有权总体优于旧结构。** `DataPathTarget`、`DataPathMemory`、`DataPathOp`、`RegistrationDomainKey`、`SubmitOutcome` 等身份与生命周期清楚；generation identity 也能拒绝 stale handle。
2. **错误模型明确。** `Status`/`Result`、per-request `initial_states`、partial commit 和 release 语义使异步数据面错误不再依赖 bool 或隐式状态。
3. **性能优化可审计。** `MetadataArena`、PRP cache、cache pinning、completion fence 均有可观察 test seam 和针对性 contract test；这是比“隐藏在 Coordinator 内部的缓存”更易审查的设计。
4. **内核隔离思路好。** 版本兼容、NVIDIA P2P、UAPI 这三类高风险依赖均有明确边界和文档。

### 7.2 可维护性问题

1. **缓存注释与当前能力有陈旧描述。** `HandleWorkspaceCache` 仍写“batch all entries target same file”“open/close single-threaded”，见 `handle_workspace_cache.h:14-19`；但当前硬件测试已覆盖 cross-device batch 与两个 host thread。应更新，避免维护者据此作出错误的线程/生命周期假设。
2. **示例变量过度压缩。** `layerwise_kv_overlap.cu` 的 `rt/dp/tgt/hk/hv/mk/mv/ts/ci/er/ec` 对熟悉 CUDA 的作者可读，但对开源用户与新维护者不友好。该文件承担“当前能力展示”的职责，应改为 `runtime/data_path/targets/hit_keys/hit_values/miss_keys/...` 等可读命名，并拆出 setup、open、prewrite、pipeline、verify 五个 helper。
3. **单函数职责过多。** `LocalNvmeDataPath::submit()` 覆盖验证、extent fan-out、PRP 构造、缓存获取、arena lease、H2D、kernel launch、operation bookkeeping。建议按这些阶段拆为私有 helper；不改变 ABI，也能显著降低 review 风险。
4. **文档入口不一致。** 根 `README.md` 链接 `Roadmap.md`，但该文件不在根目录，见 `README.md:8-15`。`PORTING.md` 仍多处引用已迁移的 `backends/local/...` 路径，例如 `PORTING.md:1-19`。这会直接影响开源用户的第一印象和可复现性。

---

## 8. 发现项与修复优先级

### P0：修复 HandleWorkspaceCache 的 reopen → eviction 悬空 GPU handle

**问题是确定的，不是推测。**

复现逻辑：

1. 首次 `open(A)` cache miss：`get_or_build()` 设置 `Entry::in_use = true`，见 `handle_workspace_cache.h:132-149`。
2. `close(A)`：`release_entry()` 设置 `in_use = false`，并把 entry 加入 LRU，见 `:152-166`；`LocalNvmeDataPath::close()` 随后删除 target，但不删除 cache entry，见 `local_nvme_data_path.cpp:674-709`。
3. 再次 `open(A)` cache hit：`get_or_build()` 只 touch LRU 并返回 entry，没有恢复 `in_use = true`，见 `handle_workspace_cache.h:122-128`。新 target 借用该 entry 的 `handle`，见 `local_nvme_data_path.cpp:636-651`。
4. 再 `open(B)` 且 cache 无空 slot：`acquire_slot_()` 可从 LRU 驱逐 A，并调用 `free_entry_gpu_()`，见 `handle_workspace_cache.h:244-261`。
5. 第二次打开的 A 仍保有已经释放的 `dev_handle`；后续 submit 会使用悬空 GPU 指针。

**最小修复建议：** 使用 `open_refcount`（不是 bool）表达一个 cache entry 被几个打开 target 借用；cache hit 增加 refcount、最后一次 close 才变为可驱逐。保留 submit pin count 作为“in-flight operation pin”，不要混淆 open ownership 与 operation ownership。新增容量为 1 的回归测试：`open(A) → close(A) → open(A) → open(B) → submit(A)`，并验证 A 不能被驱逐。

### P1：统一构建入口与发布矩阵

修复 root/standalone 的双图问题。最低要求：README 明确开发入口；更好的方案：root 通过选项驱动 standalone 的 hardware stack、module、daemon、examples、tests，避免 root 强制关闭新 hardware stack 后再手动搭建一套 libnvm/daemon。

验收应包括：

- root HOST public-only configure/build/test；
- root CUDA + Local NVMe configure/build；
- standalone HOST/CUDA 与 root target 名称不冲突；
- package/install/export 的 header 与 library 集合一致。

### P1：明确 GPU portability 策略并实现或收缩承诺

若“跨设备”是产品目标，应以 Mooncake 的做法为参考：建立 vendor profile、runtime shim、kernel qualifier/launch/fence/atomic 抽象、vendor capability matrix，并逐 vendor 产生真实 CI 编译目标。尤其 Local NVMe 的 CUDA kernel 不能只靠 include alias 获得 HIP/MUSA/MACA 支持。

若短期只支持 NVIDIA，应把 README、架构文档、CMake 选项和 capability naming 明确写为 `NVIDIA CUDA + SNVMe`，避免 `cuda_like` 造成“已支持多厂商”的误解。

### P1：收敛两套 kernel baseline 的行为差异

保留两个 baseline 是现实选择，但需要将 5.4 已验证的资源释放、map 失败回滚、`.release` cleanup 等修复同步到 5.15，或在构建中禁用/标注不安全功能。新增每 baseline 的：build、UAPI handshake、GPU smoke、queue group lifecycle、reset、coexistence/stress gate。仅编译通过不是跨内核完成标准。

### P2：补回或明确放弃 batch open

若上层将频繁管理大量小文件/target，应增加最小的 `open_batch(span<OpenRequest>)` 与每项结果，不要重新引入 Coordinator。它应复用 resolver/DataPath 的已有 routing，并允许逐项失败。若当前 workload 永远是长寿命 target，应在公共契约和示例中明确“不提供 batch open，cache 仅优化重复打开”。

### P2：使 layerwise 示例成为可靠能力说明书

- 用结构化 config 替代压缩变量和长 `main`；
- 参数化多个 GPU/多 NVMe，而非硬编码单 `CUDA device` 与 `/dev/ssnvme0`；
- 以 trace/event 或 in-flight operation 指标证明实际 read/write/compute overlap；
- 将 benchmark 结果与 correctness result 分开，避免 host-side `wait()` 时间被误读成设备流水并发度。

### P2：修复文档与测试边界

- 修复根 README 的 `Roadmap.md` 断链；
- 更新 `PORTING.md` 的旧路径；
- 更新 handle cache 的过时 concurrency/batch 注释；
- 将硬件测试根目录做成环境变量或 test fixture，而不是固定 `/mnt/nvme1`、`/mnt/nvme2`；
- 新增 P0 cache 生命周期、batch open（若实现）、root build graph 和 multi-vendor compile matrix 的测试。

---

## 9. 建议的后续执行顺序

1. **先修 P0 cache ownership，并新增回归测试。** 这是数据正确性和 GPU 内存安全边界，优先于任何抽象扩展。
2. **统一构建入口与文档。** 让一个开发命令对应一个明确的目标图，避免开源用户编到了没有 Local NVMe 的 root 图。
3. **确定跨设备承诺。** 选择“短期 NVIDIA-only 并写清楚”或“正式建设 vendor abstraction”；不要保留半实现宏。
4. **对齐 kernel baseline 语义并建立矩阵门禁。** 保证跨内核不只是“两个目录都能编译”。
5. **根据实际 workload 决定 batch open。** 如不需要，不增加抽象；如需要，按最小 Runtime API 加入。
6. **最后清理示例与历史文档。** 这是降低新贡献者门槛、使项目具备开源可维护性的必要收尾。

---

## 10. 最终判断

当前 `tutti/` 不是一次失败的重命名重构；它已经把原有 Local NVMe 数据面的关键能力放进了更正确、更可替换、更可测试的架构位置，尤其 Runtime/SPI/Resolver/DataPath 的边界、FIEMAP payload、GPU PRP IO、metadata arena、UAPI 与 kernel compatibility isolation 都具备继续演进的价值。

不过“抽象已经有了”不等于“所有历史能力和可移植性目标都已完成”。在当前状态下应作如下准确表述：

> Tutti 已完成 NVIDIA CUDA + Local NVMe 方向的核心数据面重构与硬件契约验证；已具备公共上层/设备下层分离和两个 SNVMe baseline 的移植框架。它尚未完成多 GPU 厂商编译支持、统一根构建交付、旧 batch-open 等价迁移和两个 kernel baseline 的行为对齐；此外必须先修复 handle cache 的 reopen/eviction 生命周期漏洞。

这比“已全面跨内核跨设备”更真实，也更符合一个高质量开源项目应当提供的能力边界与质量承诺。

---

## 11. Round 15 最新复核（2026-08-03）

本节以当前工作树为准，覆盖 R15 的 S1–S6：多 Local-NVMe 路由、`striped://` resolver/binding、Runtime 跨 target 合并提交、容量参数化与 layerwise simulator、fused `StripedDataPath`、重启持久化。它补充并部分更新上文的能力与测试结论；未特别修改的缺口仍保持原判断。

### 11.1 R15 实际带来的能力提升

| R15 项 | 当前源码结论 | 关键依据 |
|---|---|---|
| 同一 DataPath 的跨 target batch | **已实现** | `StorageRuntime` 现在仅按 `DataPath*` 分组，而每个 `DataPathRequest` 保留自己的 target，见 `tutti/include/tutti/storage_runtime.h:1028-1091`；SPI 也已将“同次 submit 可跨 target”写入契约，见 `tutti/include/tutti/spi/data_path.h:342-347`。 |
| 大批量单 submit / 单 kernel launch | **已实现并实测** | `LocalNvmeDataPath` 将容量、最大 in-flight、最大 request 数和最大 request bytes 参数化；Runtime 硬件测试覆盖 64 文件、512 request 的 WRITE/READ 均为一次 DataPath submit 与一次 kernel launch。 |
| 原版 layerwise 单 launch 目标 | **已实现并当前复跑** | 当前 `layerwise_kv_overlap` 使用 `in_flight=4`、`batch_entries=4096`，每个窗口调用硬断言单 submit/单 launch。此次复跑测得读 `6.9 GB/s`、两请求 overlap saving `37%`、`437` 次调用均为单轮、字节验证 `26/26` 通过。它比上文旧结论中的“仅 stream-order 展示”更接近原版的批量性能目标。 |
| 多设备条带数据路径 | **新增且架构边界正确** | `StripedResolver` 将 `striped://` 解析为 pair-private payload；`StripedDataPath` 复用 Local NVMe device primitives，在同一 GPU 上以一个 fused kernel 向 N 个 NVMe 队列派发 stripe entry，见 `tutti/data_paths/striped_local_nvme/striped_data_path.h:5-33`、`fused_submit_kernel.cuh:91-134`。没有修改 Runtime 或 SPI 签名。 |
| 条带化 E2E / 持久化 | **已实现并当前复跑** | 当前 `tutti_striped_local_nvme_contract_test` 覆盖跨 shard roundtrip、单 launch、原始 backing file 分布、跨盘读加速、生命周期、纯 Runtime 公共路径、block addressing、重启持久化和 partial commit；此次复跑成功退出，测试自身汇总 `46/0`。 |
| 社区扩展示例 | **有改进** | `doc/extending_tutti.md:26-44` 新增 striped 的第二个完整扩展示例，证明 resolver/binding/DataPath 包可不改 core 加入。 |

R15 因此显著改善了原报告中“新版 layerwise 示例功能收缩”和“多 NVMe 只可由多个 LocalNvmeDataPath 组合”的结论：现在既有 Runtime 的 cross-target 合并，也有单 GPU、多 NVMe、单 fused kernel 的 `StripedDataPath`。这仍是 NVIDIA CUDA 下的同一 GPU 拓扑能力，**不等价于多 GPU 厂商或 multi-GPU 支持**；该类 capability 仍为 `false`。

### 11.2 本次基于当前源码的验证

本次没有安装/卸载模块、没有改变 daemon 或挂载。已确认 `/mnt/nvme1`、`/mnt/nvme2` 分别挂载到 `/dev/snvme0n1`、`/dev/snvme1n1`，daemon 与 `snvme`/`snvme_core` 模块在运行。

| 验证 | 当前结果 |
|---|---|
| `cmake --build build/r15base -j8` | **成功**；包含 Local NVMe、Runtime E2E、StripedDataPath、layerwise 和 daemon targets。 |
| `ctest --test-dir build/r15base -LE hardware --output-on-failure` | **15/15 通过**。 |
| LocalNvmeDataPath contract | **当前二进制成功退出**；R15 场景包含容量参数化与默认容量 fail-closed，历史/测试内计数为 `820/0`。 |
| Runtime + Local NVMe contract | **当前二进制成功退出**；R15 覆盖 64 文件/512 request 单 launch，历史/测试内计数为 `137/0`。 |
| Striped Runtime E2E | **当前二进制成功退出，`46/0`**。 |
| layerwise simulator | **当前成功**；读 `6.9 GB/s`、overlap `37%`、`multi_round_calls=0`、`26/26` sample 校验通过。 |
| 临时文件 | `/mnt/nvme1`、`/mnt/nvme2` 下未发现 `kvlw_*`、`resolver_test/*` 或 `striped/*` 残留。 |

这使上文测试总数应更新为 R15 终态口径：非硬件 CTest `15/15`；全量注册测试为 `20` 项；LocalNvmeDataPath/Runtime/Striped 的记录计数分别为 `820/0`、`137/0`、`46/0`。这些数字不替代下面列出的生命周期缺陷回归测试。

### 11.3 R15 后仍存在且新增的 P0

#### P0-A：HandleWorkspaceCache 的 reopen → eviction 悬空 handle 未修复

原报告的 P0 仍成立。`HandleWorkspaceCache::Entry` 仍只有 `bool in_use` 与 `pin_count`，没有 open reference count，见 `handle_workspace_cache.h:56-64`。cache hit 仅 `touch_lru_()` 后返回 entry，见 `:122-128`；首次 miss 才设 `in_use=true`，见 `:132-149`；close 又无条件置 `false` 并进入 LRU，见 `:152-166`。

因此 `open(A) → close(A) → open(A)[hit] → open(B)[capacity=1] → submit(A)` 仍可驱逐 A 的借用 GPU handle。R15 现有测试仅覆盖“reopen hit”（test 63）、“in-flight pin 防驱逐”（test 64）和“close 后正常驱逐”（test 65），见 `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp:5511-5680`，未覆盖该组合序列。

**修复仍应使用 `open_refcount` 与独立的 op pin count，并新增 capacity=1 的上述回归测试。**

#### P0-B：StripedDataPath 未把 in-flight operation 关联到 memory，直接 unregister 可提前解除 DMA 映射

这是 R15 新增的确定性数据路径生命周期缺陷。

- `StripedDataPath::OpEntry` 声明了 `memory_token`，见 `striped_data_path.h:227-231`；
- `unregister_memory()` 依赖 `memory_has_inflight_ops_()` 拒绝 in-flight memory，见 `striped_data_path.cpp:577-598`；
- 该检查只比对 `op.memory_token`，见 `:1213-1218`；
- 但 submit 建立 op 时只赋值 `op.target_token = tgt_token`，见 `:955-989`，**没有为 `op.memory_token` 赋值**；该字段保持默认 `0`。

因此直接使用 `StripedDataPath` 时，GPU kernel/NVMe IO 尚在执行，`unregister_memory(real_token)` 不会看到引用并可能 `nvm_dma_unmap`，使仍在使用的 PRP IOVA 被提前撤销。`StorageRuntime` 在上层已用 inflight credits 限制 `Runtime::unregister_memory()`，所以现有 Runtime E2E 没有触发它；但这不能替代 DataPath 自身的 SPI 生命周期保证，且 direct DataPath API 与 LocalNvmeDataPath 都应安全。

R15 的 batch 也允许不同 memory identity；因此修复不应只增加一个 scalar：应如 LocalNvmeDataPath 一样在 op 保存**所有被接受 request 的 memory token**（必要时 target token 也保存集合），并令 `memory_has_inflight_ops_()` 对集合检查。新增至少两类测试：

1. direct StripedDataPath submit 后、event 未完成前 `unregister_memory()` 必须返回 `BUSY`；
2. 一个 batch 使用两个 registered GPU buffers 时，两个 memory 都必须被 BUSY 保护，drain/release 后才能 unregister。

在 P0-A/P0-B 修复前，R15 不能标记为稳定的底层数据面。

### 11.4 R15 引入的 P1/P2 边界

1. **P1：`striped://` logical name 缺少路径成分校验。** Resolver 仅检查 `name.empty()`，见 `tutti/resolvers/striped_file/resolver.h:100-124`，随后直接拼接 `<mount>/striped/<name>.shard<i>`，见 `:227-231`。`name=../x` 或包含 `/` 时可逃出 `striped/` 子目录；LocalFileResolver 验证的是同一 backing block device，见 `local_file/resolver.h:248-278`，不能保证该文件仍位于预期子目录。若 URI 来自不可信上层，应拒绝空、`.`、`..`、`/`、NUL 等路径成分，或改用固定文件名编码规则；并加入 fail-closed contract test。
2. **P1：同一 StripedDataPath 的 batch 仍只实际承载一个 striped target。** SPI 和 Runtime 已允许同一 DataPath 的跨 target batch；但 StripedArena 每 op device table 只容纳一个 target 的 N 个 shard，第二个 target 会逐 request `RESOURCE_EXHAUSTED`，见 `striped_data_path.h:14-22`、`striped_data_path.cpp:669-698`。这是已文档化的容量限制，不是静默违约；但它使 Runtime 的 data-path-only 合并在多个 striped target 时变成 partial commit。应补一个跨两个 striped target 的 Runtime 回归测试，并决定后续是保留此限制还是以 `(target, shard)` 去重扩展 table。
3. **P2：发布完整性仍未达标。** 当前工作树统计为 `91` staged、`30` unstaged、`34` untracked 项；`tutti/include/`、resolver/testing/examples 等核心重构目录处于未提交/部分 ignored 状态。即使 `git diff --check` 当前无空白错误，发布前仍必须 `git status --ignored` 审核所有应纳管源与测试，尤其不能让 `tests/` 被 `.gitignore` 隐藏。
4. **P2：根构建、batch open、多厂商 GPU、kernel baseline 对齐、README 的 Roadmap 断链仍未由 R15 解决。** R15 提升的是 CUDA 下的多 NVMe 访问，不改变这些原报告结论。

### 11.5 更新后的发布判断

R15 是实质性进展：它以正确的 resolver/binding/DataPath 扩展方式恢复并超越了旧版的多文件批量提交目标，并证明了单 GPU 对双 NVMe 的 fused striping、重启持久化、byte-exact correctness 和可测的吞吐提升。

但最终判断应更新为：

> Tutti 已完成 NVIDIA CUDA + Local NVMe + 双设备 striping 的核心重构与 R15 硬件验证；它仍不是可标记为稳定数据面的发布版本。除原有 HandleWorkspaceCache reopen/eviction P0 外，R15 StripedDataPath 还存在直接 unregister in-flight memory 的 P0 生命周期漏洞。修复两项 P0，并完成版本控制/构建入口收敛后，才适合进入对外开源发布评审。
