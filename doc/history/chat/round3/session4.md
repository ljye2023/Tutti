# TASK T-011

你是一名资深 C++ data-plane SPI 设计工程师。你只负责冻结 Tutti 的仓内 `DataPath` source-level SPI、`DataPathCapabilities`、submit/progress/op 生命周期值类型。你看不到任何其他上下文，本 prompt 已包含完整架构语义、边界和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 已完成基线

前一轮已经建立：

- `tutti/include/tutti/status.h`：`StatusCode`、`Status`、`Result<T>`
- `tutti/include/tutti/io_types.h`：`IoRequest`、`HostSubmitContext`、`ExecutionDomain`
- `tutti/spi/storage_target_resolver.h`：预期由另一个 Round 3 worker 建立

本任务假设 Resolver 壳存在，但如果你执行时它尚未存在，也必须只写自己允许的文件；不得提前实现或改写另一个 worker 的 `storage_target_resolver.h`。

目标架构没有要求逐字固定所有 C++ 签名，但已冻结 `DataPath` 的职责、capabilities 字段和 `SubmitOutcome`/`DataPathOp` 生命周期不变量。

# 架构契约

DataPath 负责：

- 打开 resolver 产生的 target；
- data-path private memory registration；
- logical request lowering；
- submit/progress/query/release；
- completion/error；
- 私有 descriptor、queue、kernel、workspace。

Runtime 不理解 PRP、descriptor、queue、kernel、FIEMAP 或 transport completion 类型。

# 必须覆盖的概念接口

至少表达以下 host-side SPI：

```text
identity / capabilities
initialize(config, resource provider) -> Status
shutdown(timeout) -> Status

open(ResolvedTarget) -> Result<DataPathTarget>
close(DataPathTarget) -> Status
registration_domain(DataPathTarget) -> Result<RegistrationDomainKey>

register_memory(MemoryView, RegistrationDomainKey)
  -> Result<DataPathMemory>
unregister_memory(DataPathMemory) -> Status

submit(DataPathRequest[], HostSubmitContext)
  -> SubmitOutcome {Status, optional DataPathOp, per-request initial state}
progress(ProgressBudget) -> Result<ProgressResult>
query(DataPathOp) -> Result<DataPathSnapshot / terminal result>
release(DataPathOp) -> Status
```

注意：本任务的 `MemoryView` 与 `ResolvedTarget` 是 Round 3 其他 worker 的公共/Resolver 头。为了不修改对方文件，采用以下**不依赖对方头文件存在**的 SPI 表达：

- `register_memory` 使用本任务定义的 `DataPathMemoryView`；
- `open` 使用 `ResolvedTarget` forward declaration + pointer/reference，SPI 文件不 include Resolver 头；
- contract test 使用 fake 类型，不实例化另一个 worker 的 `ResolvedTarget`。

# 提交与 op 生命周期不变量

`SubmitOutcome`：

- `op == null` 表示没有任何 transport request 被不可撤销地发出；
- `op != null` 表示至少一个 request 已发出或仍需观察，即使 overall `Status` 表示部分提交失败；
- per-request initial state 与输入 request 顺序一一对应；
- DataPath 必须在首次不可撤销提交前完成可预见 validation、容量预留和必要 lease；
- contract test 必须覆盖“第 K 个 request 发出后提交失败”，并证明前 K 个仍有 owner/query 路径。

`DataPathOp` 到 terminal 前持有所有私有 lease；`query()` 不销毁 operation；`release()` 只能用于 terminal operation。operation 到达 terminal 后不得再访问 caller memory。

# 能力与 progress 契约

`DataPathCapabilities` 至少覆盖以下最小字段语义：

- stable name 与 source API version；
- profile/memory kind 支持；
- `HOST_EXECUTION` / `DEVICE_EXECUTION`；
- direct/staged data movement；
- READ/WRITE；
- target、memory、length alignment；
- max single IO、max batch requests/bytes、max in-flight；
- scatter-gather；
- registration scope；
- progress model；
- `DEVICE_EXECUTION` 是否能在 caller stream 上建立真实 IO completion fence；
- 多 stream 并发与最大 concurrent streams/operations；
- device execution 是否不依赖 host query/wait；
- 多 GPU/cross-device；
- optional target features。

Capabilities 是硬约束，不是提示。

`ProgressBudget` 至少限制 wall-clock 时间和 max work units。`ProgressResult` 至少表达：

```text
work_units_consumed
operations_advanced
operations_terminal
more_work_likely
optional next_poll_deadline
```

progress 必须有界；禁止把无限 CQ busy-poll 伪装成 progress。

# 任务目标

新增公共仓内 SPI 头：

`/data/home/ryeqiu/Tutti/tutti/spi/data_path.h`

在 `namespace tutti` 中提供最小 C++17 类型，至少包括：

```text
DataPathCapabilities
DataPathMemoryView
DataPathMemory
DataPathTarget
RegistrationDomainKey
DataPathRequest
RequestInitialState
SubmitOutcome
DataPathOp
ProgressBudget
ProgressResult
DataPathSnapshot
DataPath
```

允许使用 opaque host-side handle/value wrapper，但不得把内部对象地址暴露到公共 public API；这些是仓内 SPI，不是应用 public noun。

# 最小设计约束

1. 所有 noun 是 value/RAII 类型，不使用裸 `void*` 表示 target/memory/op；
2. 不将 `DataPathTarget`、`DataPathMemory`、`DataPathOp` 放入应用 public `include/tutti/`；它们只存在于 `tutti/spi/`；
3. `DataPath` 是 virtual host-side SPI；未来 device API 不在这个头里；
4. `DataPathRequest` 应包含 public `IoRequest` 或等价最小字段，且包含 data-path memory/target identity；不得含 PRP、LBA、descriptor；
5. `RegistrationDomainKey` 是 opaque string/key，不是 controller pointer；
6. capabilities 的 optional target features 可用 `std::vector<std::string>`；不要闭集 enum；
7. source API version 用 `std::uint32_t`；
8. 不加入 cancel、priority、notification fd、failover 或 retry policy；
9. 不要求 source-level package ABI。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/spi/data_path.h`
- `/data/home/ryeqiu/Tutti/tests/data_path_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/data_path_contract/data_path_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round3/result4.md`

其中 `chat/round3/result4.md` 保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round3-session4/`

该目录位于既有 ignored `build/` 下，不计入源码文件允许列表。

禁止修改或创建任何其他文件。尤其禁止修改：

- 根或 `tutti/` 的现有 `CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tutti/include/**`
- `/data/home/ryeqiu/Tutti/tutti/spi/storage_target_resolver.h`
- 任意 Runtime、Resolver、accelerator、NVMe、libnvm、kernel 文件
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round3/result4.md` 外的任何文件

禁止提交 Git commit。

# 依赖限制

`data_path.h` 只允许 include：

```cpp
#include <tutti/status.h>
#include <tutti/io_types.h>
```

以及完成该 SPI 所需的 C++17 标准库头。

明确禁止 include 或提及：

```text
cuda
hip
maca
musa
libnvm
nvme
fiemap
grpc
yaml
backends/
io_engine/
device_manager/
```

不要在 SPI 中出现 PRP、SGL、LBA、extent、CID、doorbell、WR、fd、CUDA kernel 或 transport completion 类型。

# Contract test 要求

测试 target 与 CTest 名固定：

```text
tutti_data_path_contract_test
```

测试为普通 C++17 可执行程序，不使用 GTest，不使用 CUDA SDK。

至少覆盖：

1. fake DataPath 能实现全部 SPI 方法；
2. capabilities 可表达并读取全部最低字段语义；
3. open/register/unregister/close 使用不同 opaque identity；
4. registration domain key 不泄漏 controller pointer；
5. submit 输入 4 个 request，在第 3 个不可撤销发出后失败：
   - `SubmitOutcome.status` 非 OK；
   - `op` 仍非空；
   - 4 个 per-request initial states 顺序对应；
   - 前 3 个保持可 query；
6. `op == null` 时明确表示零 transport request；
7. `query()` 不销毁 op；
8. `release()` 只接受 terminal op；非 terminal release 返回 `BUSY` 或 `INVALID_ARGUMENT`；
9. progress budget 限制 max work units 与 timeout；fake 实现不得超过 max work units；
10. progress result 的计数、退避和 optional deadline 语义正确；
11. `DEVICE_EXECUTION` 能力字段能区分真实 completion fence 与 device-autonomous progress；
12. 不出现无限 busy-poll 或 transport completion 私有类型；
13. fake DataPath 不把 shared scratch 复用到两个 in-flight op。

测试源码只 include：

```cpp
#include <tutti/spi/data_path.h>
```

以及标准库头。

Standalone CMake 必须：

- `project(... LANGUAGES CXX)`；
- C++17；
- target-scoped include `/data/home/ryeqiu/Tutti`（使 `<tutti/spi/...>`、`<tutti/status.h>` 可解析）与 `/data/home/ryeqiu/Tutti/tutti/include`（使 `<tutti/io_types.h>` 可解析）；
- target-scoped 定义 `TUTTI_USE_HOST=1`，以满足 `io_types.h` 经由 `cuda_like.h` 的 profile 检查；
- 不查找 CUDA 或任何第三方 SDK；
- `enable_testing()` 并注册 CTest。

# 安全限制

绝对禁止：

```text
sudo
insmod
rmmod
modprobe
```

也禁止启动 daemon/client、访问 `/dev/nvme*`、执行任何真实 NVMe/CUDA/硬件 IO。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 清理专用 build 目录

只允许清理：

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round3-session4
```

## 2. Standalone configure

```bash
cmake -S /data/home/ryeqiu/Tutti/tests/data_path_contract \
  -B /data/home/ryeqiu/Tutti/build/round3-session4 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

要求 configure 日志不出现 CUDA、gRPC、yaml-cpp、libnvm、FIEMAP 或 NVMe dependency discovery。

## 3. Build 和 CTest

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round3-session4 \
  --target tutti_data_path_contract_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round3-session4 \
  --output-on-failure \
  -R '^tutti_data_path_contract_test$'
```

要求 1/1 PASS。

## 4. Public-boundary guard

```bash
grep -nEi 'cuda|hip|maca|musa|libnvm|nvme|fiemap|grpc|yaml|backends/|io_engine/|device_manager/|PRP|SGL|LBA|doorbell|descriptor|CID' \
  /data/home/ryeqiu/Tutti/tutti/spi/data_path.h
```

必须无输出；如果你的 capabilities 中确实需要以普通字符串表达 optional feature，不要新增被 guard 命中的私有实现名词。

## 5. Hygiene

```bash
git diff --check -- tutti/spi/data_path.h
```

对所有新增文件额外检查尾随空白和 EOF newline。确认本 session 只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. DataPath SPI 覆盖 open/registration/submit/progress/query/release；
2. Capabilities 覆盖架构要求的最低语义，并作为硬约束表达；
3. `SubmitOutcome` 精确表达零发出与部分发出的区别；
4. 第 K 个 request 发出后失败的 contract 测试通过；
5. `DataPathOp` query/release 生命周期明确；
6. progress 有预算、结果和退避语义；
7. SPI 不泄漏任何 NVMe/CUDA/transport 私有类型；
8. fake DataPath 可插入，不需要修改 Runtime 公共 noun；
9. HOST standalone configure/build/ctest 通过，不需要 CUDA SDK；
10. 未修改允许列表外文件；
11. 未执行任何模块、daemon 或 IO 操作；
12. 空白检查通过。

如果 `ResolvedTarget` 由另一 worker 尚未落地导致 include 不可用，不得等待或复制其文件；使用本 prompt 指定的 forward declaration + fake contract 方案。

# 结果落盘要求

完成任务和验收后，必须把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round3/result4.md`

至少包含：

1. 修改/新增文件列表
2. SPI 类型与方法的实际 public surface
3. capabilities 最小字段清单
4. submit/partial-commit/op 生命周期实现与测试结果
5. progress 预算与结果语义
6. configure/build/ctest 结果
7. public-boundary guard 结果
8. 文件边界与空白检查结果
9. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 不得预留或编写“总指挥验收”内容；总指挥会在结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round3/result4.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
