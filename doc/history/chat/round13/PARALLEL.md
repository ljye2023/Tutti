# Round 13 并行执行协议（S2 ∥ S3 约束，总指挥定）

适用：`session2.md`（T-069 第一批退役）与 `session3.md`（T-070 第二批退役）并行执行时。S1 必须先完成；S4 必须串行最后。

## 1. 文件行级分治（同一 git working tree）

根 `CMakeLists.txt`：

| Session | 拥有区域 |
|---|---|
| S2 | `nvme_storage`(429)、`block_storage`(434)、`io_engine`(438)、`coordinator`(445)、`adapters/kv_cache`(452) 的 add_subdirectory 行及对应 include/link 引用 |
| S3 | `memory`(341)、`nvmeservice` lib(401)+examples(422)、`device_manager`(426)、`examples/`(457，含 tutti_daemon) 区域及 NVMeService 路径重定向 |

legacy `tests/CMakeLists.txt`：按树分行，同上规则。

**禁止**：重排、重格式化、修正对方区域的任何内容；不得"顺手"清理无关行。

## 2. 编辑期禁止各自构建验证

共享源码树在并行编辑期间逻辑不一致（删了源码 CMake 未同步/反向），任何一方构建都会失败且无法归因。规则：

1. 双方只做编辑，不做 configure/build/ctest；
2. 动手前各自 `git diff > /tmp/r13-s{N}-before.patch` 留底；完成后 `git diff > /tmp/r13-s{N}.patch`；
3. 双方都声明完成后，由总指挥发起**联合验证**（root build + standalone HOST/CUDA + ctest）；
4. 联合验证失败时：stash 其中一方 patch 二分定位归属，修复后再验。

## 3. standalone `tutti/` 一致性归 S3

S2 的树不在 standalone 构建图内（无需触碰）；S3 必须全程保持 tutti 侧自洽（NVMeService tutti 侧为唯一源，root 只做重定向）。

## 4. 硬件与 daemon 操作串行

性能基线（S1）、S3 的 daemon 重启、硬件契约复跑均由 operator/总指挥协调，不与编辑期重叠。硬件环境要求不变（module + daemon + /mnt/nvme1）。

## 5. 验收

S2/S3 的结果文件各自记录，但**共享同一份联合验证输出**；总指挥按各自决议表逐项验收后，S4 才可启动。
