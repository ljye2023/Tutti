# TASK T-022

你是一名资深 C++ 工程师。你的任务是**分层抽象 + 代码搬运**：把 Tutti 已有的 FIEMAP extent 采集逻辑，搬到新的 SPI 层上，产出符合 `StorageTargetResolver` 契约的 `ResolvedTarget`，并在真实设备上跑通。

你看不到任何其他上下文，本 prompt 已包含全部需要的接口、源码位置、环境信息和验收标准。

# 任务定位（先读这条，它决定你的所有判断）

**你在做搬运和抽象，不是做设计评审。**

- 已有代码 `nvme_storage/src/fiemap_helper.cpp` 是**已在生产跑通的参考实现**。你的工作是把它的逻辑搬到新层，让它在新的 SPI 结构上跑通。
- **不要评判已有代码的对错。** 不要因为你认为某个策略「不安全」就改它。原样搬运其行为。
- **不要修改 `nvme_storage/**` 的任何文件。** 即使你发现问题，也不要改，也不要在结果里长篇论证它该怎么改。如需记录，一两句话点到即止。
- 唯一允许的偏离是**机械性补齐**：新的数据结构需要的字段，若旧返回类型没带出来，就在搬运时从 FIEMAP 原始数据里一并取出（详见第 4 节）。这是搬运的必要动作，不是「修 bug」。

判断标准：如果一个改动是为了「让旧逻辑适配新接口」，做。如果是为了「让旧逻辑变得更好」，不做。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 执行时机

**本任务必须单独执行，不与任何其他 session 并发。** 它依赖独占的 NVMe 挂载环境。

开始前确认无并发构建：

```bash
ps -eo pid,etime,cmd | grep -E '[c]make|[c]test' | head
```

# 1. 硬件环境（由项目负责人手工准备，你不要动）

## 环境已由负责人挂好

项目负责人会在你开始前手工完成：

1. 启动 `sudo ./build/bin/tutti_daemon --config sys_config.yaml`（保持前台运行，它 bind PCI 并创建 snvme 块设备）
2. `mkfs.ext4` + `mount /dev/snvme0n1 /mnt/nvme1`
3. 创建可写测试目录 `/mnt/nvme1/GPU0/resolver_test`（已 `chmod 777`）

已实测确认的环境（2026-07-30）：

```text
块设备      /dev/snvme0n1   (PCI 0000:08:00.0)   5.8T
挂载        /mnt/nvme1  <-  /dev/snvme0n1   ext4  rw,relatime
容量        可用约 5.5 TiB
fs 参数     4 KiB block
测试目录    /mnt/nvme1/GPU0/resolver_test   (777)
```

## 你的硬件职责边界

**你只在已挂载的目录里创建普通文件。** 具体：

- **禁止** bind / unbind 任何 PCI 设备
- **禁止** mkfs / mount / umount
- **禁止** 启动或停止 daemon
- **禁止** `insmod` / `rmmod` / `modprobe`
- **禁止** 打开任何块设备节点（`/dev/snvme*`、`/dev/nvme*`），**包括只读**
- **禁止** 触碰 `/dev/md0`、`/mnt/nvme4`（生产 RAID 数据）
- **禁止** 在 `/data` 上做 FIEMAP 测试（那是代码仓库所在的系统盘，与目标 NVMe 无关，采到的偏移无意义）

允许的操作：在测试目录下 `open` / `fallocate` / `pwrite` / `fsync` / `ftruncate` / `ioctl(FS_IOC_FIEMAP)` / `close` / `unlink`，以及只读运行 `filefrag`、`findmnt`、`df`、`stat`。

**开始前先自检环境就绪：**

```bash
findmnt /mnt/nvme1
test -w /mnt/nvme1/GPU0/resolver_test && echo 'test dir writable: OK'
df -T /mnt/nvme1
```

若挂载点不存在或不可写，**立即报告 `BLOCKED`** 并说明「需负责人先挂载环境」。**不要**试图自己挂载。

测试数据合计不超过 2 GiB，结束时清理自己创建的文件（但**不要** `rmdir` 测试目录本身，那是负责人建的）。

# 2. 要搬运的参考实现

`nvme_storage/src/fiemap_helper.cpp`（187 行，**必读全文**）+ `nvme_storage/include/fiemap_helper.h`。

其核心是 `read_extents(int fd, uint32_t nvme_block_size_bytes)`，行为要点（原样搬运，不要改）：

- 用 `fstat().st_blksize` 取 fs block size；校验 `fs_block_size % nvme_block_size == 0`，不满足报错
- 调 ioctl 前先 `fsync(fd)`；`fm_flags = FIEMAP_FLAG_SYNC`
- 单轮缓冲 `kFiemapMaxExtentsPerCall = 256` 个 extent
- 拒绝掩码（原样照抄）：

  ```c
  FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED |
  FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED |
  FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_DATA_TAIL
  ```

- **`FIEMAP_EXTENT_UNWRITTEN` 刻意不在拒绝掩码中**（`fiemap_helper.cpp:111-119` 有详细注释说明理由：fallocate 出来的块就是 UNWRITTEN，物理 LBA 已分配且稳定）。**原样保留这个行为。**
- 对 `fe_physical` 与 `fe_length` 双双做 `% nvme_block_size` 对齐检查
- 多轮循环：以 `fe_logical + fe_length` 为下一轮游标，见到 `FIEMAP_EXTENT_LAST` 结束；某轮返回数少于请求数且无 LAST 时 `break`
- 总量上限 `kNvmeFileHeaderMaxExtents = 124`（定义在 `nvme_storage/include/nvme_file_header.h`）
- 0 extent 时报错（`"fiemap returned 0 extents (file is empty / sparse?)"`）

**照抄这些行为。** 如果你觉得某处可以改进，忍住，原样搬。

## 生产调用现场（参考文件准备方式）

`nvme_storage/src/host_fs_backed_nvme_storage.cpp` 的 `create_file`（约 635-700 行）：

```text
open(O_CREAT|O_RDWR) -> fallocate(size) -> fsync
  -> read_extents(fd, kNvmeBlockSize)       // kNvmeBlockSize = 4096 (.cpp:33)
  -> linkat(<mount>/.tutti/.refs/<name>.bin)  // 硬链接，防外部 rm 释放 inode
  -> close(fd)
```

注意它只 `fallocate` 就采集（不写数据），这与 UNWRITTEN 被接受是一致的。**你的测试也应能覆盖这种「只 fallocate」的文件**，因为这是生产的真实用法。

`kNvmeBlockSize = 4096`，测试用这个值。

# 3. 目标 SPI 层（已冻结，不得修改）

## `StorageTargetResolver` 抽象

`tutti/include/tutti/spi/storage_target_resolver.h`：

```cpp
struct ResolveOptions {
    std::string scheme;
};

class StorageTargetResolver {
public:
    virtual ~StorageTargetResolver() = default;
    virtual Result<ResolvedTarget> resolve(
        std::string_view uri,
        const ResolveOptions& options) = 0;
};
```

`ResolvedTarget` 是 move-only，payload 与 lease 以 `shared_ptr<void>` 做 type erasure。

## binding（必须通过它产出，不得绕过）

`tutti/bindings/ext4_local_nvme/binding.h`，命名空间 `tutti::binding::ext4_local_nvme`：

```cpp
inline constexpr std::string_view kPayloadTypeId;            // "ext4-local-nvme-payload-v1"
inline constexpr std::uint32_t    kPayloadApiVersion = 1;
inline constexpr std::string_view kResolverTypeId;
inline constexpr std::string_view kRecommendedDataPathKey;

struct Extent {
    std::uint64_t logical_offset = 0;      // 字节
    std::uint64_t device_offset  = 0;      // 字节
    std::uint64_t length         = 0;      // 字节
};

struct NamespaceIdentity {
    std::string   controller_pci_addr;     // e.g. "0000:08:00.0"
    std::uint32_t namespace_id = 1;
    std::uint32_t block_size   = 0;        // NVMe 逻辑块大小（字节）
};

class Ext4LocalNvmePayload {
public:
    static Result<std::shared_ptr<const Ext4LocalNvmePayload>>
    create(NamespaceIdentity ns, std::vector<Extent> extents, std::uint64_t file_size);
    Result<std::uint64_t> map_to_device_offset(std::uint64_t logical_offset) const;
    // ... const 访问器
};

template <typename OwnerLease>
inline Result<ResolvedTarget> make_resolved_target(
    std::string resolver_type_id,
    std::uint64_t logical_size,
    std::shared_ptr<const Ext4LocalNvmePayload> payload,
    std::shared_ptr<OwnerLease> owner_lease);

inline Result<const Ext4LocalNvmePayload*> view_payload(const ResolvedTarget& target);
```

约束：

- `create()` 是唯一构造入口，内部强制 `validate()`（检测空洞、重叠、乱序、未覆盖 `[0, file_size)`，失败返 `DATA_LOSS`）。**依赖它，不要自己再写一套校验。**
- 产出 `ResolvedTarget` **必须**通过 `make_resolved_target()`，**不得**直接调 `ResolvedTarget::make<>()`。
- **不得**在 resolver 中出现 `kPayloadTypeId` / `kPayloadApiVersion` 的字面量或显式传参。

## Status / Result

`tutti/include/tutti/status.h`：`StatusCode` 有 `OK, INVALID_ARGUMENT, OUT_OF_RANGE, NOT_FOUND, UNSUPPORTED, NOT_READY, BUSY, RESOURCE_EXHAUSTED, TIMEOUT, DEVICE_ERROR, DATA_LOSS, INTERNAL`。`Result<T>` 提供 `Success(T)` / `Failure(Status)` / `ok()` / `has_value()` / `value()`。

# 4. 搬运时必须做的两处机械转换

## 转换 A：block 单位 → 字节单位

旧返回类型 `LbaExtent`（`nvme_storage/include/lba_extent.h`）以 **NVMe block** 为单位：

```cpp
struct LbaExtent { uint64_t start_lba; uint64_t length_blocks; };
```

binding 的 `Extent` 是**字节**语义。转换关系（旧代码 `fiemap_helper.cpp:151-152` 的逆向）：

```text
device_offset = start_lba     * block_size      // 或直接用 fe_physical
length        = length_blocks * block_size      // 或直接用 fe_length
```

因为 FIEMAP 原始字段 `fe_physical` / `fe_length` 本来就是字节，**直接用它们即可，不必先转 block 再转回来。**

## 转换 B：补齐 `logical_offset`

`LbaExtent` **没有** logical 字段，而 binding 的 `Extent` 需要 `logical_offset`。

旧代码在 `fiemap_helper.cpp:166` 已经读了 `ex.fe_logical`（用作循环游标），只是没存进返回值。**搬运时把它一起取出来即可**：

```text
logical_offset = ex.fe_logical      // FIEMAP 原始字段，字节单位
```

**这是搬运的机械补齐，不是行为改动。** 不要用「累加前一个 extent 的 length」来推算 —— 直接用 `fe_logical`，那是内核给的权威值。

# 5. 设计要求

## 类的形状

在 `tutti/resolvers/local_file/resolver.h`（可选配 `.cpp`）中定义具体类（建议名 `LocalFileResolver`）：

- 公开继承 `tutti::StorageTargetResolver`，`override resolve()`；
- **不要**添加 `resolve()` 之外的公共虚方法（SPI 只有 `resolve`，别扩大抽象面）；
- `NamespaceIdentity` 的三个字段由**构造参数**注入，`resolve()` **不探测硬件** —— resolver 只做文件系统侧解析，设备身份由上层配置提供；
- `resolve()` 校验 `options.scheme`，不匹配返回 `UNSUPPORTED`。scheme 字符串定义为常量（例如 `"file"`），不要散落字面量；
- `uri` 解析规则由你确定（最简可为「scheme 前缀 + 绝对路径」），明确记录，畸形 uri 返回 `INVALID_ARGUMENT`。

## fd 生命周期与 owner lease

`ResolvedTarget` 的第二个 type-erased 槽位是 owner lease —— 「只要 `ResolvedTarget` 存活，资源就不被释放」。

本 resolver 的关键资源是打开的 fd。要求：

- 设计一个 lease 类型持有 fd，析构 `close()`；
- 通过 `make_resolved_target()` 的 `owner_lease` 参数交给 `ResolvedTarget`；
- **必须测出**：`ResolvedTarget` 存活期间 fd 有效；析构后 fd 已关闭（用 `fcntl(fd, F_GETFD)` 返回 `-1` / `errno == EBADF` 判定）；
- lease 必须 move-safe：`ResolvedTarget` 是 move-only，move 后不得双重 close。**必须有测试覆盖。**

参考：生产路径用 `.refs/` 硬链接达到类似目的（`nvme_file.h` 有说明）。在结果中用两三句话对比一下持有 fd 与硬链接各自能保证什么即可，不需要长篇分析。

## 错误处理

- 每个失败路径返回**具体** `StatusCode`，不要一律 `INTERNAL`；
- 错误消息含足以定位的信息（哪个 extent、什么 flag、什么偏移）；
- `ioctl` 失败时把 `errno` 与 `strerror` 纳入消息，且**先把 `errno` 存入局部变量**再使用。

`StatusCode` 映射建议（自行决定并说明）：坏 flag → `DATA_LOSS` 或 `UNSUPPORTED`；对齐失败 → `INVALID_ARGUMENT` 或 `DATA_LOSS`；extent 过多 → `RESOURCE_EXHAUSTED`；ioctl 失败 → `DEVICE_ERROR` 或 `INTERNAL`；文件不存在 → `NOT_FOUND`。

## 依赖约束

`resolver.h` / `resolver.cpp` 只允许 include：

- `<tutti/status.h>`、`<tutti/spi/storage_target_resolver.h>`
- `tutti/bindings/ext4_local_nvme/binding.h`
- Linux 系统头（`<linux/fiemap.h>`、`<linux/fs.h>`、`<sys/ioctl.h>`、`<sys/stat.h>`、`<fcntl.h>`、`<unistd.h>`、`<cerrno>`、`<cstring>` 等）
- C++ 标准库头

**禁止** include：CUDA / HIP 或任何 vendor SDK；libnvm 任何头；`nvme_storage/**`（含 `fiemap_helper.h`、`lba_extent.h`、`nvme_file_header.h` —— 逻辑靠搬运，不靠 include）；`backends/**`；`io_engine/**`；`device_manager/**`；`coordinator/**`；`tutti/spi/data_path.h`；`tutti/include/tutti/storage_runtime.h`。

**关于 guard 的说明（与前几轮相反）：** resolver 是私有边界的**内侧**。`extent`、`fiemap`、`namespace`、`block_size`、`fd`、`ioctl`、`ext4`、`lba` 这些词在这里**合法且预期** —— 它的职责就是把文件系统细节转译成 binding 契约。**不要**因为这些词出现就删类型或改名。禁的只是上面列的**跨层 include**。

header-only 或 header + `.cpp` 均可，自行选择并说明理由。

# 6. 测试要求

在 `tests/resolver_contract/` 下建立独立可 configure 的测试（自带顶层 `CMakeLists.txt`，含 `project()` + `enable_testing()` + `add_test`）：

- 不依赖 gtest，用简单断言 + 非零退出码（与仓内其他 contract test 风格一致）；
- 编译选项含 `-Wall -Wextra -Werror`；
- `TUTTI_USE_HOST=1`；
- include root：`/data/home/ryeqiu/Tutti/tutti/include` 与 `/data/home/ryeqiu/Tutti`；
- CTest 测试名 `tutti_resolver_contract_test`；
- **测试根目录通过环境变量指定**（例如 `TUTTI_RESOLVER_TEST_DIR`），默认 `/mnt/nvme1/GPU0/resolver_test`。若目录不存在或不可写，**明确报错退出非零**，错误信息说明「需负责人先挂载 snvme 设备」。**不得静默降级到其他文件系统。**

**测试必须在真实挂载点上创建真实文件并真实调用 FIEMAP。**

至少覆盖：

1. **正常路径（fallocate + 写入）**：创建若干 MiB 文件，`pwrite` 全量 + `fsync`，`resolve()` 成功，extent 完整覆盖 `[0, file_size)`，`logical_size` 与文件大小一致。
2. **只 fallocate 不写入**：与生产 `create_file` 用法一致（UNWRITTEN extent）。按搬运的策略，这应当**成功**（UNWRITTEN 不在拒绝掩码中）。这条验证你确实原样搬运了策略，没有擅自加严。
3. **`view_payload()` 往返**：`resolve()` 产出的 `ResolvedTarget` 经 `view_payload()` 取回 payload，内容符合预期。
4. **`map_to_device_offset()`**：验证首字节、跨 extent 边界（若有多 extent）、末字节、`file_size`（应失败）。
5. **与 `filefrag -v` 交叉验证**：对同一文件运行 `filefrag -v <path>`，把它报告的 extent 数与物理块起始，与你采集到的结果对照（注意 `filefrag` 默认以 fs block 为单位，需换算为字节）。**两侧输出都记入结果。** 这是采集正确性的独立证据。
6. **稀疏文件**：造一个有空洞的文件（`ftruncate` 出大小后只写中间一段），观察 `resolve()` 的实际结果。由于 `fe_logical` 被如实带出，binding 的 `validate()` 应能检测到未覆盖 `[0, file_size)` 而返回 `DATA_LOSS`。**如实记录实际观察到的结果**（若与预期不同，记录真实行为，不要改代码去凑）。
7. **scheme 不匹配**：`ResolveOptions{"wrong-scheme"}` → `UNSUPPORTED`。
8. **文件不存在** → `NOT_FOUND`。
9. **畸形 uri** → `INVALID_ARGUMENT`。
10. **`block_size == 0`** → `INVALID_ARGUMENT`（旧代码同样拒绝，`fiemap_helper.cpp:51-54`）。
11. **对齐检查**：用刻意偏大的 `block_size`（例如 1 MiB）触发不对齐分支，说明这是为覆盖代码路径。
12. **fd lease 生命周期**：存活时 fd 有效；析构后已关闭。
13. **lease move 安全**：move 后不双重 close，move 目标析构时正确 close 一次。
14. **多轮 FIEMAP 循环**：优先真实造碎片。若难以可靠造出足够碎片（单轮缓冲 256），**让单轮缓冲容量可配置**，用一个很小的值（例如 2）强制走多轮路径，验证拼接结果与大缓冲一次采集**完全一致**。这条路径不允许完全不验证。

每个用例打印一行标识与结果，末尾打印通过总数。

# 7. 你只能修改或创建

- `/data/home/ryeqiu/Tutti/tutti/resolvers/local_file/resolver.h`
- `/data/home/ryeqiu/Tutti/tutti/resolvers/local_file/resolver.cpp`（若分离实现）
- `/data/home/ryeqiu/Tutti/tests/resolver_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/resolver_contract/resolver_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round6/result3.md`

构建产物只能写入 `/data/home/ryeqiu/Tutti/build/round6-session3*`。

测试数据只能写入 `/mnt/nvme1/GPU0/resolver_test/`（或 `TUTTI_RESOLVER_TEST_DIR` 指定的目录）。

禁止修改或创建任何其他文件。尤其禁止：

- 修改 `nvme_storage/**`（**即使你发现问题也不要改**）
- 修改 `tutti/bindings/**`
- 修改 `tutti/include/**`
- 修改任何 `CMakeLists.txt`，除你新建的 `tests/resolver_contract/CMakeLists.txt`
- 修改 `tests/` 下其他目录
- 修改 `scripts/**`、`sys_config.yaml`、`.gitignore`
- 修改 `chat/**` 中除 `chat/round6/result3.md` 外的文件
- 在仓库源码树内留下测试数据文件

禁止提交 Git commit。

# 8. 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 环境就绪自检

```bash
findmnt /mnt/nvme1
test -w /mnt/nvme1/GPU0/resolver_test && echo 'writable: OK'
df -T /mnt/nvme1
```

记录输出。不就绪则 `BLOCKED`。

## 2. 编译与运行

```bash
rm -rf build/round6-session3
cmake -S tests/resolver_contract -B build/round6-session3 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round6-session3 --target tutti_resolver_contract_test -j8 2>&1 | tail -20
TUTTI_RESOLVER_TEST_DIR=/mnt/nvme1/GPU0/resolver_test \
  ctest --test-dir build/round6-session3 --output-on-failure -R '^tutti_resolver_contract_test$'
```

要求 `-Werror` 零告警、`1/1 Passed`、每个用例都打印结果。

## 3. 头文件可独立编译

```bash
printf '%s\n' '#include "tutti/resolvers/local_file/resolver.h"' 'int main(){ return 0; }' \
  | /opt/rh/gcc-toolset-13/root/usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -DTUTTI_USE_HOST=1 \
    -I/data/home/ryeqiu/Tutti/tutti/include -I/data/home/ryeqiu/Tutti -x c++ -fsyntax-only -
```

## 4. 与既有契约头共存（跨 header 撞名哨兵）

```bash
printf '%s\n' '#include <tutti/memory_types.h>' '#include <tutti/storage_runtime.h>' \
  '#include <tutti/spi/storage_target_resolver.h>' '#include <tutti/spi/data_path.h>' \
  '#include "tutti/resolvers/local_file/resolver.h"' 'int main(){ return 0; }' \
  | /opt/rh/gcc-toolset-13/root/usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -DTUTTI_USE_HOST=1 \
    -I/data/home/ryeqiu/Tutti/tutti/include -I/data/home/ryeqiu/Tutti -x c++ -fsyntax-only -
```

必须通过。

## 5. 依赖约束核验

```bash
grep -n '#include' tutti/resolvers/local_file/resolver.h tutti/resolvers/local_file/resolver.cpp 2>/dev/null
grep -nE 'cuda|hip|libnvm|nvm_|nvme_storage|fiemap_helper|lba_extent|nvme_file_header|backends/|io_engine/|device_manager/|coordinator/|data_path\.h|storage_runtime\.h' \
  tutti/resolvers/local_file/resolver.h tutti/resolvers/local_file/resolver.cpp 2>/dev/null \
  || echo 'no forbidden dependency: PASS'
```

逐条说明命中（若有）。

## 6. binding 配对收敛核验

```bash
grep -nE 'ResolvedTarget::make|ext4-local-nvme-payload|kPayloadTypeId|kPayloadApiVersion' \
  tutti/resolvers/local_file/resolver.h tutti/resolvers/local_file/resolver.cpp 2>/dev/null \
  || echo 'no direct make / no payload id literal: PASS'
```

## 7. 真实 FIEMAP 硬证据

测试输出须含每个成功用例的 extent 数量、每个 extent 的 `logical_offset` / `device_offset` / `length` **运行时真实数值**（非硬编码），以及 `filefrag -v` 原始输出与换算对照。**完整记入结果。**

## 8. 搬运忠实度自查

在结果中逐条对照，说明你搬运的实现与 `fiemap_helper.cpp` 的行为是否一致：

| 行为 | 旧实现 | 你的实现 |
| --- | --- | --- |
| 拒绝掩码 | 7 个 flag | ? |
| `UNWRITTEN` | 刻意接受 | ? |
| `fsync` + `FIEMAP_FLAG_SYNC` | 有 | ? |
| 单轮缓冲 | 256 | ? |
| 总量上限 | 124 | ? |
| 对齐检查 | `fe_physical` 与 `fe_length` 双查 | ? |
| `block_size == 0` | 拒绝 | ? |
| `fs_block_size % block_size` | 校验 | ? |
| 0 extent | 报错 | ? |
| 多轮游标 | `fe_logical + fe_length` | ? |

**任何不一致都必须给出理由**，且理由只能是「新接口所必需」，不能是「我认为旧的不对」。

## 9. 测试数据清理与 Hygiene

```bash
ls -la /mnt/nvme1/GPU0/resolver_test/ | head
df -h /mnt/nvme1 | tail -1
git status --short --untracked-files=all | grep -vE '^\?\? (chat/|build/|tutti/resolvers/|tests/resolver_contract/)' | head
for f in tutti/resolvers/local_file/resolver.h tutti/resolvers/local_file/resolver.cpp \
         tests/resolver_contract/CMakeLists.txt tests/resolver_contract/resolver_contract_test.cpp; do
  test -f "$f" || continue
  printf '%s: ' "$f"
  if grep -nE '[[:blank:]]+$' "$f" >/dev/null; then echo TRAILING-WS
  elif [ "$(tail -c 1 "$f" | wc -l)" -ne 1 ]; then echo NO-EOF-NEWLINE
  else echo OK; fi
done
```

自己创建的测试文件须已清理；测试目录本身保留。

## 10. 既有测试未被破坏

```bash
for t in binding_contract storage_target_resolver_contract data_path_contract; do
  rm -rf build/round6-session3-$t
  cmake -S tests/$t -B build/round6-session3-$t -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null 2>&1
  cmake --build build/round6-session3-$t -j8 >/dev/null 2>&1
  printf '%s: ' "$t"
  ctest --test-dir build/round6-session3-$t 2>&1 | grep -E 'tests passed|tests failed'
done
```

全部必须通过。

## 11. 环境未被改动

```bash
findmnt /mnt/nvme1
pgrep -af tutti_daemon | head -2
for b in 0000:08:00.0 0000:4b:00.0 0000:57:00.0 0000:63:00.0; do
  printf '%s driver=' "$b"
  [ -e "/sys/bus/pci/devices/$b/driver" ] \
    && basename "$(readlink -f /sys/bus/pci/devices/$b/driver)" || echo '(UNBOUND)'
done
grep -E '^(snvme|snvme_core|phoenixfs) ' /proc/modules
findmnt /mnt/nvme4
```

要求：`/mnt/nvme1` 仍挂载、daemon 仍运行（你不该动它们）、模块状态不变、`/mnt/nvme4` 完好。

# 9. 成功标准

报告 `PASS` 需同时满足：

1. 未 bind/unbind/mkfs/mount/umount，未启停 daemon，未碰模块，未打开任何块设备节点；
2. `LocalFileResolver` 公开继承 `StorageTargetResolver` 并 `override resolve()`，无额外公共虚方法；
3. `NamespaceIdentity` 由构造参数注入，`resolve()` 不探测硬件；
4. 采集逻辑忠实搬运 `fiemap_helper.cpp`，第 8 节的对照表逐项说明，不一致处理由充分且仅限「新接口所必需」；
5. 两处机械转换正确：字节单位输出、`logical_offset` 取自 `fe_logical`；
6. 产出**只**经 `make_resolved_target()`，无 `ResolvedTarget::make`、无 payload id 字面量；
7. extent 集合原样交给 `create()`，未自己重复实现校验；
8. fd lease：存活时有效、析构后关闭、move 后不双重 close，三者都有测试；
9. `filefrag -v` 交叉验证通过，两侧输出已记录；
10. 14 类测试用例全部实现且通过（用例 2 与 6 须如实记录实际观察结果）；
11. 测试输出含运行时真实 extent 数值；
12. 头文件可独立编译，且与四个既有契约头同 TU 共存编译通过；
13. `-Werror` 零告警，CTest `1/1 Passed`；
14. 三个既有 contract test 未被破坏；
15. 未修改允许列表外文件（**特别是未改 `nvme_storage/**`**），源码树内无测试数据残留；
16. 空白与 EOF newline 检查通过。

已知限制如实记录（一两句话即可，不需长篇）：`device_offset` 的语义基准是承载 ext4 的块设备（本环境是整命名空间，无分区，故等于命名空间偏移）；fd lease 不阻止文件被改写；任何未实测路径。

若某处无法完成，**不要改 `nvme_storage/**` 去绕开**。停下、记录真实情况、报告 `BLOCKED`。

# 10. 结果落盘要求

写入 `/data/home/ryeqiu/Tutti/chat/round6/result3.md`，至少包含：

1. 环境就绪自检输出
2. 交付文件列表与角色，header-only 或分离实现的选择理由
3. 类的形状：继承、构造参数、scheme 常量、uri 解析规则
4. **第 8 节的搬运忠实度对照表**（逐项填写）
5. 两处机械转换的实现说明
6. `StatusCode` 映射选择及理由
7. fd lease 设计，与 `.refs/` 硬链接的简短对比
8. 14 类测试用例逐一的实现方式与结果
9. 测试完整输出（含运行时真实 extent 数值）
10. `filefrag -v` 原始输出与换算对照
11. 依赖约束核验结果
12. binding 配对收敛核验结果
13. 头文件独立编译 + 与四个契约头共存编译结果
14. 三个既有 contract test 结果
15. 测试数据清理与环境未被改动的核验
16. 已知限制（简短）
17. 最终 `PASS` / `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 命令失败就写真实错误与 `BLOCKED`，不得伪造 PASS。
- **不要**在结果中长篇评论 `nvme_storage` 既有代码的优劣。你的职责是搬运，不是评审。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round6/result3.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
