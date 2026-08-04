# TASK T-087 — Round 16 Session 2：目标架构文档同步（消解 current-structure.md 的 D1-D8）

> 对照 `doc/architecture/current-structure.md` §3 的 8 项偏差，更新 `doc/TUTTI_TARGET_ARCHITECTURE.md` 使其与实现一致。纯文档工作，无代码改动。偏差内容已在 current-structure.md 中写明，直接采用，勿重复调查。

## REQUIRED

1. **D1 striping**：§2.2 "不主动实现 WAL、striping" 改为"WAL 不实现；striping 已实现（R15）"；§4.1 组件图补 `StripedDataPath`（位置：DataPath 层，与 LocalNvmeDataPath 平级）；补一段 striped 组件定义（striped:// URI、pair-private payload、单 kernel 融合提交、M×N device table 演进方向）。
2. **D2 control 层**：§4.2 移除 `control/`（DirectNvmeResourceProvider/NvmeServiceResourceProvider）改为：DataPath 直用 libnvm（`nvm_ctrl_attach_client`）+ nvmeservice daemon（gRPC 队列分配），无中间 driver 抽象。
3. **D3 metadata 组件名**：`TieredHandleCache` → `HandleWorkspaceCache`（单层 GPU LRU + open_refcount + pin）+ `PrpPageCache` + `MetadataArena`；注明与旧两层 L1/L2 语义不等价（重新设计，非迁移）。
4. **D4 FrameworkAdapter**：标注 future（maintainer 暂缓，prompts 存 doc/history/chat/round12/deferred-adapter/）。
5. **D5 GPU 可移植性**：§2.1/§4.1 的 MACA/MUSA 改为明确路线——Mooncake 模式 vendor shim 框架（R18 立项），当前 CUDA/HOST 两档实测；注明合作伙伴沐曦负责 MUSA 实现。
6. **D6 分组**：§8.3 Routing/grouping 更新为按 DataPath 分组（一次 submit 可跨 target、一次 kernel launch；R15 S3 起）。
7. **D7 interop 位置**：`interop/cuda_like` 标注实现于 `include/tutti/cuda_like.h` + `gpu_vendor/`（公共层）。
8. **D8 执行路径**：§2.1 双路径改为当前仅 device-executed（DEVICE_EXECUTION）一条生产路径，host-executed 列 future。
9. **§1 验收条件**：补充 R15 成果（跨设备 fused 单 kernel、重启持久化、按 DataPath 分组合并提交）。
10. 文末"变更记录"加一条 2026-08-03 条目，列出以上全部修订。

## 边界

- 只改 `doc/TUTTI_TARGET_ARCHITECTURE.md`；不动 `current-structure.md`（它是偏差点名册，D 项消解后由总指挥统一更新其状态列）。
- 保持文档原有结构与语言风格（中文、表格、条款化）。

## 结果文件

`chat/round16/result2.md`：每项偏差的修订位置与摘要、文档自检（无残留过时表述）、**改动文件清单**。
