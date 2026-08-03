# TASK T-010

你是一名资深 C++ SPI 设计工程师。你只负责冻结 Tutti 的仓内 `StorageTargetResolver` source-level SPI 与带 owner 的 type-erased `ResolvedTarget`。你看不到任何其他上下文，本 prompt 已包含完整架构语义、边界和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 已完成基线

前一轮已经建立：

- `tutti/include/tutti/status.h`：`StatusCode`、`Status`、`Result<T>`
- `tutti/include/tutti/io_types.h`：`MemoryHandle`、`TargetHandle`、`IoHandle`、`IoRequest` 等
- `tutti_cuda_like` / `tutti_api`

目标架构没有要求逐字固定所有 C++ 签名，但已冻结 Resolver 的职责和 `ResolvedTarget` 的语义：

```text
resolve(uri, options) -> Result<ResolvedTarget>
```

# 架构契约

Resolver 只负责 namespace/name 到 target 资源的解析，不提交 IO、不理解 PRP/CQ/kernel。

`ResolvedTarget` 是仓内 type-erased、带 owner 的对象，公共壳至少表达：

```text
resolver_type_id
payload_type_id
source_api_version
logical_size
recommended DataPath key
shared owner of immutable payload + resource lease
```

关键不变量：

1. payload 不放入 common union；
2. Runtime 只比较 `{payload_type_id, source_api_version}` 和 compatibility，不读取 payload；
3. DataPath 通过 checked source-level type erasure 获取只读 view；类型或版本不匹配必须失败；
4. payload owner 由 `ResolvedTarget` 持有，而不是 resolver singleton 暗中保存；
5. resolver/DataPath 的成对 payload 类型以后放在独立 binding target/header，不从 public target 传播；
6. payload 在 target close 前 immutable；DataPath 不得保存超出 owner 生命周期的裸引用；
7. URI scheme 选择 resolver，不直接写死 DataPath；recommended key 只是组合提示。

本任务只定义 SPI 壳和 contract，不实现 `Ext4FiemapTargetResolver`、不解析 FIEMAP、不打开 fd、不持有文件锁，不实现 Binding payload。

# 任务目标

新增目录与头：

`/data/home/ryeqiu/Tutti/tutti/spi/storage_target_resolver.h`

在 `namespace tutti` 中提供最小 C++17 类型，至少包括：

```text
ResolveOptions
ResolvedTarget
StorageTargetResolver
```

允许必要的 detail 类型，但不要把 private payload 放入公共 variant/union。

# 最小 source-level 表达建议

你可以按以下语义设计；在测试证明不变量的前提下，字段名可微调：

```cpp
struct ResolveOptions {
    std::string scheme;
    // 本任务不增加额外 option 字段，避免过度设计。
};

class ResolvedTarget {
public:
    // move-only; no copy
    std::string_view resolver_type_id() const noexcept;
    std::string_view payload_type_id() const noexcept;
    std::uint32_t source_api_version() const noexcept;
    std::uint64_t logical_size() const noexcept;
    std::string_view recommended_data_path_key() const noexcept;
    bool valid() const noexcept;

    template <typename Payload>
    Result<const Payload*> view(
        std::string_view expected_payload_type_id,
        std::uint32_t supported_api_version) const;

    template <typename Payload, typename OwnerLease>
    static Result<ResolvedTarget> make(
        std::string resolver_type_id,
        std::string payload_type_id,
        std::uint32_t source_api_version,
        std::uint64_t logical_size,
        std::string recommended_data_path_key,
        std::shared_ptr<Payload> immutable_payload,
        std::shared_ptr<OwnerLease> owner_lease);
};

class StorageTargetResolver {
public:
    virtual ~StorageTargetResolver() = default;
    virtual Result<ResolvedTarget> resolve(
        std::string_view uri,
        const ResolveOptions& options) = 0;
};
```

要求：

- `ResolvedTarget` move-only，持有 payload 和 lease 的 shared owner；view 不延长 lease，只借出只读 payload 指针；
- `view<T>()` 必须同时检查 expected payload type 与 supported API version；不匹配返回 `UNSUPPORTED` 的 `Result`；
- 支持 payload API backward/forward compatibility 的语义不要在壳里猜；本轮要求 expected/supported 与 recorded version 匹配，或由 fake DataPath 显式声明支持集合前，只能按固定匹配测试；
- `valid()` 区分空壳；
- default/空 `ResolvedTarget` 不得可 view；
- 不引入 `std::any` 作为公共返回；内部可用 type-erased storage；
- 不要求 RTTI/dynamic_cast；type identity 用字符串和 API version；
- 不承诺 binary ABI。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/spi/storage_target_resolver.h`
- `/data/home/ryeqiu/Tutti/tests/storage_target_resolver_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/storage_target_resolver_contract/storage_target_resolver_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round3/result3.md`

其中 `chat/round3/result3.md` 保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round3-session3/`

该目录位于既有 ignored `build/` 下，不计入源码文件允许列表。

禁止修改或创建任何其他文件。尤其禁止修改：

- 根或 `tutti/` 的现有 `CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tutti/include/**`
- `/data/home/ryeqiu/Tutti/tutti/spi/data_path.h`
- 任意 Runtime、DataPath、accelerator、NVMe、libnvm、kernel 文件
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round3/result3.md` 外的任何文件

禁止提交 Git commit。

# 依赖限制

`storage_target_resolver.h` 只允许 include：

```cpp
#include <tutti/status.h>
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

不要把 fd、extent、LBA、namespace id 或 FIEMAP flags 放进公共壳。

# Contract test 要求

测试 target 与 CTest 名固定：

```text
tutti_storage_target_resolver_contract_test
```

测试为普通 C++17 可执行程序，不使用 GTest。

至少覆盖：

1. fake resolver 实现 `resolve(uri, options)` 并返回 `Result<ResolvedTarget>`；
2. fake payload 类型和 fake owner lease 类型均被 `ResolvedTarget` 持有；
3. 正确 `payload_type_id` + API version 时 `view<Payload>()` 返回只读 payload；
4. payload type 不匹配返回 `UNSUPPORTED`；
5. API version 不匹配返回 `UNSUPPORTED`；
6. 错误 resolver 返回非 OK `Status`，不生成 ResolvedTarget；
7. `ResolvedTarget` 不暴露 payload mutation；
8. `ResolvedTarget` move 后仍保持 owner lease；move-from 状态 valid 语义明确；
9. view 得到的裸指针不允许脱离 owner 后使用；测试至少验证多个 view 指向同一 immutable payload owner；
10. default/empty target 的 `valid()` 为 false，view 返回 `UNSUPPORTED`；
11. recommended DataPath key 可读，但不形成公共闭集 enum；
12. 不出现 common variant/union payload。

测试源码只 include：

```cpp
#include <tutti/spi/storage_target_resolver.h>
```

以及标准库头。

Standalone CMake 必须：

- `project(... LANGUAGES CXX)`；
- C++17；
- target-scoped include `/data/home/ryeqiu/Tutti`（使 `<tutti/spi/...>` 和 `<tutti/status.h>` 可解析）；
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

也禁止启动 daemon/client、访问 `/dev/nvme*`、执行 FIEMAP 或任何硬件 IO。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 清理专用 build 目录

只允许清理：

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round3-session3
```

## 2. Standalone configure

```bash
cmake -S /data/home/ryeqiu/Tutti/tests/storage_target_resolver_contract \
  -B /data/home/ryeqiu/Tutti/build/round3-session3 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

要求 configure 日志不出现 CUDA、gRPC、yaml-cpp、libnvm、FIEMAP 或 NVMe dependency discovery。

## 3. Build 和 CTest

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round3-session3 \
  --target tutti_storage_target_resolver_contract_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round3-session3 \
  --output-on-failure \
  -R '^tutti_storage_target_resolver_contract_test$'
```

要求 1/1 PASS。

## 4. Public-boundary guard

```bash
grep -nEi 'cuda|hip|maca|musa|libnvm|nvme|fiemap|grpc|yaml|backends/|io_engine/|device_manager/' \
  /data/home/ryeqiu/Tutti/tutti/spi/storage_target_resolver.h
```

必须无输出。

## 5. Hygiene

```bash
git diff --check -- tutti/spi/storage_target_resolver.h
```

对所有新增文件额外检查尾随空白和 EOF newline。确认本 session 只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. Resolver 接口只有 resolve，不包含 open/close/submit；
2. `ResolvedTarget` 持有 immutable payload 与 owner lease，而不是 singleton 状态；
3. checked view 同时验证 payload type 与 source API version；
4. 不匹配时返回结构化 `UNSUPPORTED`；
5. payload 未进入 common union；
6. SPI 不泄漏 filesystem/NVMe/transport/vendor 私有类型；
7. fake resolver 与 fake DataPath consumer 的 contract 测试通过；
8. HOST standalone configure/build/ctest 通过，不需要 CUDA SDK；
9. 未修改允许列表外文件；
10. 未执行任何模块、daemon、FIEMAP 或 IO 操作；
11. 空白检查通过。

如果 owner/view 生命周期难以在当前 source-level 表达中证明，记录限制，但不得为了“方便”引入全局 singleton 或 mutable payload。

# 结果落盘要求

完成任务和验收后，必须把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round3/result3.md`

至少包含：

1. 修改/新增文件列表
2. `ResolveOptions`/`ResolvedTarget`/Resolver 的实际 public surface
3. payload owner 与 lease 生命周期实现
4. type/version mismatch 行为
5. configure/build/ctest 结果
6. public-boundary guard 结果
7. 文件边界与空白检查结果
8. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 不得预留或编写“总指挥验收”内容；总指挥会在结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round3/result3.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
