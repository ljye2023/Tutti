# TASK T-012

你是一名资深 C++ 头文件边界工程师。你只负责修复一个已确认的**硬编译错误**：`tutti::MemoryKind` 在两个头文件中重复定义。你看不到任何其他上下文，本 prompt 已包含完整现状、根因、正确解法和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 已确认的 BUG（必须修复）

`namespace tutti` 中存在两个同名 `enum class MemoryKind`：

1. `/data/home/ryeqiu/Tutti/tutti/include/tutti/memory_types.h` 第 39 行：

```cpp
enum class MemoryKind {
    HOST,          // ordinary pageable host memory
    PINNED_HOST,   // page-locked host memory
    DEVICE,        // device-local memory
    MANAGED,       // unified/managed memory
};
```

这是**应用公共契约**，已冻结，**禁止修改**。

2. `/data/home/ryeqiu/Tutti/tutti/spi/data_path.h` 第 113 行：

```cpp
enum class MemoryKind {
    HOST,
    DEVICE,
};
```

这是**仓内 SPI 内部类型**，是本次要改的一方。

## 复现方式

```bash
printf '%s\n' '#include <tutti/memory_types.h>' '#include <tutti/spi/data_path.h>' 'int main(){ return 0; }' \
  | /opt/rh/gcc-toolset-13/root/usr/bin/c++ -std=c++17 -DTUTTI_USE_HOST=1 \
    -I/data/home/ryeqiu/Tutti -I/data/home/ryeqiu/Tutti/tutti/include -x c++ -fsyntax-only -
```

当前输出：

```text
error: multiple definition of 'enum class tutti::MemoryKind'
note: previous definition here (memory_types.h:39)
```

## 为什么必须修

未来的 `StorageRuntime::register_memory` 必须**同时**看到：

- 公共入参 `tutti::MemoryView`（来自 `memory_types.h`）
- SPI 出参 `tutti::DataPathMemoryView`（来自 `spi/data_path.h`）

因为它的职责就是把前者降级为后者。因此这条 include 组合是架构上必经路径，当前无法编译。

# 正确解法

`spi/data_path.h` 中的枚举**不得再占用公共名字 `tutti::MemoryKind`**。采用以下方案：

将 `spi/data_path.h` 中的

```cpp
enum class MemoryKind { HOST, DEVICE };
```

重命名为

```cpp
enum class DataPathMemoryKind { HOST, DEVICE };
```

并同步更新 `DataPathMemoryView::kind` 的字段类型与其默认值。

要求与约束：

1. **只改名，不改语义。** 仍然恰好两个枚举值 `HOST` 与 `DEVICE`，顺序不变。SPI 层只关心「host 内存还是 device 内存」，不需要区分 pinned/managed —— 那是公共层 `MemoryKind` 的职责，两者刻意不同，**不要把 SPI 枚举扩成四值去「对齐」公共层**。
2. 不要改 `DataPathMemoryView` 的字段名、字段数量或字段顺序。只改 `kind` 的类型名。
3. 不要新增 `MemoryKind` → `DataPathMemoryKind` 的转换函数。降级映射是未来 Runtime 的职责，不属于 SPI 壳，本任务不实现。
4. 不要为兼容而留 `using MemoryKind = DataPathMemoryKind;` 别名 —— 那会把冲突原样保留。
5. 不要改动 `spi/data_path.h` 中的任何其他类型、方法或注释语义。这是外科手术式改名。

# 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/spi/data_path.h`
- `/data/home/ryeqiu/Tutti/tests/data_path_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/data_path_contract/data_path_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round4/result1.md`

其中 `chat/round4/result1.md` 保存本 session 的完整原始执行结果。

生成的 CMake build/cache 文件只能写入：

`/data/home/ryeqiu/Tutti/build/round4-session1/`

该目录位于既有 ignored `build/` 下，不计入源码文件允许列表。

禁止修改或创建任何其他文件。尤其禁止修改：

- `/data/home/ryeqiu/Tutti/tutti/include/**`（尤其 `memory_types.h`，公共契约已冻结）
- `/data/home/ryeqiu/Tutti/tutti/accel/include/common/memory_kind.h`（另一处历史同名枚举，**不在本任务范围**，不要动）
- `/data/home/ryeqiu/Tutti/tutti/spi/storage_target_resolver.h`
- 根或 `tutti/` 的任何 `CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/.gitignore`
- `/data/home/ryeqiu/Tutti/chat/**` 中除 `chat/round4/result1.md` 外的任何文件
- 任意 Runtime、accelerator、NVMe、libnvm、kernel 文件

禁止提交 Git commit。

# 附带修复（同一文件内，必做）

## 1. contract test 的 CMake 缺 `-Werror`

`tests/data_path_contract/CMakeLists.txt` 当前只有：

```cmake
target_compile_options(tutti_data_path_contract_test PRIVATE
    -Wall -Wextra)
```

改为与兄弟 contract test 一致：

```cmake
    -Wall -Wextra -Werror
```

已确认加上 `-Werror` 后现有测试仍然通过，这是纯一致性修复。

## 2. 新增跨 header 共存回归测试

在 `tests/data_path_contract/data_path_contract_test.cpp` 中新增一个测试块，**在同一翻译单元**同时 include 三个头：

```cpp
#include <tutti/memory_types.h>
#include <tutti/spi/storage_target_resolver.h>
#include <tutti/spi/data_path.h>
```

该测试至少断言：

1. `tutti::MemoryKind`（公共，四值）与 `tutti::DataPathMemoryKind`（SPI，两值）是**不同类型**，用 `static_assert(!std::is_same_v<...>)`；
2. 公共 `MemoryKind` 仍可取到 `PINNED_HOST` 与 `MANAGED`（证明公共契约未被削弱）；
3. `DataPathMemoryView{}.kind` 的类型是 `DataPathMemoryKind`；
4. 三个头共存时可正常构造 `MemoryView`、`ResolvedTarget`（默认空壳即可）和 `DataPathMemoryView`。

注意：这个测试的**真正目的是证明这三个头能同时 include 而不报错**。哪怕断言很简单，只要它编译通过，回归目的就达成了。

这是本轮最重要的交付物——它是防止该冲突复发的唯一机器化保障。

# 依赖限制

`data_path.h` 修改后仍只允许 include：

```cpp
#include <tutti/status.h>
#include <tutti/io_types.h>
```

以及现有的 C++17 标准库头。**不要**为了本次修改新增 `#include <tutti/memory_types.h>`——SPI 头不应依赖公共 memory 契约，两者刻意解耦。

（contract test 作为消费者可以 include `memory_types.h`，这是允许且必要的。）

明确禁止在 `data_path.h` 中 include 或提及：

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

也不要出现 PRP、SGL、LBA、CID、doorbell、fd、extent 等私有实现名词。

# 安全限制

绝对禁止：

```text
sudo
insmod
rmmod
modprobe
```

也禁止启动 daemon/client、访问 `/dev/nvme*`、执行任何硬件 IO。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 修复前先复现（证明 BUG 真实存在）

```bash
printf '%s\n' '#include <tutti/memory_types.h>' '#include <tutti/spi/data_path.h>' 'int main(){ return 0; }' \
  | /opt/rh/gcc-toolset-13/root/usr/bin/c++ -std=c++17 -DTUTTI_USE_HOST=1 \
    -I/data/home/ryeqiu/Tutti -I/data/home/ryeqiu/Tutti/tutti/include -x c++ -fsyntax-only -
```

在结果文件中记录修复前的真实错误输出。

## 2. 修复后验证冲突消失

同样命令必须**编译通过**。

再验证三头共存：

```bash
printf '%s\n' '#include <tutti/memory_types.h>' '#include <tutti/spi/storage_target_resolver.h>' '#include <tutti/spi/data_path.h>' 'int main(){ return 0; }' \
  | /opt/rh/gcc-toolset-13/root/usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -DTUTTI_USE_HOST=1 \
    -I/data/home/ryeqiu/Tutti -I/data/home/ryeqiu/Tutti/tutti/include -x c++ -fsyntax-only -
```

必须通过。

## 3. 确认公共契约未被改动

```bash
git diff --stat -- tutti/include/
```

必须为空输出（`memory_types.h` 一个字节都不能改）。

## 4. Standalone configure / build / ctest

```bash
rm -rf /data/home/ryeqiu/Tutti/build/round4-session1

cmake -S /data/home/ryeqiu/Tutti/tests/data_path_contract \
  -B /data/home/ryeqiu/Tutti/build/round4-session1 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build /data/home/ryeqiu/Tutti/build/round4-session1 \
  --target tutti_data_path_contract_test -j8

ctest --test-dir /data/home/ryeqiu/Tutti/build/round4-session1 \
  --output-on-failure \
  -R '^tutti_data_path_contract_test$'
```

要求 1/1 PASS，且 build 在 `-Werror` 下零告警。

## 5. 确认残留旧名已清除

```bash
grep -n 'MemoryKind' /data/home/ryeqiu/Tutti/tutti/spi/data_path.h
```

输出中**不得**出现独立的 `enum class MemoryKind`；只应看到 `DataPathMemoryKind`。

## 6. Public-boundary guard

```bash
grep -nEiw 'cuda|hip|maca|musa|libnvm|nvme|fiemap|grpc|yaml|prp|sgl|lba|cid|doorbell|fd|extent' \
  /data/home/ryeqiu/Tutti/tutti/spi/data_path.h
```

注意本轮 guard 使用 `-w`（词边界），避免子串误报。允许命中的唯一情形是注释中描述「SPI 不得包含什么」的说明性文字；如有命中必须在结果中逐条说明是注释还是真实类型。

## 7. Hygiene

```bash
git diff --check -- tutti/spi/data_path.h
```

对所有改动文件额外检查尾随空白和 EOF newline。确认本 session 只触碰允许列表。

# 成功标准

只有同时满足以下条件才能报告 `PASS`：

1. 修复前的冲突已被真实复现并记录；
2. `memory_types.h` + `spi/data_path.h` 现在可同 TU 编译；
3. 三头（`memory_types.h` + `storage_target_resolver.h` + `data_path.h`）共存编译通过；
4. `tutti/include/**` 零改动；
5. SPI 枚举改名为 `DataPathMemoryKind`，仍恰好两值，语义未变；
6. 未新增兼容别名，未新增 `memory_types.h` 依赖，未新增转换函数；
7. contract test 新增三头共存回归测试并通过；
8. `-Werror` 下零告警，ctest 1/1 PASS；
9. 未修改允许列表外文件；
10. 未执行任何模块、daemon 或 IO 操作；
11. 空白检查通过。

如果你发现改名会引起本 prompt 未预料的连带破坏，**停下来**，在结果中记录具体冲突点并报告 `BLOCKED`，不要擅自扩大修改范围去「顺手修好」。

# 结果落盘要求

完成任务和验收后，必须把完整原始结果写入：

`/data/home/ryeqiu/Tutti/chat/round4/result1.md`

至少包含：

1. 修改文件列表
2. 修复前的真实冲突错误输出
3. 改名前后的精确 diff 说明（枚举与字段类型）
4. 修复后两头 / 三头共存编译结果
5. `git diff --stat -- tutti/include/` 结果（须为空）
6. 新增回归测试的内容说明与断言清单
7. configure / build / ctest 结果（含 `-Werror`）
8. 残留旧名检查结果
9. public-boundary guard 结果
10. 文件边界与空白检查结果
11. 最终 `PASS` 或 `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 如果命令失败，写入真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加到文件末尾。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round4/result1.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
