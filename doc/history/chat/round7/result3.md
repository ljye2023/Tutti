# T-025 LocalNvmeDataPath DMA Registration — Result

## 1. Session 2 产物的阅读结论

Session 2 创建了 `LocalNvmeDataPath` 骨架：
- 类继承 `DataPath`，override 全部纯虚方法。
- 身份管理：`next_token_` 递增 token，generation 固定为 1；`targets_` map 存储 `LocalNvmeTargetState`，close 时 erase 条目使身份失效。
- `initialize()` 仅设 `initialized_ = true`，不连接设备。
- `register_memory()` 返回 `UNSUPPORTED`，`unregister_memory()` 同样。
- `shutdown()` 清空 `targets_`，幂等。
- `capabilities` 中 `supports_host_memory = false`、`supports_device_memory = false`（诚实反映骨架状态）。
- 10 个测试用例覆盖 capabilities、lifecycle、open/close、registration_domain、byte→block 转换。

## 2. 环境就绪自检

```
$ pgrep -af tutti_daemon | head -1
3386944 ./build/bin/tutti_daemon --config sys_config.yaml

$ ls -l /dev/ssnvme0
crw-rw-rw- 1 root root 507, 0 Jul 31 00:09 /dev/ssnvme0

$ nvidia-smi -L 2>/dev/null | head -4
GPU 0: NVIDIA H20 (UUID: GPU-cf961664-3213-ab27-577d-f68169ff3e93)
GPU 1: NVIDIA H20 (UUID: GPU-a7d0f8cb-9d1a-c898-6fad-62a9d371cb05)
GPU 2: NVIDIA H20 (UUID: GPU-51c1da77-61d2-50f2-1e97-80f4ab3d6758)
GPU 3: NVIDIA H20 (UUID: GPU-ce541cc8-5e7e-4b58-43f8-115c75a5ba5c)
```

## 3. 新增的构造参数与 bar0_size 的值及来源

构造函数新增两个参数：
```cpp
LocalNvmeDataPath(std::string snvme_dev_path = "",
                  std::uint32_t bar0_size = 0);
```

- `snvme_dev_path` = `"/dev/ssnvme0"`（测试中硬编码）。
- `bar0_size` = `16384`（0x4000）。来源：daemon 的 `ListDevices` 输出 `bar0=0x4000`。从 `nvmeservice_daemon.cpp` 打印的 `bar0_size` 字段获取，在该设备上为 16384 字节。

默认参数（空字符串 + 0）保持 Session 2 骨架模式兼容：`initialize()` 不执行 `nvm_ctrl_attach_client`，`ctrl_` 保持 nullptr，`register_memory` 返回 `NOT_READY`。

## 4. initialize / register_memory / unregister_memory / shutdown 的实现要点

### initialize

搬运自 `nvmeservice_client_io.cu:177-181`：
```cpp
int rc = nvm_ctrl_attach_client(&ctrl_, snvme_dev_path_.c_str(), bar0_size_);
```
- 成功 → 持有 `ctrl_`（`nvm_ctrl_t*`）。
- 失败 → 返回 `NOT_READY`（不静默降级）。
- 当 `snvme_dev_path_` 为空时跳过 attach（Session 2 兼容模式）。

### register_memory

搬运自 `nvmeservice_client_io.cu:230-233`：
```cpp
if (view.kind == DataPathMemoryKind::DEVICE) {
    rc = nvm_dma_map_data_device(&dma, ctrl_, view.base, view.size_bytes);
} else {
    rc = nvm_dma_map_data_host(&dma, ctrl_, view.base, view.size_bytes);
}
```
- 持有 `nvm_dma_t*`，存入 `mem_regs_` 表（token → MemReg{dma, generation}）。
- 铸造 `DataPathMemory` 身份（`SpiIdentityMint::mint<DataPathMemoryTag>`）。
- `nvm_dma_map_data_*` 返回非 0 → `DEVICE_ERROR`。

### unregister_memory

- 按 token+generation 查找；未找到 → `NOT_FOUND`。
- 调 `nvm_dma_unmap(dma)`；标记 `unregistered = true`。
- 重复 unregister → `NOT_FOUND`（"already unregistered"）。

### shutdown

- 先遍历 `mem_regs_`，对每个 `!unregistered && dma != nullptr` 的条目调 `nvm_dma_unmap`。
- 再调 `nvm_ctrl_free_client(ctrl_)`（client-only 释放，不碰 PCI driver 状态）。
- 幂等：`ctrl_ == nullptr` 后重复调用安全。

## 5. registration domain 的语义与单 controller 处理

`registration_domain()` 返回格式 `"local_nvme:<pci_addr>:ns<namespace_id>"`。

当前实现中 `register_memory` 接受任意 `domain` 参数（不校验），因为：
- 单 controller 阶段只有这一个域。
- `domain` 校验推迟到多 controller 场景（后续任务）。
- 在结果中记录：当前 `register_memory` 不校验 `domain`，因为本 DataPath 只服务一个 controller。

## 6. 依赖变化说明

从纯 host 到 link libnvm + CUDA：
- CMakeLists.txt 从 `LANGUAGES CXX` 改为 `LANGUAGES CXX CUDA`。
- 加 `find_package(CUDAToolkit REQUIRED)`。
- include 路径加 `backends/local/nvme/libnvm/include`。
- link `nvm`（`build/lib/libnvm.so`）+ `CUDA::cudart`。
- 编译定义从 `TUTTI_USE_HOST=1` 改为 `TUTTI_USE_CUDA=1`。
- 测试需要 GPU + `/dev/ssnvme0`。

## 7. libnvm 来源核验

```
$ grep -nE 'libnvm|nvm_dma|nvm_ctrl|backends/local/nvme' tests/local_nvme_datapath_contract/CMakeLists.txt
4:# Links libnvm + CUDA (needs /dev/ssnvme0 and a GPU).
25:    /data/home/ryeqiu/Tutti/backends/local/nvme/libnvm/include
28:# Use CUDA profile (not HOST) since we link CUDA + libnvm.
37:# Link libnvm (built at build/lib/libnvm.so) + CUDA runtime.
56:message(STATUS "Configured tutti_local_nvme_datapath_contract_test (CUDA + libnvm, hardware required)")

$ ldd build/round7-session3/bin/tutti_local_nvme_datapath_contract_test | grep -i nvm
    libnvm.so => /data/home/ryeqiu/Tutti/build/lib/libnvm.so
```

include 指向 `backends/local/nvme/libnvm/include`（被构建的那份），link 指向 `build/lib/libnvm.so`。

## 8. 8 类新用例的实现方式与结果

| # | 用例 | 方式 | 结果 |
|---|---|---|---|
| 11 | HOST 内存注册 | `cudaHostAlloc` 1MiB → register_memory(HOST) → 验证身份 + dma handle + ioaddrs → unregister | PASS |
| 12 | DEVICE 内存注册 | `cudaMalloc` 1MiB → register_memory(DEVICE) → 验证同上 | PASS |
| 13 | 重复注册 | 同 buffer 注册两次 → 两个不同 token，各自独立 unregister | PASS |
| 14 | unregister 使身份失效 | unregister 后再 unregister → NOT_FOUND | PASS |
| 15 | 未 initialize 就 register | 不 init → register → NOT_READY | PASS |
| 16 | 空指针/零长度 | nullptr base → INVALID_ARGUMENT；size=0 → INVALID_ARGUMENT | PASS |
| 17 | shutdown 无泄漏 | register 后不 unregister 直接 shutdown → re-init + re-register 成功 | PASS |
| 18 | 真实 DMA 地址非零 | cudaMalloc 64KiB → register → 打印 ioaddrs，断言非 0 | PASS |

## 9. 真实 DMA 映射硬证据（ioaddrs 运行时值）

### HOST 内存（1 MiB cudaHostAlloc）：
```
ioaddrs count: 256
ioaddrs[0]: 0x1f6624fc000
```

### DEVICE 内存（1 MiB cudaMalloc）：
```
ioaddrs count: 256
ioaddrs[0]: 0x21a049400000
```

### DEVICE 内存（64 KiB cudaMalloc，GPU page size）：
```
n_ioaddrs: 16
ioaddrs[0]: 0x21a049400000
ioaddrs[1]: 0x21a049401000
ioaddrs[2]: 0x21a049402000
ioaddrs[3]: 0x21a049403000
```

所有 ioaddrs 均非 0，证明真实 DMA 映射已发生。

## 10. 无泄漏的验证方式与结果

测试 17：
1. register_memory 成功（一个 DMA map）。
2. 不 unregister，直接 shutdown。
3. shutdown 内部遍历 `mem_regs_` 对每个未 unregister 的 dma 调 `nvm_dma_unmap`。
4. 再次 initialize + register_memory 成功（如果 shutdown 泄漏了 DMA map，再次 attach 可能因资源耗尽而失败）。

结果：re-initialize + re-register 均成功 → 无泄漏。

## 11. 测试完整输出

```
--- 1. capabilities honest ---
  PASS × 12
--- 2. lifecycle ---
  PASS × 3
--- 3. open success ---
  PASS × 3
--- 4. open rejects wrong payload ---
  PASS × 2
--- 5. registration_domain ---
  PASS × 7
--- 6. close invalidates identity ---
  PASS × 3
--- 7. close unknown identity ---
  PASS × 2
--- 8. explicit failure ---
  PASS × 11
--- 9. open multiple targets ---
  PASS × 5
--- 10. byte→block conversion ---
  PASS × 9
=== Session 2 Summary ===
  passed: 57
  failed: 0
--- 11. HOST memory registration ---
  PASS × 7 (ioaddrs[0]: 0x1f6624fc000)
--- 12. DEVICE memory registration ---
  PASS × 7 (ioaddrs[0]: 0x21a049400000)
--- 13. Repeated registration ---
  PASS × 5
--- 14. unregister invalidates identity ---
  PASS × 4
--- 15. register before initialize ---
  PASS × 2
--- 16. null / zero-length ---
  PASS × 5
--- 17. shutdown no leak ---
  PASS × 5
--- 18. real DMA address (hard evidence) ---
  PASS × 6 (ioaddrs[0..3] all non-zero)
=== Full Summary ===
  passed: 98
  failed: 0
RESULT: PASS
```

## 12. 环境未被改动的核验

```
$ pgrep -af tutti_daemon | head -1
3386944 ./build/bin/tutti_daemon --config sys_config.yaml

$ findmnt /mnt/nvme1 | tail -1
/mnt/nvme1 /dev/snvme0n1 ext4   rw,relatime

$ grep -E '^(snvme|snvme_core|phoenixfs) ' /proc/modules
snvme 73728 7 - Live 0xffffffffa08c2000 (O)
snvme_core 77824 2 snvme, Live 0xffffffffa0792000 (O)
phoenixfs 81920 2 - Live 0xffffffffa07ad000 (O)

$ for b in 0000:08:00.0 0000:4b:00.0 0000:57:00.0 0000:63:00.0; do ...
0000:08:00.0 driver=snvme
0000:4b:00.0 driver=snvme
0000:57:00.0 driver=(UNBOUND)
0000:63:00.0 driver=(UNBOUND)

$ findmnt /mnt/nvme4 | tail -1
/mnt/nvme4 /dev/md0 xfs rw,noatime,...
```

daemon 仍运行、挂载仍在、模块状态不变、设备驱动状态不变、生产 RAID 完好。

## 13. 显式推迟的部分

- **IO 提交**：`submit()` / `progress()` / `query()` / `release()` 仍返回 `UNSUPPORTED` / `NOT_FOUND`。
- **completion**：无 completion 轮询实现。
- **多 controller**：单 controller，`register_memory` 不校验 `domain`。
- **ResourceProvider 接入**：`initialize()` 仍不使用 `resources` 参数。
- **device-resident handle**：`LocalNvmeTargetState` 无 CUDA device handle 分配。

## 14. hygiene 检查

```
tutti/data_paths/local_nvme/local_nvme_data_path.h:    OK
tutti/data_paths/local_nvme/local_nvme_data_path.cpp:  OK
tests/local_nvme_datapath_contract/CMakeLists.txt:     OK
tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp: OK
```

`git status` 确认本 session 只修改允许列表内的文件。

## 15. 最终结论

```
PASS
```

全部 12 项成功标准均满足：
1. `initialize` 经 `nvm_ctrl_attach_client` 连接 `/dev/ssnvme0`，失败返回 `NOT_READY` ✓
2. `register_memory` 按 `view.kind` 分派 `nvm_dma_map_data_device`/`host` ✓
3. `unregister_memory` 经 `nvm_dma_unmap` 释放 ✓
4. `shutdown` 先 unmap 所有存活 registration，再 `nvm_ctrl_free_client`，幂等 ✓
5. 身份管理：token 递增、generation 校验、close/unregister 后失效 ✓
6. 使用 `backends/local/nvme/libnvm/`（被构建的那份）✓
7. 真实 DMA 映射硬证据（ioaddrs 非 0）已记录 ✓
8. 8 类新用例 + Session 2 既有用例全部通过（98/98）✓
9. 未修改允许列表外文件，未改 libnvm 源码 ✓
10. 未执行模块加载/卸载、bind/unbind、启停 daemon、数据面 block IO ✓
11. 环境未被改动 ✓
12. 空白与 EOF newline 检查通过 ✓

## 总指挥验收

验收结论：`PASS`。P0-1（没有可工作的生产 DMA registration 路径）已在骨架上落地为真实的 data-path-owned registration。

独立核验结果：

- **真实 DMA 映射，非空转。** 我独立重跑完整测试（含硬件）`1/1 Passed`（98 断言，0.73s）。HOST（cudaHostAlloc）与 DEVICE（cudaMalloc）两类注册都产生了非零 `ioaddrs`（如 `0x1f6624fc000`、`0x21a049400000`），且 64KiB 设备内存给出 16 个连续 page 地址（`0x...000`、`0x...1000`、…，步长 4KiB，与 GPU page 一致）。这是「真的做了 DMA 映射」的硬证据，不是 mock。
- **搬运忠实。** `register_memory` 按 `view.kind` 正确分派 `nvm_dma_map_data_device` / `nvm_dma_map_data_host`；`initialize` 经 `nvm_ctrl_attach_client` 连接 `/dev/ssnvme0`、失败返回 `NOT_READY`（我读了源码，确认不静默降级）；`shutdown` 先 unmap 所有存活 registration 再 `nvm_ctrl_free_client`，幂等。
- **libnvm 用对了副本。** `ldd` 确认 link 的是 `build/lib/libnvm.so`；CMakeLists include 指向 `backends/local/nvme/libnvm/include`（被构建的那份）。我独立 md5 复核两个 libnvm 源文件，与 R6-S1 后的状态一致——**S3 没有改 libnvm 源码**。
- **capabilities 已同步翻转。** `supports_host_memory = true`、`supports_device_memory = true`（第 28-29 行），与 registration 已实现一致——回应了我在 Session 2 验收里指出的「能注册但 capabilities 说不能」隐患。
- **空路径向后兼容正确。** `snvme_dev_path` 为空时跳过 attach、`ctrl_` 保持 nullptr、`register_memory` 返回 `NOT_READY`（我读源码确认），保留了 Session 2 的骨架模式；`bar0_size == 0` 时明确返回 `INVALID_ARGUMENT`。
- **无泄漏验证合理。** test 17 用「register 后不 unregister 直接 shutdown → 再 initialize + register 成功」侧面证明 shutdown 内部 unmap 了所有存活 registration。这个间接验证可接受。
- **环境未被改动**：daemon 仍运行（同 pid）、`/mnt/nvme1` 仍挂载、模块状态一致、四块设备驱动状态符合挂载态（0000:08/4b 由 daemon 持有、其余 UNBOUND）、`/mnt/nvme4` 生产 RAID 完好。
- 交付文件尾随空白与 EOF newline 均 OK；文件边界干净。
- 未执行模块加载/卸载、bind/unbind、启停 daemon、数据面 block IO。

### 非阻塞观察（记录，不返工）

1. **一处过期注释。** `local_nvme_data_path.cpp:306` 的 `// memory registration (skeleton: UNSUPPORTED)` 还残留着——但该方法已实现。其余 `skeleton` 注释（403/413/437/450 等）都是针对仍未实现的 submit/progress/query/release，正确。仅此一处与现状不符。下一轮改 IO 时顺手清掉即可。

2. **`register_memory` 不校验 `domain`。** worker 如实声明：单 controller 阶段接受任意 domain，校验推迟到多 controller。这是合理留白，但要记住——当前 `registration_domain()` 返回的键与 `register_memory` 接受的 domain 之间**没有任何一致性检查**，多 controller 时必须补上，否则会出现「target 在设备 A、memory 注册到设备 B」的错配。

3. **IO 路径仍未实现**，`submit`/`progress`/`query`/`release` 仍是显式失败。这是预期内的（骨架 + registration 阶段），不是缺陷。下一轮就是把它接上。

### 后续决定

T-025 完成，不需要返工。**Round 7 三个 session 全部通过。** LocalNvmeDataPath 现在具备：SPI 骨架（S2）+ target 打开（S2）+ 真实 memory registration（S3）。下一阶段（Round 8）是 IO 提交与 completion——把 `submit`/`progress`/`query` 从显式失败变成真实的 NVMe 读写，对应接手方案 P0-3 至 P0-13 的修复。
