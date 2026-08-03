# Round 10 Session 4 Result: snvme kernel compat 与 GPU pinning 隔离

## 概述

将 snvme kernel module 中的内核版本差异收进显式 `compat` 单元，将 GPU pinning 收进 `peer_memory_ops` 隔离单元。模块主体（`map.c`、`pci.c`）只调用稳定内部接口，零散落版本判断、零直接 `nvidia_p2p_*` 调用。全程 compile-only，未加载模块。

两个 kernel baseline（5.4.241 + 5.4.203）compile-only 通过；userspace（libnvm / tutti_daemon）build 不回归。

## 改动文件清单

### 新建（两棵树均添加，内容相同）

| 文件 | 用途 |
|------|------|
| `snvme-*/compat.h` | compat 单元头文件：声明 `compat_get_user_pages()`，文档化所有 `LINUX_VERSION_CODE`/`KERNEL_VERSION` 条件分支集中于此 |
| `snvme-*/compat.c` | compat 单元实现：**唯一**包含 `LINUX_VERSION_CODE`/`KERNEL_VERSION` 条件分支的翻译单元（`get_user_pages` API 3 段版本分支） |
| `snvme-*/peer_memory.h` | peer_memory 单元头文件：声明 `struct peer_memory_ops`（8 个函数指针）、opaque 类型 `peer_page_table`/`peer_dma_mapping`、2 个 field accessor、`extern const struct peer_memory_ops peer_memory_ops` |
| `snvme-*/peer_memory.c` | peer_memory 单元实现：**唯一**包含 `nv-p2p.h` 和 `nvidia_p2p_*` 调用的翻译单元；吸收原 `nvfs-p2p.c` 的 `__symbol_get` 逻辑；ops 表 + opaque 类型 cast + `#error` guard |

### 删除（两棵树均删除）

| 文件 | 原因 |
|------|------|
| `snvme-*/nvfs-p2p.c` | 逻辑吸收进 `peer_memory.c`（ops 表模式替代扁平 wrapper） |
| `snvme-*/nvfs-p2p.h` | 被 `peer_memory.h` 替代（opaque 类型 + ops 表） |

### 修改

| 文件 | 改动 |
|------|------|
| `snvme-*/map.c` | 移除 `#include <linux/version.h>` 和 `#include "nvfs-p2p.h"`，改 include `peer_memory.h`+`compat.h`；`struct gpu_region` 类型从 `nvidia_p2p_*_t` 改为 opaque `peer_*`；`get_user_pages` 版本分支替换为 `compat_get_user_pages()`；全部 `nvfs_nvidia_p2p_*` 调用改为 `peer_memory_ops.*`；`->entries`/`->dma_addresses` 字段访问改为 accessor；注释更新 |
| `snvme-*/map.h` | 注释中 `nvidia_p2p_put_pages`/`nvidia_p2p_free_dma_mapping` 改为 `peer_memory_ops.*` |
| `snvme-*/pci.c` | `#include "nvfs-p2p.h"` 改为 `#include "peer_memory.h"`；`nvfs_nvidia_p2p_init/exit()` 改为 `peer_memory_ops.init/exit()`；注释和 pr_err 字符串更新 |
| `snvme-*/Makefile.in` | `snvme-objs` 中 `nvfs-p2p.o` 替换为 `peer_memory.o compat.o`；注释更新 |
| `scripts/compile_snvme_baselines.sh` | 新建：三 baseline compile-only 矩阵脚本 |

## compat 单元接口清单

### `compat.h`

| 接口 | 签名 | 用途 |
|------|------|------|
| `compat_get_user_pages` | `long (unsigned long start, unsigned long nr_pages, int write, struct page **pages)` | 内核版本可移植的 `get_user_pages()` wrapper |

**版本分支（全部集中在 `compat.c`）**：

| 条件 | API 签名 |
|------|---------|
| `LINUX_VERSION_CODE <= KERNEL_VERSION(4, 5, 7)` | `get_user_pages(ts, mm, start, nr, write, force, pages, vmas)` |
| `LINUX_VERSION_CODE <= KERNEL_VERSION(4, 8, 17)` | `get_user_pages(start, nr, write, force, pages, vmas)` |
| else (>= 4.9) | `get_user_pages(start, nr, FOLL_WRITE, pages, vmas)` |

**Feature-probe 宏（非版本号判断，文档化在 `compat.h`）**：

| 宏 | 探测方式 | 消费位置 |
|----|---------|---------|
| `HAVE_BLK_MARK_DISK_DEAD` | Makefile.in grep `Module.symvers` | `pci.c` |
| `HAVE_MODULE_MUTEX` | （未定义=不使用 module_mutex） | `peer_memory.c` |

## peer_memory 单元接口清单

### `peer_memory.h` — opaque 类型

| 类型 | 实际定义（仅 peer_memory.c 可见） |
|------|------|
| `struct peer_page_table` | = `struct nvidia_p2p_page_table`（来自 `nv-p2p.h`） |
| `struct peer_dma_mapping` | = `struct nvidia_p2p_dma_mapping`（来自 `nv-p2p.h`） |

### `peer_memory.h` — `struct peer_memory_ops`

| 字段 | 签名 | 对应 NVIDIA 符号 |
|------|------|-----------------|
| `init` | `int (*)(void)` | `__symbol_get("nvidia_p2p_*")` × 6 |
| `exit` | `void (*)(void)` | `__symbol_put("nvidia_p2p_*")` × 6 |
| `get_pages` | `int (*)(uint64_t, uint32_t, uint64_t, uint64_t, struct peer_page_table**, void(*)(void*), void*)` | `nvidia_p2p_get_pages` |
| `put_pages` | `int (*)(uint64_t, uint32_t, uint64_t, struct peer_page_table*)` | `nvidia_p2p_put_pages` |
| `dma_map_pages` | `int (*)(struct pci_dev*, struct peer_page_table*, struct peer_dma_mapping**)` | `nvidia_p2p_dma_map_pages` |
| `dma_unmap_pages` | `int (*)(struct pci_dev*, struct peer_page_table*, struct peer_dma_mapping*)` | `nvidia_p2p_dma_unmap_pages` |
| `free_dma_mapping` | `int (*)(struct peer_dma_mapping*)` | `nvidia_p2p_free_dma_mapping` |
| `free_page_table` | `int (*)(struct peer_page_table*)` | `nvidia_p2p_free_page_table` |

### `peer_memory.h` — field accessor

| 函数 | 返回 | 对应 NVIDIA 字段 |
|------|------|-----------------|
| `peer_memory_pt_entries(const struct peer_page_table*)` | `uint32_t` | `nvidia_p2p_page_table::entries` |
| `peer_memory_dm_addresses(const struct peer_dma_mapping*)` | `const uint64_t*` | `nvidia_p2p_dma_mapping::dma_addresses` |

### NVIDIA 头缺失 guard

```c
#if defined(__has_include)
#  if __has_include("nv-p2p.h")
#    include "nv-p2p.h"
#  else
#    error "nv-p2p.h not found: NVIDIA P2P headers are required ..."
#  endif
#else
#  include "nv-p2p.h"
#endif
```

缺失 `nv-p2p.h` 时编译期 `#error`，不会静默编出无 pin 能力的模块。

## 主体零版本判断 grep 证据

在 `tutti/device_manager/nvme/kernel_modules/` 下执行（排除 `.diff` 文件）：

### 1. LINUX_VERSION_CODE / KERNEL_VERSION（主体零散落）

```console
$ grep -rn "LINUX_VERSION_CODE\|KERNEL_VERSION" \
    snvme-5.4.241-1-tlinux4-0017/ snvme-5.15.0-public/ \
    --include="*.c" --include="*.h" \
  | grep -v "compat\." | grep -v "core.c.*Ubuntu ABI"
(空 — 仅 compat.c 含条件分支)
```

**唯一命中（均为注释/文档，非代码分支）**：
- `compat.h:4` — 文档注释："ALL LINUX_VERSION_CODE / KERNEL_VERSION conditional branches..."
- `compat.c` — 3 段 `#if/#elif/#else` 条件（get_user_pages 版本分支）
- `snvme-5.15.0-public/core.c:140` — 注释："not on LINUX_VERSION_CODE -- the Ubuntu ABI number is not the"（解释为何用 feature-probe 而非版本号）

### 2. nvidia_p2p_ / nv_p2p_（主体零直接调用）

```console
$ grep -rn "nvidia_p2p_\|nv_p2p_" \
    snvme-5.4.241-1-tlinux4-0017/ snvme-5.15.0-public/ \
    --include="*.c" --include="*.h" \
  | grep -v "peer_memory" | grep -v "\.diff"
(none)
```

所有 `nvidia_p2p_*`/`nv_p2p_*` 调用集中在 `peer_memory.c`；主体 `map.c`/`pci.c` 经 `peer_memory_ops.*` 间接调用。

### 3. nvfs_nvidia_p2p / nvfs-p2p（旧接口完全移除）

```console
$ grep -rn "nvfs_nvidia_p2p\|nvfs-p2p" \
    snvme-5.4.241-1-tlinux4-0017/ snvme-5.15.0-public/ \
    --include="*.c" --include="*.h" --include="*.in" \
  | grep -v "\.diff"
snvme-*/peer_memory.c:49: * runtime via __symbol_get.  Previously declared in nvfs-p2p.h; moved
```

唯一命中为 `peer_memory.c` 内部注释（记录历史来源），非代码引用。

## 两 baseline compile 矩阵

脚本：`scripts/compile_snvme_baselines.sh`（直接生成 kbuild Makefile，绕过项目级 CMake 依赖）

| ID | 源码树 | Kernel Headers | 结果 | 警告 |
|----|--------|---------------|------|------|
| A | snvme-5.4.241-1-tlinux4-0017 | 5.4.241-1-tlinux4-0017.7 | **PASS** | 2 (pre-existing) |
| B | snvme-5.4.241-1-tlinux4-0017 | 5.4.203-1-tlinux4-0011.3 | **PASS** | 2 (pre-existing) |
| C | snvme-5.15.0-public | 5.4.241-1-tlinux4-0017.7 | **FAIL** | — |

### Pre-existing 警告（Session 4 未引入）

- `pci.c:4998` — `unused variable 'i'` `[-Wunused-variable]`
- `pci.c:251` — `'curr_ctrls' defined but not used` `[-Wunused-variable]`

两警告均在 Baseline A/B 中出现，位于 pci.c 中 Session 4 未修改的代码段。

### Baseline C 失败归因

Baseline C（5.15 源码 + 5.4 headers）失败，错误集中在 **`nvme.h`**（5.15 版本头文件）：

- `blk_should_fake_timeout` — 5.15 block API，5.4 无此函数
- `blk_mq_complete_request_remote` — 5.15 block API
- `trace_block_bio_complete` — 5.15 参数签名变化
- `report_zones_cb` — 5.15 block zone 类型
- `NVME_CC_CSS_MASK` / `NVME_CC_CSS_CSI` — 5.15 NVMe 常量

**失败原因**：5.15 源码树是完整的 5.15 移植（`nvme.h`/`pci.c`/`core.c` 为 5.15 专用），其 API 与 5.4 headers 不兼容。这是两棵树结构性的版本隔离（per-tree full port），**不是 compat 单元的问题**。

**隔离价值证明**：`compat.c`、`peer_memory.c`、`map.c` 为两棵树共享的版本无关文件，在 Baseline A/B 中均编译通过（跨 5.4.241 和 5.4.203 两个 kernel 版本无需修改）。Baseline C 的失败发生在 tree-specific 文件（`nvme.h` → `core.c`），编译在到达 `peer_memory.c`/`compat.c` 之前即终止。

### Userspace 不回归

```console
$ cmake --build build --target libnvm tutti_daemon -j8
[100%] Built target libnvm
[100%] Built target tutti_daemon
```

libnvm（含 `tutti_snvme.h` UAPI 头）和 tutti_daemon 均编译链接成功，无新错误/警告。UAPI 头（`tutti/include/uapi/tutti_snvme.h`）未修改。

## NVIDIA 头依赖风险记录

| 风险 | 描述 | 当前缓解 |
|------|------|---------|
| **路径硬编码** | `nv-p2p.h` 来自 `/usr/src/nvidia-580.65.06/nvidia/`，由 CMake `find_path(driver_include NAMES "nv-p2p.h" PATHS /usr/src/nvidia-*)` 自动探测 | 探测逻辑通配 `nvidia-*`，升级驱动版本时自动适配；但若 NVIDIA 改变头文件安装路径或结构，需手动调整 |
| **版本耦合** | `peer_memory.c` 使用 `nvidia_p2p_page_table`/`nvidia_p2p_dma_mapping` 的字段（`entries`、`dma_addresses`），这些是 NVIDIA 驱动 UAPI 的一部分 | 字段访问集中在 `peer_memory.c` 的 2 个 accessor 函数中；若 NVIDIA 变更 struct 布局，只需修改 peer_memory.c |
| **运行时符号依赖** | `nvidia_p2p_*` 符号在 module init 时 `__symbol_get` 解析；NVIDIA 驱动未加载时 init 失败 → 模块加载失败（`-EOPNOTSUPP`） | fail-closed 语义保持原样；`peer_memory_ops.*` 在符号未解析时返回 `-ENOMEM` |
| **编译期头缺失** | 若 `nv-p2p.h` 不存在，`peer_memory.c` 触发 `#error`（`__has_include` guard） | 不会静默编出无 pin 能力的模块 |
| **Phoenix P2P 共存** | `map.c` 中的 Phoenix (`phxfs_p2p_*`) 路径未移入 peer_memory 单元（它不是 `nvidia_p2p_*` 依赖，是独立的函数指针路径） | Phoenix 路径已通过 `phx_register_fn`/`phx_deregister_fn` 函数指针隔离，与 peer_memory_ops 并存；语义未改变 |

## 隔离边界总结

```
模块主体 (map.c, pci.c, core.c, ...)
    │
    ├── 版本差异 ──► compat_get_user_pages()  [compat.c: 唯一 LINUX_VERSION_CODE 所在]
    │
    └── GPU pinning ──► peer_memory_ops.*     [peer_memory.c: 唯一 nv-p2p.h / nvidia_p2p_* 所在]
                          ├── opaque types (peer_page_table / peer_dma_mapping)
                          ├── accessors (peer_memory_pt_entries / peer_memory_dm_addresses)
                          └── #error guard (nv-p2p.h 缺失时编译失败)
```

## 总指挥验收（2026-08-01）

**PASS（含运行时验证）。**

独立核验：

- **grep 证据**：主体零 `LINUX_VERSION_CODE` 散落、零 `nvidia_p2p_*` 直接调用（复核为空）；两树 `compat.*`/`peer_memory.*` 字节一致。
- **编译矩阵复跑**：A（5.4.241+5.4.241 headers）PASS、B（5.4.241+5.4.203 headers）PASS——`compat.o`/`peer_memory.o`/`map.o` 跨 headers 编译干净；C（5.15+5.4 headers）FAIL 于首个对象 `core.o` 的树专属 `nvme.h`（74 个 error 全部在 5.15 API），编译未触及隔离单元，失败归因成立，满足 prompt 对第二 baseline 的要求。
- **peer_memory.c 审查**：6 个 `nvidia_p2p_*` 符号 `__symbol_get` 运行时解析、`__has_include`+`#error` guard、ops 表单一导出，与报告一致。
- **map.c diff 审查**：纯机械替换（ops 调用、opaque 类型、accessor、compat wrapper），pin/unpin 时机、引用计数、错误路径零漂移；移除的旧内核 `#warning` 为良性。
- **运行时验证**：operator 按 insmod→daemon→mount 重载 S4 重编模块（srcversion `954A0AF6243DF7810FDE7DB` 与 sysfs 一致），复跑两硬件契约 **616/0 + 115/0 PASS**，refactored `peer_memory_ops` pinning 路径经真实 GPU P2P DMA 验证；重载后 dmesg 零异常。
- **userspace 不回归**：libnvm/tutti_daemon 重编通过，UAPI 头一字节未动（grep 确认）。
- **总指挥顺手修复**：S4 引入的 4 行行尾空白（两树 `Makefile.in`/`map.c`）已清除，`git diff --check` clean，两树 `map.c` 仍一致。

## 未改动项

- **UAPI 头**（`tutti/include/uapi/tutti_snvme.h`）：一字节未动
- **pinning 语义**：pin/unpin 时机、引用计数、错误路径保持原样（仅移动与封装）
- **NVIDIA 头引用路径**：`/usr/src/nvidia-580.65.06/nvidia` 保持原样
- **Git**：未提交
