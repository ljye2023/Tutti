# TASK T-041 — Round 10 Session 2：LocalNvmeDataPath control plane 归位

## 前置条件

- Session 1 完成：libnvm/snvme 唯一事实源已确立。
- 阅读 `Roadmap.md` Phase 3 deliverable「Device Manager resource grant/accounting moved into `LocalNvmeDataPath/control/`」、`TUTTI_TARGET_ARCHITECTURE.md` 对 L2 device_manager 的处置结论（「NVMe resource/control 全部收进 `LocalNvmeDataPath` package」）。
- 现状：`tutti/device_manager/nvme/` 内含 libnvm、nvmeservice、kernel_modules 以及 `src/`+`include/` 的 device manager 驱动代码（`IDeviceDriver`/queue grant/daemon client 等）；`LocalNvmeDataPath`（`tutti/data_paths/local_nvme/`）目前直接消费其中的 ctrl/queue/DMA 资源。

## 目标

把 LocalNvmeDataPath 实际使用的控制面（controller 所有权、queue group grant/accounting、DMA allocator、可选 daemon client）收进 `tutti/data_paths/local_nvme/control/`，使 LocalNvmeDataPath 成为自包含 package；`tutti/device_manager/` 中不再保留只被 local-NVMe 使用的通用层残骸。

## 允许修改/创建

- `tutti/data_paths/local_nvme/**`（新增 `control/` 子目录）
- `tutti/device_manager/**`（移动/删除/重定向；保留 libnvm 与 kernel_modules 原位置，除非 Session 1 另有决议）
- `tutti/CMakeLists.txt`、`tests/**/CMakeLists.txt`（include/link 接线）
- `chat/round10/result2.md`

## 禁止范围

- 不改变 control plane 的任何行为：queue 数量、DMA 分配策略、daemon 协议、错误路径一律保持原语义；本 session 是结构移动+命名，不是重写。
- 不把 libnvm/nvmeservice/CUDA kernel 头文件暴露到 `tutti/include/tutti/**` 或任何 public target 的 INTERFACE。
- 不删除 nvmeservice：gRPC daemon 路径保留为可选 feature（gRPC found 才编译），但其 consumer 归属要在 result 中写清（LocalNvmeDataPath control 还是独立 daemon target）。
- 不执行模块/daemon/mount/bind/unbind/format/raw LBA IO；不 insmod/rmmod。
- 不提交 Git。

## 必须实现的行为

1. `LocalNvmeDataPath` 源码只 include 本 package（`data_paths/local_nvme/**`）+ 公共 SPI/值类型头；不再直接 include `tutti/device_manager/nvme/src|include` 中属于控制面的头。
2. controller/queue/DMA 的创建、授权计数与销毁归 `control/` 内类型所有；DataPath 生命周期（initialize/shutdown）与其一一对应，无裸外部全局。
3. `tutti/device_manager/CMakeLists.txt` 收缩：只构建仍被消费的产物；若收缩后为空壳，删除该目录接线并在 root/standalone CMake 同步移除。
4. 移动后不存在重复 symbol/ODR 风险：同一编译单元不被两个 target 以不同宏各编一遍（给出 target→source 清单证据）。

## 测试要求

- HOST profile clean configure+build+ctest 全绿（不得因移动减少现有测试）。
- CUDA profile build；显式运行一次 `tutti_local_nvme_datapath_contract_test` 与 `tutti_storage_runtime_local_nvme_contract_test`，断言数不得少于 Round 9 收口时的基线（550/115），全部通过。
- 运行后确认 `/mnt/nvme1/GPU0/resolver_test/` 无残留。

## 验收

- `chat/round10/result2.md` 含：移动前后文件/符号归属表、`git grep` 证明 DataPath 不再引用旧控制面路径、两 profile 构建与硬件回归输出。
- 总指挥复核：抽查 include 图与 target 接线，复跑硬件两契约。

## 后续依赖

- 与 Session 3（UAPI）可并行；Session 5 门禁依赖本 session 完成。
