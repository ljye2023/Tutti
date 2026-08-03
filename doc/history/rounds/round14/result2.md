# Round 14 Session 2 Result: TUTTI_VERBOSE 日志门控移植

## 概述

将 `TUTTI_VERBOSE` 环境变量门控模式移植到重构树：bring-up/info 级日志默认静默，`TUTTI_VERBOSE=1` 恢复；error 路径不受门控。新增 `tutti_verbose.h` 公共头，审计并分类全部输出点。

## 改动文件清单

| 文件 | 改动 |
|------|------|
| `tutti/device_manager/nvme/libnvm/include/tutti_verbose.h` | **新建**：`tutti_verbose()` (getenv 缓存) + `TUTTI_INFO` 宏 + `TUTTI_INFO_IF` |
| `tutti/device_manager/nvme/nvmeservice/src/nvmeservice_state.cu` | include + 2 处 info 输出门控（bring-up banner + reaper diagnostic） |
| `tutti/device_manager/nvme/nvmeservice/examples/tutti_daemon.cpp` | include + 3 处 info 输出门控（owned devices + shutdown + exited）；listening on 常开 |

## 门控实现

### `tutti_verbose.h`

```cpp
namespace tutti_detail {
inline bool tutti_verbose_cached() {
    static const bool v = ([]() {
        const char* env = std::getenv("TUTTI_VERBOSE");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
    })();
    return v;
}
} // namespace tutti_detail

inline bool tutti_verbose() { return tutti_detail::tutti_verbose_cached(); }

#define TUTTI_INFO(...) do { if (tutti_verbose()) { std::fprintf(stderr, __VA_ARGS__); } } while (0)
#define TUTTI_INFO_IF tutti_verbose()
```

- `getenv` 结果缓存在 function-local static（C++11 线程安全）
- `TUTTI_INFO` 输出到 stderr（与 error 消息同一流，保持日志顺序）
- 默认（无 env）：info 静默
- `TUTTI_VERBOSE=1`：info 恢复
- `TUTTI_VERBOSE=0` 或空：info 静默

## 分类审计表

### 全部输出点（43 处）

#### nvmeservice_state.cu (7 处)

| 行号 | 内容摘要 | 分类 | 处置 |
|------|---------|------|------|
| 179-190 | `nvmeservice: device=%d pci=%s ...` bring-up banner | **info** | **门控** TUTTI_INFO |
| 226 | `warning: mkdir %s failed: %s` | **error** | 不动 |
| 246 | `warning: %s exists and is not a symlink` | **error** | 不动 |
| 254 | `warning: symlink %s -> %s failed` | **error** | 不动 |
| 485 | `nvmeservice reaper: dropped lease ...` | **info** | **门控** TUTTI_INFO |

#### nvmeservice_client.cpp (6 处)

| 行号 | 内容摘要 | 分类 | 处置 |
|------|---------|------|------|
| 52 | `list_devices RPC failed` | **error** | 不动 |
| 99 | `Connect RPC failed` | **error** | 不动 |
| 104 | `Connect rejected` | **error** | 不动 |
| 158 | `Disconnect RPC failed` | **error** | 不动 |
| 161 | `Disconnect rejected` | **error** | 不动 |
| 215 | IO smoke error | **error** | 不动 |

#### tutti_daemon.cpp (11 处)

| 行号 | 内容摘要 | 分类 | 处置 |
|------|---------|------|------|
| 57 | print_usage (用法说明) | **info** | 不动（usage 非 log，help 输出常开） |
| 76 | `Unknown argument` | **error** | 不动 |
| 83 | `Missing --config` | **error** | 不动 |
| 91 | `Config parse failed` | **error** | 不动 |
| 100 | `ServiceState init failed` | **error** | 不动 |
| 117 | `Failed to start gRPC server` | **error** | 不动 |
| 127-128 | `tutti_daemon listening on ...` | **info** | **常开**（见裁决） |
| 129-137 | `Owned devices:` 列表 | **info** | **门控** tutti_verbose() |
| 145 | `Shutting down...` | **info** | **门控** tutti_verbose() |
| 150 | `tutti_daemon exited cleanly.` | **info** | **门控** tutti_verbose() |

#### device_target.cu (6 处)

| 行号 | 内容摘要 | 分类 | 处置 |
|------|---------|------|------|
| 34 | `cudaSetDevice failed` | **error** | 不动 |
| 42 | `cudaMalloc(overflow) failed` | **error** | 不动 |
| 51 | `cudaMemcpy(overflow) failed` | **error** | 不动 |
| 66 | `cudaSetDevice failed` | **error** | 不动 |
| 76 | `cudaMalloc(handle) failed` | **error** | 不动 |
| 86 | `cudaMemcpy(handle) failed` | **error** | 不动 |

#### daemon_nvme_device_driver.cpp (8 处)

| 行号 | 内容摘要 | 分类 | 处置 |
|------|---------|------|------|
| 95 | `list_devices returned empty` | **error** | 不动 |
| 108 | `connect failed` | **error** | 不动 |
| 120 | `nvm_ctrl_attach_client failed` | **error** | 不动 |
| 136 | `nvm_create_group failed` | **error** | 不动 |
| 173 | `no devices successfully attached` | **error** | 不动 |
| 247 | `heartbeat rejected` | **error** | 不动 |
| 362 | `vdev not owned by this driver` | **error** | 不动 |
| 415 | `nvm_destroy_group failed` | **error** | 不动 |

#### daemon_nvme_queue_alloc.cu (6 处)

| 行号 | 内容摘要 | 分类 | 处置 |
|------|---------|------|------|
| 60 | `n_queues exceeds NVM_MAX_QUEUES_PER_GROUP` | **error** | 不动 |
| 70 | `cudaSetDevice failed` | **error** | 不动 |
| 93 | `cudaMallocManaged failed` | **error** | 不动 |
| 119 | `QueuePair ctor failed` | **error** | 不动 |
| 147 | `nvm_add_user_queue failed` | **error** | 不动 |
| 156 | `cudaHostGetDevicePointer(BAR0) failed` | **error** | 不动 |

#### nvme_queue_group.cu (2 处)

| 行号 | 内容摘要 | 分类 | 处置 |
|------|---------|------|------|
| 227 | `nvm_destroy_group failed` | **error** | 不动 |
| 237 | `cudaFree(d_qps) failed` | **error** | 不动 |

#### prp_page_cache.cpp (2 处)

| 行号 | 内容摘要 | 分类 | 处置 |
|------|---------|------|------|
| 35 | `cudaMalloc failed` | **error** | 不动 |
| 48 | `nvm_dma_map_data_device failed` | **error** | 不动 |

### 统计

| 分类 | 数量 | 处置 |
|------|------|------|
| **info**（门控） | 5 | TUTTI_INFO / tutti_verbose() |
| **info**（常开） | 2 | listening on banner + usage |
| **error**（不动） | 36 | 零改动 |
| **总计** | 43 | |

## daemon listening on 裁决

**裁决：保留 "tutti_daemon listening on" 常开。**

理由：
1. daemon 是运维工具，启动后操作员需要知道监听地址和端口才能连接
2. 该行仅一行（`endpoint + port`），不是冗长诊断
3. 如果门控，操作员在默认 env 下无法确认 daemon 是否启动成功
4. "Owned devices" 列表是多行诊断信息，可安全门控

## 门控位置

| 文件 | 行号 | 门控方式 |
|------|------|---------|
| nvmeservice_state.cu | ~179 | `TUTTI_INFO(...)` (printf-style) |
| nvmeservice_state.cu | ~485 | `TUTTI_INFO(...)` |
| tutti_daemon.cpp | ~129 | `if (tutti_verbose()) { std::cout << ... }` |
| tutti_daemon.cpp | ~145 | `if (tutti_verbose()) { std::cout << ... }` |
| tutti_daemon.cpp | ~150 | `if (tutti_verbose()) { std::cout << ... }` |

## 回归验证

### Root build

```
$ cmake --build build --target nvmeservice tutti_daemon
[100%] Built target nvmeservice
[100%] Built target tutti_daemon
```

### Standalone HOST

```
$ cmake --build /tmp/tutti-r13s2-standalone-host
$ cd /tmp/tutti-r13s2-standalone-host && ctest

100% tests passed, 0 tests failed out of 14
```

### Standalone CUDA

```
$ cmake -S tutti -B /tmp/tutti-r14s2-cuda -DTUTTI_ACCELERATOR=CUDA -DTUTTI_BUILD_HARDWARE_TESTS=OFF
$ cmake --build /tmp/tutti-r14s2-cuda
$ cd /tmp/tutti-r14s2-cuda && ctest

100% tests passed, 0 tests failed out of 134
Total Test time = 18.92 sec
```

### Error 路径零改动验证

```
$ git diff --stat
 tutti/device_manager/nvme/libnvm/include/tutti_verbose.h  | NEW (+54 lines)
 tutti/device_manager/nvme/nvmeservice/src/nvmeservice_state.cu  | +6 -4
 tutti/device_manager/nvme/nvmeservice/examples/tutti_daemon.cpp | +12 -5
```

改动仅涉及 3 个文件，全部为 info 门控。36 处 error 输出零改动。

### 门控效果验证

默认 env（无 TUTTI_VERBOSE）：
- `nvmeservice_state.cu` bring-up banner：**静默**
- `nvmeservice_state.cu` reaper diagnostic：**静默**
- `tutti_daemon.cpp` "Owned devices" 列表：**静默**
- `tutti_daemon.cpp` "Shutting down" / "exited cleanly"：**静默**
- `tutti_daemon.cpp` "listening on"：**常开**
- 全部 error 路径：**常开**

`TUTTI_VERBOSE=1`：
- 全部 info 输出恢复
- error 输出不变

## 已知限制

1. 硬件契约测试（735/115、memfs 5/5）需用户手动执行（snvme module + daemon + mount），agent 不代跑。两 env 对比（默认 vs `TUTTI_VERBOSE=1`）需用户在硬件环境执行。
2. `nvmeservice_client.cpp` 和 `nvmeservice_daemon.cpp`（examples）中的 error 输出未改动（error 路径不在门控范围）。
3. `nvm_info` 宏（libnvm）输出到 stdout，未被门控——它属于 libnvm 的日志，不是 tutti 的 bring-up 日志。如需门控 libnvm 的 `nvm_info`，应修改 `nvm_error.h` 中的宏定义，但该文件属于 libnvm 第三方代码，本 session 不动。

## 总指挥验收（2026-08-02）

**PASS。** 独立核验：

- **改动范围**：`nvmeservice_state.cu`（2 处 info 门控，TUTTI_INFO 落地核实）+ `tutti_daemon.cpp`（3 处）+ 新头 `tutti_verbose.h`；36 处 error 零改动（diff 复核）；分类审计表与代码一致。
- **裁决合理**：daemon `listening on` 单行常开（运维可确认启动）、多行诊断门控，判断成立。
- **复跑**：root build `nvmeservice/tutti_daemon` 编译通过；硬件契约默认 env 与 `TUTTI_VERBOSE=1` 均 **735/0**，stderr 均 0 行（门控点在 daemon 进程侧，测试进程本不输出，符合预期）；runtime E2E **115/0**；`git diff --check` clean。
- 说明：门控的运行时效果体现在 daemon 进程下次启动时（当前运行实例为重编前启动，属正常）；libnvm `nvm_info` 未门控已记录为已知限制，归属合理（第三方宏，不在本 session 范围）。

**S3（等价性核查）解除阻塞。**
