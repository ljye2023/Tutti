# T-017 Diagnostic Report: `ioctl_queue_helper err is -1` (×4)

## 1. Q1: daemon 实际链接哪份 libnvm

### 结论

daemon 链接的是 `backends/local/nvme/libnvm/` 副本。

### 证据

```
$ ldd build/bin/tutti_daemon | grep libnvm
	libnvm.so => /data/home/ryeqiu/Tutti/build/bin/../lib/libnvm.so

$ nm -CD build/lib/libnvm.so | grep nvm_ctrl_free
000000000000e0a0 T nvm_ctrl_free

$ addr2line -e build/lib/libnvm.so -C -f 0xe0a0
nvm_ctrl_free
/data/home/ryeqiu/Tutti/backends/local/nvme/libnvm/src/ctrl.cpp:224
```

`addr2line` 将 `nvm_ctrl_free` 符号定位到 `backends/local/nvme/libnvm/src/ctrl.cpp:224`，而非 `tutti/device_manager/nvme/libnvm/src/ctrl.cpp`。

### 双副本风险

两份 libnvm 源码树内容逐字节一致（subagent 已验证），但存在两份独立副本：

| 副本 | 路径 |
|------|------|
| A（实际链接） | `backends/local/nvme/libnvm/` |
| B（未链接） | `tutti/device_manager/nvme/libnvm/` |

风险：修改 A 而忘记同步 B（或反之）会导致行为不一致。两份副本的 `device.cpp:327` 都有同一条 `printf`。

### 额外发现

`nvmeservice` 库（`libnvmeservice.so`）中的 `~ServiceState()` 析构函数也来自 `backends/local/` 树：

```
$ addr2line -e build/lib/libnvmeservice.so -C -f 0x69a230
nvmeservice::ServiceState::~ServiceState()
/data/home/ryeqiu/Tutti/backends/local/NVMeService/src/nvmeservice_state.cu:97
```

这是一个 `.cu` 文件（不是 `.cpp`），被 `file(GLOB ... src/*.cu)` 纳入编译。

---

## 2. Q2: 4 次失败的完整调用链

### 调用链

```
examples/tutti_daemon.cpp:153     return 0;
                                  → state (shared_ptr<ServiceState>) 离开作用域
                                  → ~ServiceState()

backends/local/NVMeService/src/nvmeservice_state.cu:97-108
  ~ServiceState()
    for (auto& dev : devices_) {        // 4 devices = 4 iterations
        nvmeservice_state.cu:104       nvm_ctrl_free(dev.ctrl);
    }

backends/local/nvme/libnvm/src/ctrl.cpp:222-232
  nvm_ctrl_free(ctrl)
    ctrl.cpp:226                       nvm_queue_clear(ctrl);  // 返回值被忽略

backends/local/nvme/libnvm/src/linux/device.cpp:372-374
  nvm_queue_clear(ctrl)
    return ioctl_queue_helper(ctrl, 0, NVM_CLEAR_IOQ_NUM);

backends/local/nvme/libnvm/src/linux/device.cpp:277-332
  ioctl_queue_helper(ctrl, 0, NVM_CLEAR_IOQ_NUM)
    device.cpp:299-304                 case NVM_CLEAR_IOQ_NUM:
                                        struct nvm_ioctl_dev request;
                                        memset(&request, 0, sizeof(request));
                                        err = ioctl(fd_dev, NVM_CLEAR_IOQ_NUM, &request);
    device.cpp:326-329                 if (err < 0) {
                                          printf("ioctl_queue_helper err is %d\n", err);
                                          return errno;
                                      }
```

### 时序解释

`examples/tutti_daemon.cpp:145-153`:
```cpp
std::cout << "Shutting down...\n";       // line 145
server->Shutdown();                      // line 147
server->Wait();                          // line 148
state->stop_reaper();                    // line 149
std::cout << "tutti_daemon exited cleanly.\n";  // line 150
return 0;                                // line 153 → state 析构
```

`state` 是 `std::shared_ptr<ServiceState>`（`tutti_daemon.cpp:97`），在 `return 0` 后离开 `{}` 作用域（`tutti_daemon.cpp:96`），触发 `~ServiceState()`。因此 4 条错误出现在 "tutti_daemon exited cleanly." **之后**。

### 调用入口

三个入口中只有 `nvm_queue_clear()` 被调用：

| 入口 | 何时调用 | 本场景 |
|------|----------|--------|
| `nvm_queue_clear(ctrl)` → `NVM_CLEAR_IOQ_NUM` | `nvm_ctrl_free()` teardown | **被调用，失败** |
| `nvm_queue_set(ctrl, q_num)` → `NVM_SET_IOQ_NUM` | setup（B3 bring-up 不使用） | 未调用 |
| `nvm_queue_share(ctrl)` → `NVM_SET_SHARE_REG` | setup（B3 bring-up 不使用） | 未调用 |

---

## 3. Q3: 真正的 errno

### 根因：内核模块未实现 `NVM_CLEAR_IOQ_NUM` handler

内核模块 `snvm_dev_map_ioctl()`（`pci.c:4986-6132`）的 switch 语句中 **没有 `case NVM_CLEAR_IOQ_NUM:`**。

已实现的 case 列表（`pci.c` snvme-5.4.241）：

| 行号 | case |
|------|------|
| 5007 | `NVM_MAP_HOST_MEMORY` |
| 5132 | `NVM_MAP_DEVICE_MEMORY` |
| 5231 | `NVM_UNMAP_HOST_MEMORY` |
| 5292 | `NVM_UNMAP_DEVICE_MEMORY` |
| 5331 | `NVM_UNMAP_DEVICE_QUEUE_MEMORY` |
| 5345 | `NVM_GET_DEV_INFO` |
| 5513 | `NVM_RAW_ADMIN_CMD` |
| 5603 | `NVM_CREATE_QUEUE_GROUP` |
| 5702 | `NVM_DESTROY_QUEUE_GROUP` |
| 5754 | `NVM_ADD_USER_QUEUE` |
| 6068 | `NVM_SET_KERNEL_IOQ_CAP` |

**缺失**：`NVM_CLEAR_IOQ_NUM`、`NVM_SET_IOQ_NUM`、`NVM_SET_SHARE_REG`。

snvme-5.15.0-public 版本同样缺失（`grep -c 'case NVM_CLEAR_IOQ_NUM'` = 0）。

### default 分支行为

`pci.c:6093-6128`:
```c
default:
    if (_IOC_TYPE(cmd) == 'N') {
        ret = -ENOTTY;       // line 6122
    } else {
        pr_notice("snvme: unknown /dev/ssnvme ioctl 0x%x from pid %d\n", cmd, current->pid);
        ret = -EINVAL;       // line 6126
    }
    break;
```

`NVM_IOCTL_TYPE` = `0x80`（`ioctl.h:14`），不是 `'N'`（0x4e）。因此走 `else` 分支：

- 内核返回 `-EINVAL`
- 内核打印 `pr_notice("snvme: unknown /dev/ssnvme ioctl 0x%x from pid %d\n", ...)`
- 用户态 `ioctl()` 返回 `-1`，`errno = EINVAL` (22)

### 候选 errno 逐个评估

| 候选 errno | 支持证据 | 反对证据 | 判定 |
|-----------|----------|----------|------|
| `EINVAL` | `NVM_IOCTL_TYPE=0x80 ≠ 'N'` → default else 分支 → `-EINVAL` (`pci.c:6126`) | — | **最可能** |
| `ENOTTY` | default 分支有 `ENOTTY` 路径 | 仅当 `_IOC_TYPE == 'N'` 时；`NVM_IOCTL_TYPE=0x80` 不匹配 | 排除 |
| `ENODEV` | 设备可能已被 unbind | `nvm_queue_clear` 在 `nvm_device_unbind` **之前**调用，设备仍 bound | 排除 |
| `EBADF` | fd 可能在 teardown 中已关闭 | `nvm_queue_clear` 在 `_nvm_ctrl_put`（关 fd）**之前**调用 | 排除 |
| `EBUSY` | 设备可能有活跃 queue | daemon 已 Stop 所有 client，无活跃 queue | 无证据 |

### 结论

**最可能 errno = `EINVAL` (22)。** 置信度：高（源码可证）。

`ioctl_queue_helper` 打印 `err`（`ioctl()` 返回值，恒为 `-1`）而非 `errno`（`device.cpp:327`），导致真实错误码被吞没。`return errno` 带走了 `errno` 但无人消费（`nvm_ctrl_free` 忽略返回值）。

---

## 4. Q4: teardown 顺序分析

### `nvm_ctrl_free()` 顺序（`ctrl.cpp:222-232`）

```
1. nvm_queue_clear(ctrl)       — NVM_CLEAR_IOQ_NUM ioctl  → 失败 (EINVAL)，返回值被忽略
2. nvm_device_unbind(ctrl)     — SNVM_DEVICE_UNBIND ioctl → 解除 PCI 绑定
3. nvm_chrdev_remove(...)      — SNVM_CHRDEV_REMOVE ioctl → 拆除 /dev/ssnvmeN
4. _nvm_ctrl_put(container)    — close fd_dev + fd_control
```

### 顺序是否正确？

顺序本身**没有倒置**：先清 queue 计数 → 再 unbind → 再拆 chrdev → 最后关 fd。这是合理的 teardown 顺序。

问题不在顺序，而在于 `nvm_queue_clear` 试图调用的 `NVM_CLEAR_IOQ_NUM` ioctl 在内核侧**从未实现**。这不是"先 unbind 再 clear"的顺序倒置，而是"clear 这个 ioctl 根本不存在"。

### 日志出现在 "exited cleanly" 之后的原因

`~ServiceState()` 在 `return 0` 之后由 shared_ptr 析构触发。此时 gRPC server 已 Shutdown+Wait，reaper 已 stop。析构函数遍历 4 个设备，每个设备调用 `nvm_ctrl_free` → `nvm_queue_clear` → 失败 → printf。这些 printf 输出到 stdout，在 "tutti_daemon exited cleanly." 之后出现。

---

## 5. Q5: 影响面评估

### 是否导致内核 IO queue 资源泄漏？

**不会。** `NVM_CLEAR_IOQ_NUM` 的用途是"reset accounting counters"（`pci.c:4436` 注释）。但内核从未实现这个 handler，说明内核侧的 queue 资源清理不依赖此 ioctl。实际的资源清理由后续的 `nvm_device_unbind`（unbind 时内核释放所有 queue）和 `_nvm_ctrl_put`（close fd 触发 `snvm_dev_release` cascade-cleanup）完成。

### 是否导致设备无法被后续 bind？

**不会。** `nvm_device_unbind` 在 `nvm_queue_clear` 之后执行，成功解除 PCI 绑定。下一轮 daemon 启动时会重新执行 B3 bind+probe。实测：daemon 多次启动/退出均正常。

### 上一轮「无害噪声」判断是否成立？

**结论成立，但理由错误。**

| | 上一轮判断 | 实际情况 |
|---|-----------|----------|
| 结论 | 无害噪声 | 无害噪声（正确） |
| 理由 | "驱动 unbind 清理噪声" | 内核未实现 `NVM_CLEAR_IOQ_NUM` handler，ioctl 恒失败 |
| 时机 | 暗示发生在 unbind 过程中 | 发生在 unbind **之前**，在 `~ServiceState()` 析构中 |
| errno | 未查证 | `EINVAL`（可从源码确定） |

### 是否仍应修？

**是。** 理由：

1. **可诊断性**：`printf` 打印 `err`（恒为 -1）而非 `errno`，使诊断被卡住。应打印 `errno`。
2. **内核日志噪声**：每次 daemon 退出，内核 `pr_notice` 打印 4 条 "unknown ioctl" 消息到 dmesg。
3. **代码一致性**：同一文件中 `nvm_queue_setup()`（`device.cpp:349-369`）打印 `errno`，而 `ioctl_queue_helper` 打印 `err`，风格不一致。
4. **死代码**：`nvm_queue_clear(ctrl)` 调用一个永远失败的 ioctl，返回值被忽略，实际效果是 no-op + 日志噪声。

---

## 6. Q6: 最小修复方案

### 第一步：只提升可诊断性（不改行为）

**文件**：两份 `device.cpp`（必须同步修改）

| 文件 | 行号 | 改动 |
|------|------|------|
| `backends/local/nvme/libnvm/src/linux/device.cpp` | 327 | `printf("ioctl_queue_helper err is %d\n", err)` → `printf("ioctl_queue_helper err is %d, errno=%d (%s), type=%d\n", err, errno, strerror(errno), (int)type)` |
| `tutti/device_manager/nvme/libnvm/src/linux/device.cpp` | 327 | 同上 |

**不需要重编内核模块**，只需重编 `libnvm.so` 并重启 daemon。

**验证**：重启 daemon → SIGTERM → 确认日志打印 `errno=22 (Invalid argument), type=10`（`NVM_CLEAR_IOQ_NUM` = 10）。

**风险**：极低。仅修改日志格式，不改控制流。

### 第二步：修根因（消除噪声）

**方案 A（推荐）：移除 userspace 死调用**

在 `nvm_ctrl_free()` 中移除 `nvm_queue_clear(ctrl)` 调用（`ctrl.cpp:226`），因为：
- 内核从未实现此 ioctl
- 返回值已被忽略
- 后续 unbind + chrdev_remove + fd close 完成全部清理

| 文件 | 行号 | 改动 |
|------|------|------|
| `backends/local/nvme/libnvm/src/ctrl.cpp` | 226 | 删除或注释 `nvm_queue_clear(ctrl);` |
| `tutti/device_manager/nvme/libnvm/src/ctrl.cpp` | 226 | 同上 |

**不需要重编内核模块**，只需重编 `libnvm.so` 并重启 daemon。

**风险**：低。如果未来内核模块实现了 `NVM_CLEAR_IOQ_NUM`，需要在 `nvm_ctrl_free` 中恢复调用。应在删除处加注释说明原因。

**方案 B（备选）：实现内核 handler**

在 `pci.c` 的 switch 中添加 `case NVM_CLEAR_IOQ_NUM:`，实现 counter reset 逻辑。需要重编内核模块并 insmod。风险较高，不建议本轮做。

### 双副本同步

每次修改必须同时改两份：

| 副本 A（实际链接） | 副本 B（未链接但存在） |
|---|---|
| `backends/local/nvme/libnvm/src/linux/device.cpp` | `tutti/device_manager/nvme/libnvm/src/linux/device.cpp` |
| `backends/local/nvme/libnvm/src/ctrl.cpp` | `tutti/device_manager/nvme/libnvm/src/ctrl.cpp` |

### 验证手段

| 验证项 | 方法 | 需要重启 daemon | 需要重编内核模块 |
|--------|------|:---:|:---:|
| errno 打印正确 | 启动 daemon → SIGTERM → 查 daemon.log | 是 | 否 |
| 无 "ioctl_queue_helper err" 日志 | 同上 | 是 | 否 |
| 无内核 pr_notice | `dmesg | grep "unknown.*ssnvme"` | 是 | 否 |
| attach 功能不回归 | `run_attach_smoke.sh --execute` | 是 | 否 |
| 设备可重新 bind | daemon 二次启动 | 是 | 否 |

---

## 7. 待运行确认清单

以下项需要运行 daemon 才能最终确认，本任务禁止运行：

### 7.1 确认 errno = EINVAL

**最小复现步骤**：
1. 启动 daemon（`tutti_daemon --config ...`）
2. 等待 "listening" 消息
3. SIGTERM

**抓取命令**：
```bash
# 在 daemon 退出后检查 dmesg
dmesg | tail -20 | grep "unknown.*ssnvme"
# 预期：4 条 "snvme: unknown /dev/ssnvme ioctl 0x... from pid ..."
```

**能区分的假设**：
- 如果 dmesg 出现 "unknown ioctl" → 确认走 default 分支 → `EINVAL`
- 如果不出现 → 说明 ioctl 在到达 handler 前就失败了（可能是 fd 已关闭 → `EBADF`）

### 7.2 确认第一步修复后 errno 被正确打印

**前置**：完成第一步修复（device.cpp:327 打印 errno），重编 libnvm.so

**验证命令**：
```bash
# 启动 daemon → SIGTERM → 检查日志
grep "ioctl_queue_helper" daemon.log
# 预期：ioctl_queue_helper err is -1, errno=22 (Invalid argument), type=10
```

### 7.3 确认第二步修复后噪声消除

**前置**：完成方案 A（移除 nvm_queue_clear 调用），重编 libnvm.so

**验证命令**：
```bash
# 启动 daemon → SIGTERM → 检查日志
grep "ioctl_queue_helper" daemon.log
# 预期：无输出（0 条）
```

---

## 8. 查阅的关键文件清单

| 文件 | 行号范围 | 用途 |
|------|----------|------|
| `examples/tutti_daemon.cpp` | 96-153 | daemon main，state 生命周期 |
| `backends/local/NVMeService/src/nvmeservice_state.cu` | 89-108 | ServiceState 构造/析构 |
| `backends/local/nvme/libnvm/src/ctrl.cpp` | 198-232 | `nvm_ctrl_free()` teardown 顺序 |
| `backends/local/nvme/libnvm/src/linux/device.cpp` | 277-377 | `ioctl_queue_helper` + 3 个入口函数 |
| `backends/local/nvme/libnvm/include/ioctl.h` | 14, 162-266, 477-496 | ioctl 号定义、结构体定义 |
| `tutti/device_manager/nvme/kernel_modules/snvme-5.4.241-1-tlinux4-0017/pci.c` | 4986-6132 | 内核 ioctl dispatcher（switch + default） |
| `tutti/device_manager/nvme/kernel_modules/snvme-5.15.0-public/pci.c` | — | 5.15 版本（同样缺失 handler） |
| `tests/service_client/.work/logs/20260730-161630/daemon.log` | 全文 | 实测日志 |
| `examples/CMakeLists.txt` | 122-135 | tutti_daemon 链接配置 |
| `tutti/device_manager/nvme/libnvm/CMakeLists.txt` | 12-29 | libnvm 源码 glob |
| `tutti/device_manager/nvme/nvmeservice/CMakeLists.txt` | 66-78 | nvmeservice 源码 glob（含 .cu） |

---

## 9. 最终结论

**DIAGNOSED**

根因已定论：内核模块 `snvm_dev_map_ioctl()` 从未实现 `case NVM_CLEAR_IOQ_NUM:` handler。daemon 析构时 `nvm_ctrl_free()` → `nvm_queue_clear()` 发送此 ioctl，恒走 default 分支返回 `EINVAL`。`ioctl_queue_helper` 打印 `err`（恒为 -1）而非 `errno`，导致诊断被卡住。

影响面：无害。后续 `nvm_device_unbind` + `_nvm_ctrl_put` 完成全部清理。上一轮「无害噪声」结论正确但理由错误。

待运行确认：errno 实际值（源码推断为 `EINVAL`，需 `strace` 或修复后日志确认）。

## 总指挥验收

验收结论：`DIAGNOSED` —— 接受该定论。这是一份高质量诊断，根因已在源码层闭合，且纠正了上一轮我自己的错误归因。

### 我逐条独立核验了关键断言

**内核缺失 handler（最关键的一条）：**

```text
grep -c 'case NVM_CLEAR_IOQ_NUM' .../snvme-5.4.241-1-tlinux4-0017/pci.c
  -> 0
```

已实现的 11 个 case 与 worker 列出的清单**逐行吻合**（`NVM_MAP_HOST_MEMORY` 5007 … `NVM_SET_KERNEL_IOQ_CAP` 6068）。`NVM_CLEAR_IOQ_NUM` 确实从未实现。

**default 分支走向：** 我读了 `pci.c:6119-6128` 原文，确认

```c
if (_IOC_TYPE(cmd) == 'N') { ... ret = -ENOTTY; }
else { pr_notice("snvme: unknown /dev/ssnvme ioctl ..."); ret = -EINVAL; }
```

而 `NVM_IOCTL_TYPE = 0x80`（`ioctl.h:14`）≠ `'N'`（0x4e），因此走 `else` → `-EINVAL`。推理链完整成立。

**teardown 调用链：** 我读了 `ctrl.cpp` 的 `nvm_ctrl_free()` 原文，顺序确为

```c
nvm_queue_clear(ctrl);        // 返回值被忽略
nvm_device_unbind(ctrl);
nvm_chrdev_remove(...);
_nvm_ctrl_put(container);
```

`nvm_queue_clear` 确实在 `unbind` 与 `close fd` **之前**执行。这直接支撑了 worker 排除 `ENODEV` 与 `EBADF` 的判断 —— 那两个候选被排除的理由是正确的，不是敷衍。

**libnvm 副本归属：** `ldd build/bin/tutti_daemon` 确认链接 `build/lib/libnvm.so`；worker 用 `addr2line` 把符号定位到 `backends/local/nvme/libnvm/src/ctrl.cpp:224`。用 `addr2line` 而不是靠 CMake 猜，方法正确。

### worker 的方法论值得肯定

- **候选 errno 逐个给出支持/反对证据**，而不是只报一个答案。被排除的三个（`ENOTTY`/`ENODEV`/`EBADF`）都给了具体的反对理由，且理由经我核验成立。
- **严格遵守只读约束**：`git status` 确认 `backends/` 与 `tutti/device_manager/` 零改动，未启动 daemon，未碰模块。
- **明确纠正了上一轮的错误归因**，并且把「结论对、理由错」拆开说清楚（第 5 节的对照表）。这正是我要的态度 —— 上一轮我判定"无害噪声"结论成立但未查证，worker 补上了查证并指出我的时序描述也是错的（发生在 unbind **之前**，不是 unbind 过程中）。
- 主动区分「源码可证」与「需运行确认」，没有为了拿 `DIAGNOSED` 而把推断包装成实测。

### 我追加的一项排查：这个缺陷是否影响功能路径？

worker 指出 `NVM_SET_IOQ_NUM` 与 `NVM_SET_SHARE_REG` 在内核侧**同样缺失**。这引出一个比 teardown 噪声严重得多的疑问：attach 配置里有 `kernel_ioq_cap: 32`（四个设备都有），如果它经由未实现的 `NVM_SET_IOQ_NUM` 下发，那么**该配置将静默失效**，这就不是噪声而是功能缺陷了。

我查证结果：**不影响。**

```text
grep -rn 'nvm_queue_setup|nvm_queue_set\b|nvm_queue_share' \
    backends/local/NVMeService/src/ examples/
  -> (无命中)
```

daemon 完全不调用这三个入口。`kernel_ioq_cap` 走的是另一条路径：`nvm_set_kernel_ioq_cap_fd()` → `NVM_SET_KERNEL_IOQ_CAP`（`device.cpp:765`，pre-bind 阶段），而该 ioctl 在内核侧**已实现**（`pci.c:6068`）。因此配置正常生效，worker 的影响面定界「无害」是准确的。

这个追查还给出一个比 worker 更强的结论：**`ioctl_queue_helper` 的三个入口全部指向未实现的 ioctl，且没有一个在功能路径上 —— 整个 `ioctl_queue_helper` 家族是彻底的死代码。** worker 说 `nvm_queue_clear` 是死代码，实际范围比它说的更大。这使 Q6 的方案 A（移除死调用）比原先评估的更安全，也更有必要。

### 对修复方案的裁决

采纳 worker 的两步方案，并按上述发现调整优先级：

- **第一步（打印 errno）：** 采纳。但考虑到整个家族都是死代码，这一步的价值主要是「万一将来内核实现了 handler，日志能用」。可以做，优先级不高。
- **第二步方案 A（移除 `nvm_ctrl_free` 中的 `nvm_queue_clear` 调用）：** 采纳，且提升优先级。理由：它不仅消除用户态 4 条日志，还消除每次 daemon 退出时内核 `pr_notice` 打进 dmesg 的 4 条噪声。删除处必须留注释说明「内核未实现该 ioctl，若将来实现需恢复调用」。
- **方案 B（实现内核 handler）：** 不采纳。需重编内核模块并 insmod，风险与收益不匹配 —— 既然清理由 unbind + fd close 完成，没有必须实现它的理由。
- **双副本同步是硬要求。** 两处 `device.cpp:327` 与 `ctrl.cpp:226` 必须同改。更长期看，两份 libnvm 副本本身是技术债，应择机收敛为一份，但那是独立任务。

### 非阻塞观察

1. **`nvm_ctrl_free()` 忽略 `nvm_queue_clear()` 返回值**（`ctrl.cpp:226`）。即便将来内核实现了该 handler，失败也不会被上层感知。若恢复该调用，应同时决定失败是否需要上报。

2. **`ioctl_queue_helper` 的 `default:` 分支返回正数 `EINVAL`，而失败分支返回 `errno`（也是正数），成功返回 0** —— 三种路径的返回值约定混在一起，调用方无法区分「参数错」与「ioctl 失败」。属既有设计瑕疵，本轮不动。

3. worker 顺带发现 `~ServiceState()` 位于 `.cu` 文件（`nvmeservice_state.cu:97`）且经 `file(GLOB ... src/*.cu)` 纳入编译。与本现象无关，但作为构建结构信息记录。

### 后续决定

T-017 诊断完成，接受 `DIAGNOSED`。修复**不在本轮实施**（本轮定位为只读诊断，且修复需重编 libnvm 并重启 daemon 验证）。

Round 6 应包含一个 libnvm 死代码清理任务，范围：两份副本的 `ctrl.cpp:226` + `device.cpp:327`，验证方式为重编 libnvm、重跑 `run_attach_smoke.sh --execute`、确认用户态与 dmesg 双侧噪声均消失、attach 无回归。该任务需要启动 daemon，必须单独排期，不与其他硬件任务并发。
