# TASK — Round 19 Session 1：batch open（P2-1，legacy 搬家）

**日期：** 2026-08-03（预生成，启动前总指挥复核）
**前置依赖：** 无硬依赖（不碰 StripedDataPath 内部）；建议与 R19 S2 同轮顺序执行。
**参照：** legacy `Coordinator::open_gpu_files_batch`（`third_pkgs/Tutti/` 历史代码）——搬家语义，不复活 Coordinator 模式。

---

## 背景

KV cache 场景每层要 open 几百个文件。当前 `StorageRuntime::open()` 单文件串行（resolver FIEMAP + DataPath open + workspace 构建），N 文件 = N 倍延迟。legacy 有 `open_gpu_files_batch` 批量实现，按 maintainer 要求对着它搬家。

## 设计要点

1. **public API**：`StorageRuntime::open_batch(const std::vector<std::string>& uris, const OpenOptions&) → std::vector<Result<TargetHandle>>`——逐项结果，fail-closed（单项失败不影响其他项，每项独立状态），与现有 submit 的 per-request 语义风格一致。
2. **内部并行化**：resolver 解析（FIEMAP，host 侧 IO 密集）与 DataPath open（GPU workspace 构建）管线化/并行（线程池或至少 IO 交叠）；保持 resolver/binding/DataPath 三层职责，不在 Runtime 里长出新抽象。
3. striped URI 与普通 file URI 混合 batch 必须工作（路由按 URI scheme 正常走）。
4. 与 handle cache 的交互（cache ON 时 batch 内同 extent 签名文件去重）需走查。

## 验收

1. 契约：batch open 混合场景（含部分失败项——不存在文件/非法 URI/跨设备），逐项状态正确，全部 TargetHandle 可正常 submit IO 并字节校验。
2. 性能：500 文件 batch open 墙钟 vs 串行 open 的对比数字（`[perf]` 格式，展示即可）。
3. 回归一次：842/137/66 + 非硬件 15。
4. result：改动清单 + 契约证据 + perf 对比。

## 硬约束

- O_DIRECT 政策；partial-commit 语义风格一致；防缠结（改动清单）。
- public API 只增不改（`open()` 保留，行为不变）。
- 启动前总指挥复核：当时 S4 是否已改 simulator 的 open 路径（若 simulator 已用 batch 模式，对齐其需求）。
