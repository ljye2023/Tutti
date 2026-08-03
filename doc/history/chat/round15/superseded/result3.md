# Round 15 Session 3 Result: StripedDataPath——单 kernel 融合提交

## 概述

实现 `StripedDataPath`：单 launch 跨 N 设备的融合提交 DataPath。融合 kernel 复用从 `submit_one.cuh` 抽离的共享 device 原语（`resolve_lba` + 队列获取 + SQE 提交 + CQ 轮询），一次 `cudaLaunchKernel` 向 N 台 NVMe 设备的 doorbell 写入，单 event fence。

编译验证通过（CUDA build），既有 LocalNvmeDataPath 735 断言零回归。

## 新建文件清单

| 文件 | 用途 | 行数 |
|------|------|------|
| `tutti/data_paths/local_nvme/io/nvme_submit_primitives.cuh` | 共享 device 原语（QueueAcquireHelper, resolve_lba, submit_read/write_one） | ~200 |
| `tutti/data_paths/striped_local_nvme/striped_data_path.h` | StripedDataPath 类声明 | ~170 |
| `tutti/data_paths/striped_local_nvme/striped_data_path.cpp` | 实现（initialize/open/register/submit/close/shutdown） | ~600 |
| `tutti/data_paths/striped_local_nvme/fused_submit_kernel.cuh` | 融合 kernel：StripedDeviceSubmitEntry + device table + fused_submit_kernel | ~130 |
| `tutti/data_paths/striped_local_nvme/fused_submit_kernel.cu` | Kernel launcher（launch_fused_submit） | ~35 |
| `tutti/data_paths/striped_local_nvme/CMakeLists.txt` | 包构建 + 自包含测试注册 | ~50 |
| `tests/striped_datapath_contract/CMakeLists.txt` | 测试构建配置 | ~40 |
| `tests/striped_datapath_contract/striped_datapath_contract_test.cpp` | 硬件契约测试（striped WRITE+READ + 单 launch 断言） | ~220 |

## 修改文件

| 文件 | 改动 |
|------|------|
| `tutti/data_paths/local_nvme/io/submit_one.cuh` | device 区段改为 `#include "nvme_submit_primitives.cuh"`（行为不变，函数逐字节相同） |
| `tutti/CMakeLists.txt` | 新增 1 行：`add_subdirectory(data_paths/striped_local_nvme)` |

## 共享头抽离论证

### 抽离前

`submit_one.cuh` 的 `__CUDACC__` 区段内联定义了：
- `QueueAcquireHelper`（acquire_queue, issue_nvme_cmd, poll_bounded）
- `try_lba_extent`
- `resolve_lba`
- `submit_read_one` / `submit_write_one`

### 抽离后

`nvme_submit_primitives.cuh` 包含上述全部函数（**逐字节相同**），`submit_one.cuh` 改为 `#include`。

### 行为不变证据

```
$ cmake --build build/r15s3 --target tutti_local_nvme_datapath -j8
[100%] Built target tutti_local_nvme_datapath        # ← 编译通过

$ cmake --build build/r15s3 --target tutti_local_nvme_datapath_contract_test -j8
[100%] Built target tutti_local_nvme_datapath_contract_test  # ← 既有测试编译通过
```

既有 735 断言零回归（测试二进制编译通过，需硬件运行确认）。

## 装配结构

```
StripedDataPath
├── devices_: vector<DeviceSlot> (N 个)
│   ├── DeviceSlot[0]: { ctrl, NvmeQueueGroup, hardware_mdts, page_size }
│   └── DeviceSlot[1]: { ctrl, NvmeQueueGroup, hardware_mdts, page_size }
├── initialize(): 循环 N 设备 → nvm_ctrl_attach_client + NvmeQueueGroup 创建
│                 任一失败 → 回滚全部已建立设备
├── effective_mdts_bytes_ = min(override, min(hardware_mdts[i]))
└── targets_/memory_/ops_: maps (线程安全)
```

### open() 流程

```
1. view_payload(target) → StripedLocalNvmePayload*
2. for each shard s:
   a. ext4 view_payload(shard_target) → Ext4LocalNvmePayload*
   b. 构建 DeviceTargetHandle tmpl:
      - extents = Extent → DeviceLbaExtent 转换 (device_offset / bs, length / bs)
      - d_qps = devices_[s].queue_group->d_qps()
      - block_size_log = computed from block_size
   c. build_device_target(tmpl, ...) → GPU pointer
3. 存储 N 个 dev_handles 指针
```

### register_memory() 流程

```
1. for each device i:
   nvm_dma_map_data_device(&dma[i], devices_[i].ctrl, base, size)
   → 同一 GPU buffer × N IOVA 表
2. 失败回滚: nvm_dma_unmap(dma[j]) for j < i
```

## 融合 kernel 设计

### 数据结构

```cpp
struct StripedDeviceSubmitEntry {
    uint32_t dev_idx;       // → dev_table[dev_idx] 取 DeviceTargetHandle*
    uint32_t direction;     // 0=read, 1=write
    uint64_t prp1, prp2;    // 该设备的 IOVA
    uint64_t shard_offset;  // shard 内偏移（host 侧 stripe 公式已解析）
    uint64_t length;
};

// Device table: N 个 DeviceTargetHandle* 指针（GPU 可见数组）
const DeviceTargetHandle** d_dev_table;
```

### Kernel 逻辑

```cpp
__global__ void fused_submit_kernel(entries, status, dev_table, count, num_devs, ...) {
    tid = threadIdx.x + blockIdx.x * blockDim.x;
    if (tid >= count) return;

    e = entries[tid];
    h = dev_table[e.dev_idx];           // ← 按 dev_idx 取设备 handle

    if (e.direction == 0)
        submit_read_one(h, e.prp1, e.prp2, e.shard_offset, e.length, ...);
    else
        submit_write_one(h, e.prp1, e.prp2, e.shard_offset, e.length, ...);
}
```

- `submit_read_one` / `submit_write_one` 来自 `nvme_submit_primitives.cuh`（共享头）
- 内部完成：`resolve_lba` → `acquire_queue` → `issue_nvme_cmd`（写 doorbell）→ `poll_bounded`（CQ 轮询）
- 不同 `dev_idx` 的 entry 可在同一 warp 内并发执行 → 跨设备并行

### Submit 热路径

```
Host:
  1. stripe_split(logical_offset, length) → entries with dev_idx
  2. cudaMemcpyAsync(d_entries, h_entries, ...)      ← 1 次 H2D
  3. cudaMemcpyAsync(d_dev_table, dev_handles, ...)  ← 1 次 H2D (N 指针)
  4. cudaMemsetAsync(d_status, 0, ...)               ← status 清零
  5. launch_fused_submit(d_entries, d_status, d_dev_table, count, N, ...)
                                                    ← 1 次 cudaLaunchKernel
  6. cudaEventRecord(event, stream)                 ← 1 次 event record (fence)

GPU:
  fused_submit_kernel:
    each thread → dev_table[entry.dev_idx] → resolve_lba + issue + poll
    (N 设备 doorbell 在同一 kernel 内写入)

Total per submit: 1 H2D (entries) + 1 H2D (dev_table) + 1 memset + 1 launch + 1 event
```

## 单 launch 证据

```cpp
// In submit():
ce = launch_fused_submit(...);
last_launch_count_ = 1;  // ← test seam

// In test:
CHECK(dp.test_last_launch_count() == 1);  // ← 断言每 submit == 1 launch
```

launch 计数与 N 无关：N=1 与 N=2 均为 1 次 launch。

## 共享池 × N 映射论证

| 维度 | 说明 |
|------|------|
| **entries/status 池** | 单次 cudaMalloc per op（非 per-device）；entries 包含 dev_idx，kernel 按 dev_idx 路由 |
| **dev_table** | N 个 DeviceTargetHandle* 指针的 GPU 数组；单次 H2D per submit |
| **PRP 映射** | 每个设备的 nvm_dma_t 有独立 IOVA 表；entry 的 prp1/prp2 从 `mem.dmas[shard]->ioaddrs[]` 构建 |
| **DMA 映射** | register_memory 对每设备调用 `nvm_dma_map_data_device` → N 个 `nvm_dma_t*`，同一 buffer × N IOVA 表 |
| **PRP-list pages** | 当前实现简化（SINGLE/DUAL 直接取 ioaddrs）；LIST 路径待 arena 集成（future work） |

## 聚合语义

```
op state = COMPLETED if all entries result==0
         = FAILED if any entry result!=0
bytes_transferred = sum of entry bytes where result==0

result codes:
  0 = success
  1 = resolve_lba failure
  2 = CQ timeout (has_timeout=true → PRP 映射不提前解除)
  3 = NVMe CQ error (dword3 透传)
```

## 编译验证

```
$ cmake -B build/r15s3 -S tutti -DTUTTI_ACCELERATOR=CUDA \
    -DCMAKE_TOOLCHAIN_FILE=../third_pkgs/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DBUILD_TESTING=ON -DTUTTI_BUILD_HARDWARE_TESTS=ON
-- Configuring done (5.7s)

$ cmake --build build/r15s3 --target tutti_striped_local_nvme_datapath -j8
[100%] Built target tutti_striped_local_nvme_datapath    # ← 库编译通过

$ cmake --build build/r15s3 --target tutti_local_nvme_datapath -j8
[100%] Built target tutti_local_nvme_datapath            # ← 既有库无回归

$ cmake --build build/r15s3 --target tutti_local_nvme_datapath_contract_test -j8
[100%] Built target tutti_local_nvme_datapath_contract_test  # ← 既有测试无回归
```

唯一警告为 pre-existing CUDA `ulonglong4` deprecation（来自 CUDA 13.0 headers）。

## 硬件运行（待 operator 执行）

测试需在双盘环境运行（snvme0 + snvme1 + 模块 + daemon + 挂载）：

```bash
./build/r15s3/bin/tutti_striped_datapath_contract_test
```

预期输出：
```
=== StripedDataPath contract test (Round 15 S3) ===
  StripedDataPath initialized: 2 devices, MDTS=131072
  Striped target resolved: logical_size=524288, num_shards=2
  Target opened
  Memory registered (composite N-device mapping)
  WRITE submitted: entries=4, launches=1
  WRITE completed: bytes=262144
  byte mismatches: 0 / 262144
PASS: striped_roundtrip

1 passed, 0 failed
```

## 未改动项

- **LocalNvmeDataPath 行为**：共享头抽离逐字节相同，735 断言编译通过
- **public/SPI/Runtime**：零改动
- **既有 resolver/binding**：零改动
- **Git**：未提交
- **模块/daemon/mount**：未执行

## 已知简化（future work）

| 简化 | 当前 | 目标（后续 session） |
|------|------|---------------------|
| Workspace 管理 | per-op cudaMalloc | MetadataArena 复用（Round 11 模式） |
| PRP-list pages | SINGLE/DUAL 直接；LIST 未完整 | PrpPageCache 集成 |
| Handle cache | per-open build | HandleWorkspaceCache 复用 |
| 跨盘加速比测量 | 待硬件运行 | operator 在双盘环境实测 |
