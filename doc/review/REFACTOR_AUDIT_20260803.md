# Tutti 重构独立审计报告

**审计日期：** 2026-08-03
**审计对象：** 当前工作树 `tutti/`，对照 `third_pkgs/Tutti/` 原始实现、`COMMUNITY_MEETING_001.md`（重构目标）、`doc/design/`（历史关键设计）、`TUTTI_REFACTOR_TAKEOVER.md` / `TUTTI_TARGET_ARCHITECTURE.md`（上一轮方案）与 `chat/`（重构过程记录）。
**范围约束：** 与用户确认一致——应用接入、访存范围扩展不属于本阶段验收项。

---

## 1. 结论摘要

### 1.1 总体判断

本次重构**已经完成了最重要的架构转向**：原来 `Coordinator → memory → block_storage/nvme_storage → io_engine` 的单体调用链，被拆为稳定公共类型、`StorageRuntime`、`DataPath` SPI、`StorageTargetResolver` SPI、binding payload 和具体 `LocalNvmeDataPath`。公共上层不再包含 libnvm、CUDA Queue、SNVMe ioctl 或内核细节；Local NVMe 的具体实现收敛到私有目录。**"在本地原有代码上增加合理抽象、上层与底层分离"这一核心目标基本达成。**

关键数据面能力没有被简单删除：GPU 注册、FIEMAP 物理映射、GPU→NVMe PRP IO、MDTS/extent fan-out、SINGLE/DUAL/LIST PRP、批量 IO、异步完成、stream 顺序、元数据 arena、PRP/handle cache 都有生产实现与硬件契约测试。实测 `137/0`（Runtime+Local NVMe E2E）、`820/0`（DataPath 硬件契约）、`46/0`（Striped 多盘契约）通过，`layerwise_kv_overlap` 端到端示例可跑且结果校验通过，说明重构不止停留在接口层。

**R15（Round 15）已全部完成并硬件验收**：S1 多设备底座、S2 StripedResolver+Binding、S3 Runtime 按 DataPath 分组、S4 容量参数化+simulator、S5 StripedDataPath 单 kernel 融合提交、S6 striped Runtime E2E + 重启持久化 + 门禁。跨设备能力已从"示例层"下沉为正式的 `StripedDataPath` 实现（`tutti/data_paths/striped_local_nvme/`，单 `cudaLaunchKernel` 融合 N 台 NVMe），并有 46/0 硬件契约（含单 launch 计数、跨盘加速比 >1.3×、round-robin 落盘验证、in-flight close BUSY、重启持久化）。

但项目**尚不能以"已全面跨内核、成熟开源底座"验收**。主要阻塞项：

1. **P0：`HandleWorkspaceCache` 存在已确认的 reopen → eviction 悬空 GPU handle 生命周期漏洞**（详见第 7 节，代码路径已逐行核实；R15 未触碰该文件，漏洞仍在，且现有测试未覆盖该组合场景）。
2. **跨 GPU 厂商编译未实现。** `cuda_like.h` 实际只有 CUDA 与 HOST 两个 profile；Local NVMe kernel 与 CMake 直接绑定 NVIDIA CUDA。它不是 Mooncake 那种已落地的多 vendor `cuda_alike` 层。
3. **构建入口分裂。** `cmake -S tutti` 是新架构正确入口；仓库根 `cmake -S .` 为兼容旧 root target 强制关闭 nested `tutti/` 硬件栈，却自行构建 libnvm/daemon 并强依赖 gRPC，形成混合构建图。
4. **跨内核是"两个 baseline 的移植框架"，不是任意内核兼容。** 5.4 Tencent Linux 与 5.15 public 的版本分支/NVIDIA P2P 隔离良好；但 `PORTING.md` 明确记录 5.15 仍缺 5.4 已修复的部分生命周期/失败回滚修复，行为不等价。
5. **旧版批量文件打开与 L1/L2 缓存语义没有完全等价迁移。** 批量 IO 已迁移；批量 `open` 没有；旧两层 L1 GPU/L2 host-pinned cache 被有意收缩为单层 GPU handle cache + PRP page cache + per-op arena，方向合理但应写清"新语义 ≠ 旧语义等价"。

**发布判断：** 当前可作为 "NVIDIA CUDA + Local NVMe 重构验证版本"，不应在修复 P0 前标记为稳定数据面或对外承诺多厂商 GPU 支持。

### 1.2 目标符合度总表

| 目标 | 评估 | 依据 |
|---|---|---|
| 上层与底层分离 | **基本满足** | 公共 Runtime/SPI 无 Local NVMe、CUDA、libnvm 私有类型；设备实现位于 `data_paths/local_nvme/`，有 header hygiene 负向测试 |
| 可替换的数据路径抽象 | **满足** | `DataPath` SPI 覆盖 target/memory/submit/progress/release；resolver/binding 把文件解析与传输路径解耦 |
| GPU file ↔ NVMe file 映射 | **满足且增强** | `LocalFileResolver` 用 FIEMAP、校验 backing block device/extent 覆盖，产出 `ext4_local_nvme` payload |
| 批量 IO、PRP、异步 stream 顺序 | **满足** | 820 断言硬件契约覆盖 fan-out、SINGLE/DUAL/LIST、partial commit、跨 stream |
| 多设备 striped 数据面（原版跨盘 shard） | **满足（R15 S5/S6）** | `StripedDataPath` 单 kernel 融合 N 盘提交；46/0 契约含单 launch、跨盘加速比、round-robin 分布、重启持久化 |
| 原版 batch 文件打开 | **未等价迁移** | 新 Runtime 只有单目标 `open()`；无 `open_batch()` 批量语义 |
| L1/L2 元数据内存池/缓存 | **有意简化，不等价** | 有 `MetadataArena`、GPU handle LRU、PRP cache；无旧 L1 GPU/L2 host-pinned inclusive cache 与 batch promotion |
| 跨内核编译 | **部分满足** | 两 baseline、compat/P2P/UAPI 隔离良好；非通用内核兼容层，baseline 行为有差异 |
| 跨 GPU 厂商编译 | **未满足** | 实际支持 CUDA + HOST；MACA/MUSA 分支 `#error` 未实现；kernel 直接用 CUDA runtime |
| 编译底座采用 cuda-like 方式 | **结构上有，实质未达** | 有 `cuda_like.h` selector 与 `gpu_vendor/` 目录骨架，但只有 CUDA/HOST shim；与 Mooncake 多厂商 `cuda_alike` 差距明显 |
| 应用接入 | 不纳入本阶段 | 符合范围 |
| 访存范围扩展 | 不纳入本阶段 | 符合范围 |

---

## 2. 当前架构是否合理

### 2.1 分层结果（合理部分）

最合理的部分是"文件如何解析"与"如何访问该文件"分离：

- `tutti/include/tutti/`：稳定 value types、状态类型、Runtime 与 SPI；
- `StorageTargetResolver`：URI/文件系统对象 → 不透明 `ResolvedTarget` payload；
- `bindings/ext4_local_nvme/`：定义 payload type id/version 与读写视图；
- `LocalNvmeDataPath`：controller、queue group、DMA registration、GPU target handle、PRP、completion；
- `StorageRuntime`：按 URI scheme 找 resolver、按 binding 推荐 DataPath key 路由、统一 memory/IO handle 生命周期；
- `device_manager/nvme/`：libnvm、daemon、SNVMe 模块，位于数据路径下方，不泄漏到公共头。

具体证据：

- `DataPath` SPI 只暴露不透明身份、byte range、能力与操作状态，明确禁止 transport/device/kernel 私有类型进入接口（`tutti/include/tutti/spi/data_path.h`）。
- `StorageRuntime` 通过 resolver URI scheme + `recommended_data_path_key` 组装，不直接依赖 Local NVMe（`tutti/include/tutti/storage_runtime.h`）。
- `LocalNvmeDataPath` 私有头才引入 metadata、`nvm_ctrl.h`、`nvm_dma.h`（`tutti/data_paths/local_nvme/local_nvme_data_path.h`）。
- header hygiene 负向测试证明：只链接公共 API 的消费者不能意外 include Local NVMe 私有头。

这不是"把旧类改名"的抽象，而是实际的依赖方向修正。

### 2.2 仍需承认的边界

- 隔离是**公共消费者隔离**，不是"示例也完全不知本地设备"。硬件示例仍显式构造 `LocalNvmeDataPath` 与 `LocalFileResolver` 并写死配置。设备 bring-up/benchmark 合理，但不代表完全 provider-neutral 应用范式。
- `StorageRuntime` 是单头实现且体量大（约 1600 行）；`LocalNvmeDataPath::submit()` 同时承担验证、fan-out、PRP 分配、H2D、kernel launch、缓存 pinning、操作建账。正确性已被强测试覆盖，但维护成本随下一个 DataPath 出现而上升。
- 测试环境备注：硬件 contract 测试实际使用 `/mnt/nvme1`、`/mnt/nvme2` 下的 `GPU0/resolver_test` 临时目录并自行清理；`/mnt/nvme4` 可用但现有测试未配置使用，本审计未为它另造挂载假设。

---

## 3. 原版 `kv_cache_layerwise_overlap.cu` 能力迁移评估（关键优化是否放进来）

该文件是用户指定的"老版本所有能力的代表"。按能力逐项比对：

| 原版能力 | 当前状态 | 证据与评价 |
|---|---|---|
| 批量创建/打开 GpuFile | **未等价迁移** | 原版调 `Coordinator::open_gpu_files_batch()`（`third_pkgs/Tutti/examples/adapters/kv_cache_layerwise_overlap.cu:261-301`），底层并行批量建 shard。新版 `StorageRuntime::open()` 是单目标 API，示例逐个 `open`（`tutti/examples/layerwise_kv_overlap/layerwise_kv_overlap.cu:314-321`）。cache 降低重复 open 成本，但不提供批量 API/批量错误结果/批量原子性 |
| 批量读写 | **已迁移** | Runtime/SPI 可一次提交多 request；Local NVMe 展平为 device entries（`local_nvme_data_path.cpp`）。硬件测试覆盖 mixed batch、partial commit、跨设备 group-by-target |
| FIEMAP → LBA 映射 | **已迁移且增强** | 职责移到 `LocalFileResolver`（`tutti/resolvers/local_file/resolver.h`），校验 backing device、unsafe flags 与完整覆盖 |
| GPU memory → NVMe PRP | **已迁移** | `register_memory()` 走 `nvm_dma_map_data_device`；submit 按 DMA IOVA 构造 PRP1/2/list |
| MDTS 与跨 extent fan-out | **已迁移** | `min(remaining, effective_mdts)` + extent end 切分；实测 256 KiB fan-out、跨 segment、SINGLE/DUAL/LIST |
| L1 GPU + L2 host-pinned handle cache | **有意简化，不等价** | 新版 = `targets_`（host template）+ `HandleWorkspaceCache`（单层 GPU LRU）+ `PrpPageCache`（LIST page）+ `MetadataArena`（event/entries/status/PRP 复用）。保留热路径复用，无二层 inclusive LRU、batch promotion、stream-fenced slot reuse。对"每文件长驻 target"模型可接受；对"海量短生命周期文件批量打开"不是等价替代 |
| per-op 内存池 | **已迁移且改进** | `MetadataArena` 预分配 per-op event、entry/status GPU arrays、DMA-mapped PRP pool；submit 只 acquire/release lease；测试覆盖 exhaustion、recovery、hot-path zero allocation |
| handle/PRP 缓存 | **已实现，但有 P0** | 见第 7 节 |
| 读/算/写三 stream overlap | **能力保留，示例非完全三向在飞** | 新示例保留 `s_r/s_c/s_w` + event DAG（`layerwise_kv_overlap.cu:270-275, 419-456`），但每个 `windowed_submit_wait()` 都先 `wait()`，注释明说 read/write 非同时在飞，重叠主要来自 compute 与 host 阻塞 IO 等待。有效 stream-order 演示 + E2E benchmark，但不等同于持续三向流水 |
| 多 NVMe、多 GPU placement | **已迁移（R15 S5/S6）** | 原版示例要求 ≥2 设备并 round-robin shard（`kv_cache_layerwise_overlap.cu:245-246, 287-294`）。新版 `LocalNvmeDataPath` 单实例 `supports_multi_gpu=false`，但 `StripedDataPath`（`tutti/data_paths/striped_local_nvme/`）实现跨 N 盘单 kernel 融合提交：host 侧 stripe 切分 + 单 H2D + 单 launch + 单 event；测试 82-86 覆盖 roundtrip/单 launch(N=1,N=2)/跨盘加速比(1.43-1.51×)/round-robin 分布/生命周期；Runtime 契约测试另覆盖 dual-device IO 与 fault isolation（测试 78-81） |
| 持久化 / 重启重开 | **已实现（R15 S6）** | striped 契约测试 89：Phase1 写入后完整 teardown（close→unregister→shutdown→销毁 env_a），Phase2 全新 Runtime+Resolver+DataPath 重开同一 `striped://` URI 读回字节校验通过 |

### 3.1 对"关键优化是否放进来"的直接回答

- **batch 文件打开：没有完整放进来。** 有 target handle cache，但没有 public `open_batch()`；功能/效率语义上明确缺口。
- **batch IO：已经放进来。** 且比旧上层 API 更明确地表达 partial commit、每 request 初始状态、operation 生命周期与资源限额（`SubmitOutcome`/`initial_states` 契约）。
- **L1/L2 元数据内存池：没有原样照搬，但核心热路径优化已放进来。** `MetadataArena`、GPU handle cache、PRP cache 是更小、更贴近 Local NVMe 的拆法，定位应为"简化后的新设计"，不是"旧 cache 同名迁移"。
- **GPU file-NVMe file 映射：已放进来，且架构位置更正确。** 文件物理映射在 resolver，GPU DMA/NVMe command 在 DataPath，避免上层 Coordinator 同时掌控文件系统与设备协议。

---

## 4. 编译底座与可移植性（与 Mooncake 对比）

### 4.1 关键结论：Tutti 的 `cuda_like` 只是 "CUDA/Host profile selector"，不是 Mooncake 的多厂商 device abstraction

**Tutti 现状：**

- CMake profile 只接受 `CUDA` 与 `HOST`（`tutti/CMakeLists.txt`）。
- CUDA profile 直接要求 NVIDIA `CUDAToolkit`，链接 `CUDA::cudart` 与 `CUDA::cuda_driver`（`tutti/cmake/accelerators/CUDA.cmake`）。
- `cuda_like.h` 只允许 `TUTTI_USE_CUDA` 或 `TUTTI_USE_HOST` 二选一；`TUTTI_USE_MACA`/`TUTTI_USE_MUSA` 分支明确 `#error "shim is not implemented"`（`tutti/include/tutti/cuda_like.h:31-35`）。
- Local NVMe 实现直接 include `<cuda_runtime.h>`，使用 CUDA event/stream/memcpy 与 CUDA `.cu` kernel（`local_nvme_data_path.cpp`、`io/`）。
- HOST profile 是硬件无关 contract test 的同步 shim，不是可执行 GPU 后端（`tutti/cmake/accelerators/HOST.cmake`）。
- **GPU kernel 编译没有为不同厂商提供不同宏**：kernel 代码直接写 `__global__`/`__device__`，没有 vendor qualifier 映射层；也没有 MACA/HIP 的编译路径。`gpu_vendor/` 目录只有 `host.h`。

**Mooncake 做法（对照）：**

- `third_pkgs/Mooncake/mooncake-transfer-engine/include/cuda_alike.h` 是真实 vendor selector，覆盖 CUDA、HIP、MUSA、MLU、MACA、SUNRISE 等，各厂商有独立 `gpu_vendor/*.h` shim。
- CMake 通过 `USE_CUDA`/`USE_HIP`/`USE_MACA` 等开关选择 profile。

**结论：** 若"跨设备编译"指 NVIDIA / AMD / MUSA / MACA 等不同 GPU 厂商，当前**不满足**。仅增加几个宏名没有意义；必须同时抽象 host runtime API、kernel qualifier/launch、stream/event、atomic/fence、device memory、peer-memory/driver capability 与 CMake toolchain profile。这是与用户提问"编译底座是否和 Mooncake 一样、是否为不同厂商提供不同宏"最直接的回答：**结构上借鉴了（selector + gpu_vendor 目录），实质只落地了 CUDA + HOST 两档。**

### 4.2 跨内核工作（这部分是优点）

- 两套 baseline：`tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017/` 与 `snvme-5.15.0-public/`。
- 内核版本条件集中在 `compat.c`/`compat.h`（唯一含 `LINUX_VERSION_CODE` 的翻译单元）。
- NVIDIA P2P 接触面集中在 `peer_memory.c`/`peer_memory.h`（唯一含 `nv-p2p.h` 的单元，`peer_memory_ops` 函数指针表 + opaque 类型）。
- kernel/userspace 共用固定宽度 UAPI，带 ABI version/capabilities fail-closed handshake 与 layout assertions（`tutti/include/uapi/tutti_snvme.h`）。

但应描述为"**针对两个 NVMe baseline 的可移植框架**"，不是"跨 Linux 内核自动兼容"。`PORTING.md` 提示 5.15 仍有部分 5.4 已修复未回填的生命周期/失败回滚问题。

---

## 5. 构建图与 Round 15 状态

### 5.1 正确的新架构入口

`cmake -S tutti -B <build>` 是新架构合理入口：始终构建 `tutti_api`/`tutti_spi`，CUDA + hardware stack 条件满足时构建 `device_manager` 与 `data_paths/local_nvme`。可：
- `TUTTI_ACCELERATOR=HOST` → 硬件无关 API/SPI contract 基座；
- `TUTTI_ACCELERATOR=CUDA` → Local NVMe 生产路径；
- 避免 libnvm 私有 include 泄漏到公共头。

本审计实测：HOST profile 全新配置 + 全量编译 + 非硬件契约测试全绿（见 §6）。

### 5.2 根 CMake 的问题

根 `CMakeLists.txt` 同时保留旧兼容职责：
- 在 add `tutti/` 前强制 `TUTTI_BUILD_HARDWARE_STACK=OFF`；
- 随后自行构建 libnvm、SNVMe module、nvmeservice、daemon；
- 无条件 `find_package(gRPC CONFIG REQUIRED)`，即使只希望编译 HOST 公共 API 也会引入 daemon 依赖。

造成"根构建 = public API + 旧式硬件库/daemon 混合图；standalone 构建 = 新 LocalNvmeDataPath 图"的认知负担。下一阶段应二选一：root 成为真正 superbuild（选项驱动），或明确 root 只管部署工具、README 标注 `cmake -S tutti` 为唯一开发入口。

### 5.3 Round 15 现状（2026-08-03，**已全部完成**）

按 `chat/round15/result5.md` / `result6.md`（与当前树一致）：
- **绿色基线（build/r15base）：** datapath 契约 **820/0**、Runtime E2E **137/0**、striped 契约 **46/0**、HOST/CUDA 非硬件 ctest 各 **15/15**；全量 ctest 20/20；`git diff --check` clean。
- **S1（多设备底座）/S2（StripedResolver+Binding）**：已验收。
- **S3（Runtime 按 DataPath 分组）/S4（容量参数化+simulator）**：已验收。
- **S5（StripedDataPath 融合单 kernel）**：**已实现并硬件验证**。`tutti/data_paths/striped_local_nvme/` 全套（`striped_data_path.*`、`striped_arena.*`、`fused_submit_kernel.*`）；共享 device 原语抽离到 `tutti/data_paths/local_nvme/io/nvme_submit_primitives.cuh`（`submit_one.cuh` device 区段逐字节搬移，820/0+137/0 零回归）；单 launch 计数、跨盘加速比 1.43-1.51×、round-robin 落盘、BUSY/drain 生命周期均过；PRP SINGLE/DUAL/LIST 全路径无留尾；submit 路径零 per-op cudaMalloc。踩坑记录：`initial_states` 默认值（REJECTED）被误当"跳过已拒绝"判据导致整批误拒，已用独立 `std::vector<bool> rejected` 解耦修复（host 逻辑 bug，非 kernel hang）。
- **S6（striped Runtime E2E + 重启持久化 + 门禁）**：**已实现并硬件验证**。新增测试 87-90：零 striped 感知的 public 全路径、block 编址（KV-pool 模型）、重启持久化（全新 env 重开同 URI 读回校验）、故障 partial commit。仅改测试文件与 `doc/extending_tutti.md`，核心代码零改动。
- 历史缠结备份：`~/tutti-r15-tangled-20260803.tar.gz`（S3/S4 缠结回退记录，已清理）。

---

## 6. 实测验证（本审计实际执行）

复用运行中的 SNVMe、daemon 与已挂载测试环境（未 insmod/rmmod、未改设备）。

| 验证 | 结果 | 说明 |
|---|---|---|
| standalone HOST profile 全新配置+编译 | **通过** | `cmake -S tutti -B /tmp/tutti-host-check -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON` + `cmake --build . -j8` 全目标成功 |
| HOST 非硬件契约测试 | **全绿** | io_types / status(12) / data_path / storage_target_resolver(12) / binding(12) / storage_runtime(36) / striped_resolver(59) 等 |
| CUDA 硬件无关契约 | **全绿** | status、io_types、memory_types、binding、data_path、resolver、storage_target_resolver、storage_runtime、striped_resolver、memfs_sample、cuda_like 等（build/r15base/bin） |
| resolver 契约 | **22/22** | payload 兼容性、fd-lease 等 |
| StorageRuntime + Local NVMe E2E | **137/0** | 装配/open、lazy registration、SINGLE/DUAL/LIST、跨 extent、mixed batch、partial commit、同/双 stream、双 host thread、timeout、shutdown retry、重复 lifecycle、容量参数化 section 8/9 |
| LocalNvmeDataPath 硬件契约 | **820/0** | capabilities、target/memory identity、host/device registration、PRP、completion error、arena exhaustion/reuse/zero-alloc、handle/PRP cache、fan-out、dual device、cross-device group-by-target、fault isolation、S4 容量 section 84/85 |
| Striped Local-NVMe 契约 | **46/0** | 82 roundtrip（单/跨 shard、非对齐、LIST）、83 单 launch（N=1/N=2）、84 跨盘加速比（复测 1.51×）、85 round-robin 分布、86 生命周期 BUSY/drain、87 public 全路径、88 block 编址、89 重启持久化、90 故障 partial commit |
| `tutti_layerwise_kv_overlap`（缩小规模：16 layers / 32K ctx / 256 chunk / 1 req） | **通过** | Phase A-H 全过，结果校验 OK，windowed 单轮断言成立（rounds==calls，multi_round==0） |

注：`layerwise_kv_overlap` 默认数据目录为 `/mnt/nvme1/GPU0`，测试后清理 `kvlw_*` 临时文件。

**已证明：** 真实 GPU→NVMe 读写字节校验、PRP 全形态、MDTS fan-out、Runtime 路由与 partial commit、双 stream/双 thread、arena/cache 正常与故障路径、单设备 DataPath 组合成多设备 Runtime 路径、跨盘 striped 单 kernel 融合与重启持久化。

**尚未充分证明：** handle cache 的"关闭后命中重开 + 触发 eviction"生命周期序列（P0，未测覆盖）；batch open（API 不存在）；layerwise 真正三向同时在飞；非 CUDA 编译；两 kernel baseline 运行时行为一致性矩阵；root CMake 作为统一发布构建。

---

## 7. 代码质量、命名与开源可维护性

### 7.1 命名与接口评价（用户重点关注项）

**正面：**

- 接口命名与所有权总体优于旧结构：`DataPathTarget`、`DataPathMemory`、`DataPathOp`、`RegistrationDomainKey`、`SubmitOutcome`、`IoRequestState::ACCEPTED/REJECTED`、generation identity 拒 stale handle——身份与生命周期清楚。
- 错误模型明确：`Status`/`Result`、per-request `initial_states`、partial commit、release 语义，异步数据面错误不再依赖 bool 或隐式状态。
- 性能优化可审计：`MetadataArena`、PRP cache、cache pinning、completion fence 有可观察 test seam 与针对性 contract test。

**可改进：**

1. **示例变量过度压缩。** `layerwise_kv_overlap.cu` 中 `rt/dp/tgt/hk/hv/mk/mv/ts/ci/er/ec/gb/gg` 对熟悉 CUDA 的作者可读，但对外部新维护者不友好。该文件承担"当前能力展示"职责，应改为 `runtime/data_path/targets/hit_keys/hit_values/miss_keys/...` 等可读命名，并拆出 setup/open/prewrite/pipeline/verify 辅助函数。
2. **单函数职责过多。** `LocalNvmeDataPath::submit()` 覆盖验证、extent fan-out、PRP 构造、缓存获取、arena lease、H2D、kernel launch、操作建账。建议按阶段拆私有 helper，不改变 ABI 也能显著降低 review 风险。
3. **`StorageRuntime` 单头过大。** 公共头包含完整实现（~1600 行），降低可读性；可考虑把实现迁到 `.cpp`（header-only 对测试友好，但作为开源公共 API 不利）。
4. **缓存注释过时。** `handle_workspace_cache.h` 仍写"batch all entries target same file""open/close single-threaded"，但当前硬件测试已覆盖 cross-device batch 与双 host thread。应更新，避免维护者据此作错误线程/生命周期假设。
5. **命名不一致痕迹。** `cuda_like.h` 保留 `TUTTI_USE_MACA`/`TUTTI_USE_MUSA` 互斥检查与 `#error` 占位，但并无对应实现——保留骨架可以，但文档必须明确"未实现"，否则 `cuda_like` 易被误读为已支持多厂商。
6. **文档断链/陈旧。** 根 `README.md` 链接 `Roadmap.md` 但该文件不在根目录；`PORTING.md` 仍引用已迁移的旧路径。直接影响开源第一印象与可复现性。

### 7.2 已确认的 P0 缺陷（必须修复）

**HandleWorkspaceCache reopen → eviction 悬空 GPU handle**（代码路径逐行核实）：

1. 首次 `open(A)` cache miss：`get_or_build()` 设 `Entry::in_use = true`（`handle_workspace_cache.h:138`）。
2. `close(A)`：`release_entry()` 设 `in_use = false` 并加入 LRU（`:155-166`）；`LocalNvmeDataPath::close()` 删 target 但不删 cache entry（`local_nvme_data_path.cpp:674-709`）。
3. 再次 `open(A)` cache hit：`get_or_build()` 只 `touch_lru_()` 返回 entry，**没有恢复 `in_use = true`**（`:122-128`）；新 target 借用该 entry 的 `handle`。
4. 再 `open(B)` 且 cache 无空 slot：`acquire_slot_()` 可从 LRU 驱逐 A 并 `free_entry_gpu_()`（`:244-262`）。
5. 第二次打开的 A 仍保有已释放的 `dev_handle`；后续 submit 使用悬空 GPU 指针 → 数据面崩溃/静默错误。

**最小修复建议：** 用 `open_refcount`（int）替代 bool `in_use`：cache hit 增 refcount；最后一次 close 才置可驱逐。保留 submit `pin_count` 作 in-flight operation pin，不混淆 open ownership 与 operation ownership。新增容量为 1 的回归测试：`open(A)→close(A)→open(A)→open(B)→submit(A)`，断言 A 不被驱逐。

---

## 8. 发现项与修复优先级

### P0
- 修复 `HandleWorkspaceCache` reopen→eviction 悬空 GPU handle（§7.2），并加回归测试。

### P1
- **统一构建入口与发布矩阵。** root/standalone 双图二选一；验收：root HOST public-only 可配/可建/可测，root CUDA+Local NVMe 可建，standalone 与 root target 不冲突，install/export header 集合一致。
- **明确 GPU portability 策略并兑现或收缩承诺。** 参考 Mooncake：vendor profile + runtime shim + kernel qualifier/launch/fence/atomic 抽象 + vendor capability matrix + 每 vendor 真实 CI 编译目标。若短期只做 NVIDIA，则 README/架构文档/CMake 选项/capability naming 明确写 "NVIDIA CUDA + SNVMe"，删除易误导的占位宏或注明未实现。
- **收敛两 kernel baseline 行为差异。** 将 5.4 已验证的资源释放/map 失败回滚/.release cleanup 同步到 5.15 或构建期禁用标注；新增每 baseline 的 build、UAPI handshake、GPU smoke、queue group lifecycle、reset、coexistence/stress gate。仅"两目录都能编译"不算跨内核完成。

### P2
- **补回或明确放弃 batch open。** 若上层会频繁管理大量小文件/target，加最小 `open_batch(span<OpenRequest>)` + 逐项结果，复用 resolver/DataPath 已有 routing；若 workload 永远是长寿命 target，在公共契约与示例中明确"不提供 batch open，cache 仅优化重复打开"。
- **把 layerwise 示例做成可靠能力说明书。** 结构化 config、参数化多 GPU/多 NVMe、用 trace/in-flight 指标证明真实 overlap、benchmark 与 correctness 分离。
- **修复文档与测试边界。** 修根 README 断链、PORTING.md 旧路径、handle cache 过时注释；硬件测试根目录做成环境变量/fixture；补 P0 cache 生命周期、batch open（若实现）、root build graph、multi-vendor compile matrix 测试。

---

## 9. 建议执行顺序

1. **先修 P0 cache ownership + 回归测试**（数据正确性与 GPU 内存安全边界，优先于任何抽象扩展）。
2. **统一构建入口与文档**（一个开发命令 = 一个明确目标图）。
3. **确定跨设备承诺**（短期 NVIDIA-only 并写清，或正式建设 vendor abstraction；删除半实现宏）。
4. **对齐 kernel baseline 语义 + 矩阵门禁**。
5. **按实际 workload 决定 batch open**（不需要则不加抽象，需要则最小 Runtime API）。
6. **收尾 R15 遗留文档**：StripedDataPath/S6 已交付，剩余为示例可读性、`doc/extending_tutti.md` 之外的历史文档更新与贡献者入门文档，降低新贡献者门槛。

---

## 10. 最终判断

当前 `tutti/` 不是失败的重命名重构：它把原有 Local NVMe 数据面的关键能力放进了更正确、更可替换、更可测试的架构位置——Runtime/SPI/Resolver/DataPath 边界、FIEMAP payload、GPU PRP IO、metadata arena、UAPI 与 kernel compatibility isolation 都具备继续演进的价值；批量 IO、PRP 全形态、stream 顺序、**跨设备 striped 单 kernel 融合**与重启持久化均有硬件验证。

但"抽象已经有了"不等于"所有历史能力和可移植性目标都已完成"。准确表述应为：

> **Tutti 已完成 NVIDIA CUDA + Local NVMe 方向的核心数据面重构与硬件契约验证（datapath 820/0、Runtime E2E 137/0、striped 46/0、全量 ctest 20/20），含跨盘 StripedDataPath 单 kernel 融合提交与重启持久化；具备公共上层/设备下层分离和两个 SNVMe baseline 的移植框架。编译底座采纳了 cuda-like 的 selector 骨架但只落地 CUDA/HOST 两档，尚未实现多 GPU 厂商编译；尚未完成旧 batch-open 等价迁移、统一根构建交付与两个 kernel baseline 行为对齐；此外必须先修复 handle cache 的 reopen/eviction 生命周期漏洞（P0）。**

这比"已全面跨内核跨设备"更真实，也更符合高质量开源项目应提供的能力边界与质量承诺。
