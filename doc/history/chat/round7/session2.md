# TASK T-024

你是一名资深 C++ 存储工程师。你的任务是**分层抽象 + 代码搬运**：创建第一个 `DataPath` 实现 `LocalNvmeDataPath` 的**骨架**，把现有 NVMe 后端的结构搬到已冻结的 `DataPath` SPI 上，并用 contract test 证明骨架成立。

你看不到任何其他上下文，本 prompt 已包含全部需要的接口、搬运源和验收标准。

# 任务定位（先读这条）

**你在做搬运和抽象，不是做设计评审。**

- 已冻结的 `DataPath` SPI（`tutti/include/tutti/spi/data_path.h`）和 binding（`tutti/bindings/ext4_local_nvme/binding.h`）是**不可修改的契约**。你的工作是让它们被一个具体实现落地。
- 搬运源是 `tutti/backends/nvme/` 下的现有 NVMe 后端。**原样搬运其结构与行为**；不要评判它的对错，不要因为你认为某处可以更好就改它。
- **不要修改** `tutti/backends/**`、`nvme_storage/**`、`tutti/bindings/**`、`tutti/include/**`、`tutti/resolvers/**`。
- 本任务只做**骨架**：`DataPath` SPI 的全部方法都有定义，但只有 lifecycle / capabilities / target open/close / registration_domain 有真实行为；`register_memory` / `submit` / `progress` / `query` / `release` 返回明确的 `UNSUPPORTED` 或 `NOT_READY`（**显式失败**，不是假装成功）。真正的 registration 与 IO 是后续任务。

判断标准：改动是为了「让现有 NVMe 结构适配 DataPath SPI」→ 做；为了「改进现有 NVMe 逻辑」→ 不做。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 执行时机

本任务是**纯软件**任务，不碰任何硬件，可在 HOST profile 下编译测试。**不要**与任何修改 `tutti/include/**` 或 `tutti/bindings/**` 的任务并发。

开始前确认无并发构建：

```bash
ps -eo pid,etime,cmd | grep -E '[c]make|[c]test' | head
```

# 1. 你要实现的抽象（已冻结，不得修改）

## `DataPath` SPI

`tutti/include/tutti/spi/data_path.h`。你必须**完整实现**这个抽象类（命名空间 `tutti`）：

```cpp
class DataPath {
public:
    virtual ~DataPath() = default;

    virtual const DataPathCapabilities& capabilities() const = 0;

    virtual Status initialize(const DataPathConfig& config,
                              ResourceProvider& resources) = 0;
    virtual Status shutdown(std::uint64_t timeout_ns) = 0;

    virtual Result<DataPathTarget> open(const ResolvedTarget& target) = 0;
    virtual Status close(DataPathTarget target) = 0;
    virtual Result<RegistrationDomainKey> registration_domain(
        DataPathTarget target) const = 0;

    virtual Result<DataPathMemory> register_memory(
        const DataPathMemoryView& view,
        const RegistrationDomainKey& domain) = 0;
    virtual Status unregister_memory(DataPathMemory memory) = 0;

    virtual SubmitOutcome submit(const DataPathRequest* requests,
                                 std::size_t count,
                                 const HostSubmitContext& ctx) = 0;
    virtual Result<ProgressResult> progress(ProgressBudget budget) = 0;
    virtual Result<DataPathSnapshot> query(DataPathOp op) const = 0;
    virtual Status release(DataPathOp op) = 0;
};
```

关键值类型（同文件，只读）：

- `DataPathCapabilities` —— 硬约束能力集（名字、版本、执行域、内存类型、方向、对齐、上限、registration scope、progress model 等）。
- `DataPathTarget` / `DataPathMemory` / `DataPathOp` —— phantom-tag 不透明身份，经 `detail::SpiIdentityMint::mint<Tag>(token, generation)` 铸造；`generation != 0` 为有效。
- `RegistrationDomainKey{ std::string value }` —— 不透明字符串键。
- `DataPathMemoryView{ void* base; uint64_t size_bytes; int32_t device_id; DataPathMemoryKind kind }`。
- `ResolvedTarget` —— 前向声明于 SPI，真实定义在 `tutti/include/tutti/spi/storage_target_resolver.h`。

## binding（target 载荷的取得方式）

`tutti/bindings/ext4_local_nvme/binding.h`，命名空间 `tutti::binding::ext4_local_nvme`：

```cpp
inline constexpr std::string_view kRecommendedDataPathKey;
inline Result<const Ext4LocalNvmePayload*> view_payload(const ResolvedTarget& target);

class Ext4LocalNvmePayload {
public:
    static Result<std::shared_ptr<const Ext4LocalNvmePayload>>
    create(NamespaceIdentity ns, std::vector<Extent> extents, std::uint64_t file_size);
    // const 访问器：namespace()、extents()、logical_size()（具体名字以源码为准）
};

struct NamespaceIdentity { std::string controller_pci_addr; uint32_t namespace_id; uint32_t block_size; };
struct Extent { uint64_t logical_offset; uint64_t device_offset; uint64_t length; };
```

**你的 `open()` 必须通过 `view_payload()` 取得 payload**，不要自己去 include `storage_target_resolver.h` 然后强行 `view<>`。binding 已经把配对收敛好了。

**注意 payload 与旧结构的单位差异**（这是搬运的关键转换点）：

- binding 的 `Extent` 是**字节**语义：`logical_offset` / `device_offset` / `length` 全是字节。
- 旧 `NvmeFileDeviceHandle` 的 extent 是 `LbaExtent`（`start_lba` / `length_blocks`，**block** 单位）。
- 转换：`start_lba = device_offset / block_size`，`length_blocks = length / block_size`。旧 `NvmeFileDeviceHandle` 用 `LbaExtent` 是按 block 对齐的（payload 的 `validate()` 已保证 `device_offset` 与 `length` 是 `block_size` 的倍数，因为 resolver 做了对齐检查）。
- binding 的 `logical_offset`（字节）在旧结构里没有直接对应项 —— 旧设备侧按「extent 顺序 walk」推出逻辑位置。你的 `open()` 可以选择保留 `logical_offset`（如果你判定后续需要）或按旧结构的约定只存 block 范围。**在结果中说明你的选择及理由**，但这是骨架，重点是结构落地。

# 2. 搬运源（读，原样搬运结构与行为）

## `tutti/backends/nvme/include/nvme_backend.h` + `src/nvme_backend.cpp`

`NvmeBackend` 的 lifecycle 与资源管理：

- `initialize(IDeviceManager*, BackendConfig)`：打开 vdevice roster，初始化 PRP cache，失败回滚。
- `shutdown()`：幂等，归还所有 vdevice、释放资源。
- target handle 的追踪与缓存（`target_handles_` 的 `TargetHandleEntry`、`target_handle_cache_` 的 `TargetCacheKey`）。
- `backend_name()` / `metadata()`（能力汇报的参考）。

**注意**：`NvmeBackend` 通过 `IDeviceManager` 拿 vdevice。你的骨架**不接入 Device Manager**（那是后续 control-plane 任务）。骨架的 `initialize()` 只做最小初始化（capabilities 定型、状态置位），**不要**去打开任何设备、不要链接 libnvm、不要 CUDA。在结果中说明：Device Manager 接入被显式推迟，骨架的 `initialize` 是「无设备」的。

## `tutti/backends/nvme/include/nvme_target_handle.h`

`NvmeFileDeviceHandle` 是 GPU-resident 的文件句柄：

```cpp
struct NvmeFileDeviceHandle {
    uint64_t file_id;
    uint64_t logical_size_bytes;
    uint32_t nvme_block_size;
    uint32_t nvme_block_size_log;
    uint32_t namespace_id;
    uint32_t   num_extents;
    LbaExtent* extents;           // GPU pointer, inline array
    LbaExtent* extents_overflow;  // GPU pointer, nullptr if <= 8
    nvm_queue_t* d_qps;           // GPU-resident QueuePair[]
    uint32_t     queue_quota;
    static constexpr uint32_t MAX_INLINE_EXTENTS = 8;
};
```

注释（`:17-22`）说明它由 `acquire_target_handle()` 经 `cudaMalloc` 分配、`release_target_handle()` 经 `cudaFree` 释放，且所有指针必须是 CUDA 可访问内存，不能存 host 堆指针。

**骨架的处理**：`NvmeFileDeviceHandle` 依赖 `nvm_types.h`（libnvm 的 `nvm_queue_t`）和 CUDA —— 这两个都是骨架**不引入**的依赖。因此：

- **不要**在骨架里 `#include <nvm_types.h>`、不要 `cudaMalloc`、不要 link CUDA 或 libnvm。
- 你可以定义一个**host 侧的 target 状态结构**（例如存 `NamespaceIdentity` + 转换后的 extent 列表 + 一个 mint 出的 `DataPathTarget` 身份），把「将来要 cudaMalloc 一个 `NvmeFileDeviceHandle`」这件事记录为 TODO/后续任务。
- 在结果中说明：device-resident 句柄的分配（cudaMalloc + d_qps + extent H2D）被显式推迟到 IO 提交路径落地时，因为它依赖 CUDA 与 queue，骨架不引入。

## `tutti/backends/nvme/src/nvme_backend.cpp` 的 target handle 缓存逻辑

`acquire_target_handle` 的缓存（`target_handle_cache_`，key = `(target_id, start_lba, vdev_index)`）与 `release_target_handle` 的释放。

**注意一个已知问题（不要修，只需不引入）**：`nvme_target_handle.cpp:185-213` 的 `release_target_handle` 只从 `target_handles_` 删除并 `cudaFree`，**没有**清 `target_handle_cache_` 的对应项，导致后续 acquire 可能返回已释放的指针（悬空）。这是接手方案 P0-8 记录的问题。**你的骨架不要复制这个缺陷** —— 骨架的 target 表应当让 `close()` 真正使身份失效（generation 校验），并在结果中说明你如何避免这个悬空问题。这是「适配新接口所必需」的正确性改动，不属于「顺手改进」。

# 3. 骨架的设计要求

## 类与文件

在 `tutti/data_paths/local_nvme/` 下创建：

- `local_nvme_data_path.h`（`LocalNvmeDataPath` 类，公开继承 `tutti::DataPath`）
- `local_nvme_data_path.cpp`（实现）

命名空间建议 `tutti::data_paths::local_nvme`（或你选定的合理命名空间，说明理由）。

## 必须真实实现的方法

### `capabilities()`

返回一个**如实**的 `DataPathCapabilities`。骨架阶段，诚实地填：

- `name`（例如 `"local_nvme"`）、`source_api_version = 1`；
- 骨架尚不能做的，**如实置 false / 0**（例如 `supports_device_execution`、`supports_direct` 等取决于你是否已能实现 —— 骨架阶段多为 false）。
- 对齐约束参考生产：`nvme_block_size`（4096）可作为 `target_alignment_bytes` / `memory_alignment_bytes` / `length_alignment_bytes` 的参考，但**若你的骨架还无法保证这些对齐下的 IO，就如实设为 1 或标注**。**不要虚报能力** —— `DataPathCapabilities` 是硬约束，虚报会让 Runtime 误信。在结果中逐项说明你填的值及依据。

### `initialize(config, resources)`

最小初始化：定型 capabilities、置「已初始化」状态。**不打开设备、不链 libnvm、不 CUDA**。返回 OK。

`ResourceProvider&` 参数：SPI 只前向声明了它。骨架**不使用**它（它只是将来 control-plane 资源的入口）。在结果中说明骨架为何可以不触碰它。

### `shutdown(timeout_ns)`

幂等：释放所有打开的 target（使其身份失效），置「未初始化」。重复调用安全返回 OK。

### `open(resolvedTarget)` → `DataPathTarget`

1. 通过 `view_payload()` 取得 payload；失败（类型不匹配 / 版本不支持）→ 返回相应错误。
2. 校验 namespace identity 与 extents 有效（payload 的 `validate()` 已保证基本完整性，这里做骨架层面的必要检查，例如 `block_size != 0`）。
3. 做字节 → block 的转换（`device_offset / block_size`、`length / block_size`），存入 host 侧 target 状态。
4. 铸造一个 `DataPathTarget` 身份（`SpiIdentityMint::mint<DataPathTargetTag>(token, generation)`），token 用递增计数器，generation 从 1 起。
5. 存入 target 表（`token → 状态`），返回身份。

### `close(target)` → `Status`

按 `token + generation` 校验身份存在且未被关闭；使身份失效（后续 `registration_domain` / `close` 对该身份返回错误）。返回 OK。重复 close 已关闭身份 → 明确错误（不要静默成功）。

### `registration_domain(target)` → `RegistrationDomainKey`

返回一个**从 target 身份派生**的不透明字符串键（例如由 namespace 的 `controller_pci_addr` + `namespace_id` 派生，如 `"local_nvme:0000:08:00.0:ns1"`）。**不得**返回任何裸指针或对象地址。校验身份有效，无效返回错误。

## 必须显式失败的方法（骨架阶段）

这些方法返回**明确的错误**，不是假装成功，不是空操作：

- `register_memory(...)` → 返回 `Result<DataPathMemory>::Failure(Status(StatusCode::UNSUPPORTED, "...not yet implemented"))` 或 `NOT_READY`。说明原因（registration 是后续任务）。
- `unregister_memory(...)` → `Status(StatusCode::UNSUPPORTED, ...)`。
- `submit(...)` → 返回一个 `SubmitOutcome`，其 `status` 为 `UNSUPPORTED`，`op == std::nullopt`（零发出），`initial_states` 每项都是 `REJECTED` + `UNSUPPORTED`。**注意保持 SPI 不变量**：`initial_states.size() == count`、`op == nullopt` 表示零发出。
- `progress(...)` → `Result<ProgressResult>::Failure(Status(StatusCode::UNSUPPORTED, ...))` 或返回一个合法的 `ProgressResult{ work_units_consumed=0, ... }`。**选择其一并说明理由**；若返回 ProgressResult，必须是有界的（消耗 0、不伪装忙等）。
- `query(op)` → 骨架没有任何 op，任何 `DataPathOp` 都查不到 → 返回错误（`NOT_FOUND` 或 `INVALID_ARGUMENT`）。
- `release(op)` → 骨架没有 op → 返回错误。

## 身份管理

- `DataPathTarget` 用 `SpiIdentityMint` 铸造，token 递增、generation ≥ 1。
- `close()` 使身份失效后，任何对该身份的后续操作（`close` / `registration_domain`）必须返回错误 —— 这就是避免旧代码 P0-8 悬空问题的关键：**generation 或显式的 alive 标志必须在 close 后翻转为无效**。

## 依赖约束

`local_nvme_data_path.h/.cpp` 只允许 include：

- `<tutti/status.h>`、`<tutti/io_types.h>`
- `<tutti/spi/data_path.h>`
- `<tutti/spi/storage_target_resolver.h>`（`ResolvedTarget` 的定义，`open()` 的参数类型需要）
- `tutti/bindings/ext4_local_nvme/binding.h`（`view_payload`）
- C++ 标准库头

**禁止** include：CUDA / HIP 或任何 vendor SDK；libnvm 任何头（`nvm_types.h`、`nvm_dma.h` 等）；`nvme_storage/**`；`backends/**`；`io_engine/**`；`device_manager/**`；`coordinator/**`；`tutti/include/tutti/storage_runtime.h`。

**说明**：骨架**不** link CUDA 或 libnvm。device-resident 句柄、PRP、queue 都是后续任务。

# 4. 测试要求

在 `tests/local_nvme_datapath_contract/` 下建独立可 configure 测试（自带顶层 `CMakeLists.txt`，`project()` + `enable_testing()` + `add_test`）：

- 不依赖 gtest；简单断言 + 非零退出码。
- `-Wall -Wextra -Werror`；`TUTTI_USE_HOST=1`。
- include root：`/data/home/ryeqiu/Tutti/tutti/include` 与 `/data/home/ryeqiu/Tutti`。
- 链接你的 `LocalNvmeDataPath` 实现（把 `.cpp` 直接编进测试，或建一个小 static lib，自行选择）。
- CTest 测试名 `tutti_local_nvme_datapath_contract_test`。
- **不需要任何硬件**：用 binding 的 `create()` 造一个**合成** `ResolvedTarget`（构造若干合法 extent 的 `Ext4LocalNvmePayload`，经 `make_resolved_target()` 包装），不需要真实文件、FIEMAP、NVMe、CUDA。

至少覆盖：

1. **capabilities 如实**：非空 name、version ≥ 1；逐项打印并人工核对不虚报。
2. **lifecycle**：`initialize` → OK；重复 `shutdown` → 幂等 OK。
3. **open 成功**：合成合法 `ResolvedTarget` → `open()` 返回有效 `DataPathTarget`（`valid()` 为真）。
4. **open 拒绝错误 payload**：用 binding 造一个 payload type id 不匹配的 `ResolvedTarget`（直接用 `ResolvedTarget::make<>` 造一个非 ext4_local_nvme 的 payload，或用错误的 type id），`open()` 应失败。
5. **registration_domain**：对打开的 target 返回非空键，且键**不含**指针特征（可打印出来人工核对是 `local_nvme:...` 这类派生串）；同一 target 两次调用返回相同键。
6. **close 使身份失效**：`close()` 后再 `close()` 同身份 → 错误；再 `registration_domain()` 同身份 → 错误。这验证你不会重蹈旧代码 P0-8 悬空问题。
7. **close 未知身份**：对一个从未 mint 的 `DataPathTarget`（默认构造，`valid()` 为假）`close()` → 错误，不 crash。
8. **显式失败**：`register_memory` / `submit` / `progress` / `query` / `release` 都返回明确错误，且 `submit` 的不变量成立（`op == nullopt`、`initial_states.size() == count`、每项 `REJECTED`）。
9. **open 多个 target**：连续 open 多个，身份互不相同（token 递增），互不影响。
10. **字节 → block 转换正确性**：用一组已知 extent（例如 `device_offset=8192`、`length=4096`、`block_size=4096`），验证 `open()` 后存的 block 值正确（`start_lba=2`、`length_blocks=1`）。如果你的结构保留这些值，通过某种方式可观测地验证（打印或暴露只读访问器）。

每个用例打印一行标识与结果，末尾打印通过总数。

# 5. 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/data_paths/local_nvme/local_nvme_data_path.h`
- `/data/home/ryeqiu/Tutti/tutti/data_paths/local_nvme/local_nvme_data_path.cpp`
- `/data/home/ryeqiu/Tutti/tests/local_nvme_datapath_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round7/result2.md`

构建产物只能写入 `/data/home/ryeqiu/Tutti/build/round7-session2*`。

禁止修改或创建任何其他文件。尤其禁止：

- 修改 `tutti/include/**`、`tutti/bindings/**`、`tutti/resolvers/**`、`tutti/backends/**`、`nvme_storage/**`
- 修改任何 `CMakeLists.txt`，除你新建的 `tests/local_nvme_datapath_contract/CMakeLists.txt`
- 修改 `tests/` 下其他目录
- 修改 `.gitignore`、`chat/**` 中除 `chat/round7/result2.md` 外的文件
- link CUDA / libnvm，include 任何禁止的 header

禁止提交 Git commit。

# 6. 安全限制

绝对禁止 `sudo` / `insmod` / `rmmod` / `modprobe`；禁止启动 daemon/client；禁止访问 `/dev/nvme*`、`/dev/ssnvme*`；禁止执行 CUDA 调用；禁止任何硬件 IO。

# 7. 验收步骤

## 1. 编译与运行

```bash
rm -rf build/round7-session2
cmake -S tests/local_nvme_datapath_contract -B build/round7-session2 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round7-session2 --target tutti_local_nvme_datapath_contract_test -j8 2>&1 | tail -15
ctest --test-dir build/round7-session2 --output-on-failure -R '^tutti_local_nvme_datapath_contract_test$'
```

要求 `-Werror` 零告警、`1/1 Passed`。

## 2. 头文件可独立编译

```bash
printf '%s\n' '#include "tutti/data_paths/local_nvme/local_nvme_data_path.h"' 'int main(){ return 0; }' \
  | /opt/rh/gcc-toolset-13/root/usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -DTUTTI_USE_HOST=1 \
    -I/data/home/ryeqiu/Tutti/tutti/include -I/data/home/ryeqiu/Tutti -x c++ -fsyntax-only -
```

## 3. 与既有契约头共存（撞名哨兵）

```bash
printf '%s\n' \
  '#include <tutti/memory_types.h>' \
  '#include <tutti/spi/data_path.h>' \
  '#include <tutti/spi/storage_target_resolver.h>' \
  '#include "tutti/data_paths/local_nvme/local_nvme_data_path.h"' \
  'int main(){ return 0; }' \
  | /opt/rh/gcc-toolset-13/root/usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -DTUTTI_USE_HOST=1 \
    -I/data/home/ryeqiu/Tutti/tutti/include -I/data/home/ryeqiu/Tutti -x c++ -fsyntax-only -
```

必须通过（无重复定义、无命名冲突）。

## 4. 依赖约束核验

```bash
grep -n '#include' tutti/data_paths/local_nvme/local_nvme_data_path.{h,cpp}
grep -nE 'cuda|hip|libnvm|nvm_|nvm_dma|nvme_storage|backends/|io_engine/|device_manager/|coordinator/|storage_runtime\.h' \
  tutti/data_paths/local_nvme/local_nvme_data_path.{h,cpp} || echo 'no forbidden dependency: PASS'
```

逐条说明命中（若有）。**注意**：`local_nvme` 这个名字本身含 `nvme`，类名/注释里出现 `nvme` 是合法的；禁的是真实的 `#include` 依赖与 libnvm 符号。区分清楚。

## 5. 无 CUDA / libnvm 链接证据

```bash
grep -nE 'target_link_libraries|find_package|cuda|libnvm|nvm' tests/local_nvme_datapath_contract/CMakeLists.txt
```

确认测试只 link 你的实现 + 标准库，没有 CUDA、没有 libnvm。

## 6. 既有测试未被破坏

```bash
for t in data_path_contract storage_target_resolver_contract binding_contract; do
  rm -rf build/round7-session2-$t
  cmake -S tests/$t -B build/round7-session2-$t -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null 2>&1
  cmake --build build/round7-session2-$t -j8 >/dev/null 2>&1
  printf '%s: ' "$t"
  ctest --test-dir build/round7-session2-$t 2>&1 | grep -E 'tests passed|tests failed'
done
```

全部必须通过。

## 7. Hygiene

```bash
for f in tutti/data_paths/local_nvme/local_nvme_data_path.h \
         tutti/data_paths/local_nvme/local_nvme_data_path.cpp \
         tests/local_nvme_datapath_contract/CMakeLists.txt \
         tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp; do
  test -f "$f" || continue
  printf '%s: ' "$f"
  if grep -nE '[[:blank:]]+$' "$f" >/dev/null; then echo TRAILING-WS
  elif [ "$(tail -c 1 "$f" | wc -l)" -ne 1 ]; then echo NO-EOF-NEWLINE
  else echo OK; fi
done
git status --short --untracked-files=all | head -20
```

# 8. 成功标准

报告 `PASS` 需同时满足：

1. `LocalNvmeDataPath` 公开继承 `tutti::DataPath` 并实现**全部** SPI 方法；
2. capabilities 如实填写、逐项有依据、无虚报；
3. `open()` 经 `view_payload()` 取得 payload，做字节→block 转换，铸造有效 `DataPathTarget`；
4. `close()` 使身份失效，后续对该身份的操作返回错误（避免旧 P0-8 悬空）；
5. `registration_domain()` 返回从 target 身份派生的不透明字符串键，无裸指针；
6. `register_memory` / `submit` / `progress` / `query` / `release` 显式失败且保持 SPI 不变量；
7. 生命周期幂等；
8. 依赖约束满足，**未 link CUDA / libnvm**，未 include 任何禁止 header；
9. 头文件可独立编译，与三个既有契约头同 TU 共存编译通过；
10. 10 类测试用例全部实现且通过；
11. `-Werror` 零告警，CTest `1/1 Passed`；
12. 三个既有 contract test 未被破坏；
13. 未修改允许列表外文件；
14. 未执行任何硬件/系统操作；
15. 空白与 EOF newline 检查通过。

如实记录被显式推迟的部分（Device Manager 接入、device-resident 句柄分配、registration、IO 提交）。**记录这些不影响 PASS。**

# 9. 结果落盘要求

写入 `/data/home/ryeqiu/Tutti/chat/round7/result2.md`，至少包含：

1. 交付文件列表与角色
2. 类的形状：继承、命名空间、构造、成员
3. **capabilities 逐项的值及依据**
4. lifecycle / open / close / registration_domain 的实现要点
5. **字节→block 转换的处理，以及 `logical_offset` 保留与否的选择及理由**
6. **如何避免旧代码 P0-8 悬空问题**
7. 显式失败各方法的返回与理由（特别是 `submit` 的不变量、`progress` 的选择）
8. 显式推迟的部分及理由（Device Manager、device 句柄、registration、IO）
9. 10 类测试用例逐一的实现与结果
10. 测试完整输出
11. 依赖约束核验结果
12. 无 CUDA / libnvm 链接证据
13. 头文件独立编译 + 共存编译结果
14. 三个既有 contract test 结果
15. hygiene 检查
16. 最终 `PASS` / `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 命令失败就写真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round7/result2.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
