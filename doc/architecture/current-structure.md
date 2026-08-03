# Tutti 当前项目结构（对照目标架构）

**日期：** 2026-08-03（refact 分支 26f1f7e + 8de7c5f 提交后）
**对照基准：** [`doc/TUTTI_TARGET_ARCHITECTURE.md`](../TUTTI_TARGET_ARCHITECTURE.md)（2026-07-30 草案）
**用途：** 给 maintainer review 用——当前实现结构与目标架构的逐项对应与偏差声明。

---

## 1. 当前目录树（生产部分）

```text
tutti/
├── include/tutti/                 # 公共 API + SPI（厂商中立，header-only）
│   ├── storage_runtime.h          #   StorageRuntime 唯一公共门面（~1600 行）
│   ├── io_types.h / memory_types.h / status.h
│   ├── cuda_like.h                #   CUDA-like profile 选择器（CUDA/HOST；MACA/MUSA=#error 占位）
│   ├── gpu_vendor/host.h          #   HOST profile shim
│   └── spi/
│       ├── data_path.h            #   DataPath SPI（9 纯虚方法 + 跨 target submit 契约注释）
│       └── storage_target_resolver.h
├── include/uapi/
│   └── tutti_snvme.h              # kernel/userspace 共享 UAPI（ABI 版本握手 + layout 断言）
│
├── bindings/                      # pair-private payload 契约
│   ├── ext4_local_nvme/           #   ext4 文件 ↔ local NVMe payload
│   ├── striped_local_nvme/        #   striped 逻辑目标 ↔ N 子目标 bundle
│   └── memfs/                     #   社区扩展样例（含 MemfsDataPath）
│
├── resolvers/
│   ├── local_file/                #   file:// → FIEMAP + backing 设备校验（fail-closed）
│   ├── striped_file/              #   striped:// → N 个 LocalFileResolver 组合
│   └── memfs/                     #   样例
│
├── data_paths/
│   ├── local_nvme/                # 生产 DataPath（单设备）
│   │   ├── local_nvme_data_path.{h,cpp}
│   │   ├── io/                    #   queue group、device target、submit_one kernel、
│   │   │                          #   nvme_submit_primitives.cuh（共享原语，S5 抽离）
│   │   └── metadata/              #   MetadataArena、HandleWorkspaceCache、PrpPageCache
│   └── striped_local_nvme/        # 跨设备 fused DataPath（单 launch 融合提交）
│       ├── striped_data_path.{h,cpp}
│       ├── striped_arena.{h,cpp}
│       └── fused_submit_kernel.{cuh,cu}
│
├── device_manager/nvme/           # 硬件栈唯一事实源
│   ├── libnvm/                    #   用户态 NVMe 库
│   ├── nvmeservice/               #   gRPC daemon（设备 bring-up + 队列分配）
│   └── kernel_modules/            #   snvme 双 baseline（5.4.241-tlinux4 / 5.15-public）
│       └── 各自 compat.{c,h} + peer_memory.{c,h} 隔离单元
│
├── testing/                       # MockDataPath 契约套件（社区扩展测试工具）
├── examples/layerwise_kv_overlap/ # layerwise KV overlap simulator（硬件示例）
└── cmake/accelerators/            # CUDA.cmake / HOST.cmake profile 定义

tests/                             # 契约测试（repo 根，由 tutti/CMakeLists.txt 接线）
```

---

## 2. 目标架构 → 当前实现 对照图

```text
目标架构（§4.1 用户态总图）                当前实现
─────────────────────────────────────────────────────────────────────────
Applications / Framework Adapters    ──►  ⚠ 未实现（Phase 5 显式暂缓；
  C++ app | VllmAdapter | LmCache           prompts 存 doc/history/chat/
                                           round12/deferred-adapter/）
                                           C++ app 侧 = examples/layerwise_kv_overlap

StorageRuntime                     ──►  ✅ tutti/include/tutti/storage_runtime.h
  Memory/Target/Io registries            （registries/routing/backpressure/completion
  routing/backpressure/completion         全部落地；R15 S3 起按 DataPath 分组合并提交）

tutti/cuda_like.h                  ──►  ◐ include/tutti/cuda_like.h + gpu_vendor/host.h
  CUDA | MACA | MUSA shim                 CUDA/HOST 两档实测；MACA/MUSA 仅 #error 占位

StorageTargetResolver/Binding      ──►  ✅ resolvers/ + bindings/
                                         local_file、striped_file、memfs（样例）
                                         ext4_local_nvme、striped_local_nvme、memfs

DataPath                           ──►  ✅ spi/data_path.h + 两个实现：
  LocalNvmeDataPath / future             data_paths/local_nvme（生产）
                                         data_paths/striped_local_nvme（R15 新增，
                                         目标架构未预见——见 §3 偏差 D1）

local-NVMe 部署单元                ──►  ✅ device_manager/nvme/{libnvm,nvmeservice,
  libnvm / NVMeService / snvme            kernel_modules}
  唯一版本化 UAPI                         ✅ include/uapi/tutti_snvme.h（ABI=1 握手）
  kernel compat ops                       ✅ compat.{c,h}（唯一含 LINUX_VERSION_CODE）
  peer-memory ops                         ✅ peer_memory.{c,h}（peer_memory_ops 表）
```

---

## 3. 与目标架构的偏差（必须知晓的 8 项）

| # | 目标架构的说法 | 当前实际 | 处置建议 |
|---|---|---|---|
| D1 | §2.2 明确不做"WAL、**striping**" | R15 实现了完整 striping（StripedResolver + binding + StripedDataPath fused 单 launch），经 maintainer 决策并硬件验收 | 更新目标架构文档：striping 从"不做"移到"已做"，补 StripedDataPath 组件定义 |
| D2 | §4.2 local-NVMe package 内含 `control/`（DirectNvmeResourceProvider / NvmeServiceResourceProvider） | `control/` 已删除（2026-08-03 死代码清理：DataPath 直链 libnvm，`nvm_ctrl_attach_client` + daemon 队列分配，driver 抽象无消费者） | 更新目标架构 §4.2：控制面 = DataPath 直用 libnvm + nvmeservice daemon，无中间 driver 层 |
| D3 | §4.2 metadata 含 `TieredHandleCache` | 单层 `HandleWorkspaceCache`（GPU LRU + pin）+ `PrpPageCache` + `MetadataArena`；旧两层 L1/L2 未迁移（有意简化，语义不等价） | 更新组件名；文档明确"重新设计的缓存模型 ≠ 旧 L1/L2 迁移" |
| D4 | §4.1 FrameworkAdapter 在调用链顶层 | 未实现（maintainer 暂缓） | 目标架构保留为 future；当前调用链顶层 = C++ 应用直用 StorageRuntime |
| D5 | §2.1 "NVIDIA / MACA / MUSA" CUDA-like profile | 只有 CUDA/HOST 两档；MACA/MUSA 为 #error 占位 | 二选一：写死 NVIDIA-only，或立项 vendor abstraction（见 roadmap P1） |
| D6 | §4.1 StorageRuntime "routing/grouping" 按 target 分组语义隐含 | R15 S3 起按 DataPath 分组（一次 submit 跨 target、一次 kernel launch）；SPI 契约注释已同步 | 目标架构 §8.3 Routing/grouping 节需更新为 data-path-only 分组 |
| D7 | §4.2 `interop/cuda_like` 为 package 内子层 | 实现为 `include/tutti/cuda_like.h` + `gpu_vendor/`（公共层，非 local_nvme 私有） | 位置说明即可 |
| D8 | §2.1 "host-initiated/device-executed + host-executed 两条路径" | 当前仅 device-executed（DEVICE_EXECUTION）一条生产路径；`supports_host_execution=false` | 目标架构缩小为单路径，或将 host-executed 列为 future |

---

## 4. 目标架构验收条件逐条核对（§1 的五条）

| # | 验收条件 | 状态 | 证据 |
|---|---|---|---|
| 1 | 应用可用 cuda_like API 但不 include 厂商 shim/libnvm/PRP/LBA/FIEMAP | ✅ | header_hygiene 负向测试（26 个私有头） |
| 2 | StorageRuntime 不理解具体 storage descriptor 或 DataPath kernel | ✅ | Runtime 只经 SPI；pair-private payload |
| 3 | 新增 DataPath/profile/Resolver 不改公共请求模型 | ✅ | memfs + striped 两个零 core 改动实证 |
| 4 | metadata pool/kernel/completion 可独立优化 | ✅ | R11 arena/cache 与 R15 容量参数化均未动公共 API |
| 5 | file/KV 路径从公共 API 走完注册/提交/完成/释放 | ✅ | runtime E2E 137/0 + striped 46/0 |

**额外达成（超出目标架构）：** 跨设备 fused 单 kernel 提交（D1）、重启持久化契约（S6 test 89）、Runtime 合并提交（D6）。

**未达成：** 多厂商 GPU（D5）、FrameworkAdapter（D4）、host-executed 路径（D8）、旧 batch-open 与 L1/L2 等价语义（D3）。

---

## 5. 已知缺陷索引

当前 P0 级缺陷与完整待办见 [`../ROADMAP.md`](../ROADMAP.md)（由四份评审合成，doc/review/）。两项 P0：

1. `HandleWorkspaceCache` reopen→eviction 悬空 GPU handle（`metadata/handle_workspace_cache.h`）
2. `StripedDataPath` op 未记录 memory token，direct unregister 可提前解除 in-flight DMA 映射（`striped_data_path.cpp`）
