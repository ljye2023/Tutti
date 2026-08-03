# T-009 Session Result

## 1. 修改/新增文件列表

| 文件 | 操作 |
|------|------|
| `tutti/include/tutti/memory_types.h` | 新增 |
| `tests/memory_types_contract/CMakeLists.txt` | 新增 |
| `tests/memory_types_contract/memory_types_contract_test.cpp` | 新增 |

## 2. 实际 public surface

```cpp
namespace tutti {

enum class MemoryKind {
    HOST,
    PINNED_HOST,
    DEVICE,
    MANAGED,
};

enum class MemoryOwnership {
    RUNTIME_OWNED,
    CALLER_OWNED,
};

struct MemoryView {
    void*           address;
    std::uint64_t   size;
    MemoryKind      expected_kind;
    MemoryOwnership ownership;
    std::int32_t    expected_device_id;  // < 0 = unspecified
    std::string     expected_profile;    // empty = unspecified
};

} // namespace tutti
```

- `MemoryKind`: exactly 4 values, no `EXTERNAL`, no `IMPORTED`
- `MemoryOwnership`: exactly 2 values, no `IMPORTED`
- `MemoryView`: exactly 6 frozen fields, aggregate value type
- Includes: `<cstdint>`, `<string>` only
- No vendor headers, no transport fields

## 3. Expected profile/device/kind 的 unset 语义

Deliberately NOT using `std::optional`; sentinels instead:

| Field | Type | Unset sentinel | Notes |
|-------|------|----------------|-------|
| `expected_device_id` | `std::int32_t` | `< 0` (typically `-1`) | negative = device not specified |
| `expected_profile` | `std::string` | `""` (empty) | empty = profile not specified |
| `expected_kind` | `MemoryKind` | none (always set) | caller always declares expected position |
| `address` | `void*` | `nullptr` | caller may use null for placeholder |
| `size` | `std::uint64_t` | `0` | caller may use zero for placeholder |

## 4. 与旧 `EXTERNAL` 语义的拆分说明

旧设计中 `EXTERNAL` 曾是 `MemoryKind` 的一员，用于表示"caller 分配的 GPU memory"。

新设计将其拆解为正交的两个维度：

- **位置** (`MemoryKind`): 只描述 memory 物理位置（HOST / PINNED_HOST / DEVICE / MANAGED）
- **所有权** (`MemoryOwnership`): 只描述 allocation owner（RUNTIME_OWNED / CALLER_OWNED）

旧 `EXTERNAL` 语义 = 新 `DEVICE + CALLER_OWNED`。

这样做的好处：
- 位置与所有权正交，所有 4×2=8 组合都可表达
- 新增 profile 不需要修改 `MemoryKind` 公共名词
- `MemoryView` 不泄漏 transport/DMA 私有字段

## 5. Configure/build/ctest 结果

### Configure

```
-- The CXX compiler identification is GNU 13.1.1
-- Detecting CXX compiler ABI info - done
-- Configured tutti_memory_types_contract_test (standalone, hardware-free)
-- Configuring done (0.2s)
-- Generating done (0.0s)
```

- 无 CUDA、gRPC、yaml-cpp、libnvm、NVMe 依赖发现 ✓
- Standalone `project(... LANGUAGES CXX)` ✓
- `TUTTI_USE_HOST=1` target-scoped ✓

### Build

```
[ 50%] Building CXX object CMakeFiles/tutti_memory_types_contract_test.dir/memory_types_contract_test.cpp.o
[100%] Linking CXX executable bin/tutti_memory_types_contract_test
[100%] Built target tutti_memory_types_contract_test
```

### CTest

```
Test project /data/home/ryeqiu/Tutti/build/round3-session2
    Start 1: tutti_memory_types_contract_test
1/1 Test #1: tutti_memory_types_contract_test ...   Passed    0.00 sec

100% tests passed, 0 tests failed out of 1
Total Test time (real) = 0.00 sec
```

## 6. Public-boundary guard 结果

```
grep -nEi 'cuda|hip|maca|musa|libnvm|nvme|grpc|yaml|backends/|io_engine/|device_manager/|iova|rkey|backend_private|PRP' \
  /data/home/ryeqiu/Tutti/tutti/include/tutti/memory_types.h
```

输出：

```
51:enum class MemoryOwnership {
73:    MemoryOwnership ownership;
```

分析：仅有 2 行匹配，均为 spec 冻结要求的类型名 `MemoryOwnership`（含子串 "hip"）。这是子串误匹配（grep `-i` 模式中的 `hip` 用于检测 AMD HIP 引用），不是实际的 AMD HIP 依赖。

所有注释中的 `CUDA`、`IOVA`、`PRP`、`rkey`、`backend_private` 等词汇已全部移除。

补充检查：
- `expected_profile` 是 `std::string`，不是 enum ✓
- `MemoryKind` 无 `EXTERNAL`/`IMPORTED` 成员 ✓
- `MemoryOwnership` 无 `IMPORTED` 成员 ✓

## 7. 文件边界与空白检查结果

### git diff --check

```
git diff --check -- tutti/include/tutti/memory_types.h
EXIT=0
```
无空白错误 ✓

### 尾随空白

所有新增文件检查 `grep -rnP ' +$'` → 无匹配 ✓

### EOF newline

所有新增文件均以 `\n` 结尾 ✓

### 文件边界

本 session 实际触碰的文件：
- `tutti/include/tutti/memory_types.h`（新增）
- `tests/memory_types_contract/CMakeLists.txt`（新增）
- `tests/memory_types_contract/memory_types_contract_test.cpp`（新增）
- `chat/round3/result2.md`（新增，本文件）

未修改允许列表外的任何文件 ✓

### 安全限制

未执行 sudo/insmod/rmmod/modprobe ✓
未启动 daemon/client ✓
未访问 /dev/nvme* ✓
未执行任何硬件 IO ✓

## 8. 最终结论

PASS

## 总指挥验收

验收结论：`PASS`（含 1 项非阻塞后续项）。

独立核验结果：

- `MemoryKind` 恰好四值，`MemoryOwnership` 恰好两值；源码中 `EXTERNAL`/`IMPORTED` 只出现在说明注释，无枚举成员。
- 位置与所有权正交；旧 `EXTERNAL` 已正确拆分为 `DEVICE + CALLER_OWNED`，测试覆盖全部 4×2 组合。
- `MemoryView` 严格六字段，类型与顺序符合冻结契约；`expected_profile` 为 `std::string`，未使用闭集 vendor enum。
- 头文件仅 include `<cstdint>`、`<string>`，未依赖 `tutti/cuda_like.h`。
- 独立编译验证：不定义任何 `TUTTI_USE_*` 宏时 `memory_types.h` 仍可单独编译，确认真正 hardware-free。
- 跨契约组合验证：以 `Result<MemoryHandle> reg(const MemoryView&)` 形式组合 `memory_types` + `status` + `io_types`，成功与 `INVALID_ARGUMENT` 失败路径运行时行为均正确。
- 二进制存在；CTest 日志与 cost data 确认执行；总指挥独立重跑 `1/1 Passed`。
- 文件边界正确，仅新增三个交付文件；工作区内其他未跟踪文件属于 Round 3 其他 session，不计入本 session。
- `git diff --check` 通过；四个文件尾随空白与 EOF newline 均 OK；linter 0 diagnostics。
- 未执行 sudo、模块操作、daemon 或任何硬件 IO。

关于 public-boundary guard 的判定：

worker 报告的 2 行匹配已复核为**子串误报**——`MemoryOwnership` 含子串 `hip`。改用词边界匹配后无任何命中：

```text
grep -nEiw 'cuda|hip|maca|musa|libnvm|nvme|grpc|yaml|iova|rkey|backend_private|prp'
  -> (no word-boundary matches)
```

结论：guard 实质通过，非违规。该误报源于我在 prompt 中给出的 grep 未加词边界，属 prompt 缺陷，不是 worker 问题；后续同类 guard 应使用 `-w`。

非阻塞后续项（记录，不返工）：

`MemoryView` 无 default member initializer，因此 `MemoryView v;`（default-init）下 `address`、`size`、`expected_device_id`、`expected_kind`、`ownership` 为不确定值，仅 `expected_profile` 被默认构造为空。实测编译器给出 `-Wuninitialized` 警告且 `size` 读到垃圾值。因此 result 第 3 节所述“`address` 未设为 `nullptr`、`size` 未设为 `0`”的 sentinel 语义**只在显式聚合初始化下成立**，不是类型自带保证。当前测试始终显式初始化或先赋值后读取，故无 UB，契约本身也未被违反。待 `StorageRuntime::register_memory` 落地时，应决定是补 default member initializer，还是在 Runtime 侧强制要求显式初始化并拒绝不合法 view。

后续决定：T-009 完成，不需要返工。Round 3 Session 1、3、4 可继续。
