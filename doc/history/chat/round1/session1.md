# TASK T-001

你是一名资深架构文档工程师，只负责同步 Tutti 当前活动 Roadmap。你看不到任何其他上下文，本 prompt 已包含完成任务所需的全部信息。

# 项目位置

`/data/home/ryeqiu/Tutti`

当前版本仍为 `v0.1`，禁止擅自升级版本号。

# 任务目标

重写活动 `Roadmap.md`，使其与已经确定的 Tutti 目标架构一致；同时更新 `README.md` 的文档入口。

旧 Roadmap 中的 L0-L6、全局 DeviceManager、IO Engine Layer、`IBackendProvider`、`CPU_SUBMIT/GPU_SUBMIT`、public raw block range 等内容已经过时，不能继续作为活动架构。

# 你只能修改这些文件

- `/data/home/ryeqiu/Tutti/Roadmap.md`
- `/data/home/ryeqiu/Tutti/README.md`

禁止修改任何其他文件，包括：

- `TUTTI_TARGET_ARCHITECTURE.md`
- `TUTTI_REFACTOR_TAKEOVER.md`
- `doc/history/README.md`
- 任意源码、CMake、测试和内核模块文件

禁止提交 Git commit。

# 已冻结的目标架构

## 项目定位

Tutti 是 CPU/GPU companion unified storage runtime，主要服务 file/KV-cache 场景。

规范性架构文档为：

- `TUTTI_TARGET_ARCHITECTURE.md`

当前实现与迁移风险分析为：

- `TUTTI_REFACTOR_TAKEOVER.md`

`Roadmap.md` 只负责版本快照、当前状态、实施阶段和里程碑，不再重复定义另一套接口。

## 核心组件

- `StorageRuntime`
  - host control plane
  - memory/target/IO handle lifecycle
  - validation、grouping、backpressure、status、query/wait
- NVIDIA-first CUDA-like API
  - 应用统一使用 CUDA 风格名称
  - 默认 `TUTTI_ACCELERATOR=CUDA`
  - NVIDIA 直接 include CUDA headers
  - 沐曦使用 `MACA` profile/shim
  - MUSA 等兼容厂商使用对应 profile/shim
- `DataPath`
  - 完整 data-plane SPI
  - open target
  - data-path memory registration
  - submit/progress/query
  - completion/error
- `StorageTargetResolver`
  - URI/name 到带 owner 的 `ResolvedTarget`
- Resolver/DataPath `Binding`
  - pair-private payload，不进入公共 union
- `FrameworkAdapter`
  - vLLM、LMCache 等框架生命周期和布局适配
- `LocalNvmeDataPath`
  - 第一个完整参考实现
  - control、metadata、IO、CUDA-like interop、libnvm、NVMeService、snvme

## IO 发起与执行

两个维度必须区分：

```text
                         ExecutionDomain
                     HOST              DEVICE
InitiationDomain
HOST_API             当前支持          当前主路径
DEVICE_API           不适用            未来按需求实现
```

本轮实现：

- host API + host execution
- host API + device execution

未来允许 caller device kernel 直接发起 IO，但本轮只保留 `DataPath/interop/cuda_like/device_api` sidecar 位置，不冻结通用 device ABI。

不支持 cooperative host/device submit。

## 当前明确非目标

- public `raw_device`
- 恢复已有两套 RawDevice
- WAL、striping、通用 create/remove/list
- cancel、priority、failover、notification
- 动态插件、`dlopen`、稳定第三方 C++ binary ABI
- 本轮实现 device-initiated IO
- 本轮主动实现 GDS、RDMA、Mooncake DataPath 或新 GPU 厂商
- 同一 build 同时启用多个 CUDA-like profile

固定 LBA 只能作为 `LocalNvmeDataPath` 私有测试 fixture。

# Roadmap.md 必须具有的结构

必须保留以下版本治理要求：

```text
# Tutti Unified Storage Runtime Roadmap

## Status
## v0.1 Positioning
## v0.1 Feature Snapshot
## v0.1 Known Bugs Snapshot
## Normative Architecture
## Current Implementation Status
## Active Roadmap
## Versioning Rules
## Out of Scope for v0.1
```

## `v0.1 Feature Snapshot` 至少包含

- `StorageRuntime` host control plane
- opaque `MemoryHandle`、`TargetHandle`、`IoHandle`
- `IoRequest`
- NVIDIA-first CUDA-like API
- compile profile：
  - `CUDA` 默认
  - `HOST` 测试
  - `MACA/MUSA` 社区兼容路径
- `DataPath` SPI
- `StorageTargetResolver` + Binding
- `LocalNvmeDataPath`
- file/KV-cache 主线
- host-initiated host/device execution
- async stream/event completion
- Framework Adapter 在 core 外
- snvme 属于 local-NVMe 部署基线

## `v0.1 Known Bugs Snapshot` 至少包含

只写摘要，不复制长篇审查：

- public registration 到真实 DMA mapping 尚未打通
- 当前 IO engine 仍理解 NVMe 私有 descriptor
- mixed target、memory offset/alignment 等数据正确性问题
- async 共享 scratch 生命周期错误
- PRP-list DMA address 问题
- SQE 初始化、CQ timeout、status 传播问题
- 新旧 libnvm/NVMeService/snvme 双事实源
- standalone 默认测试构建当前不可重现
- file target resolver 尚未接入
- GPU persistence 尚未形成稳定契约

## Active Roadmap 阶段必须是

```text
Phase 0: Architecture and Build Baseline
Phase 1: Stable Host Runtime Contracts
Phase 2: File-backed Local NVMe Vertical Slice
Phase 3: Local NVMe and Kernel Boundary Consolidation
Phase 4: Metadata Pools and True Async IO
Phase 5: First Real Framework Adapter
Phase 6: Community Extension Proof
Phase 7: Legacy Tree Retirement
```

Phase 0 必须明确：

- `TUTTI_TARGET_ARCHITECTURE.md` 是唯一规范架构
- 单一 build/source owner
- `TUTTI_ACCELERATOR=CUDA|HOST|MACA|MUSA|...`
- target-scoped `TUTTI_USE_<PROFILE>`
- 未选 SDK 零依赖
- clean hardware-free build

Phase 6 必须明确：

- feature ON/OFF CI
- CUDA-like profile contract
- MockDataPath
- Resolver/Binding sample
- 新 feature 主要新增 package/tests/docs
- 不修改 Runtime 公共 storage noun/算法

# README.md 要求

保持简洁，只更新文档入口：

- 项目一句话定位
- 明确 `TUTTI_TARGET_ARCHITECTURE.md` 是规范目标架构
- 明确 `Roadmap.md` 是活动版本路线
- 明确 `TUTTI_REFACTOR_TAKEOVER.md` 是当前实现/迁移分析
- 保留 build/test 文档链接

# 文档风格

- 使用英文，匹配当前 README/Roadmap 风格
- Markdown 标题清晰
- 不使用 emoji
- 不复制 2000 行目标架构，只写摘要并链接
- 不虚构已完成状态
- 必须区分 target、current implementation、future
- 不再使用 `IBackendProvider`、`BackendRegistry`、`DeviceManager Layer`、`IO Engine Layer` 作为目标架构
- `raw_device` 只能出现在明确的 out-of-scope/non-goal 语境

# 自检命令

执行：

```bash
cd /data/home/ryeqiu/Tutti
grep -nE 'IBackendProvider|CPU_SUBMIT|GPU_SUBMIT|BLOCK_RANGE|Device Manager Layer|IO Engine Layer' Roadmap.md
grep -nE 'TUTTI_TARGET_ARCHITECTURE|TUTTI_REFACTOR_TAKEOVER|Roadmap' README.md Roadmap.md
git diff --check -- Roadmap.md README.md
git diff -- Roadmap.md README.md
```

第一个 grep 除非用于明确说明“旧设计已废弃”，否则应无结果。

# 输出要求

只返回一个 Markdown 代码块，内容必须包含：

1. 修改文件列表
2. `Roadmap.md` 的完整最终内容
3. `README.md` 的完整最终内容
4. 自检命令及真实输出摘要
5. 如有阻塞，明确写 `BLOCKED` 和原因

不要解释、不要寒暄、不要修改或输出其他文件。
