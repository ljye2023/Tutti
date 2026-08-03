# TASK T-025

你是一名资深 Linux 存储/CUDA 工程师。你的任务是**分层抽象 + 代码搬运**：在 Session 2 已创建的 `LocalNvmeDataPath` 骨架上，搬运 libnvm 的 client-attach 与 DMA 注册逻辑，实现**真实的 data-path memory registration**。这是重构接手方案 P0-1（没有可工作的生产 DMA registration 路径）的修复。

你看不到任何其他上下文，本 prompt 已包含全部需要的接口、搬运源和验收标准。

# 任务定位（先读这条）

**你在做搬运和抽象，不是做设计评审。**

- 搬运源是 libnvm 的 client-attach + DMA map 既有用法。**原样搬运其调用方式与生命周期**；不要评判 libnvm 的对错。
- Session 2 创建的骨架（`LocalNvmeDataPath`）与已冻结的 SPI/binding 是既有契约。**不要改** `tutti/include/**`、`tutti/bindings/**`、`tutti/resolvers/**`、`nvme_storage/**`、`tutti/backends/**`、`backends/**`。
- 你只**扩展** Session 2 的类，把 `register_memory` / `unregister_memory` 从「显式失败」改成「真实注册」，并让 `initialize`/`shutdown` 管理 controller 连接。

# 前置依赖（重要）

**本任务必须在 Session 2 完成之后执行。** Session 2 已创建：

```text
tutti/data_paths/local_nvme/local_nvme_data_path.h
tutti/data_paths/local_nvme/local_nvme_data_path.cpp
tests/local_nvme_datapath_contract/...
```

你要**先读这三个文件**，理解 Session 2 建立的类结构（capabilities、target 表、身份管理、显式失败的 register/unregister），然后在**不改变其既有行为**的前提下扩展。

若 Session 2 的产物不存在或结构与本 prompt 描述严重不符，报告 `BLOCKED` 并说明，**不要**从头重写。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 执行时机

**本任务必须单独执行，不与任何其他 session 并发。** 它需要 daemon 运行（提供 `/dev/ssnvme<N>`）与 GPU（DMA 映射），是独占硬件操作。

开始前确认：

```bash
ps -eo pid,etime,cmd | grep -E '[c]make|[c]test' | head
findmnt /mnt/nvme1 | tail -1
pgrep -af tutti_daemon | head -1
ls -l /dev/ssnvme* 2>/dev/null
```

**环境前提（由负责人保持）**：daemon 正在运行，已创建 `/dev/ssnvme0`（对应 `0000:08:00.0`）。若 `/dev/ssnvme0` 不存在或 daemon 未运行，报告 `BLOCKED` 并说明「需负责人先启动 daemon」。**不要**自己启动 daemon。

# 1. 背景：为什么这是 P0-1 的修复

接手方案 P0-1 指出：当前没有可工作的生产 DMA registration 路径 —— 旧的 `IAccelerator` 没有 `dma_map`，`MemoryRegion::backend_private` 被滥用为单一 `ioaddrs` 指针。修复方向是：**`DataPath` 负责 memory registration**，Runtime 只保存 registration handle 与生命周期。

本任务就是把 `nvm_dma_map_data_*` 的调用搬到 `LocalNvmeDataPath::register_memory` 里，让它成为 data-path-owned registration 的第一个真实实现。

# 2. 要搬运的 libnvm 用法（读，原样搬运）

## controller 获取（client-only，无需 gRPC）

`backends/local/nvme/libnvm/include/nvm_ctrl.h:150-193` 说明：

- `nvm_ctrl_attach_client(&ctrl, snvme_dev_path, bar0_size)` —— **client-only**，打开已存在的 `/dev/ssnvme<N>`、mmap BAR0、内部 cudaHostRegister，包成新的 `nvm_ctrl_t`。**不碰 bind/chrdev ioctl，不需要 gRPC**。之后可在自己的 fd 上 `nvm_create_group` / `nvm_dma_map_*` / `nvm_add_user_queue`（per-fd scoped，不影响 owner 或其他 client）。
- `nvm_ctrl_free_client(ctrl)` —— client-only 释放：关闭 attach 的 fd，内核 `snvm_dev_release` 级联清理该 fd 上的 group 与 DATA map。**不碰 PCI driver 状态**。

**搬运源**（完整流程）：`tutti/device_manager/nvme/nvmeservice/examples/nvmeservice_client_io.cu:168-232`：

```c
nvm_ctrl_t* ctrl = nullptr;
rc = nvm_ctrl_attach_client(&ctrl, a->snvme_dev_path, (uint32_t)a->bar0_size);
// ...
rc = nvm_dma_map_data_device(&dma_wbuf, ctrl, wbuf_dev, GPU_PAGE_SIZE);
rc = nvm_dma_map_data_device(&dma_rbuf, ctrl, rbuf_dev, GPU_PAGE_SIZE);
```

## DMA 注册

`backends/local/nvme/libnvm/include/nvm_dma.h`：

```c
int nvm_dma_map_data_host(nvm_dma_t** handle, const nvm_ctrl_t* ctrl,
                          void* vaddr, size_t size);
int nvm_dma_map_data_device(nvm_dma_t** handle, const nvm_ctrl_t* ctrl,
                            void* devptr, size_t size);
```

- `nvm_dma_map_data_device` —— 注册 **GPU device** 内存（`view.kind == DataPathMemoryKind::DEVICE`）。
- `nvm_dma_map_data_host` —— 注册 **host** 内存（`view.kind == DataPathMemoryKind::HOST`）。
- 成功返回 0，`handle` 得到 `nvm_dma_t*`；失败返回非 0。
- 释放用 `nvm_dma_unmap(nvm_dma_t*)`。

**另一个搬运源**：`tutti/tests/backends/nvme/nvme_backend_test.cpp:391` 附近（`GpuSubmitSingleBlock` 中 `nvm_dma_map_data_device(&dma, nvme->ctrl, gpu_buf, blk)`）。

**`nvm_dma_t` 的关键字段**（你需要保存它以供后续 IO 与 unmap）：定义在 `libnvm/include/nvm_types.h`，含 `ioaddrs[]`（controller 可见的 DMA 地址数组）。本任务只需**持有** `nvm_dma_t*` 并在 unmap 时传给 `nvm_dma_unmap`；不需要读 `ioaddrs`（那是 IO 提交路径的事）。

# 3. 要扩展的 `LocalNvmeDataPath`

## 需要新增的构造参数

controller 连接信息由**构造参数注入**（与 resolver 的 `NamespaceIdentity` 注入一致），`initialize()` 时不探测硬件。建议新增：

```cpp
LocalNvmeDataPath(std::string snvme_dev_path,   // e.g. "/dev/ssnvme0"
                  std::uint32_t bar0_size,
                  /* Session 2 已有的构造参数，若有 */)
```

`bar0_size` 如何确定：你可以从 daemon 的 `ListDevices` 拿到，或在测试中硬编码一个从环境读到的值。**在结果中说明你用的值及其来源。**（若不确定，可参考 client_io 示例里 `a->bar0_size` 的来源，或在结果中记录你如何确定它。）

## `initialize(config, resources)`

在 Session 2 的最小初始化基础上，增加 **client attach**：

```cpp
int rc = nvm_ctrl_attach_client(&ctrl_, snvme_dev_path_.c_str(), bar0_size_);
if (rc != 0) {
    return Status(StatusCode::NOT_READY,
                  "nvm_ctrl_attach_client(" + snvme_dev_path_ + ") failed: rc " +
                  std::to_string(rc));
}
```

- 成功则持有 `ctrl_`（`nvm_ctrl_t*`），供 `register_memory` 使用。
- 失败返回 `NOT_READY`（设备未就绪，区别于参数错误）。**不要**静默降级成「无 controller 也可用」—— 接手方案 P0-7 明确要求缺依赖时返回结构化错误，不能 mock。
- `ResourceProvider&` 仍不使用（同 Session 2）。

## `shutdown(timeout_ns)`

- 先注销所有仍存活的 registration（对每个未 unregister 的 `nvm_dma_t*` 调 `nvm_dma_unmap`），再 `nvm_ctrl_free_client(ctrl_)`，置 `ctrl_ = nullptr`。
- 保持 Session 2 的幂等性：重复调用安全，`ctrl_ == nullptr` 时跳过 free。
- **不能泄漏**：所有 `nvm_dma_t*` 必须在 shutdown 时被 unmap（若调用方忘了 unregister）。这是 RAII 的要求 —— 在结果中说明你如何保证无泄漏。

## `register_memory(view, domain)` → `DataPathMemory`

1. 校验：`initialize` 已完成（`ctrl_ != nullptr`），否则 `NOT_READY`；`view.base != nullptr`、`view.size_bytes > 0`，否则 `INVALID_ARGUMENT`。
2. 校验 `domain`：本 DataPath 只服务一个 controller（构造时注入的那个）。若 `domain` 与本 DataPath 的 registration domain 不符，返回 `INVALID_ARGUMENT` 或 `UNSUPPORTED`（在结果中说明你的选择与理由）。单 controller 阶段的 domain 语义由你定义并记录。
3. 按 `view.kind` 分派：

   ```cpp
   nvm_dma_t* dma = nullptr;
   int rc;
   if (view.kind == DataPathMemoryKind::DEVICE) {
       rc = nvm_dma_map_data_device(&dma, ctrl_, view.base, view.size_bytes);
   } else {  // HOST
       rc = nvm_dma_map_data_host(&dma, ctrl_, view.base, view.size_bytes);
   }
   if (rc != 0 || dma == nullptr) {
       return Result<DataPathMemory>::Failure(
           Status(StatusCode::DEVICE_ERROR,
                  "nvm_dma_map_data_* failed: rc " + std::to_string(rc)));
   }
   ```

4. 铸造 `DataPathMemory` 身份（`SpiIdentityMint::mint<DataPathMemoryTag>(token, generation)`，复用 Session 2 的身份管理），存入 registration 表（`token → nvm_dma_t*`），返回身份。

## `unregister_memory(memory)` → `Status`

- 按 `token + generation` 校验身份存在且未注销；从表中取出 `nvm_dma_t*`，调 `nvm_dma_unmap(dma)`，使身份失效。
- 重复 unregister 同一身份 → 明确错误（不静默成功）。
- 未知身份 → 错误。

## 依赖变化（重要）

Session 2 的骨架**不** link libnvm / CUDA（纯 host）。本任务**必须** link 它们，因为 attach 与 DMA map 需要：

- **libnvm**：`nvm_ctrl_attach_client`、`nvm_dma_map_data_*`、`nvm_dma_unmap`、`nvm_ctrl_free_client`。
- **CUDA**：`nvm_ctrl_attach_client` 内部 cudaHostRegister；测试需要 `cudaMalloc`/`cudaFree`/`cudaHostAlloc`。

因此本任务会修改 Session 2 的 `.h`/`.cpp`（加 `#include <nvm_ctrl.h>`、`<nvm_dma.h>` 等）与测试的 `CMakeLists.txt`（加 libnvm + CUDA 链接）。**这意味着测试从 HOST profile 变为需要 CUDA toolkit 与 GPU。** 在结果中明确这个依赖变化。

**include 约束更新**：本任务**允许** include libnvm 头（`<nvm_ctrl.h>`、`<nvm_dma.h>`、`<nvm_types.h>`）与 CUDA 头。仍**禁止** include：`nvme_storage/**`、`tutti/backends/**`、`io_engine/**`、`device_manager/**` 的**上层**头（但 `device_manager/nvme/libnvm/include/` 下的 libnvm 公共头允许——注意仓内有两份 libnvm，见下）。

## libnvm 两份副本（必须用被构建的那份）

仓内有两份 libnvm，当前字节相同，但只有 `backends/local/nvme/libnvm/` 被根构建编译：

```text
backends/local/nvme/libnvm/            <- 用这份（构建实际编译的）
tutti/device_manager/nvme/libnvm/      <- 根构建不编译，不要用
```

include 路径与 link 都必须指向 `backends/local/nvme/libnvm/`。测试 link 的 `libnvm.so` 在 `build/lib/libnvm.so`。在结果中给出你用的 include 与 link 路径，证明是这份。

# 4. 测试要求

把 `tests/local_nvme_datapath_contract/` 的测试扩展为 **CUDA profile**（保留 Session 2 的用例，新增 registration 用例）。你需要：

- 修改 `tests/local_nvme_datapath_contract/CMakeLists.txt`：加 libnvm + CUDA 的 include 与 link；改用 CUDA 编译（`enable_language(CUDA)` 或用 CUDA toolkit 的编译器对 `.cpp` 也能调 cudart，自行选择并说明）。`TUTTI_USE_CUDA=1`（或保持 HOST 宏但链 CUDA——说明你的选择与影响）。
- 保留 Session 2 的全部既有用例（它们不应因引入 CUDA/libnvm 而失效）。
- **硬件前提**：daemon 运行（`/dev/ssnvme0` 存在）+ 有 GPU。测试开头自检 `/dev/ssnvme0` 可打开、CUDA 可用，否则明确报错退出非零（不静默跳过）。

新增 registration 用例（至少）：

1. **HOST 内存注册**：`cudaHostAlloc`（或 `malloc`，说明选择）一块内存 → `register_memory(HOST view)` → 返回有效 `DataPathMemory`；`unregister_memory` → OK。
2. **DEVICE 内存注册**：`cudaMalloc` 一块 GPU 内存 → `register_memory(DEVICE view)` → 有效身份；`unregister_memory` → OK。
3. **重复注册**：同一 buffer 注册两次 → 两个不同身份（token 递增），各自可独立 unregister。
4. **unregister 使身份失效**：unregister 后再 unregister 同身份 → 错误；再对其操作 → 错误。
5. **未 initialize 就 register**：在未 initialize 的 DataPath 上 `register_memory` → `NOT_READY`。
6. **空指针 / 零长度**：`view.base == nullptr` 或 `size_bytes == 0` → `INVALID_ARGUMENT`。
7. **shutdown 无泄漏**：register 后**不** unregister 直接 shutdown → shutdown 成功且内部 unmap 了所有 registration（可通过再次 initialize + register 成功、无资源耗尽来侧面验证，或你设计的其他可观测方式）。在结果中说明你的验证方式。
8. **真实 DMA 地址非空**（硬证据）：注册成功后，从保存的 `nvm_dma_t*` 读取 `ioaddrs[0]`（或等价字段），**打印其运行时真实值**并断言非 0。这是「真的做了 DMA 映射」的唯一硬证据。

每个用例打印一行标识与结果，末尾打印通过总数。**保留 Session 2 的既有输出，在其后追加新用例。**

# 5. 你只能修改

- `/data/home/ryeqiu/Tutti/tutti/data_paths/local_nvme/local_nvme_data_path.h`
- `/data/home/ryeqiu/Tutti/tutti/data_paths/local_nvme/local_nvme_data_path.cpp`
- `/data/home/ryeqiu/Tutti/tests/local_nvme_datapath_contract/CMakeLists.txt`
- `/data/home/ryeqiu/Tutti/tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round7/result3.md`

构建产物只能写入 `/data/home/ryeqiu/Tutti/build/round7-session3*`。

禁止修改或创建任何其他文件。尤其禁止：

- 修改 `tutti/include/**`、`tutti/bindings/**`、`tutti/resolvers/**`、`tutti/backends/**`、`nvme_storage/**`、`backends/**`
- 修改两份 libnvm 的**源码**（你只 link 它，不改它）
- 修改 `tests/` 下其他目录、任何其他 `CMakeLists.txt`
- bind / unbind / mkfs / mount / umount / 启停 daemon / 加载卸载内核模块
- 修改 `.gitignore`、`chat/**` 中除 `chat/round7/result3.md` 外的文件

禁止提交 Git commit。

# 6. 安全限制

绝对禁止 `insmod` / `rmmod` / `modprobe`；禁止 bind / unbind PCI 设备；禁止 mkfs / mount / umount；禁止启动或停止 daemon（它由负责人保持运行）；禁止写 `/sys` / `/proc`。

**允许**：用 libnvm client API attach 已存在的 `/dev/ssnvme0`（这只打开/mmap，不改 bind 状态）；CUDA 内存分配与 DMA 映射；只读运行 `ls` / `findmnt` / `pgrep`。

**禁止**对 `/dev/ssnvme0` 做任何**数据面 block IO**（本任务只测 DMA 映射，不发 NVMe 读写命令）；禁止触碰 `/dev/md0`、`/mnt/nvme4`（生产数据）。

DMA 映射本身**不是** block IO —— 它只是把内存注册给 controller，不传输数据。这是本任务的范围边界。

# 7. 验收步骤

## 1. 环境就绪自检

```bash
pgrep -af tutti_daemon | head -1
ls -l /dev/ssnvme0
nvidia-smi -L 2>/dev/null | head -3 || echo '(check GPU visibility)'
```

daemon 在、`/dev/ssnvme0` 在、GPU 可见。不就绪则 `BLOCKED`。

## 2. 编译与运行

```bash
rm -rf build/round7-session3
cmake -S tests/local_nvme_datapath_contract -B build/round7-session3 -DCMAKE_BUILD_TYPE=RelWithDebInfo 2>&1 | tail -15
cmake --build build/round7-session3 --target tutti_local_nvme_datapath_contract_test -j8 2>&1 | tail -20
ctest --test-dir build/round7-session3 --output-on-failure -R '^tutti_local_nvme_datapath_contract_test$'
```

要求零新增告警、既有用例与新用例全部通过。

## 3. libnvm 来源核验

```bash
grep -nE 'libnvm|nvm_dma|nvm_ctrl|backends/local/nvme' tests/local_nvme_datapath_contract/CMakeLists.txt
ldd build/round7-session3/bin/tutti_local_nvme_datapath_contract_test 2>/dev/null | grep -i nvm
```

确认 include 与 link 都指向 `backends/local/nvme/libnvm/`（被构建的那份），不是 `tutti/device_manager/nvme/libnvm/`。

## 4. 真实 DMA 映射的硬证据

测试输出必须含注册成功后 `ioaddrs` 的运行时真实值（非 0）。完整记入结果。

## 5. 环境未被改动

```bash
pgrep -af tutti_daemon | head -1
findmnt /mnt/nvme1 | tail -1
grep -E '^(snvme|snvme_core|phoenixfs) ' /proc/modules
for b in 0000:08:00.0 0000:4b:00.0 0000:57:00.0 0000:63:00.0; do
  printf '%s driver=' "$b"
  [ -e "/sys/bus/pci/devices/$b/driver" ] \
    && basename "$(readlink -f /sys/bus/pci/devices/$b/driver)" || echo '(UNBOUND)'
done
findmnt /mnt/nvme4 | tail -1
```

daemon 仍运行（它的 client attach 不改变其 owner 状态）、挂载仍在、模块状态不变、四块设备驱动状态不变（daemon 持有的仍持有，其余 UNBOUND）、生产 RAID 完好。

## 6. Hygiene

```bash
git diff --check -- tutti/data_paths/local_nvme/ tests/local_nvme_datapath_contract/
git status --short --untracked-files=all | head -20
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
```

# 8. 成功标准

报告 `PASS` 需同时满足：

1. `initialize` 经 `nvm_ctrl_attach_client` 连接 `/dev/ssnvme0`，失败返回 `NOT_READY`（不静默降级）；
2. `register_memory` 按 `view.kind` 正确分派 `nvm_dma_map_data_device` / `nvm_dma_map_data_host`，持有 `nvm_dma_t*`，铸造有效 `DataPathMemory`；
3. `unregister_memory` 经 `nvm_dma_unmap` 释放并使身份失效；
4. `shutdown` 先 unmap 所有存活 registration，再 `nvm_ctrl_free_client`，幂等，**无泄漏**；
5. 身份管理正确：token 递增、generation 校验、close/unregister 后失效；
6. 使用 `backends/local/nvme/libnvm/`（被构建的那份）；
7. 真实 DMA 映射硬证据（`ioaddrs` 非 0 的运行时值）已记录；
8. 8 类新用例 + Session 2 既有用例全部通过；
9. 未修改允许列表外文件，未改 libnvm 源码；
10. 未执行模块加载/卸载、bind/unbind、启停 daemon、数据面 block IO；
11. 环境未被改动（daemon/挂载/模块/设备/生产 RAID）；
12. 空白与 EOF newline 检查通过。

如实记录被显式推迟的部分（IO 提交、completion、多 controller、ResourceProvider 接入）。**这些不影响 PASS。**

# 9. 结果落盘要求

写入 `/data/home/ryeqiu/Tutti/chat/round7/result3.md`，至少包含：

1. Session 2 产物的阅读结论（类结构、身份管理、既有行为）
2. 环境就绪自检
3. 新增的构造参数与 `bar0_size` 的值及来源
4. initialize / register_memory / unregister_memory / shutdown 的实现要点（搬运自哪些源、单位/语义）
5. registration domain 的语义与单 controller 处理
6. 依赖变化说明（从纯 host 到 link libnvm + CUDA）
7. libnvm 来源核验（用的是被构建的那份）
8. 8 类新用例的实现方式与结果
9. **真实 DMA 映射硬证据**（`ioaddrs` 运行时值）
10. 无泄漏的验证方式与结果
11. 测试完整输出（既有 + 新用例）
12. 环境未被改动的核验
13. 显式推迟的部分
14. hygiene 检查
15. 最终 `PASS` / `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 命令失败就写真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round7/result3.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
