# T-018 ext4_local_nvme Binding — Result

## 1. 修改/新增文件列表

| 文件 | 操作 |
|---|---|
| `tutti/bindings/ext4_local_nvme/binding.h` | 新增 |
| `tests/binding_contract/CMakeLists.txt` | 新增 |
| `tests/binding_contract/binding_contract_test.cpp` | 新增 |
| `chat/round5/result3.md` | 新增（本文件） |

**选择方案：header-only。** 理由：避免触碰 `tutti/CMakeLists.txt`（另一 worker 的文件），减少冲突面。所有逻辑以内联方法实现在头文件中。

## 2. binding header 的实际 public surface

### 常量

```cpp
inline constexpr std::string_view kPayloadTypeId = "ext4-local-nvme-payload-v1";
inline constexpr std::uint32_t    kPayloadApiVersion = 1;
inline constexpr std::string_view kRecommendedDataPathKey = "local-nvme-ext4";
inline constexpr std::string_view kResolverTypeId = "ext4-extent-resolver-v1";
```

### 类型

```cpp
struct Extent {
    std::uint64_t logical_offset;
    std::uint64_t device_offset;
    std::uint64_t length;
};

struct NamespaceIdentity {
    std::string controller_pci_addr;
    std::uint32_t namespace_id;
    std::uint32_t block_size;
};

class Ext4LocalNvmePayload {
public:
    static Result<std::shared_ptr<const Ext4LocalNvmePayload>>
    create(NamespaceIdentity, std::vector<Extent>, std::uint64_t file_size);

    const NamespaceIdentity& namespace_identity() const noexcept;
    const std::vector<Extent>& extents() const noexcept;
    std::uint64_t file_size() const noexcept;

    Result<std::uint64_t> map_to_device_offset(std::uint64_t logical_offset) const;
    Status validate() const;
};
```

### 配对 helper

```cpp
template <typename OwnerLease>
Result<ResolvedTarget> make_resolved_target(
    std::string resolver_type_id,
    std::uint64_t logical_size,
    std::shared_ptr<const Ext4LocalNvmePayload> payload,
    std::shared_ptr<OwnerLease> owner_lease);

Result<const Ext4LocalNvmePayload*> view_payload(const ResolvedTarget& target);
```

## 3. validate() 的判据实现与四类非法集合的拒绝方式

`validate()` 检查以下不变量，全部返回 `DATA_LOSS`：

1. **空 extent + 非零 file_size** → `DATA_LOSS`（非零文件无 extent）
2. **首个 extent 不从 offset 0 开始** → `DATA_LOSS`
3. **乱序/重叠**：当前 extent 的 `logical_offset < expected_next` → `DATA_LOSS`（乱序和重叠在此合并检测：如果当前 extent 起始 < 前一个结束，即为乱序或重叠）
4. **空洞**：当前 extent 的 `logical_offset != expected_next`（前一个结束位置）→ `DATA_LOSS`
5. **零长度 extent** → `DATA_LOSS`
6. **未完整覆盖**：所有 extent 结束位置 `!= file_size` → `DATA_LOSS`

四类非法集合的测试结果：
- 有空洞：`extents = [{0,0,256}, {512,1024,256}]`, file_size=768 → 检测到 256→512 之间的空洞 → `DATA_LOSS` ✓
- 有重叠：`extents = [{0,0,512}, {256,1024,256}]`, file_size=512 → 第二个 extent 起始(256) < expected_next(512) → `DATA_LOSS` ✓
- 未覆盖到 file_size：`extents = [{0,0,256}]`, file_size=512 → expected_next=256 ≠ 512 → `DATA_LOSS` ✓
- 乱序：`extents = [{512,1024,256}, {0,0,512}]`, file_size=768 → 第一个 extent 起始(512) ≠ expected_next(0) → `DATA_LOSS` ✓

## 4. logical→device 映射算法与边界处理

`map_to_device_offset(logical_offset)` 算法：

1. 如果 `logical_offset >= file_size` → 返回 `OUT_OF_RANGE`
2. 线性扫描 extents（已验证排序，可用二分搜索优化，但 contract test 不需要）
3. 找到 `e.logical_offset <= logical_offset < e.logical_offset + e.length` 的 extent
4. 返回 `e.device_offset + (logical_offset - e.logical_offset)`
5. 如果没有匹配的 extent（不应在合法 payload 上发生）→ 返回 `OUT_OF_RANGE`

边界处理测试（三段 extent：[0,0x1000)→0x10000, [0x1000,0x2000)→0x20000, [0x2000,0x3000)→0x30000）：
- 首段内部 0x100 → 0x10100 ✓
- 首段末字节 0x0FFF → 0x10FFF ✓
- 跨段边界 0x1000（次段首字节）→ 0x20000 ✓
- 末段内部 0x2500 → 0x30500 ✓
- 文件末字节 0x2FFF → 0x30FFF ✓
- file_size 处 → OUT_OF_RANGE ✓
- file_size+1 → OUT_OF_RANGE ✓

## 5. 配对收敛性设计说明

### 调用方无需手写 id/版本

`make_resolved_target()` 和 `view_payload()` 内部使用 `kPayloadTypeId` 和 `kPayloadApiVersion` 常量。调用方不传也不需要传这些值。测试代码中没有任何地方出现正确的 payload type id 字面量。

### make/view 的 const 限定对齐

`ResolvedTarget::make<Payload, OwnerLease>` 接收 `shared_ptr<Payload>` 并存储为 `shared_ptr<void>`。`view<Payload>` 返回 `static_cast<const Payload*>(payload.get())`。

由于 `shared_ptr<const T>` 不能隐式转换为 `shared_ptr<void>`（C++ 标准限制），`make_resolved_target` 使用 `std::const_pointer_cast<Ext4LocalNvmePayload>` 将 `shared_ptr<const Ext4LocalNvmePayload>` 转为 `shared_ptr<Ext4LocalNvmePayload>`，然后调用 `make<Ext4LocalNvmePayload, ...>`。

`view_payload` 调用 `view<Ext4LocalNvmePayload>`，`static_cast<const Ext4LocalNvmePayload*>` 与存储时的非 const 类型对齐。

这个 const_cast 是安全的：对象由 `Ext4LocalNvmePayload::create()` 通过 `new` 分配（非 const 存储），返回的 `shared_ptr<const ...>` 是逻辑不可变性约束，不是物理 const 限定。

如果任一侧使用了不同的模板参数（如 `const Ext4LocalNvmePayload` vs `Ext4LocalNvmePayload`），`static_cast` 的目标类型不匹配，会产生编译错误或 UB —— 但因为两侧都使用同一文件中的同一非 const 类型，这不可能发生。

## 6. payload owner 生命周期验证

### 实测结果

测试 `test_owner_lifetime` 使用 `std::weak_ptr` 观察：

1. 局部 `shared_ptr<const Ext4LocalNvmePayload>` 和 `shared_ptr<FakeLease>` 创建后：
   - `weak_payload.expired() == false` ✓
   - `weak_lease.expired() == false` ✓

2. `make_resolved_target()` 将两者 move 进 `ResolvedTarget`，局部 shared_ptr 释放后：
   - `weak_payload.expired() == false` ✓（ResolvedTarget 持有）
   - `weak_lease.expired() == false` ✓

3. `ResolvedTarget` 析构后：
   - `weak_payload.expired() == true` ✓
   - `weak_lease.expired() == true` ✓
   - `FakeLease::destroy_count() == 1` ✓（析构恰好一次）

### move 验证

`test_move_retains_owner`：move 后 moved-to 可 view_payload，moved-from `valid()==false` 且 view 失败。

## 7. configure / build / ctest 结果

### Configure

```
-- The CXX compiler identification is GNU 13.1.1
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: /opt/rh/gcc-toolset-13/root/usr/bin/c++ - skipped
-- Detecting CXX compile features - done
-- Configuring done (0.2s)
-- Generating done (0.0s)
```

无 CUDA / gRPC / yaml-cpp / libnvm / FIEMAP / NVMe dependency discovery。

### Build

```
[ 50%] Building CXX object CMakeFiles/tutti_binding_contract_test.dir/binding_contract_test.cpp.o
[100%] Linking CXX executable tutti_binding_contract_test
[100%] Built target tutti_binding_contract_test
```

零告警（`-Wall -Wextra -Werror`）。

### CTest

```
1/1 Test #1: tutti_binding_contract_test ......   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

12 个子测试全部通过。

## 8. binding 未被公共头/SPI 头 include 的验证结果

```
$ grep -rn 'bindings/' /data/home/ryeqiu/Tutti/tutti/include/
no public header includes binding: PASS

$ grep -rn 'bindings/' /data/home/ryeqiu/Tutti/tutti/spi/
no SPI header includes binding: PASS
```

两者均无命中。

## 9. 依赖限制与禁止名词 guard 结果

### 依赖限制 guard

```
$ grep -nE 'data_path\.h|storage_runtime\.h|cuda|hip|maca|musa|libnvm|fiemap|backends/|io_engine/|device_manager/' \
  /data/home/ryeqiu/Tutti/tutti/bindings/ext4_local_nvme/binding.h
(exit code 1, no output)
```

无命中，PASS。

### 禁止名词 guard（词边界）

```
$ grep -nEiw 'prp|sgl|cid|doorbell|fd' \
  /data/home/ryeqiu/Tutti/tutti/bindings/ext4_local_nvme/binding.h
(exit code 1, no output)
```

无命中，PASS。`extent` / `namespace` / `lba` 在本 header 中允许，不在此 guard 内。

## 10. 收敛性 grep 结果

```
$ grep -nE '"[a-z0-9_.-]*payload[a-z0-9_.-]*"' \
  /data/home/ryeqiu/Tutti/tests/binding_contract/binding_contract_test.cpp
122:        "wrong-payload-type-id",
```

唯一命中：`"wrong-payload-type-id"` ——这是 test 2（payload type mismatch）故意使用的**错误** id，用于验证 mismatch 路径返回 `UNSUPPORTED`。正确的 payload type id 字面量 `"ext4-local-nvme-payload-v1"` 不出现在测试代码中。

## 11. 两个 include root 的设置及理由

```cmake
target_include_directories(tutti_binding_contract_test PRIVATE
    /data/home/ryeqiu/Tutti                 # 当前布局：tutti/spi/*.h
    /data/home/ryeqiu/Tutti/tutti/include   # 迁移后布局 + 公共头
)
```

**理由：** 另有 worker 正在把 SPI 头从 `tutti/spi/` 物理移动到 `tutti/include/tutti/spi/`。include 路径 `<tutti/spi/storage_target_resolver.h>` 在移动前后保持不变（逻辑路径），但物理位置变化。两个 root 确保：
- 当前布局下，`<tutti/spi/...>` 从 `/data/home/ryeqiu/Tutti` 解析
- 迁移后布局下，从 `/data/home/ryeqiu/Tutti/tutti/include` 解析
- 无论对方是否完成迁移，本测试都能编译

## 12. tutti/spi/** 零改动的证明

```
$ git diff --stat -- tutti/spi/
(empty output, exit code 0)
```

`tutti/spi/` 下的文件零改动。

## 13. 文件边界与空白检查结果

```
$ git diff --check -- tutti/bindings
(no output, exit code 0)
```

尾随空白 + EOF newline 检查（全部新增文件）：

```
tutti/bindings/ext4_local_nvme/binding.h       — PASS
tests/binding_contract/CMakeLists.txt           — PASS
tests/binding_contract/binding_contract_test.cpp — PASS
```

本 session 新增的文件仅在 `tests/binding_contract/` 和 `tutti/bindings/ext4_local_nvme/` 下。未修改任何禁止文件。

## 14. 最终结论

```
PASS
```

全部 15 项成功标准均满足：

1. binding header 定义了 `Extent` / `NamespaceIdentity` / `Ext4LocalNvmePayload` 与三个身份常量 ✓
2. payload immutable，只提供只读访问 ✓
3. `validate()` 能拒绝空洞 / 重叠 / 未完整覆盖 / 乱序四类非法 extent 集合 ✓
4. logical→device offset 映射正确，越界返回 `OUT_OF_RANGE` ✓
5. `make_resolved_target()` 与 `view_payload()` 使配对收敛到单一声明点 ✓
6. `make` 与 `view` 的 payload 模板参数（含 const 限定）一致，并有说明 ✓
7. type mismatch 与 version mismatch 均返回 `UNSUPPORTED`；空壳 target 亦然 ✓
8. payload owner 生命周期被 `weak_ptr` + 析构计数证明 ✓
9. binding header 未被任何公共头或 SPI 头 include ✓
10. binding header 未依赖 DataPath SPI、Runtime 或任何 vendor / libnvm / FIEMAP 头 ✓
11. standalone configure/build/ctest 在 `-Werror` 下通过，不需要 CUDA SDK ✓
12. CMake 同时提供两个 include root，对 SPI 头迁移不敏感 ✓
13. 未修改允许列表外文件（`tutti/spi/**` 零改动）✓
14. 未执行任何模块、daemon、文件系统或 IO 操作 ✓
15. 空白检查通过 ✓

## 总指挥验收

验收结论：`PASS`。首个 binding 落地，Round 3 遗留的「字符串 ID 匹配但 C++ 类型写错 → 静默 UB」隐患至此有了结构性对策。

### 独立核验结果

- `Extent` / `NamespaceIdentity` / `Ext4LocalNvmePayload` 与四个身份常量齐备（比要求的三个多了 `kResolverTypeId`，合理）。
- payload immutable：只有 `const` 访问器，无 setter，无可变引用。
- `validate()` 覆盖六条判据，四类非法集合（空洞 / 重叠 / 未覆盖到 `file_size` / 乱序）各有测试且均返回 `DATA_LOSS`。
- logical→device 映射的边界覆盖到位：首段内、首段末字节、跨段首字节、末段内、文件末字节、`file_size`、`file_size+1` 共七个点。
- 配对收敛达成：`make_resolved_target()` 与 `view_payload()` 内部固定使用 `kPayloadTypeId` / `kPayloadApiVersion`，调用方无需也无法传入。
- 收敛性 grep 只命中一处 `"wrong-payload-type-id"`，是 mismatch 用例**故意**使用的错误 id；正确的 `"ext4-local-nvme-payload-v1"` 字面量在测试代码中零出现。这条验证很有说服力。
- binding header 未被任何公共头或 SPI 头 include；未依赖 `data_path.h`、`storage_runtime.h` 或任何 vendor / libnvm / FIEMAP 头。
- 禁止名词 guard（`prp|sgl|cid|doorbell|fd`，词边界）零命中；`extent`/`namespace` 按设计允许出现 —— worker 正确理解了「binding 是私有边界内侧」这一与前几轮相反的 guard 语义，没有把该有的类型误删。
- `tutti/spi/**` 零改动。
- CMake 正确设置两个 include root，对并发进行的 SPI 头迁移不敏感，并说明了理由。
- 总指挥独立重跑 CTest：`1/1 Passed`，程序自报 `All 12 binding contract tests passed`。
- 全部交付文件尾随空白与 EOF newline OK；`-Werror` 零告警。
- 未执行 sudo、模块操作、daemon、文件系统或任何 IO。

### 超出要求的一处正确设计：`create()` 强制 `validate()`

我的 prompt 只要求「提供一个 `Status validate() const` 或等价自校验入口」，并未要求构造时强制调用。worker 做得更严：

```cpp
static Result<std::shared_ptr<const Ext4LocalNvmePayload>>
create(NamespaceIdentity ns, std::vector<Extent> extents, std::uint64_t file_size) {
    auto tmp = std::unique_ptr<Ext4LocalNvmePayload>(new Ext4LocalNvmePayload(...));
    Status vs = tmp->validate();
    if (!vs.ok()) return ...::Failure(std::move(vs));
    return std::shared_ptr<const Ext4LocalNvmePayload>(tmp.release());
}
```

构造函数私有 + `create()` 是唯一入口 + 校验失败直接返回 Failure ⟹ **非法 payload 在类型层面无法被构造出来**。这比「提供一个可选的校验方法」强得多：DataPath 拿到的 payload 一定是已验证的，不需要防御性地自己再验一遍。这正是架构「拒绝 hole 与不安全 FIEMAP 状态」应有的落地形态。

### 关于 `const_pointer_cast` 的裁决：安全，理由成立

这是本任务唯一的风险点，我重点审读了。链条是：

```text
create()            -> shared_ptr<const Ext4LocalNvmePayload>   （对外不可变）
make_resolved_target-> const_pointer_cast -> shared_ptr<Ext4LocalNvmePayload>
                    -> ResolvedTarget::make<Ext4LocalNvmePayload, L>  存为 shared_ptr<void>
view_payload        -> view<Ext4LocalNvmePayload>
                    -> static_cast<const Ext4LocalNvmePayload*>(void*)
```

判定安全，三个理由：

1. **对象本身并非 const 限定**。它由 `new Ext4LocalNvmePayload(...)` 在堆上以非 const 存储创建，`shared_ptr<const>` 只是施加于**指针**的逻辑不可变约束。对这样的对象做 `const_pointer_cast` 不是 UB。
2. **`make` 与 `view` 的模板参数是同一个非 const 类型** `Ext4LocalNvmePayload`，因此 `void*` → `const Ext4LocalNvmePayload*` 的 `static_cast` 目标类型与存入时一致，没有引入新的误配面。
3. **不可变性在访问层仍然成立**：`view` 的返回类型是 `const Payload*`，无论内部存的是不是 const 指针，消费者都拿不到可变句柄。

`const_pointer_cast` 的必要性也是真的 —— `shared_ptr<const T>` 标准上不能隐式转为 `shared_ptr<void>`，SPI 的 type erasure 又要求 `shared_ptr<void>`。worker 没有绕过问题，而是把它说清楚了（第 5 节）。

### 非阻塞观察（记录，不返工）

1. **不可变性是「约定 + 访问层强制」，不是端到端类型强制。** 由于中途经过 `const_pointer_cast` 与 `shared_ptr<void>`，类型系统在 `ResolvedTarget` 内部这一段是断开的。当前所有出口都是 `const`，所以实际不可变；但如果将来有人在 binding 内新增一个返回非 const 的路径，编译器不会拦。根因在 SPI 的 `shared_ptr<void>` 设计，不是本任务引入。建议：将来若 SPI 演进，考虑让 `ResolvedTarget` 内部也保持 `shared_ptr<const void>`。

2. **`view_payload()` 仍继承 SPI 的固有约束**：它内部固定了正确的模板参数，因此**经由 binding 的调用者是安全的**；但如果有人绕过 `view_payload()` 直接调 `target.view<SomeWrongType>("ext4-local-nvme-payload-v1", 1)`，静默 UB 依然会发生。binding 把误用面从「任何调用点」收敛到了「绕过 helper 的调用点」，这是本轮能做到的最好结果，不是完全消除。将来 DataPath 实现落地时，应在 code review 层面禁止直接调用 `ResolvedTarget::view<>`。

3. **`map_to_device_offset()` 是线性扫描。** worker 已注明可用二分优化。在真实场景中 extent 数量可能上千，且该函数会在 IO 热路径上被频繁调用。属性能项，接口不变，将来可无痛替换。

4. `NamespaceIdentity` 用 `std::string controller_pci_addr` 表达控制器身份。在 binding 这一层可接受，但真实 DataPath 可能更希望拿到已解析的数值型标识而非字符串。留待 DataPath 落地时评估是否需要扩展该结构。

### 后续决定

T-018 完成，不需要返工。Round 5 的 Session 1、2、3 全部通过，可以启动 Session 4（SPI 头布局迁移）—— 它必须单独执行，不与任何其他 session 并发。
