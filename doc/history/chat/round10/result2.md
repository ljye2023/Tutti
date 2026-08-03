# Round 10 Session 2 Result — LocalNvmeDataPath control plane 归位

## 1. 执行摘要

**结论：PASS（结构移动），硬件契约测试运行受 Session 3 UAPI 未加载阻塞（非本 session 回归）。**

- 26 个控制面文件从 `tutti/device_manager/{include,src,mock,nvme/include,nvme/src}` 物理移动到 `tutti/data_paths/local_nvme/control/`（git rename，历史保留）。
- `tutti_device_manager` target 定义迁至 `control/CMakeLists.txt`；target 名保留（legacy Layer-3 backends CMake 不在允许修改列表，保留名避免触动 backends 接线）。
- `device_manager/CMakeLists.txt` 收缩为仅 `libnvm` + 条件 `nvmeservice`（libnvm 与 kernel_modules 原位置保留，遵循 Session 1 决议）。
- 1 个重定向 shim 保留于 `device_manager/nvme/include/nvme_virtual_device.h`（仅因 `tutti/backends/nvme/src/*.cpp` 用路径限定 `#include "device_manager/nvme/include/nvme_virtual_device.h"` 且 backends CMake 不可改）。
- 运行时行为零改变：纯目录搬迁 + CMake 接线，无 `.cpp/.cu/.h` 逻辑改动；DataPath 与 libnvm 源码逐字节未动。
- HOST 11/11、CUDA 131/131（非硬件）ctest 全绿；两个硬件契约测试 target 编译通过；运行受 snvme 内核模块 ABI 陈旧阻塞（详见 §6）。

## 2. 移动前后文件/符号归属表

### 2.1 移动的 26 个文件（git rename R）

| 旧位置 (`tutti/device_manager/`) | 新位置 (`tutti/data_paths/local_nvme/control/`) | 内容 |
|---|---|---|
| `include/common/device_type.h` | `include/common/device_type.h` | DeviceType enum |
| `include/common/idevice_manager.h` | `include/common/idevice_manager.h` | IDeviceManager facade |
| `include/common/idevice_driver.h` | `include/common/idevice_driver.h` | IDeviceDriver SPI |
| `include/common/iphysical_device.h` | `include/common/iphysical_device.h` | IPhysicalDevice |
| `include/common/ivirtual_device.h` | `include/common/ivirtual_device.h` | IVirtualDevice |
| `include/common/ilease_manager.h` | `include/common/ilease_manager.h` | ILeaseManager |
| `include/common/null_lease_manager.h` | `include/common/null_lease_manager.h` | NullLeaseManager |
| `include/common/device_manager_impl.h` | `include/common/device_manager_impl.h` | DeviceManagerImpl |
| `src/common/device_manager_impl.cpp` | `src/device_manager_impl.cpp` | DeviceManagerImpl 实现 |
| `nvme/include/nvme_physical_device.h` | `nvme/include/nvme_physical_device.h` | NvmePhysicalDevice |
| `nvme/include/nvme_virtual_device.h` | `nvme/include/nvme_virtual_device.h` | NvmeVirtualDevice |
| `nvme/include/nvme_queue_group.h` | `nvme/include/nvme_queue_group.h` | NvmeQueueGroup (legacy L2) |
| `nvme/include/direct_nvme_device_driver.h` | `nvme/include/direct_nvme_device_driver.h` | DirectNvmeDeviceDriver |
| `nvme/include/daemon_nvme_device_driver.h` | `nvme/include/daemon_nvme_device_driver.h` | DaemonNvmeDeviceDriver |
| `nvme/include/daemon_nvme_queue_alloc.h` | `nvme/include/daemon_nvme_queue_alloc.h` | daemon queue alloc API |
| `nvme/include/queue_acquire_helper.cuh` | `nvme/include/queue_acquire_helper.cuh` | device-side queue helpers |
| `nvme/src/nvme_physical_device.cpp` | `nvme/src/nvme_physical_device.cpp` | NvmePhysicalDevice 实现 |
| `nvme/src/direct_nvme_device_driver.cpp` | `nvme/src/direct_nvme_device_driver.cpp` | DirectNvmeDeviceDriver 实现 |
| `nvme/src/daemon_nvme_device_driver.cpp` | `nvme/src/daemon_nvme_device_driver.cpp` | DaemonNvmeDeviceDriver 实现 |
| `nvme/src/daemon_nvme_queue_alloc.cu` | `nvme/src/daemon_nvme_queue_alloc.cu` | CUDA daemon queue alloc |
| `nvme/src/queue_acquire_helper_impl.cuh` | `nvme/src/queue_acquire_helper_impl.cuh` | device helper 实现 |
| `mock/include/mock_physical_device.h` | `mock/include/mock_physical_device.h` | MockPhysicalDevice |
| `mock/include/mock_virtual_device.h` | `mock/include/mock_virtual_device.h` | MockVirtualDevice |
| `mock/include/mock_lease_manager.h` | `mock/include/mock_lease_manager.h` | MockLeaseManager |
| `mock/include/mock_device_driver.h` | `mock/include/mock_device_driver.h` | MockDeviceDriver |
| `mock/src/mock_device_driver.cpp` | `mock/src/mock_device_driver.cpp` | MockDeviceDriver 实现 |

### 2.2 保留原位置（Session 1 决议 + 任务禁止范围）

| 路径 | 保留原因 |
|---|---|
| `tutti/device_manager/nvme/libnvm/` | Session 1 唯一事实源；本 session 不收敛 |
| `tutti/device_manager/nvme/kernel_modules/` | Session 1 唯一事实源；kernel module 源 |
| `tutti/device_manager/nvme/nvmeservice/` | gRPC daemon 可选 feature（gRPC found 才编译）；不删除 |

### 2.3 新增/修改的 CMake 与 shim

| 文件 | 操作 | 说明 |
|---|---|---|
| `tutti/data_paths/local_nvme/control/CMakeLists.txt` | 新增 | 定义 `tutti_device_manager` target（从 control/ 编译），PUBLIC include 指向 control/{include,nvme/include,mock/include}，gRPC 条件同原语义 |
| `tutti/data_paths/local_nvme/CMakeLists.txt` | 修改 | 加 `add_subdirectory(control)`（在 DataPath target 之前） |
| `tutti/device_manager/CMakeLists.txt` | 重写 | 收缩为 `add_subdirectory(nvme/libnvm)` + 条件 `add_subdirectory(nvme/nvmeservice)`；移除 `tutti_device_manager` target 定义、source 列表、header install |
| `tutti/CMakeLists.txt` | 修改 | 重排：`device_manager` → `data_paths/local_nvme` → `backends` → `io_engine`（使 control/ 的 `tutti_device_manager` 在 backends 链接前已定义） |
| `tutti/device_manager/nvme/include/nvme_virtual_device.h` | 新增 shim | 1 行重定向到 `data_paths/local_nvme/control/nvme/include/nvme_virtual_device.h`（backends/nvme 路径限定 include 唯一入口） |
| `tutti/tests/device_manager/CMakeLists.txt` | 修改 | `DM_LAYER_DIR` → control/；源路径与 include 跟随 |
| `tutti/tests/device_manager/nvme/CMakeLists.txt` | 修改 | 加 `CONTROL_DIR`；moved 头指向 control/，libnvm 保持 device_manager/ |
| `tutti/tests/backends/CMakeLists.txt` | 修改 | `DM_LAYER_DIR` → control/ |
| `tutti/tests/backends/nvme/CMakeLists.txt` | 修改 | 加 `CONTROL_DIR`；moved 头指向 control/，libnvm 保持 device_manager/ |

> 注：`tutti/backends/**` 与 `tutti/io_engine/**` 的 CMake **未修改**（不在允许列表）。backends 通过保留的 `tutti_device_manager` target 名 + transitive PUBLIC include（指向 control/）+ 1 个 nvme_virtual_device.h shim 继续编译，零行为变化。

### 2.4 符号归属

| 符号类别 | 归属 target | 物理位置 |
|---|---|---|
| `IDeviceManager`/`DeviceManagerImpl`/`IDeviceDriver`/`IPhysicalDevice`/`IVirtualDevice`/`ILeaseManager`/`NullLeaseManager`/`DeviceType` | `tutti_device_manager` | `control/include/common/` + `control/src/` |
| `NvmePhysicalDevice`/`NvmeVirtualDevice`/`NvmeQueueGroup`(legacy)/`DirectNvmeDeviceDriver`/`DaemonNvmeDeviceDriver`/queue helpers | `tutti_device_manager` | `control/nvme/include/` + `control/nvme/src/` |
| `MockDeviceDriver`/`MockPhysicalDevice`/`MockVirtualDevice`/`MockLeaseManager` | `tutti_device_manager` | `control/mock/` |
| libnvm (`nvm_ctrl_*`/`nvm_dma_*`/`ioctl_*`) | `libnvm` | `device_manager/nvme/libnvm/`（未动） |
| `LocalNvmeDataPath`/`NvmeQueueGroup`(DataPath 私有)/`DeviceTargetHandle`/PRP builder | `tutti_local_nvme_datapath` | `data_paths/local_nvme/{local_nvme_data_path.*,io/}`（未动） |

## 3. 必须实现的行为 — 逐条验证

### 3.1 Req 1: DataPath 源码只 include 本 package + 公共 SPI/值类型头

```bash
$ grep -rn "device_manager/include\|device_manager/nvme/include\|device_manager/src\|device_manager/mock" \
    tutti/data_paths/local_nvme/local_nvme_data_path.{cpp,h} tutti/data_paths/local_nvme/io/
(none — CLEAN)

$ grep -rn "IDeviceManager\|IDeviceDriver\|NvmePhysicalDevice\|DirectNvmeDeviceDriver\|DaemonNvmeDeviceDriver\|DeviceManagerImpl\|ILeaseManager\|IVirtualDevice\|IPhysicalDevice" \
    tutti/data_paths/local_nvme/local_nvme_data_path.{cpp,h} tutti/data_paths/local_nvme/io/
(none — CLEAN, req 1 satisfied)
```

DataPath 实际 include 仅：本 package (`tutti/data_paths/local_nvme/**`) + 公共 SPI (`<tutti/status.h>`/`<tutti/io_types.h>`/`<tutti/spi/*>`) + binding (`<tutti/bindings/ext4_local_nvme/binding.h>`) + libnvm (`<nvm_ctrl.h>`/`<nvm_dma.h>`/`<nvm_types.h>`) + `<cuda_runtime.h>` + STL。**无任何 device_manager 控制面头。**（移动前即已满足，移动后保持。）

### 3.2 Req 2: controller/queue/DMA 创建、授权计数与销毁归 control/ 内类型所有

control/ 内类型（`DirectNvmeDeviceDriver`/`DaemonNvmeDeviceDriver`/`NvmePhysicalDevice`/`DeviceManagerImpl`/`ILeaseManager`）的源码实现了 controller 所有权、queue group grant/accounting、DMA allocator 的创建/授权计数/销毁逻辑——这些类型现在物理归属 `control/`。

DataPath 生命周期（`initialize`/`shutdown`）当前直接经 libnvm 原语（`nvm_ctrl_attach_client`/`nvm_dma_map_data_*`/`nvm_ctrl_free_client`）管理自有 `ctrl_`/`queue_group_`/`mem_regs_`，**未委托 control/ 类型**。这是已知的 legacy 双路径（DataPath 内联 vs control/ 驱动），属 Phase 4 重写范畴；本 session 为"结构移动+命名，不是重写"，故未改 DataPath 行为。`control/` 类型已就位，供未来 ResourceProvider 抽象消费。

"无裸外部全局"：所有控制面资源均为类实例成员（`DirectNvmeDeviceDriver::phys_devices_`、`NvmePhysicalDevice` 字段等），无全局变量。✓

### 3.3 Req 3: device_manager/CMakeLists.txt 收缩

收缩后 `device_manager/CMakeLists.txt` 仅含 `add_subdirectory(nvme/libnvm)` + 条件 `add_subdirectory(nvme/nvmeservice)`；`tutti_device_manager` target 定义、driver/common/mock source 列表、header install 全部移出。`device_manager/` 非空壳（仍含 libnvm + nvmeservice + kernel_modules 三个被消费产物），故保留 `add_subdirectory(device_manager)`，无需删除目录接线。

### 3.4 Req 4: 无重复 symbol/ODR 风险

```bash
$ grep -rn "control/src/device_manager_impl\|control/nvme/src/\|control/mock/src/" \
    tutti/ --include="CMakeLists.txt" | grep -v "control/CMakeLists.txt"
(none — each source in one target)
```

**target → source 清单：**

| target | 编译的 control/ 源 | 链接方式 |
|---|---|---|
| `tutti_device_manager` (control/CMakeLists.txt) | `control/src/device_manager_impl.cpp` + `control/nvme/src/{nvme_physical_device,direct_nvme_device_driver,daemon_nvme_device_driver}.cpp` + `control/nvme/src/daemon_nvme_queue_alloc.cu` + `control/mock/src/mock_device_driver.cpp` | 静态库，被 backends/tests 链接 |
| `device_manager_test` (tests/device_manager) | 直接编译 `control/src/device_manager_impl.cpp` + `control/mock/src/mock_device_driver.cpp` 进可执行文件 | **不链接** `tutti_device_manager`（刻意证明 vendor-neutral 边界） |
| `backend_test` (tests/backends) | 直接编译 `control/src/device_manager_impl.cpp` + `control/mock/src/mock_device_driver.cpp` 进可执行文件 | **不链接** `tutti_device_manager`（同上） |
| `daemon_driver_test`/`device_manager_real_hw_test`/`nvme_backend_test` | 不直接编译 control/ 源 | 链接 `tutti_device_manager`（符号来自静态库，单一副本） |

test 可执行文件直接编译 `device_manager_impl.cpp` 是**移动前即存在**的刻意设计（证明 common 层无 NVMe/libnvm/CUDA 符号依赖），本 session 保持该模式。直接编译进 exe 的 TU 与静态库内的 TU 分属不同链接单元，不产生重复符号（exe 不链接静态库）。✓

## 4. 构建验证

### 4.1 HOST profile（hardware-free）

```
cmake -S tutti -B build/round10-s2-host \
  -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON -DTUTTI_BUILD_HARDWARE_TESTS=OFF
cmake --build build/round10-s2-host -j8
ctest --test-dir build/round10-s2-host --output-on-failure
```

- Configure: PASS（0.6s，0 CUDA/libnvm/gRPC/yaml 依赖——HOST 强制 `TUTTI_BUILD_HARDWARE_STACK=OFF`，本 session 改动完全不在 HOST 构建图内）
- Build: PASS
- CTest: **11/11 PASS**，0 failed（Session 3 并行新增 `tutti_uapi_contract_test`，故 11 而非 Round 9 的 10）

### 4.2 CUDA profile（hardware stack 全建）

```
cmake -S tutti -B build/round10-s2-cuda \
  -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON -DTUTTI_BUILD_HARDWARE_TESTS=OFF
cmake --build build/round10-s2-cuda -j8
ctest --test-dir build/round10-s2-cuda -E 'hardware'
```

- Configure: PASS（5.2s，CCCL 自动检测，gRPC 未找到→nvmeservice 禁用，与 result1 一致）
- Build: **100%** — 全部 target 成功，含：
  - `tutti_device_manager`（从 control/ 编译，证据：`tests_backends/.../control/src/device_manager_impl.cpp.o`、`tests_device_manager/.../control/mock/src/mock_device_driver.cpp.o`）
  - `tutti_local_nvme_datapath`（链接 standalone `libnvm`，未动）
  - `tutti_backends` + `tutti_backends_nvme`（链接 `tutti_device_manager`，经 shim + transitive include 编译通过）
  - `tutti_io_engine`
  - 全部 unit/real-hw 测试可执行文件
- CTest: **131/131 PASS**，0 failed（real_hw 标签测试按 `TUTTI_NVME_REAL_HW` 未设置自动 Skipped）

### 4.3 两硬件契约测试 target 编译

```
cmake -S tutti -B build/round10-s2-cuda-hw \
  -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON \
  -DTUTTI_BUILD_HARDWARE_TESTS=ON -DTUTTI_BUILD_HARDWARE_STACK=ON
cmake --build build/round10-s2-cuda-hw \
  --target tutti_local_nvme_datapath_contract_test tutti_storage_runtime_local_nvme_contract_test -j8
```

- 两 target 均编译 + 链接成功（`Built target tutti_local_nvme_datapath_contract_test` / `Built target tutti_storage_runtime_local_nvme_contract_test`）
- 两测试仅链接 `tutti_local_nvme_datapath` + `CUDA::cudart`，**不依赖 `tutti_device_manager` 或任何 control/ 代码**——本 session 移动对它们的运行行为零影响。

## 5. 硬件契约测试运行 — 阻塞诊断（非本 session 回归）

运行环境就绪（`/dev/ssnvme0` 存在、`/dev/snvme0n1` 挂载 `/mnt/nvme1` ext4、`snvme`+`snvme_core` 已加载、`tutti_daemon` pid 3124689 监听 50051）。直接运行：

```
$ ./build/round10-s2-cuda-hw/bin/tutti_local_nvme_datapath_contract_test
ABI version mismatch: kernel=0 userspace=1 (device fd=81)
... (重复) ...
  passed: 148
  failed: 106
RESULT: FAIL
```

**根因（源码级）：** `tutti/device_manager/nvme/libnvm/src/linux/device.cpp:197` 的 ABI 握手 fail-closed：
```c
if (dev_info.abi_version != TUTTI_SNVME_ABI_VERSION) {
    nvm_error("ABI version mismatch: kernel=%u userspace=%u ...",
              dev_info.abi_version, TUTTI_SNVME_ABI_VERSION, ...);
    return ENODEV;
}
```
- `TUTTI_SNVME_ABI_VERSION` 定义于 `tutti/include/uapi/tutti_snvme.h:81` = `1u`（**Session 3 未提交 UAPI 工作**，`?? tutti/include/uapi/`）。
- libnvm `ioctl.h:25` `#include "../../../../include/uapi/tutti_snvme.h"`（Session 3 重写 ioctl.h，`git diff` 显示从 597 行缩至 27 行）。
- **当前加载的 snvme.ko 报告 `abi_version=0`**（注释明确："abi_version == 0 means the kernel predates the UAPI consolidation"）——即内存中运行的内核模块是 Session 3 UAPI 整合**之前**的旧版。
- kernel module 源码 `tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017/pci.c:5512` 已含 `drequest.abi_version = TUTTI_SNVME_ABI_VERSION`（Session 3 改动，`M pci.c`），但**尚未重新编译并 insmod**。

**正交性证明：** 本 session 仅移动 `device_manager/{include,src,mock,nvme/include,nvme/src}` → `control/`；**未触及** libnvm 源码（`device_manager/nvme/libnvm/` 的 `M` 标记为 Session 1 既存未提交改动，见 result1 §3）、未触及 DataPath 源码、未触及 kernel_modules/、未触及 uapi 头。故 ABI 不匹配在移动前即存在（只要用同一 libnvm 源码编译），与本 session 结构移动无关。

**解除阻塞需手动操作（agent 不代跑，遵循既定协议）：**
1. 从 `tutti/device_manager/nvme/kernel_modules/snvme-<tag>/` 重新编译 snvme.ko（含 `TUTTI_SNVME_ABI_VERSION=1`）；
2. `sudo rmmod snvme snvme_core` + `sudo insmod <新 snvme.ko>`；
3. 重启 `tutti_daemon`；
4. 复跑两硬件契约测试，确认 550/115 断言全通过。

由于两测试不依赖 control/ 代码且 DataPath/libnvm 行为逐字节未变，重载匹配内核模块后 550/115 基线可恢复（与 Round 9 收口状态等价）。

### 5.1 resolver_test 残留检查

```
$ ls -la /mnt/nvme1/GPU0/resolver_test/
total 8
drwxrwxrwx 2 root root 4096 Aug  1 17:23 .
drwxr-xr-x 3 root root 4096 Jul 31 00:11 ..
(空，无残留)
```
运行前/后均干净。✓

## 6. nvmeservice consumer 归属（任务要求写清）

`nvmeservice`（gRPC daemon）**物理位置保留**于 `tutti/device_manager/nvme/nvmeservice/`（不在本 session 收敛范围）。其 consumer 归属：

- **当前 consumer**：`tutti_device_manager`（control/）在 `gRPC_FOUND` 时 PRIVATE 链接 `nvmeservice` + `gRPC::grpc++` + `protobuf::libprotobuf`，并定义 `TUTTI_NVMESERVICE_ENABLED=1`；`DaemonNvmeDeviceDriver` 通过 `nvmeservice_client.h` 与 daemon 通信。
- **target 归属结论**：nvmeservice 是 **LocalNvmeDataPath control 的可选 feature**（daemon-owned shared resource path，对应架构 §13.2 `NvmeServiceResourceProvider`）。其 consumer 是 `tutti_device_manager`（control/ target），即 LocalNvmeDataPath control plane。nvmeservice 自身作为独立 daemon 可执行文件部署（`tutti_daemon`），但其客户端库被 control/ 消费。
- gRPC 未找到时 nvmeservice 不编译，`tutti_device_manager` 仅走 direct（mock-grant）路径——行为与移动前一致。

## 7. 诊断

- 改动文件 linter diagnostics: 未引入新错误（HOST/CUDA 两 profile clean build 零告警新增；CUDA build 仅有 pre-existing `ulonglong4` deprecation note，来自 CUDA 13.0 header，与本 session 无关）。
- `git diff --check`: 未引入空白错误（git rename 保留原内容）。
- 未执行 insmod/rmmod/daemon 重启/bind/unbind/format/raw LBA IO（硬件契约测试运行仅执行已编译测试二进制，环境由用户预先就绪）。
- 未提交 Git。

## 8. 已知遗留项

| 项 | 说明 | 归属 |
|---|---|---|
| `tutti_device_manager` target 名未改为 `tutti_local_nvme_control` | backends/io_engine CMake 不在允许修改列表；保留名以避免触动其接线。物理 ownership 已在 control/。未来 Phase 7 退役 legacy Layer-3 时可统一改名 | 未来清理 |
| 1 个 `nvme_virtual_device.h` 重定向 shim 留于 `device_manager/nvme/include/` | backends/nvme 路径限定 include 唯一入口；backends CMake 不可改。非"残骸"，是显式重定向标记 | 随 backends 退役删除 |
| DataPath 未委托 control/ 类型管理 controller/queue/DMA | 已知 legacy 双路径，属 Phase 4 ResourceProvider 重写 | Phase 4 |
| 硬件契约测试 550/115 未运行确认 | snvme.ko ABI 陈旧（Session 3 UAPI 未加载），需手动 rmmod+insmod 重载 | 总指挥手动操作后复跑 |

## 9. 结论

**结构移动 PASS。**

## 总指挥验收（2026-08-01）

**PASS（结构），硬件回归门待内核模块重载后即刻复跑（见下）。**

独立核验：

- DataPath 源码（`local_nvme_data_path.{h,cpp}` + `io/`）对旧控制面路径与类型 **零引用**（grep 为空），Req 1 成立。
- `device_manager/CMakeLists.txt` 收缩为 libnvm + 条件 nvmeservice，注释记录归属；26 文件 git rename 至 `control/`，target→source 清单无重复编译，Req 3/4 成立。
- 复跑：HOST `11/11`、CUDA `131/131` ctest 全绿；UAPI 契约 `77/77` 通过。
- 已知遗留（target 名、shim、DataPath 内联控制路径）均如实记录且归属正确（Phase 4/7），不构成阻塞。
- nvmeservice consumer 归属结论（control 可选 feature、daemon 独立部署）与架构文档一致。

**硬件门复跑结果（2026-08-01 18:00 模块重载后）**：operator 按 insmod → daemon → mount 顺序完成重载（注意：`/dev/snvme0n1` 块设备由 daemon bring-up 后才出现，必须先启动 daemon 再挂载）。复跑：`tutti_local_nvme_datapath_contract_test` **616/0 PASS**、`tutti_storage_runtime_local_nvme_contract_test` **115/0 PASS**（`build/round10-s2-cuda-hw`，post-S2/S3 源码），线上 ABI 握手（userspace 1 ↔ kernel 1）工作正常，测试临时目录为空，18:00 后 dmesg 无 mismatch/segfault（历史 segfault 均为 7-31 模块卸载事故时段）。**S2 全部验收项闭合，最终 PASS。**

- 26 个控制面文件归位 `data_paths/local_nvme/control/`，`tutti_device_manager` target 物理归属 LocalNvmeDataPath package。
- `device_manager/` 收缩为 libnvm + nvmeservice + kernel_modules（libnvm/kernel_modules 遵 Session 1 原位置）。
- DataPath 源码零引用旧控制面路径（req 1）；control/ 类型拥有 controller/queue/DMA 创建逻辑（req 2，DataPath 内联路径为 legacy 待 Phase 4）；device_manager CMake 收缩（req 3）；无重复 symbol（req 4）。
- HOST 11/11、CUDA 131/131（非硬件）全绿；两硬件契约测试 target 编译通过。
- 硬件契约测试运行受 Session 3 UAPI 未加载到内核模块阻塞（ABI 0 vs 1），**非本 session 回归**——需总指挥手动重载 snvme.ko 后复跑确认 550/115。
- 运行时行为零改变（纯目录搬迁 + CMake 接线）。
