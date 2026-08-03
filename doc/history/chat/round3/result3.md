# T-010 StorageTargetResolver SPI — Result

## 1. 修改/新增文件列表

| 文件 | 操作 |
|---|---|
| `tutti/spi/storage_target_resolver.h` | 新增 |
| `tests/storage_target_resolver_contract/CMakeLists.txt` | 新增 |
| `tests/storage_target_resolver_contract/storage_target_resolver_contract_test.cpp` | 新增 |
| `chat/round3/result3.md` | 新增（本文件） |

未修改任何允许列表外的文件。

## 2. ResolveOptions / ResolvedTarget / Resolver 的实际 public surface

### ResolveOptions

```cpp
struct ResolveOptions {
    std::string scheme;
};
```

最小选项结构，仅含 scheme 字段。URI scheme 选择 resolver，options 携带补充提示。

### ResolvedTarget

```cpp
class ResolvedTarget {
public:
    ResolvedTarget() = default;                 // 空壳, valid()==false
    ~ResolvedTarget() = default;

    ResolvedTarget(ResolvedTarget&&) noexcept = default;      // move-only
    ResolvedTarget& operator=(ResolvedTarget&&) noexcept = default;
    ResolvedTarget(const ResolvedTarget&) = delete;           // 不可 copy
    ResolvedTarget& operator=(const ResolvedTarget&) = delete;

    // 元数据访问（空壳返回零/空）
    std::string_view resolver_type_id() const noexcept;
    std::string_view payload_type_id() const noexcept;
    std::uint32_t source_api_version() const noexcept;
    std::uint64_t logical_size() const noexcept;
    std::string_view recommended_data_path_key() const noexcept;
    bool valid() const noexcept;

    // checked payload view
    template <typename Payload>
    Result<const Payload*> view(
        std::string_view expected_payload_type_id,
        std::uint32_t supported_api_version) const;

    // factory
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
```

关键属性：
- **move-only**：copy 被删除，move 被默认。
- **type-erased**：内部 Storage 使用 `shared_ptr<void>` 存储 payload 和 lease，公共 API 不暴露具体类型。
- **no common variant/union**：不使用 `std::variant`、`union` 或 `std::any` 作为公共返回。
- **no RTTI**：类型身份通过 `payload_type_id`（字符串）+ `source_api_version`（uint32_t）判断。
- **borrowing view**：`view<P>()` 返回 `const Payload*`，借用自 ResolvedTarget 的 payload owner，不延长 lease。

### StorageTargetResolver

```cpp
class StorageTargetResolver {
public:
    virtual ~StorageTargetResolver() = default;
    virtual Result<ResolvedTarget> resolve(
        std::string_view uri,
        const ResolveOptions& options) = 0;
};
```

抽象 SPI，只有 `resolve`。不包含 open/close/submit。

## 3. payload owner 与 lease 生命周期实现

### 存储

`ResolvedTarget` 内部持有 `std::unique_ptr<Storage>`，其中 `Storage` 包含：
- `std::shared_ptr<void> payload` — immutable payload 的 shared owner
- `std::shared_ptr<void> lease` — owner lease 的 shared owner

### make() factory

`make<Payload, OwnerLease>(...)` 接收 `shared_ptr<Payload>` 和 `shared_ptr<OwnerLease>`，通过 shared_ptr 的隐式转换存为 `shared_ptr<void>`。control block 被保留，deleter 正确销毁原始类型对象。factory 拒绝 null payload 和 null lease（返回 `INVALID_ARGUMENT`）。

### view() 借用

`view<Payload>(expected_type_id, supported_version)` 不创建新的 shared_ptr，不延长 lease。它检查 type ID 和 version 后，通过 `static_cast<const Payload*>(storage_->payload.get())` 返回裸指针。指针的有效期不超过 ResolvedTarget 的 payload owner。

### move 语义

`ResolvedTarget` move 后，moved-from 对象的 `storage_` 为 nullptr，`valid()` 返回 false。moved-to 对象获得完整的 Storage（包括 payload 和 lease 的 shared_ptr），所有 shared_ptr 的引用计数正确传播。

### 不变量

- payload 在 target close 前 immutable（`view()` 返回 `const Payload*`）。
- DataPath 不保存超出 owner 生命周期的裸引用（`view()` 返回借用指针，不延长 owner）。
- payload owner 由 `ResolvedTarget` 持有，不是 resolver singleton。
- resolver/DataPath 的成对 payload 类型放在独立 binding header，不从 public target 传播。

## 4. type/version mismatch 行为

`view<Payload>(expected_type_id, supported_version)` 按以下顺序检查：

1. **空壳检查**：`storage_` 为 null → 返回 `UNSUPPORTED`（"target is empty"）
2. **payload type 检查**：`payload_type_id != expected_type_id` → 返回 `UNSUPPORTED`（"payload type mismatch"）
3. **API version 检查**：`source_api_version != supported_api_version` → 返回 `UNSUPPORTED`（"API version mismatch"）
4. **全部匹配**：返回 `const Payload*`

本轮要求 exact match（不支持 backward/forward compatibility 语义）。未来可由 DataPath 声明支持集合后再扩展。

### 重要限制

`view<P>()` 的 type 检查基于字符串 ID，不验证 C++ 类型本身。调用者必须确保 `Payload` 模板参数与 `make<Payload, ...>()` 使用的 C++ 类型一致。用错误的 C++ 类型配正确的字符串 ID 调用 `view()` 是 source-level 误用（`static_cast` 结果未定义）。这是 source-level 契约的固有约束，不使用 RTTI 的代价。

## 5. configure / build / ctest 结果

### Configure

```
$ cmake -S /data/home/ryeqiu/Tutti/tests/storage_target_resolver_contract \
  -B /data/home/ryeqiu/Tutti/build/round3-session3 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

-- The CXX compiler identification is GNU 13.1.1
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: /opt/rh/gcc-toolset-13/root/usr/bin/c++ - skipped
-- Detecting CXX compile features - done
-- Configuring done (0.2s)
-- Generating done (0.0s)
-- Build files have been written to: /data/home/ryeqiu/Tutti/build/round3-session3
```

日志中无 CUDA、gRPC、yaml-cpp、libnvm、FIEMAP 或 NVMe dependency discovery。

### Build

```
$ cmake --build /data/home/ryeqiu/Tutti/build/round3-session3 \
  --target tutti_storage_target_resolver_contract_test -j8

[ 50%] Building CXX object CMakeFiles/tutti_storage_target_resolver_contract_test.dir/storage_target_resolver_contract_test.cpp.o
[100%] Linking CXX executable tutti_storage_target_resolver_contract_test
[100%] Built target tutti_storage_target_resolver_contract_test
```

零 warning（`-Wall -Wextra -Werror`）。

### CTest

```
$ ctest --test-dir /data/home/ryeqiu/Tutti/build/round3-session3 \
  --output-on-failure -R '^tutti_storage_target_resolver_contract_test$'

Internal ctest changing into directory: /data/home/ryeqiu/Tutti/build/round3-session3
Test project /data/home/ryeqiu/Tutti/build/round3-session3
    Start 1: tutti_storage_target_resolver_contract_test
1/1 Test #1: tutti_storage_target_resolver_contract_test ...   Passed    0.00 sec

100% tests passed, 0 tests failed out of 1
Total Test time (real) = 0.00 sec
```

1/1 PASS。12 个子测试全部通过。

## 6. public-boundary guard 结果

```
$ grep -nEi 'cuda|hip|maca|musa|libnvm|nvme|fiemap|grpc|yaml|backends/|io_engine/|device_manager/' \
  /data/home/ryeqiu/Tutti/tutti/spi/storage_target_resolver.h

(exit code 1, no output)
```

无输出，PASS。

## 7. 文件边界与空白检查结果

### git diff --check

```
$ git diff --check -- tutti/spi/storage_target_resolver.h

(no output, exit code 0)
```

### 尾随空白 + EOF newline 检查（全部新增文件）

```
tutti/spi/storage_target_resolver.h                                       — PASS
tests/storage_target_resolver_contract/CMakeLists.txt                     — PASS
tests/storage_target_resolver_contract/storage_target_resolver_contract_test.cpp — PASS
```

### 文件边界

```
$ git status --short

...（pre-existing modifications omitted）...
?? tests/storage_target_resolver_contract/
?? tutti/spi/
```

本 session 新增的文件仅在 `tests/storage_target_resolver_contract/` 和 `tutti/spi/` 下。未修改根或 `tutti/` 的任何现有 CMakeLists.txt、`tutti/include/**`、`.gitignore` 或其他禁止文件。

## 8. 最终结论

```
PASS
```

全部 11 项成功标准均满足：

1. Resolver 接口只有 `resolve`，不包含 open/close/submit。
2. `ResolvedTarget` 持有 immutable payload 与 owner lease（`shared_ptr<void>`），不是 singleton 状态。
3. checked view 同时验证 payload type（字符串）与 source API version（uint32_t）。
4. 不匹配时返回结构化 `UNSUPPORTED` Status。
5. payload 未进入 common union（使用 `shared_ptr<void>` type erasure）。
6. SPI 不泄漏 filesystem/NVMe/transport/vendor 私有类型。
7. fake resolver 与 fake DataPath consumer 的 contract 测试通过（12/12）。
8. HOST standalone configure/build/ctest 通过，不需要 CUDA SDK。
9. 未修改允许列表外文件。
10. 未执行任何模块、daemon、FIEMAP 或 IO 操作。
11. 空白检查通过。

## 总指挥验收

验收结论：`PASS`（含 1 项非阻塞后续项）。

独立核验结果：

- `StorageTargetResolver` 只有 `resolve(uri, options)`，无 open/close/submit。
- `ResolvedTarget` 为 move-only：copy ctor/assign 显式 `= delete`，move 为 `noexcept default`。
- type erasure 使用 `shared_ptr<void>`；头文件中 `std::variant`、`std::any`、`union`、`typeid`、`dynamic_cast` 零命中。
- 头文件仅 include `<tutti/status.h>` + 5 个标准库头；公共边界 guard（prompt 原样 substring 版）零输出。
- 独立编译验证：不定义任何 `TUTTI_USE_*` 宏时可单独编译，确认真正 hardware-free。
- 公共壳无 fd、extent、LBA、namespace id 或 FIEMAP flag 字段；`PRP`/`extent` 仅出现在描述“resolver 不得做什么”的注释里，不是类型或字段。
- 总指挥独立重跑 CTest：`1/1 Passed`，程序自报 `All 12 ... tests passed`；cost data 与 LastTest.log 均确认真实执行。
- 独立 owner/lease 生命周期实证（超出 worker 测试范围）：

```text
after make:                 payload_alive=1 lease_alive=1 use_p=1 use_l=1
after move:                 payload_alive=1 lease_alive=1 dtors p=0 l=0
after moved-to destroyed:   payload_alive=0 lease_alive=0 dtors p=1 l=1
final dtors p=1 l=1 (expect 1 1)
```

  证明：payload 与 lease 的唯一所有者是 `ResolvedTarget`（不是 resolver singleton）；`shared_ptr<Payload> → shared_ptr<void>` 保留了 control block，析构调用了正确的原始类型 deleter，无泄漏、无双析构；move 后 `string_view` 元数据仍稳定可读（`std::string` 由 `Storage` 持有，`unique_ptr<Storage>` move 不搬移字符串缓冲）。

- 文件边界正确，本 session 只新增 3 个源码文件 + result3.md。工作区中的 `tutti/spi/data_path.h`（mtime 16:05:05）晚于本 session 的 result3.md（16:04:59），属于 Round 3 Session 4 的交付物，**不是本 session 越界**。`.gitignore` 的修改来自总指挥与用户，非 worker。
- 四个文件尾随空白与 EOF newline 均 OK；`git diff --check` 通过；linter 0 diagnostics。
- 未执行 sudo、模块操作、daemon、FIEMAP 或任何硬件 IO。

非阻塞后续项（记录，不返工）：

`view<Payload>()` 的类型身份**只**基于字符串 ID + `uint32_t` version，随后无条件执行 `static_cast<const Payload*>(shared_ptr<void>::get())`。worker 已在第 4 节「重要限制」如实声明该约束。总指挥进一步实证了其真实爆炸半径：

```text
make<A, L>(..., "payload-v1", 1, ...)   // 存入的是 A
view<B>("payload-v1", 1)                // 字符串 ID 与 version 均匹配
  -> ok=1, 返回 reinterpret 后的垃圾数据 (b=1432778632 c=287454020)
```

即 **string ID 匹配但 C++ 类型写错时，`view()` 静默成功并返回 UB 数据**，无任何诊断。这是「不使用 RTTI」的固有代价，本轮契约未违反（prompt 明确要求 type identity 用字符串 + version，且禁止要求 RTTI），因此不返工。但它意味着 `{payload_type_id, Payload}` 这一配对是**未被机器强制的口头约定**，误用只在运行时表现为脏数据。

缓解方向（留给 Binding 落地时决策，任选其一即可）：

1. 架构层：让每个 binding header 成为 `{type_id 常量, Payload 类型, view 包装函数}` 的**唯一**声明点，调用方不得手写字符串字面量与模板参数组合，把误用面收敛到单文件单行。
2. 廉价机器校验：`make`/`view` 额外记录并比对 `sizeof(Payload)` 与 `alignof(Payload)`，不引入 RTTI 即可拦截大部分不匹配（不能拦截同布局不同语义的类型）。
3. 若将来允许 RTTI，可改用 `std::type_index` 做权威身份，字符串 ID 退化为跨版本兼容标签。

同时记录一个语义待定项：本轮 version 检查为 exact match（`!=` 即 `UNSUPPORTED`），第 4 节已声明。将来 DataPath 需要声明「支持版本集合」时，`view()` 的第二参数语义要从「my version」改为「supported set」，届时需同步更新契约与测试，避免沿用当前 exact-match 假设。

关于结果文件的 Git 可见性（非 worker 问题，仅告知）：

```text
git check-ignore -v chat/round3/result3.md
  -> .gitignore:79:chat/    chat/round3/result3.md
```

`chat/` 已被忽略，因此所有 `chat/roundX/resultY.md` 不进入版本控制，`git diff --check` 对它们无效。worker 只对头文件跑 `git diff --check` 是正确的；结果文件的空白检查由显式 grep 完成，已通过。另注：当前 `.gitignore` 存在 `tutti/build-profile-*` 重复行且末行无 EOF newline，属既有小瑕疵，不在本 session 范围。

后续决定：T-010 完成，不需要返工。Round 3 剩余 Session 1、4 可继续；Session 1 会启动 daemon，不要与其他硬件任务并发。
