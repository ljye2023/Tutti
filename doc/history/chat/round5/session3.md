# TASK T-018

你是一名资深 C++ 边界设计工程师。你只负责建立 Tutti **第一个 binding**：resolver 与 DataPath 之间成对的私有 payload 类型，以及证明其配对语义的 contract test。你看不到任何其他上下文，本 prompt 已包含完整架构语义、已有基线、边界和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 已完成基线（**只读，禁止修改**）

## `tutti/spi/storage_target_resolver.h`

```cpp
struct ResolveOptions { std::string scheme; };

class ResolvedTarget {
public:
    ResolvedTarget() = default;                              // 空壳，valid()==false
    ResolvedTarget(ResolvedTarget&&) noexcept = default;     // move-only
    ResolvedTarget(const ResolvedTarget&) = delete;

    std::string_view resolver_type_id() const noexcept;
    std::string_view payload_type_id() const noexcept;
    std::uint32_t    source_api_version() const noexcept;
    std::uint64_t    logical_size() const noexcept;
    std::string_view recommended_data_path_key() const noexcept;
    bool             valid() const noexcept;

    // 受检查的只读 view：payload_type_id 与 source_api_version 必须同时匹配，
    // 否则返回 UNSUPPORTED。返回的指针借用自本对象的 payload owner，不延长 lease。
    template <typename Payload>
    Result<const Payload*> view(std::string_view expected_payload_type_id,
                                std::uint32_t supported_api_version) const;

    // 工厂：payload 与 owner lease 均以 shared_ptr 传入，二者不得为空。
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
    virtual Result<ResolvedTarget> resolve(std::string_view uri,
                                           const ResolveOptions& options) = 0;
};
```

内部用 `shared_ptr<void>` 做 type erasure，不使用 RTTI；类型身份 = 字符串 ID + `uint32_t` 版本。

**已知固有约束（重要）：** `view<Payload>()` 只比对字符串 ID 与版本，随后无条件 `static_cast`。若字符串 ID 匹配但 C++ 模板参数写错，会**静默返回 UB 数据**。这正是 binding 存在的理由 —— 见下文任务目标。

## `tutti/spi/data_path.h`

已冻结，含 `DataPath` 抽象类、`DataPathCapabilities`、`DataPathMemoryView`、opaque identities（`DataPathTarget`/`DataPathMemory`/`DataPathOp`）、`SubmitOutcome`、`ProgressBudget`/`ProgressResult` 等。其中：

```cpp
virtual Result<DataPathTarget> open(const ResolvedTarget& target) = 0;
```

## `tutti/include/tutti/status.h`

`StatusCode`（12 值，含 `UNSUPPORTED`、`INVALID_ARGUMENT`、`NOT_FOUND`、`DATA_LOSS`、`OUT_OF_RANGE`）、`Status`、`Result<T>`。

# 架构契约（目标架构第 12 章）

关于 binding，架构已冻结以下要求：

- resolver 与 DataPath 的**成对 payload 类型**放在独立 binding target/header，命名示例 `tutti_binding_ext4_local_nvme`，**只被这两个实现链接**；
- binding header **可以**定义 extent 与 namespace identity，但**不得**从 `tutti_api` / `tutti_runtime` PUBLIC 传播；
- binding header 只向 resolver 与 DataPath implementation target 提供 **PRIVATE** headers，不进入 public Runtime；
- Runtime 只比较 `{payload_type_id, source_api_version}` 和 compatibility，**不读取 payload**；
- payload 在 target 关闭前 immutable；DataPath 不得保存超出 owner 生命周期的裸引用；
- **每个实际组合必须有 binding contract test，覆盖 type/version mismatch 和 payload owner 生命周期。**

## 与公共头的关键区别（务必理解）

公共头（`tutti/include/tutti/**`）与 SPI 头（`tutti/spi/**`）**禁止**出现 extent、LBA、namespace id、fd、PRP 等私有名词。

**binding header 恰恰相反 —— 它就是这些私有类型的合法归宿。** 它是私有边界的内侧，不是外侧。所以本任务的 guard 与前几轮不同：extent / namespace / LBA 在 binding header 中是**允许且预期**的；真正要守的是「binding header 不被任何公共头 include」。

# 任务目标

## 1. 新增 binding header

`/data/home/ryeqiu/Tutti/tutti/bindings/ext4_local_nvme/binding.h`

在 `namespace tutti::binding::ext4_local_nvme` 中定义 resolver 与 DataPath 之间的成对私有契约，至少包括：

```text
kPayloadTypeId          // 字符串常量，payload 类型身份
kPayloadApiVersion      // std::uint32_t，payload API 版本
kRecommendedDataPathKey // 字符串常量，推荐的 DataPath key

Extent                  // 一段物理映射：logical_offset / device_offset / length
NamespaceIdentity       // NVMe namespace 身份：controller/namespace 标识 + block size
Ext4LocalNvmePayload    // 上述二者组合成的 immutable payload
```

### `Ext4LocalNvmePayload` 的语义要求

- **immutable**：只提供只读访问；不提供任何 setter 或可变引用；
- 至少携带：`NamespaceIdentity`、有序 `std::vector<Extent>`、`file_size`；
- 提供只读查询方法，至少一个：把文件内 logical byte offset 映射到 device byte offset。该方法必须能表达「offset 落在空洞或超出范围」的失败，用 `Result<std::uint64_t>` 返回（`OUT_OF_RANGE`）；
- 提供一个 `Status validate() const`（或等价的自校验入口），检查 extent 集合是否满足：按 `logical_offset` 升序、无重叠、无空洞、完整覆盖 `[0, file_size)`。不满足返回 `DATA_LOSS` 或 `INVALID_ARGUMENT`（自行选定并在注释中固定语义）。

`validate()` 是本 binding 的核心价值：架构要求 ext4 resolver「拒绝 hole 以及不能安全直写的 FIEMAP 状态」「extent 必须按 `fe_logical` 完整、无重叠覆盖 `[0, file_size)`」。本轮不解析真实 FIEMAP，但**这条不变量必须在 binding 层可被校验和测试**。

### 2. 提供收敛后的配对入口（本任务最重要的设计点）

为了消除前文所述「字符串 ID 匹配但 C++ 类型写错 → 静默 UB」的风险，binding header 必须成为 `{type_id, Payload 类型}` 配对的**唯一声明点**。

提供两个 helper，让 resolver 与 DataPath **都不需要手写字符串字面量或模板参数**：

```cpp
// resolver 侧：把 payload + lease 打包为 ResolvedTarget，
// 内部固定使用本 binding 的 kPayloadTypeId / kPayloadApiVersion。
template <typename OwnerLease>
Result<ResolvedTarget> make_resolved_target(
    std::string resolver_type_id,
    std::uint64_t logical_size,
    std::shared_ptr<const Ext4LocalNvmePayload> payload,
    std::shared_ptr<OwnerLease> owner_lease);

// DataPath 侧：受检查地取出只读 payload view，
// 内部固定使用本 binding 的 kPayloadTypeId / kPayloadApiVersion。
Result<const Ext4LocalNvmePayload*> view_payload(const ResolvedTarget& target);
```

要求：

- 调用方**不得**、也**不需要**再传字符串 ID 或版本号；
- `view_payload()` 内部调用 `target.view<Ext4LocalNvmePayload>(kPayloadTypeId, kPayloadApiVersion)`，把误配面收敛到本文件这一行；
- 两个 helper 必须在同一文件，使「谁产生」与「谁消费」的类型约定物理上无法分叉。

注意 `ResolvedTarget::make` 的 payload 模板参数与 `view` 的必须一致。如果你在 `make_resolved_target` 里用 `shared_ptr<const Ext4LocalNvmePayload>`，请确认 `view_payload` 的 `static_cast` 目标类型与之匹配（const 限定要一致，否则就是你自己引入了新的误配）。这一点请在结果中明确说明你如何保证。

## 3. 不要实现的东西

- **不**解析真实 FIEMAP，**不**打开任何 fd，**不**读 `/sys`，**不**碰任何真实文件系统；
- **不**实现 `Ext4FiemapTargetResolver` 真身；
- **不**实现 `LocalNvmeDataPath` 真身；
- **不**实现 owner lease 的真实资源持有（测试里用 fake lease）；
- **不**建 CMake library target（本轮 header-only；建 target 会碰 `tutti/CMakeLists.txt`，那是另一 worker 的文件）。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/bindings/ext4_local_nvme/binding.h`
- `/data/home/ryeqiu/Tutti/tests/binding_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/binding_contract/binding_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round5/result3.md`

其中 `chat/round5/result3.md` 保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round5-session3/`

禁止修改或创建任何其他文件。尤其禁止修改：

- `/data/home/ryeqiu/Tutti/tutti/spi/**`（**另有 worker 正在移动这些文件，绝对不要碰**）
- `/data/home/ryeqiu/Tutti/tutti/include/**`
- 根或 `tutti/` 的任何 `CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/tests/` 下除 `tests/binding_contract/` 外的任何目录
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round5/result3.md` 外的任何文件
- 任意 Runtime、accelerator、NVMe、libnvm、kernel 文件

禁止提交 Git commit。

# 并发 worker 注意事项（重要）

**另有 worker 正在把 SPI 头从 `tutti/spi/` 物理移动到 `tutti/include/tutti/spi/`。** include 路径 `<tutti/spi/...>` 在移动前后**保持不变**，但**物理位置会变**。

因此你的 standalone CMake 必须同时提供两个 include root，使其对布局变化不敏感：

```cmake
target_include_directories(<test> PRIVATE
    /data/home/ryeqiu/Tutti                 # 当前布局：tutti/spi/*.h
    /data/home/ryeqiu/Tutti/tutti/include   # 迁移后布局 + 公共头
)
```

两个都加上，无论对方是否已完成迁移，你都能编译。请在结果中说明你这样做的原因。

同理，`binding.h` 中 include SPI 头时使用 `<tutti/spi/storage_target_resolver.h>`（尖括号 + 该逻辑路径），**不要**用相对路径 `"../../spi/..."`，那样迁移后必然断裂。

# 依赖限制

`binding.h` 只允许 include：

```cpp
#include <tutti/status.h>
#include <tutti/spi/storage_target_resolver.h>
```

以及所需的 C++17 标准库头。

**禁止** include：

- `tutti/spi/data_path.h`（binding 不应依赖 DataPath SPI；它只定义被双方共享的 payload）
- `tutti/include/tutti/storage_runtime.h`（binding 绝不进入 public Runtime）
- 任何 vendor SDK（`cuda`、`hip`、`maca`、`musa`）
- 任何真实 libnvm / snvme / FIEMAP 系统头（`<linux/fiemap.h>`、`libnvm.h` 等）
- `backends/`、`io_engine/`、`device_manager/` 下的任何头

**允许**在 binding header 中出现的私有名词（这是它的职责）：`extent`、`namespace`、`logical offset`、`device offset`、`block size`。

**不允许**出现的：真实 FIEMAP flag 常量、`fd`、PRP、SGL、CID、doorbell、CUDA kernel、transport completion 类型。理由：那些属于 DataPath 内部实现，不属于 resolver↔DataPath 的共享契约。

# Contract test 要求

target 与 CTest 名固定：

```text
tutti_binding_contract_test
```

普通 C++17 可执行程序，不用 GTest，不用 CUDA SDK，不做任何真实 IO。

必须实现：

- 一个 **fake resolver**，继承 `tutti::StorageTargetResolver`，通过 `make_resolved_target()` 产出携带 `Ext4LocalNvmePayload` 的 `ResolvedTarget`；
- 一个 **fake DataPath consumer**（不必继承 `DataPath`，一个普通类即可，因为不许 include `data_path.h`），通过 `view_payload()` 取出 payload。

至少覆盖：

1. **正常配对**：fake resolver 产出 → fake consumer 用 `view_payload()` 成功取到 payload，字段内容正确；
2. **payload type mismatch**：手工用 `ResolvedTarget::make` 造一个 payload_type_id 不同的 target，`view_payload()` 返回 `UNSUPPORTED`；
3. **API version mismatch**：造一个版本不同的 target，`view_payload()` 返回 `UNSUPPORTED`；
4. **空壳 target**：默认构造的 `ResolvedTarget`，`view_payload()` 返回 `UNSUPPORTED`，且 `valid()` 为 false；
5. **payload owner 生命周期**：证明 payload 与 lease 的唯一所有者是 `ResolvedTarget`。用 `std::weak_ptr` 观察：局部 `shared_ptr` 释放后 payload 仍存活；`ResolvedTarget` 析构后 payload 与 lease 均被释放且**析构函数各只调用一次**（用计数器验证）；
6. **move 后 owner 不丢失**：move 到新对象后仍可 `view_payload()`，moved-from 的 `valid()` 为 false，且 payload 未被提前释放；
7. **immutable**：`view_payload()` 返回 `const` 指针；用注释标出「若解注释则编译失败」的写操作，或用 `static_assert` 证明返回类型带 const；
8. **extent 映射正确**：多段 extent 下，logical offset → device offset 映射结果正确（至少覆盖首段内、跨段边界、末段内三个点）；
9. **映射越界**：offset ≥ file_size 时返回 `OUT_OF_RANGE`；
10. **`validate()` 接受合法 extent 集合**：升序、无重叠、无空洞、完整覆盖 `[0, file_size)` → OK；
11. **`validate()` 拒绝非法集合**：至少分别覆盖「有空洞」「有重叠」「未覆盖到 file_size」「乱序」四种，各自返回非 OK；
12. **配对收敛性**：证明测试代码中**没有任何地方**手写 payload type id 字符串字面量或版本号 —— 全部经由 binding header 的常量与 helper。可用一个说明性断言 + 结果文件中的说明来表达。

测试源码只允许 include：

```cpp
#include <tutti/bindings/ext4_local_nvme/binding.h>
```

以及标准库头。（可以 include `<tutti/spi/storage_target_resolver.h>` 用于 mismatch 用例手工造 target。）

Standalone CMake 必须：`project(... LANGUAGES CXX)`；C++17；上述两个 include root；target-scoped 定义 `TUTTI_USE_HOST=1`（`storage_target_resolver.h` 只依赖 `status.h`，但加上以防传递依赖需要）；`-Wall -Wextra -Werror`；不查找 CUDA 或任何第三方 SDK；`enable_testing()` 并注册 CTest。

# 安全限制

绝对禁止 `sudo` / `insmod` / `rmmod` / `modprobe`；禁止启动 daemon/client、访问 `/dev/nvme*`、执行 FIEMAP、打开任何真实文件、执行任何硬件 IO。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 清理与 configure

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round5-session3

cmake -S /data/home/ryeqiu/Tutti/tests/binding_contract \
  -B /data/home/ryeqiu/Tutti/build/round5-session3 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

要求日志无 CUDA / gRPC / yaml-cpp / libnvm / FIEMAP / NVMe dependency discovery。

## 2. Build 与 CTest

```bash
cmake --build /data/home/ryeqiu/Tutti/build/round5-session3 \
  --target tutti_binding_contract_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round5-session3 \
  --output-on-failure -R '^tutti_binding_contract_test$'
```

要求 `-Werror` 零告警，1/1 PASS。

## 3. binding 未被公共头 include（关键边界）

```bash
grep -rn 'bindings/' /data/home/ryeqiu/Tutti/tutti/include/ || echo 'no public header includes binding: PASS'
grep -rn 'bindings/' /data/home/ryeqiu/Tutti/tutti/spi/ 2>/dev/null || echo 'no SPI header includes binding: PASS'
```

两者都必须无命中（若 `tutti/spi/` 已被并发 worker 移走，第二条改查 `tutti/include/tutti/spi/`，两处都查一遍并说明）。

## 4. binding 未依赖 DataPath SPI / Runtime / vendor SDK

```bash
grep -nE 'data_path\.h|storage_runtime\.h|cuda|hip|maca|musa|libnvm|fiemap|backends/|io_engine/|device_manager/' \
  /data/home/ryeqiu/Tutti/tutti/bindings/ext4_local_nvme/binding.h
```

必须无命中。

## 5. 禁止名词 guard（词边界）

```bash
grep -nEiw 'prp|sgl|cid|doorbell|fd' \
  /data/home/ryeqiu/Tutti/tutti/bindings/ext4_local_nvme/binding.h
```

如有命中须逐条说明是注释还是真实类型/字段。注意 `extent` / `namespace` / `lba` 在本 header 中**允许**，不在此 guard 内。

## 6. 未手写 type id 字面量（收敛性验证）

```bash
grep -nE '"[a-z0-9_.-]*payload[a-z0-9_.-]*"' \
  /data/home/ryeqiu/Tutti/tests/binding_contract/binding_contract_test.cpp
```

除 mismatch 用例故意使用的**错误** id 外，不应出现本 binding 的正确 payload type id 字面量。请在结果中逐条说明每处命中的用途。

## 7. Hygiene

```bash
git diff --check -- tutti/bindings
```

对所有新增文件额外检查尾随空白与 EOF newline。确认只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. binding header 定义了 `Extent` / `NamespaceIdentity` / `Ext4LocalNvmePayload` 与三个身份常量；
2. payload immutable，只提供只读访问；
3. `validate()` 能拒绝空洞 / 重叠 / 未完整覆盖 / 乱序四类非法 extent 集合；
4. logical→device offset 映射正确，越界返回 `OUT_OF_RANGE`；
5. `make_resolved_target()` 与 `view_payload()` 使配对收敛到单一声明点，调用方无需手写 id 或版本；
6. `make` 与 `view` 的 payload 模板参数（含 const 限定）一致，并有说明；
7. type mismatch 与 version mismatch 均返回 `UNSUPPORTED`；空壳 target 亦然；
8. payload owner 生命周期被 `weak_ptr` + 析构计数证明：唯一所有者是 `ResolvedTarget`，move 不丢失，析构各一次；
9. binding header 未被任何公共头或 SPI 头 include；
10. binding header 未依赖 DataPath SPI、Runtime 或任何 vendor / libnvm / FIEMAP 头；
11. standalone configure/build/ctest 在 `-Werror` 下通过，不需要 CUDA SDK；
12. CMake 同时提供两个 include root，对 SPI 头迁移不敏感；
13. 未修改允许列表外文件（**特别是 `tutti/spi/**` 零改动**）；
14. 未执行任何模块、daemon、文件系统或 IO 操作；
15. 空白检查通过。

如果并发 worker 的 SPI 头迁移导致你编译失败，**说明你的 include root 或 include 写法有问题**。请按本 prompt 的两 root 方案修正自己的 CMake，**不要**去改 SPI 头，也不要等待对方。

# 结果落盘要求

把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round5/result3.md`

至少包含：

1. 修改/新增文件列表
2. binding header 的实际 public surface（常量、三个类型、两个 helper）
3. `validate()` 的判据实现与四类非法集合的拒绝方式
4. logical→device 映射算法与边界处理
5. 配对收敛性设计说明：如何保证调用方无需手写 id/版本，以及 `make`/`view` 的 const 限定如何对齐
6. payload owner 生命周期验证的实测输出（weak_ptr 存活状态 + 析构计数）
7. configure / build / ctest 结果（含 `-Werror`）
8. binding 未被公共头/SPI 头 include 的验证结果
9. 依赖限制与禁止名词 guard 结果（逐条说明命中）
10. 收敛性 grep 结果（逐条说明每处字面量用途）
11. 两个 include root 的设置及理由
12. `tutti/spi/**` 零改动的证明
13. 文件边界与空白检查结果
14. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 命令失败就写真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round5/result3.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
