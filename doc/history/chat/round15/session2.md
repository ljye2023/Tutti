# TASK T-077 — Round 15 Session 2：StripedResolver + Binding（逻辑单文件 → N 设备子目标）

## 前置条件

- Session 1 验收通过（多设备底座可信）；阅读 `chat/round15/session1.md` result、legacy `block_storage/include/gpu_file_resolve.h` 的 striping 语义（round-robin、granularity 概念）、`tutti/resolvers/local_file/resolver.h`、`tutti/bindings/ext4_local_nvme/binding.h`。
- 抽象决议（总指挥已定）：对外零新公共名词——striped 文件经普通 `rt.open(uri)` 得到一个普通 `TargetHandle`；striping 元数据全部进 pair-private payload。

## 目标

实现 `striped://` Resolver 与配套 Binding：一个逻辑 URI 解析为 N 个 per-device 子目标的 bundle，携带 stripe 元数据；lease 持有全部子目标资源。

## 允许修改/创建

- `tutti/resolvers/striped_file/`（新建 package）
- `tutti/bindings/striped_local_nvme/`（新建）
- `tests/`（resolver 契约测试；hardware-free 部分用临时文件即可，真实 backing 校验属硬件部分）
- `tutti/CMakeLists.txt`（一行接线，`include(CTest)` 之后的 BUILD_TESTING 块——吸取 Round 12 S3 教训）
- `chat/round15/result2.md`

## 禁止范围

- 零 core 改动（`tutti/include/tutti/**`、Runtime、既有 resolver/binding/DataPath 不动）。
- 不实现 StripedDataPath（Session 3）。
- 不执行模块/daemon/mount/mkfs 操作；不提交 Git。

## 必须实现的行为

1. **URI 格式**：`striped://<name>?devs=<mount1,mount2,...>&unit=<bytes>`（参数解析严格，非法即拒）；每 shard 的 backing file 路径规则明确（如 `<mount>/striped/<name>.shard<i>`），文档化。
2. **Resolver 结构**：构造注入 N 个 `LocalFileResolver` 实例（复用现有 fail-closed 语义：backing device 校验、FIEMAP unsafe flags 拒绝全部继承）；`resolve()` 依次解析 N 个 backing file → bundle。
3. **payload（pair-private）**：`{num_shards, stripe_unit, 每个 shard 的 ResolvedTarget（各自含 lease）}`；`logical_size = num_shards × min(shard_size)`；`recommended_data_path_key = "striped-local-nvme"`。
4. **stripe 语义**：unit 粒度 round-robin；`unit` 必须 block 对齐（4 KiB 倍数）且 ≥ block_size；offset 映射公式 `shard = (off/unit)%N, shard_off = (off/(unit*N))*unit + off%unit`（写入头注释与测试）。
5. **Binding**：type/version 常量 + payload 兼容性声明（仿 ext4_local_nvme）；sample 级文档注释。
6. **lease 语义**：bundle 的 release_lease 释放全部 N 个子 lease；部分失败时已建立的 lease 全部回滚（fail-closed）。

## 测试要求

- hardware-free：临时目录造文件即可测 URI 解析、payload 结构、映射公式边界（首/末 unit、跨 shard 边界、非对齐拒绝）、lease 回滚；
- 真实 backing 校验路径复用 LocalFileResolver（其 fail-closed 已由 Round 9 S3 覆盖，此处只需集成冒烟）；
- 纳入默认 BUILD_TESTING。

## 验收

- `chat/round15/result2.md`：URI 规范、payload 结构、映射公式证明、测试输出、HOST/CUDA ctest 全绿。
- 总指挥复核：零 core 改动；映射公式边界测试充分；lease 回滚无泄漏。

## 后续依赖

- S3（StripedDataPath）依赖本 session 的 payload 与映射语义。
