# TASK T-035 — Round 9 Session 5：正式 Public Runtime → File → Local-NVMe 验收门

## 前置条件

必须在 Session 1–4 全部 `PASS` 后串行执行。若 build/test target 未正式接线、Runtime 并发/失败语义未闭合、resolver 未 fail-closed 或 LocalNvmeDataPath 仍无有界错误传播，则报告 `BLOCKED`，不要部分开始。

当前 test 53 已证明一个 4 KiB happy path，但它仍附属在 LocalNvmeDataPath contract 二进制中。本 session 要建立独立、正式、只使用公开 Runtime API 的 hardware acceptance gate。

## 目标

建立一个 opt-in、可复现的 `StorageRuntime → LocalFileResolver → LocalNvmeDataPath` 端到端测试 target。应用侧数据面操作必须只调用：

```text
StorageRuntime::create
register_memory / open
submit / query / wait / release_io
close / unregister_memory / shutdown
```

测试 fixture 可以构造私有 resolver/DataPath 并注入 `RuntimeComponents`，但不得绕过 Runtime 直接调用 `LocalNvmeDataPath::open/register_memory/submit/progress/query/release`。

## 允许修改/创建

- `tests/storage_runtime_local_nvme_contract/**`（推荐新建独立 test package）
- `tutti/CMakeLists.txt`
- 必要的 local-NVMe/resolver private test fixture 文件
- 仅为公开 Runtime 测试暴露的问题做最小修复：`tutti/include/tutti/storage_runtime.h`、`tutti/data_paths/local_nvme/**`、`tutti/resolvers/local_file/**`
- `chat/round9/result5.md`

## 禁止范围

- 不修改 public nouns/SPI 以迎合测试；不引入 framework、Coordinator、old IO engine、raw API、cache/benchmark。
- 不调用 local DataPath 私有 submit 或手工 `nvm_dma_map_data_*()`。
- 不操作模块、daemon、mount、bind/unbind、raw block device；只对 resolver 得到的受控临时文件 IO。
- 不接触 `/mnt/nvme4`，不提交 Git。

## 必测场景

1. **Assembly/open**：注入 resolver/DataPath，`open(file://...)` 成功；未知 scheme、resolver 拒绝、DataPath key 不匹配都返回结构化错误且无 private target 泄漏。
2. **Memory**：注册 64 KiB 对齐 GPU buffer；首次 submit 才建立 data-path registration，后续同 domain submit 复用；unregister 后 DMA map 正确释放。
3. **真实数据**：经 public API 做 4 KiB SINGLE、8 KiB DUAL、1 MiB LIST 的 WRITE/READ 并逐字节验证；至少一例跨 extent file-offset IO。
4. **Batch**：mixed target/memory/direction 的 Runtime grouping；partial commit 保留 public IoHandle 与 per-request initial state，已接受 request 数据真实完成。
5. **顺序与并发**：同 stream producer→IO→consumer；两个 stream 同时 in-flight 并独立回读；至少两个 host thread 经 Runtime submit/query/release。
6. **失败与 timeout**：验证 resolver reject、DataPath error、CQ timeout/stream observation timeout、shutdown timeout→retry。所有已发出 IO 在 public IoHandle 下可观察，不得无主。
7. **teardown**：`release_io → close → unregister_memory → shutdown`；重复 lifecycle 无残留临时文件/handle/DMA mapping。

## CMake/执行规则

- 该 test 仅在 `TUTTI_BUILD_HARDWARE_TESTS=ON` 且 CUDA/local-NVMe private target 可用时注册；必须有 `LABELS hardware;local_nvme;runtime_e2e`。
- `TUTTI_BUILD_HARDWARE_TESTS=OFF` 时 HOST/default CTest 完全不配置该测试、不访问 CUDA/libnvm/mount。
- 运行前后只检查环境，不修改环境：设备、daemon、mount、module 状态均由操作者负责。

## 验收

1. Session 1 HOST CTest 全绿；
2. CUDA compile/link gate 通过；
3. 显式 hardware CTest 通过，输出每个场景的实际完成/错误结果；
4. 临时文件目录为空；daemon/mount/module/RAID 未变；
5. public consumer include 只需 `tutti_api` / public headers，不出现 libnvm、PRP、extent、LBA、fd；
6. `git diff --check`、linter 0 diagnostics。

## 结果落盘

写 `chat/round9/result5.md`：

- 注入装配和公共 API 调用链；
- SINGLE/DUAL/LIST/cross-extent 的真实回读；
- batch/partial/error/timeout/双 stream/host-thread 证据；
- lazy registration 与 teardown 证据；
- CMake label/default off 行为；
- 环境与清理；
- 最终 `PASS`/`BLOCKED`。

不要写“总指挥验收”，不要提交 Git。