# Round 16 Session 2 结果：目标架构文档同步（消解 current-structure.md 的 D1-D8）

状态：**完成**。纯文档工作，无代码改动。对照 `doc/architecture/current-structure.md` §3 的 8 项偏差，逐项修订 `doc/TUTTI_TARGET_ARCHITECTURE.md` 使其与实现一致。

---

## 每项偏差的修订位置与摘要

| 偏差 | 修订位置 | 摘要 |
|------|---------|------|
| **D1 striping** | §2.2（line ~117）、§4.1 组件图（line ~219-237）、§13.6（新增，line ~1427-1450）、§17 目标目录（line ~1691-1696） | "不主动实现 striping" → "striping 已实现（R15）"；组件图补 `StripedDataPath`（与 `LocalNvmeDataPath` 平级）；新增 §13.6 striped 组件定义（`striped://` URI、pair-private payload、单 kernel 融合提交、M×N device table 演进方向、零核心改动）；目标目录补 `data_paths/striped_local_nvme/`、`resolvers/striped_file/`、`bindings/striped_local_nvme/` |
| **D2 control 层** | §4.2（line ~275-292）、§13.2（line ~1331-1346）、§17 目标目录（line ~1683）、§24.6（line ~2259-2261） | 移除 `control/`（`DirectNvmeResourceProvider`/`NvmeServiceResourceProvider`）→ DataPath 直用 libnvm（`nvm_ctrl_attach_client`）+ nvmeservice daemon（gRPC 队列分配），无中间 driver 抽象；§13.2 重写为直链装配说明 |
| **D3 metadata 组件名** | §4.2（line ~269-273）、§9.5（line ~899）、§9.7（line ~934）、§24.4（line ~2162, 2184, 2196）、§24.6（line ~2255） | `TieredHandleCache` → `HandleWorkspaceCache`（单层 GPU LRU + `open_refcount` + `pin`）+ `PrpPageCache` + `MetadataArena`；§9.7 加注"当前实现为单层，非旧 L1/L2 迁移"；§9.5 迁移表更新 |
| **D4 FrameworkAdapter** | §4.1 组件图（line ~219）、§15.1（line ~1512） | 组件图标注"future，maintainer 暂缓，prompts 存 doc/history/chat/round12/deferred-adapter/"；§15.1 加偏差消解说明 |
| **D5 GPU 可移植性** | §2.1（line ~79）、§4.1 组件图（line ~225-226） | MACA/MUSA → 明确路线：Mooncake 模式 vendor shim 框架（R18 立项），当前 CUDA/HOST 两档实测；合作伙伴沐曦负责 MUSA 实现；组件图改为"CUDA \| HOST 实测 / MACA/MUSA #error 占位" |
| **D6 分组** | §8.3（line ~777-789） | Routing/grouping 更新为按 DataPath 分组（一次 submit 可跨 target、一次 kernel launch；R15 S3 起）；加偏差消解说明 |
| **D7 interop 位置** | §4.2（line ~274）、§17 目标目录（line ~1684）、§24.6（line ~2262） | `interop/cuda_like` 标注实现于 `include/tutti/cuda_like.h` + `gpu_vendor/`（公共层，非 local_nvme 私有） |
| **D8 执行路径** | §2.1（line ~77-78） | 双路径 → 当前仅 device-executed（`DEVICE_EXECUTION`）一条生产路径，`supports_host_execution=false`；host-executed 列 future |
| **§1 验收条件** | §1（line ~67） | 补充第 6 条：R15 成果（跨设备 fused 单 kernel、重启持久化、按 DataPath 分组合并提交） |
| **变更记录** | 文末 §25（新增） | 2026-08-03 条目，列出以上全部修订 |

---

## 文档自检（无残留过时表述）

```
$ grep -n "TieredHandleCache" doc/TUTTI_TARGET_ARCHITECTURE.md
284:> **偏差消解 D3**：`TieredHandleCache` 已替换为 ...（偏差消解说明，正确）
2524:| **D3 metadata 组件名** | ...（变更记录条目，正确）
（无其他残留）

$ grep -n "DirectNvmeResourceProvider\|NvmeServiceResourceProvider" doc/TUTTI_TARGET_ARCHITECTURE.md
282, 1331, 2523（均为偏差消解说明或变更记录，正确）
（无其他残留）

$ grep -n "control/" doc/TUTTI_TARGET_ARCHITECTURE.md
282, 1331, 2290, 2523（均为偏差消解说明或变更记录；2144/2456 为 §24.2 旧迁移总图中的"control/resource grant"描述性文字，非目录路径）
（无残留目录路径）
```

---

## 改动文件清单

| 文件 | 改动类型 |
|------|---------|
| `doc/TUTTI_TARGET_ARCHITECTURE.md` | 修改（D1-D8 偏差消解 + §1 验收条件补充 + §25 变更记录新增） |
| `doc/history/chat/round16/result2.md` | 新增（本文件） |

未改动：`doc/architecture/current-structure.md`（偏差点名册，D 项消解后由总指挥统一更新其状态列）、任何代码文件。

## 总指挥验收（2026-08-03）

**PASS。** 独立核验：D1-D8 全部落档——组件图补 StripedDataPath 平级位（:238）、§13.6 striped 组件定义（:1425）、§8.3 按 DataPath 分组、§13.2 control 层移除改直链、D3 组件名全量替换（残留 grep 仅余偏差消解说明与变更记录两处正确引用）、§1 验收条件补第 6 条 R15 成果、§25 变更记录新增。current-structure.md 的状态列由总指挥在下一次结构文档刷新时统一更新（D1-D8 标记为已消解）。
