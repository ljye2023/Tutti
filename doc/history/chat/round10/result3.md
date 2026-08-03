# Round 10 Session 3 Result: snvme 共享 UAPI 头与 ABI 握手

## 概述

建立了 `tutti/include/uapi/tutti_snvme.h` 作为 snvme kernel module 与 libnvm userspace 的唯一共享 UAPI 头文件。所有 ioctl struct 使用固定宽度类型，每个 struct 有 `_Static_assert` 锁定 LP64 布局，并新增 ABI version/capability 握手机制（fail-closed）。

## 改动文件清单

### 新建

| 文件 | 用途 |
|------|------|
| `tutti/include/uapi/tutti_snvme.h` | 共享 UAPI 头：全部 struct/ioctl 定义、固定宽度类型、static_assert、ABI version/capability、32/64-bit compat 策略文档 |
| `tests/uapi_contract/CMakeLists.txt` | UAPI 契约测试 CMake 配置 |
| `tests/uapi_contract/uapi_contract_test.cpp` | 硬件无关 UAPI 契约测试（77 项检查） |

### 修改

| 文件 | 改动 |
|------|------|
| `tutti/device_manager/nvme/libnvm/include/ioctl.h` | 替换为薄包装，`#include "../../../../include/uapi/tutti_snvme.h"` |
| `tutti/device_manager/nvme/libnvm/CMakeLists.txt` | 添加 `${PROJECT_SOURCE_DIR}/include/uapi` 到 PUBLIC include 路径 |
| `tutti/device_manager/nvme/libnvm/src/linux/device.cpp` | `ioaddrs` 赋值加 `(uint64_t)(uintptr_t)` cast；`ioctl_get_dev_info` 和 `nvm_wait_dev_info` 加 ABI 版本握手 fail-closed 检查 |
| `tutti/device_manager/nvme/kernel_modules/snvme-5.15.0-public/pci.c` | `ioaddrs` cast 加 `(uintptr_t)`；NVM_GET_DEV_INFO handler 填充 `abi_version` 和 `capabilities` |
| `tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017/pci.c` | 同上 |
| `tutti/CMakeLists.txt` | 注册 `tests/uapi_contract` 到 hardware-free 测试列表 |

## UAPI Struct/断言清单

### 固定宽度类型迁移

| Struct | 原 width-dependent 字段 | 迁移后 | LP64 布局变化 |
|--------|------------------------|--------|--------------|
| `nvm_ioctl_map` | `size_t n_pages` | `uint64_t n_pages` | 无（LP64: size_t == uint64_t == 8B） |
| `nvm_ioctl_map` | `uint64_t* ioaddrs` | `uint64_t ioaddrs` | 无（LP64: 指针 == uint64_t == 8B） |
| `nvm_ioctl_map` | `int ioq_idx, is_cq` | `int32_t ioq_idx, is_cq` | 无（LP64: int == int32_t == 4B） |
| `nvm_ioctl_dev` | `size_t max_data_size` | `uint64_t max_data_size` | 无 |
| `nvm_ioctl_dev` | `size_t block_size` | `uint64_t block_size` | 无 |
| `pci_device_addr` | `int domain/bus/slot/func` | `int32_t domain/bus/slot/func` | 无 |

### static_assert 清单

| Struct | sizeof 断言 | offset 断言数 |
|--------|------------|--------------|
| `nvm_ioctl_map` | == 40 | 6 (vaddr_start@0, n_pages@8, ioaddrs@16, ioq_idx@24, group_id@32, map_kind@36) |
| `nvm_ioctl_dev` | == 104 | 11 (nr_user_q@0, start_cq_idx@4, dstrd@8, max_data_size@16, block_size@24, disk_name@32, q_depth@64, bar0_size@68, max_user_qid@72, sgl_supported@80, abi_version@84, capabilities@88) |
| `nvm_ioctl_setup` | == 160 | 4 (ioq_num@0, cap_kernel_ioq@8, nr_groups@20, groups@32) |
| `pci_device_addr` | == 16 | 4 (domain@0, bus@4, slot@8, func@12) |
| `nvm_ioctl_raw_admin` | == 92 | 3 (sqe@0, result_dw0@64, nvme_status@72) |
| `nvm_ioctl_queue_group` | == 32 | 2 (group_id@0, max_queues@8) |
| `nvm_user_queue_pair_in` | == 16 | 1 (sq_vaddr@0) |
| `nvm_user_queue_pair_out` | == 16 | 1 (sq_doorbell_offset@0) |
| `nvm_ioctl_add_user_queue` | == 544 | 4 (group_id@0, nr_pairs@4, pairs@32, out_pairs@288) |
| `nvm_queue_group` | == 16 | 3 (owner_id@0, count@4, numa_node@8) |

### ABI 版本与 Capability

```
TUTTI_SNVME_ABI_VERSION = 1

Capability bits:
  TUTTI_SNVME_CAP_QUEUE_GROUPS    = (1 << 0)  — NVM_CREATE/DESTROY_QUEUE_GROUP
  TUTTI_SNVME_CAP_USER_QUEUES     = (1 << 1)  — NVM_ADD_USER_QUEUE
  TUTTI_SNVME_CAP_RAW_ADMIN       = (1 << 2)  — NVM_RAW_ADMIN_CMD
  TUTTI_SNVME_CAP_KERNEL_IOQ_CAP  = (1 << 3)  — NVM_SET_KERNEL_IOQ_CAP
  TUTTI_SNVME_CAP_MAP_KIND_TAG    = (1 << 4)  — B6 map_kind discrimination

TUTTI_SNVME_CAP_ALL = 0x1F (bitwise OR of all above)
```

`struct nvm_ioctl_dev` 中 `reserved1[0]` 和 `reserved1[1]` 被重命名为 `abi_version` 和 `capabilities`（同一 offset、同一 size，非布局变更）。kernel 在 NVM_GET_DEV_INFO handler 中填充这两个字段。

## 握手 fail-closed 路径证据

### 路径 1：`ioctl_get_dev_info()`（legacy bring-up）

`device.cpp:178-196`:
```cpp
if (dev_info.abi_version != TUTTI_SNVME_ABI_VERSION) {
    nvm_error("ABI version mismatch: kernel=%u userspace=%u ...",
              (unsigned)dev_info.abi_version,
              (unsigned)TUTTI_SNVME_ABI_VERSION, ...);
    return ENODEV;  // fail-closed
}
```

### 路径 2：`nvm_wait_dev_info()`（B3 bring-up）

`device.cpp:594-602`:
```cpp
if (out_info->abi_version != TUTTI_SNVME_ABI_VERSION) {
    nvm_error("ABI version mismatch (B3): kernel=%u userspace=%u ...", ...);
    return ENODEV;  // fail-closed
}
```

### Fail-closed 场景

| 场景 | kernel abi_version | userspace TUTTI_SNVME_ABI_VERSION | 结果 |
|------|-------------------|----------------------------------|------|
| 版本匹配 | 1 | 1 | PASS，继续初始化 |
| 旧 kernel（pre-UAPI） | 0（memset 零） | 1 | FAIL，返回 ENODEV |
| 未来版本 | 2 | 1 | FAIL，返回 ENODEV |
| 降级版本 | 1 | 2 | FAIL，返回 ENODEV |

### 单测验证

`tests/uapi_contract/uapi_contract_test.cpp` 的 `test_handshake_fail_closed()` 测试用例覆盖了以上全部场景：
- Case 1: matching version → pass
- Case 2: old kernel (v=0) → fail-closed
- Case 3: future version → fail-closed
- Case 4: capabilities bitmask 检查
- Case 5: 缺少需要的 capability → 可检测

## 两边引用同一物理头文件的证据

### Include 解析链

```
Kernel side:
  pci.c:37 → #include "ioctl.h"
    → found via -I.../libnvm/include/ (Makefile.in @module_ccflags@)
    → libnvm/include/ioctl.h:24 → #include "../../../../include/uapi/tutti_snvme.h"
    → resolves to tutti/include/uapi/tutti_snvme.h  ✓

Userspace side:
  device.cpp:21 → #include "ioctl.h"
    → found via libnvm PUBLIC include path (CMakeLists.txt)
    → libnvm/include/ioctl.h:24 → #include "../../../../include/uapi/tutti_snvme.h"
    → resolves to tutti/include/uapi/tutti_snvme.h  ✓
```

### 物理文件

```
$ ls -la tutti/include/uapi/tutti_snvme.h
-rw-r--r-- 1 root root 27675 Aug  1 17:12 tutti/include/uapi/tutti_snvme.h
```

### 删除的重复定义

审计确认：kernel module 目录中**从未存在** `ioctl.h` 的本地副本。两个 `pci.c` 都通过 `#include "ioctl.h"` 引用 `libnvm/include/ioctl.h`，而该文件现在转发到唯一的 `tutti_snvme.h`。因此无需删除任何重复定义——重复定义在改动前就不存在。

唯一需要说明的是 `NVM_CTRL_IOCTL_TYOE`（原始 header 中的 typo）被保留为 `NVM_CTRL_IOCTL_TYPE` 的 alias，以避免破坏可能引用旧名称的 out-of-tree 代码。这不是重复定义——ioctl 命令号由 magic 值 (0x90) 决定，与宏名无关。

## Kernel compile-only 日志

```
$ cd snvme-5.4.241-1-tlinux4-0017
$ make  # kernel 5.4.241-1-tlinux4-0017.7

  CC [M]  core.o
  CC [M]  multipath.o
  LD [M]  snvme-core.o
  CC [M]  nvfs-pci.o
  CC [M]  nvfs-p2p.o        (需临时 nv-p2p.h stub，本机无 NVIDIA driver dev header)
  CC [M]  list.o
  CC [M]  ctrl.o
  CC [M]  map.o
  CC [M]  pci.o             ← 包含 UAPI 改动的文件，零 error
  LD [M]  snvme.o
  Building modules, stage 2.
  MODPOST 2 modules
  LD [M]  snvme-core.ko
  LD [M]  snvme.ko          ← 模块链接成功

Exit code: 0
```

`pci.o` 编译零 error、零与 UAPI 改动相关的 warning。仅有 pre-existing warning（unused variable `i`、unused `curr_ctrls`）。

注：`nv-p2p.h`（NVIDIA driver peer-to-peer header）在本机不可用，编译验证使用了临时 stub。stub 仅提供类型声明，不影响 pci.c 的编译验证有效性。验证后 stub 已删除。

## HOST/CUDA build 验证

### HOST profile

```
$ cmake -S tutti -B /tmp/tutti-uapi-build -DBUILD_TESTING=ON -DTUTTI_ACCELERATOR=HOST
$ cmake --build /tmp/tutti-uapi-build
$ cd /tmp/tutti-uapi-build && ctest

100% tests passed, 0 tests failed out of 11
Total Test time = 0.03 sec

Tests:
  1. cuda_like_contract_test  ........................   Passed
  2. tutti_public_api_usage_test  ....................   Passed
  3. tutti_spi_consumer_test  .........................   Passed
  4. tutti_data_path_contract_test  ...................   Passed
  5. tutti_storage_runtime_contract_test  .............   Passed
  6. tutti_status_contract_test  ......................   Passed
  7. tutti_memory_types_contract_test  ................   Passed
  8. tutti_io_types_contract_test  ....................   Passed
  9. tutti_storage_target_resolver_contract_test  .....   Passed
 10. tutti_binding_contract_test  .....................   Passed
 11. tutti_uapi_contract_test  ........................   Passed  ← 新增
```

### CUDA profile

```
$ cmake -S tutti -B tutti/build-profile-cuda -DTUTTI_ACCELERATOR=CUDA
$ cmake --build tutti/build-profile-cuda --target libnvm

[100%] Built target libnvm  ← 含 device.cpp 的 ioaddrs cast + ABI 握手，零 error 零 warning

$ cmake --build tutti/build-profile-cuda --target tutti_uapi_contract_test
$ ./bin/tutti_uapi_contract_test
All 77 UAPI contract checks passed.
```

## UAPI 契约测试覆盖

`tests/uapi_contract/uapi_contract_test.cpp` — 77 项检查，8 个测试函数：

| 测试函数 | 检查项 | 验证内容 |
|---------|-------|---------|
| `test_abi_version` | 2 | TUTTI_SNVME_ABI_VERSION == 1 且 != 0 |
| `test_capability_bits` | 15 | 5 个 cap bit 各为 2 的幂且互不重叠；CAP_ALL == OR(所有 bit) |
| `test_struct_sizes` | 10 | 10 个 struct 的 sizeof 匹配 LP64 布局 |
| `test_field_offsets` | 16 | 关键字段 offset 匹配 LP64 布局 |
| `test_fixed_width_types` | 7 | 字段 sizeof 确认无 width-dependent 类型 |
| `test_ioctl_numbers` | 9 | magic/nr/dir 编码正确；typo alias 一致 |
| `test_constants` | 9 | DISK_NAME_LEN、NVM_MAX_* 等常量值稳定 |
| `test_handshake_fail_closed` | 9 | 模拟握手 5 种场景（匹配/旧 kernel/未来/有cap/缺cap） |

## 32/64-bit compat 策略

已在 `tutti_snvme.h` 尾部以文档化注释落地。要点：

1. **全部 struct 在 32/64 compat 下布局不变**：所有字段已迁移为固定宽度类型（`uint64_t` 代替 `size_t` 和指针），无 `long`/`size_t`/指针宽度依赖。
2. **无 `compat_ptr` 需求**：`nvm_ioctl_map::ioaddrs` 原为 `uint64_t*`（指针），现为 `uint64_t`（整数），在 32/64 位均为 8 字节。kernel 通过 `(void __user *)(uintptr_t)` cast 访问。
3. **`.compat_ioctl` 注册**：Session 4 需为 `snvm_dev_fops` 和 `snvm_fops` 注册 `.compat_ioctl` 入口，使 32-bit syscall 路由到正确 handler。
4. **标准 NVMe ioctl**（`NVME_IOCTL_*`，由 `ioctl.c` 处理）已有上游 compat 处理（`COMPAT_FOR_U64_ALIGNMENT`/`nvme_user_io32`），非 snvme 专属，Session 4 验证即可。
5. **转换实现**留给 Session 4。

## 总指挥验收（2026-08-01）

**PASS（代码与构建），线上握手与硬件回归门待内核模块重载后复跑。**

独立核验：

- `tutti/include/uapi/tutti_snvme.h`：`TUTTI_SNVME_ABI_VERSION=1`、5 个 capability bit + `CAP_ALL`、全套 `_Static_assert`（sizeof + 字段 offset）在位；`nvm_ioctl_dev` 的 `abi_version`/`capabilities` 复用 reserved 字段，LP64 布局零变更声明与断言一致。
- userspace 握手 fail-closed 双路径（`ioctl_get_dev_info`、`nvm_wait_dev_info`）均为 `!= → ENODEV`；kernel 侧 `pci.c:5512-5513` 填充 `TUTTI_SNVME_ABI_VERSION` + `CAP_ALL`；两侧经 `libnvm/include/ioctl.h` 转发包含同一物理头。
- 复跑 `tutti_uapi_contract_test`：**77/77 通过**；HOST `11/11`、CUDA `131/131` 全绿。
- 内核模块由总指挥经根 build `modules` target 重编：使用真实 `/usr/src/nvidia-580.65.06/nvidia/nv-p2p.h`（**不需要 stub**——worker 手动编译缺 `-I` 才需要 stub，根 build 的 ccflags 已含正确路径），仅有 §已知限制 声明的 pre-existing warnings；`vermagic 5.4.241-1-tlinux4-0017.7` 与运行内核精确匹配。
- 32/64-bit compat 策略以注释+断言落地，转换实现正确推迟到 Session 4。

**握手真实验证已完成（2026-08-01 18:00 模块重载后）**：userspace abi=1 ↔ kernel abi=1 握手通过，两硬件契约 616/115 全 PASS（详见 result2.md 验收节复跑记录）。旧内核（abi=0）被拒的 fail-closed 路径也已由 17:38 前的阻塞运行实证。**S3 全部验收项闭合，最终 PASS。**

## 已知限制

1. `nv-p2p.h`（NVIDIA driver P2P header）在本机不可用，kernel module compile-only 使用了临时 stub。生产构建需安装 NVIDIA driver dev package。
2. `pci_device_addr` 的 `int32_t` 字段在 `sscanf("%x")` 中技术上是 UB（`%x` 期望 `unsigned int*`），但 LP64 上 `int32_t == int`，行为正确。这是 pre-existing 问题，本 session 不修。
3. `ioctl.h` 通过相对路径 `../../../../include/uapi/tutti_snvme.h` 引用共享头。如果目录结构变化，需同步更新。CMake 也添加了 `${PROJECT_SOURCE_DIR}/include/uapi` 到 libnvm 的 PUBLIC include 路径，供直接 `#include <tutti_snvme.h>` 使用。
