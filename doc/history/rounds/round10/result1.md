# Round 10 Session 1 Result — libnvm/snvme 双树事实源审计与唯一 source owner 收敛

## 1. 执行摘要

**结论：PASS**

- 唯一 source owner 选定：`tutti/device_manager/nvme/{libnvm,kernel_modules}/`
- root 树 `backends/local/nvme/libnvm/` 和 `backends/local/kernel_modules/` 已 `git rm -rf` 删除
- 根 build 与 standalone build 均从同一批源文件编译 libnvm（compile_commands.json 证据见 §5）
- 不再存在第二份 libnvm/snvme 实现（`find` + `git grep` 确认）
- 未提交改动无丢失（root 树的未提交改动与 tutti 树等价，详见 §3）
- 运行时行为零改变（纯路径重定向 + 删除重复源，无 .cpp/.cu/.h 逻辑改动）

## 2. 双树逐文件差异审计表

### 2.1 libnvm 源码（`backends/local/nvme/libnvm/` vs `tutti/device_manager/nvme/libnvm/`）

两树文件结构完全一致：`include/` 22 个 `.h`，`src/` 14 个文件，`src/linux/` 2 个文件。`diff -rq` 结果：**只有 2 个文件不同**，其余全部字节相同。

| 文件 | 工作树差异 | root 树未提交改动 | tutti 树未提交改动 | 结论 | 归并决议 |
|---|---|---|---|---|---|
| `include/nvm_cmd.h` | **不同** | 无（HEAD 版） | 有：+`#include <string.h>` +`nvm_cmd_clear()` 静态内联（SQE 零初始化） | **仅 tutti 树有修复** | 保留 tutti 树（新版）；root 树无未提交改动，删除无损失 |
| `include/nvm_parallel_queue.h` | **不同** | 无（HEAD 版） | 有：+`NVM_CQ_TIMEOUT` 宏 +`cq_poll_bounded()` 函数（CQ 超时保护） | **仅 tutti 树有修复** | 保留 tutti 树（新版）；root 树无未提交改动，删除无损失 |
| `src/ctrl.cpp` | 相同 | 有：删 `nvm_queue_clear()` 调用 +注释（kernel 无 NVM_CLEAR_IOQ_NUM handler） | 有：**完全等价**（blob hash `8a24e23..2410032` 两树一致） | **等价修复** | 保留 tutti 树；root 树改动不丢失 |
| `src/linux/device.cpp` | 相同 | 有：`printf`→`nvm_error()` +`return saved_errno` | 有：**完全等价**（blob hash `da3432b..155f4a3` 两树一致） | **等价修复** | 保留 tutti 树；root 树改动不丢失 |
| 其余 36 个文件（20 include + 12 src + 2 linux） | 相同 | 无 | 无 | **完全相同** | 保留 tutti 树 |
| `CMakeLists.txt` | N/A | 无 | 有（standalone 构建定义） | 仅 tutti 树有 | 保留 tutti 树 |

**未提交改动等价性结论**：root 树仅有的 2 个未提交文件（`ctrl.cpp`、`device.cpp`）与 tutti 树**字节级等价**（相同 git blob hash）。root 树的 `nvm_cmd.h` 和 `nvm_parallel_queue.h` **无未提交改动**（HEAD 版本），删除不会丢失任何用户改动。

### 2.2 snvme kernel_modules（`backends/local/kernel_modules/` vs `tutti/device_manager/nvme/kernel_modules/`）

两树目录结构一致：`snvme-5.15.0-public/`、`snvme-5.4.241-1-tlinux4-0017/`、`test/`、`PORTING.md`。`diff -rq` 结果：8 个文件不同 + root 树有额外编译产物（gitignored）和 1 个额外源文件。

| 文件 | 差异类型 | 归并决议 |
|---|---|---|
| `snvme-5.15.0-public/Makefile.in` | **仅 tutti 树有修复**：+`HAVE_BLK_MARK_DISK_DEAD` 检测（`blk_set_queue_dying`→`blk_mark_disk_dead` 兼容，v5.17 回移） | 保留 tutti 树 |
| `snvme-5.15.0-public/core.c` | **仅 tutti 树有修复**：+`#ifdef HAVE_BLK_MARK_DISK_DEAD` 条件编译 | 保留 tutti 树 |
| `snvme-5.15.0-public/multipath.c` | **仅 tutti 树有修复**：同上条件编译 | 保留 tutti 树 |
| `test/Makefile` | **root 树有额外内容**：root 有 `snvme_smoke_recycle` target；`UAPI_INC` 路径不同（root: `../../nvme/libnvm/include`，tutti: `../../libnvm/include`） | 保留 tutti 树（已移除 recycle target，路径已适配 tutti 树结构） |
| `test/snvme_smoke.c` | **仅 tutti 树有修复**：+`SNVME_TEST_KERNEL_IOQ_CAP` env 覆盖 | 保留 tutti 树 |
| `test/snvme_smoke_addq.c` | **仅 tutti 树有修复**：同上 | 保留 tutti 树 |
| `test/snvme_smoke_gpu.cu` | **仅 tutti 树有修复**：同上 | 保留 tutti 树 |
| `test/snvme_smoke_io.c` | **仅 tutti 树有修复**：同上 | 保留 tutti 树 |
| `test/snvme_smoke_recycle.c` | **仅 root 树有**（git 跟踪），tutti 树无此文件 | tutti 树的 `test/Makefile` 已主动移除 `recycle` target。删除 root 树后此文件消失。这是 tutti 树的已决定状态（recycle 测试已退役）。如需恢复可从 git 历史检出。 |
| `test/snvme_smoke*`（无扩展名） | root 树有编译产物 | gitignored，已清理 |
| `snvme-5.4.241-1-tlinux4-0017/*` | 两树相同 | 保留 tutti 树 |
| `PORTING.md` | 两树相同（均含旧路径引用，属文档） | 保留 tutti 树 |

**kernel_modules 未提交改动**：两棵树的 `git status` 均为空（无未提交改动），删除 root 树无丢失风险。

## 3. 未提交改动审计（归并前置条件）

任务要求"动手前必须先 `git diff` 审计两树间未提交改动是否等价"。

| 树 | 文件 | 未提交改动内容 | 与 tutti 树等价？ |
|---|---|---|---|
| root `backends/local/nvme/libnvm/src/ctrl.cpp` | 删 `nvm_queue_clear(ctrl)` +注释 | **是**（blob `8a24e23..2410032`） |
| root `backends/local/nvme/libnvm/src/linux/device.cpp` | `printf`→`nvm_error()` +`return saved_errno` | **是**（blob `da3432b..155f4a3`） |
| root `backends/local/nvme/libnvm/include/nvm_cmd.h` | 无未提交改动 | N/A（root 是旧版） |
| root `backends/local/nvme/libnvm/include/nvm_parallel_queue.h` | 无未提交改动 | N/A（root 是旧版） |
| root `backends/local/kernel_modules/*` | 无未提交改动 | N/A |
| tutti `.../libnvm/src/ctrl.cpp` | 同上 | **是** |
| tutti `.../libnvm/src/linux/device.cpp` | 同上 | **是** |
| tutti `.../libnvm/include/nvm_cmd.h` | +`nvm_cmd_clear()` +`#include <string.h>` | 仅 tutti 树有（新版） |
| tutti `.../libnvm/include/nvm_parallel_queue.h` | +`NVM_CQ_TIMEOUT` +`cq_poll_bounded()` | 仅 tutti 树有（新版） |

**结论**：root 树的未提交改动**全部**在 tutti 树中有等价或更新版本。删除 root 树不会丢失任何用户未提交改动。归并方向：**tutti 树为唯一事实源**。

## 4. 归并执行

### 4.1 CMake 路径重定向（4 个文件，9 处）

| 文件 | 行 | 旧路径 | 新路径 |
|---|---|---|---|
| `CMakeLists.txt` | 70 (注释) | `backends/local/nvme/libnvm/src/` | `tutti/device_manager/nvme/libnvm/src/` |
| `CMakeLists.txt` | 147-148 | `backends/local/nvme/libnvm/{src,include}` | `tutti/device_manager/nvme/libnvm/{src,include}` |
| `CMakeLists.txt` | 177 | `backends/local/nvme/libnvm/` | `tutti/device_manager/nvme/libnvm/` |
| `CMakeLists.txt` | 187 (注释) | `backends/local/kernel_modules/snvme-<tag>/` | `tutti/device_manager/nvme/kernel_modules/snvme-<tag>/` |
| `CMakeLists.txt` | 199 | `backends/local/kernel_modules` | `tutti/device_manager/nvme/kernel_modules` |
| `CMakeLists.txt` | 254 | `backends/local/nvme/libnvm/include` | `tutti/device_manager/nvme/libnvm/include` |
| `device_manager/CMakeLists.txt` | 34 | `backends/local/nvme/libnvm/include` | `tutti/device_manager/nvme/libnvm/include` |
| `memory/CMakeLists.txt` | 22 | `backends/local/nvme/libnvm/include` | `tutti/device_manager/nvme/libnvm/include` |
| `backends/local/nvme/test/Makefile` | 38 | `$(REPO_ROOT)/backends/local/nvme/libnvm/include` | `$(REPO_ROOT)/tutti/device_manager/nvme/libnvm/include` |

### 4.2 root 树删除

```
git rm -rf backends/local/nvme/libnvm/      # 38 files (libnvm 源码)
git rm -rf backends/local/kernel_modules/   # 56 files (snvme kernel modules + test)
rm -rf backends/local/kernel_modules/test/  # 清理 gitignored 编译产物残留
```

删除后 `backends/local/` 仅保留 `NVMeService/`（本 session 不收敛，Session 2 处理）和 `nvme/test/`（L1 libnvm smoke，路径已重定向）。

### 4.3 配置期分叉保护

删除 root 树后，任何后续"只改一边"在物理上不可能——因为只有 `tutti/device_manager/nvme/` 一份源码存在。根 build 和 standalone build 的 CMake 路径都指向同一目录，configure 期即从同一 glob 结果取源文件。

## 5. 两 build 引用同一事实源 — 命令级证据

### 5.1 compile_commands.json 对比

```bash
# ROOT build (project libnvm, from repo root)
python3 -c "
import json
with open('build/round10-root/compile_commands.json') as f:
    cmds = json.load(f)
for e in cmds:
    if 'libnvm' in e['file'] and ('ctrl.cpp' in e['file'] or 'device.cpp' in e['file'] or 'queue.cpp' in e['file']):
        print('ROOT ', e['file'])
"
# 输出：
# ROOT  /data/home/ryeqiu/Tutti/tutti/device_manager/nvme/libnvm/src/ctrl.cpp
# ROOT  /data/home/ryeqiu/Tutti/tutti/device_manager/nvme/libnvm/src/queue.cpp
# ROOT  /data/home/ryeqiu/Tutti/tutti/device_manager/nvme/libnvm/src/linux/device.cpp

# CUDA standalone build (cmake -S tutti)
python3 -c "
import json
with open('build/round10-cuda/compile_commands.json') as f:
    cmds = json.load(f)
for e in cmds:
    if 'libnvm' in e['file'] and ('ctrl.cpp' in e['file'] or 'device.cpp' in e['file'] or 'queue.cpp' in e['file']):
        print('CUDA ', e['file'])
"
# 输出：
# CUDA  /data/home/ryeqiu/Tutti/tutti/device_manager/nvme/libnvm/src/ctrl.cpp
# CUDA  /data/home/ryeqiu/Tutti/tutti/device_manager/nvme/libnvm/src/linux/device.cpp
# CUDA  /data/home/ryeqiu/Tutti/tutti/device_manager/nvme/libnvm/src/queue.cpp
```

**两 build 的 libnvm 源文件路径完全相同**——均来自 `tutti/device_manager/nvme/libnvm/src/`。

### 5.2 根 build configure 日志（snvme baseline 从 tutti 树）

```
-- Using snvme kernel baseline: 5.4.241-1-tlinux4-0017
   (/data/home/ryeqiu/Tutti/tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017)
-- Configuring module build in /data/home/ryeqiu/Tutti/build/round10-root/module
-- Configuring libnvm without SmartIO
```

### 5.3 根 build verbose（libnvm target 从 tutti 树编译）

```
Dependencies file "CMakeFiles/libnvm.dir/tutti/device_manager/nvme/libnvm/src/ctrl.cpp.o.d" ...
Dependencies file "CMakeFiles/libnvm.dir/tutti/device_manager/nvme/libnvm/src/linux/device.cpp.o.d" ...
Dependencies file "CMakeFiles/libnvm.dir/tutti/device_manager/nvme/libnvm/src/queue.cpp.o.d" ...
```

### 5.4 不再存在第二份 libnvm/snvme 实现

```bash
$ find . -type d -name libnvm -not -path './build/*' -not -path './third_pkgs/*'
./tutti/device_manager/nvme/libnvm          # 唯一事实源

$ find . -type d -name kernel_modules -not -path './build/*' -not -path './third_pkgs/*'
./tutti/device_manager/nvme/kernel_modules  # 唯一事实源

$ find . -type d -name 'snvme-*' -not -path './build/*' -not -path './third_pkgs/*'
./tutti/device_manager/nvme/kernel_modules/snvme-5.15.0-public
./tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017

$ git grep -l "" -- 'backends/local/nvme/libnvm/*'
# (empty — root 树已删除)
```

## 6. 构建验证

### 6.1 根 build（project libnvm, repo root）

```
cmake -S . -B build/round10-root \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE=third_pkgs/vcpkg/scripts/buildsystems/vcpkg.cmake
```

- **Configure**: PASS（2.6s）— gRPC/protobuf/yaml-cpp/libunwind 均通过 vcpkg 找到
- **核心 target build**: PASS

```
cmake --build build/round10-root --target libnvm nvmeservice tutti_memory tutti_device_manager modules -j8
```

| Target | 结果 | 源 |
|---|---|---|
| `libnvm` | PASS | `tutti/device_manager/nvme/libnvm/src/*.cpp` |
| `nvmeservice` | PASS | `backends/local/NVMeService/src/*.cpp` + proto |
| `tutti_memory` | PASS | `memory/src/*.cu` |
| `tutti_device_manager` | PASS | `device_manager/src/*.cpp` + `device_manager/src/*.cu` |
| `modules` | PASS | `tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017/` |

**预先存在的非回归问题**：`layer1_smoke_test`（根 `tests/CMakeLists.txt`）链接失败，错误 `cannot find -ltutti_accel`。原因：根 `CMakeLists.txt:343` 强制 `TUTTI_BUILD_HARDWARE_STACK=OFF` → `tutti_accel` target 不创建，但 `tests/layer1_smoke_test.cu` 引用之。**此问题在归并前已存在**（`TUTTI_BUILD_HARDWARE_STACK` 一直是 `FORCE OFF`），与 libnvm/snvme 归并无关。属 Phase 7（legacy 树退役）范畴。

### 6.2 standalone HOST（`TUTTI_ACCELERATOR=HOST`）

```
cmake -S tutti -B build/round10-host \
  -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON \
  -DTUTTI_BUILD_HARDWARE_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round10-host -j8
ctest --test-dir build/round10-host --output-on-failure
```

- **Configure**: PASS（0.6s）— 0 CUDA/libnvm/gRPC/yaml 依赖
- **Build**: PASS — 10 个测试可执行文件
- **CTest**: **10/10 PASS**，0 failed

| # | Test | 结果 |
|---|---|---|
| 1 | cuda_like_contract_test | Passed |
| 2 | tutti_public_api_usage_test | Passed |
| 3 | tutti_spi_consumer_test | Passed |
| 4 | tutti_data_path_contract_test | Passed |
| 5 | tutti_storage_runtime_contract_test | Passed |
| 6 | tutti_status_contract_test | Passed |
| 7 | tutti_memory_types_contract_test | Passed |
| 8 | tutti_io_types_contract_test | Passed |
| 9 | tutti_storage_target_resolver_contract_test | Passed |
| 10 | tutti_binding_contract_test | Passed |

### 6.3 standalone CUDA（`TUTTI_ACCELERATOR=CUDA`）

```
cmake -S tutti -B build/round10-cuda \
  -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON \
  -DTUTTI_BUILD_HARDWARE_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round10-cuda -j8
ctest --test-dir build/round10-cuda --output-on-failure -E 'hardware'
```

- **Configure**: PASS（5.8s）— CCCL include 自动检测，hardware stack target 全部创建
- **Build**: PASS — 全部 target（含 `tutti_local_nvme_datapath` 链接 standalone `libnvm`）
- **CTest**: **130/130 PASS**，0 failed（hardware tests 按 `TUTTI_BUILD_HARDWARE_TESTS=OFF` + `TUTTI_NVME_REAL_HW` 未设置自动 Skipped）

## 7. 前后 git status 对比

### 7.1 归并前（BEFORE）

```
 M .gitignore
 M CMakeLists.txt
 M README.md
 M Roadmap.md
 M backends/local/nvme/libnvm/src/ctrl.cpp           # root 树未提交改动（与 tutti 等价）
 M backends/local/nvme/libnvm/src/linux/device.cpp   # root 树未提交改动（与 tutti 等价）
 D doc/history/README.md
 M tutti/CMakeLists.txt
 M tutti/build.sh
 M tutti/device_manager/nvme/libnvm/include/nvm_cmd.h          # tutti 树未提交改动（新版）
 M tutti/device_manager/nvme/libnvm/include/nvm_parallel_queue.h # tutti 树未提交改动（新版）
 M tutti/device_manager/nvme/libnvm/src/ctrl.cpp               # tutti 树未提交改动
 M tutti/device_manager/nvme/libnvm/src/linux/device.cpp        # tutti 树未提交改动
 M tutti/tests/io_engine/CMakeLists.txt
?? COMMUNITY_MEETING_001.md
?? MAIN_IO_PATH.md
?? MAIN_MEMORY_PRP_PATH.md
?? TUTTI_REFACTOR_TAKEOVER.md
?? TUTTI_TARGET_ARCHITECTURE.md
?? tutti/bindings/
?? tutti/cmake/
?? tutti/data_paths/
?? tutti/include/
?? tutti/resolvers/
```

**特征**：两棵 libnvm 树的 `src/ctrl.cpp`、`src/linux/device.cpp` 同时显示为 modified——同一份修复被打了两遍，dual-tree 危害是活的事实。

### 7.2 归并后（AFTER）

```
 M .gitignore
 M CMakeLists.txt                                      # 路径重定向（+注释更新）
 M README.md
 M Roadmap.md
 D doc/history/README.md
 M device_manager/CMakeLists.txt                        # include 路径重定向
 M memory/CMakeLists.txt                                # include 路径重定向
 M tutti/CMakeLists.txt
 M tutti/build.sh
 M tutti/device_manager/nvme/libnvm/include/nvm_cmd.h  # 保留（唯一事实源）
 M tutti/device_manager/nvme/libnvm/include/nvm_parallel_queue.h  # 保留
 M tutti/device_manager/nvme/libnvm/src/ctrl.cpp       # 保留（唯一事实源）
 M tutti/device_manager/nvme/libnvm/src/linux/device.cpp  # 保留
 M tutti/tests/io_engine/CMakeLists.txt
 M backends/local/nvme/test/Makefile                    # LIBNVM_INC 路径重定向
D  backends/local/kernel_modules/PORTING.md            # 删除（root 树）
D  backends/local/kernel_modules/snvme-5.15.0-public/* # 删除（root 树，~25 文件）
D  backends/local/kernel_modules/snvme-5.4.241-1-tlinux4-0017/*  # 删除（root 树，~20 文件）
D  backends/local/kernel_modules/test/*                # 删除（root 树）
D  backends/local/nvme/libnvm/include/*                # 删除（root 树，22 文件）
D  backends/local/nvme/libnvm/src/*                    # 删除（root 树，16 文件）
?? COMMUNITY_MEETING_001.md
?? MAIN_IO_PATH.md
?? MAIN_MEMORY_PRP_PATH.md
?? TUTTI_REFACTOR_TAKEOVER.md
?? TUTTI_TARGET_ARCHITECTURE.md
?? tutti/bindings/
?? tutti/cmake/
?? tutti/data_paths/
?? tutti/include/
?? tutti/resolvers/
```

**特征**：
- `backends/local/nvme/libnvm/*` 和 `backends/local/kernel_modules/*` 全部变为 `D`（deleted，staged）
- 不再有"两树同时 modified"的 dual-tree 危害
- 新增 `device_manager/CMakeLists.txt`、`memory/CMakeLists.txt`、`backends/local/nvme/test/Makefile` 的路径重定向
- tutti 树的 4 个未提交改动保留不变（唯一事实源）

## 8. 已知遗留项（非本 session 范围）

| 项 | 说明 | 归属 |
|---|---|---|
| `snvme_smoke_recycle.c` | 仅 root 树有，tutti 树 `test/Makefile` 已移除 recycle target。删除 root 树后此文件消失 | 如需恢复可 `git show HEAD:backends/local/kernel_modules/test/snvme_smoke_recycle.c` |
| `layer1_smoke_test` 链接失败 | `cannot find -ltutti_accel`，预先存在（`TUTTI_BUILD_HARDWARE_STACK` 强制 OFF） | Phase 7（legacy 树退役） |
| `backends/local/NVMeService/` | 仍从根 build 编译，未收敛 | Session 2（control plane 归位） |
| 文档中的旧路径引用 | `PORTING.md`、`NVMeService.md`、`doc/build_and_test.md` 等仍引用 `backends/local/...` 旧路径 | 文档更新属后续清理 |
| `backends/local/nvme/test/Makefile` 注释 | line 4/11 注释仍引用 `backends/local/kernel_modules/test/`（描述性，不影响编译） | 文档更新属后续清理 |

## 9. 诊断

- 改动文件 linter diagnostics: 未引入新错误
- `git diff --check`: 未引入空白错误
- 未执行 insmod/rmmod/daemon/mount/bind/format/raw LBA IO
- 未提交 Git

## 10. 结论

**PASS**

## 总指挥验收（2026-08-01）

**PASS。** 独立复跑与抽查全部通过：

- **等价性审计**：两树 HEAD blob 同为 `8a24e23`（root 删除 diff 与 tutti 未提交 diff 对比证实），tutti 工作区保留 `2410032` 修复版，未提交改动无丢失；kernel_modules 两树均无未提交改动。
- **唯一事实源**：`find` 确认 libnvm/kernel_modules 仅剩 `tutti/device_manager/nvme/`；tutti 树源码改动仅原有 4 文件，无新增逻辑改动；残留旧路径仅 `backends/local/nvme/test/Makefile` 2 行注释（§8 已声明，不影响编译）。
- **三端构建复跑**：根 build `libnvm`/`nvmeservice`/`modules` 全 PASS（`snvme.ko`+`snvme-core.ko` 从重定向后的 tutti 树编译产出）；standalone HOST `10/10`；standalone CUDA `130/130`（hardware 按配置 skip）。
- **硬件无回归**：S1 后重跑 `tutti_local_nvme_datapath_contract_test` **616/0**、`tutti_storage_runtime_local_nvme_contract_test` **115/0**，测试临时目录为空。
- `git diff --check` clean，CMake 改动诊断 0。
- 分叉保护以「物理上只剩一份源码」实现，满足 prompt 的 configure 期发现要求；`layer1_smoke_test` 链接失败经核对确为预先存在（`TUTTI_BUILD_HARDWARE_STACK` FORCE OFF），不属本 session 回归，留 Phase 7。

**S2（control plane 归位）与 S3（UAPI）解除阻塞，可按依赖图并行启动。**

- 唯一 source owner：`tutti/device_manager/nvme/{libnvm,kernel_modules}/`
- root 树 `backends/local/nvme/libnvm/` + `backends/local/kernel_modules/` 已删除
- 根 build + standalone HOST + standalone CUDA 三者从同一批源文件编译 libnvm（compile_commands.json 证据）
- HOST 10/10 + CUDA 130/130 ctest 全通过，无回归
- 未提交改动无丢失（root 树改动与 tutti 树等价）
- 运行时行为零改变（纯路径重定向 + 删除重复源）
