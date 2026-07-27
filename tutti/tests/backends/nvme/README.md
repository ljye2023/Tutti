# tutti/tests/backends/nvme — NvmeBackend 测试说明

## 目录结构

```
tests/backends/nvme/
├── CMakeLists.txt        # 构建配置
├── nvme_backend_test.cpp # 全部测试用例
└── README.md             # 本文档
```

## 构建

使用项目统一构建脚本（需在 `tutti/` 目录下执行）：

```bash
bash build.sh
```

默认启用测试（`BUILD_TESTING=ON` 已在 `build.sh` 中设置）。构建产物输出到 `tutti/build/bin/nvme_backend_test`。

### 构建依赖

| 依赖 | 说明 |
|------|------|
| CUDA Toolkit | nvcc 编译器，cudaMalloc/cudaMemcpy 等运行时 |
| libnvm | 项目内置，位于 `device_manager/nvme/libnvm/` |
| gRPC | 可选；不存在时 `TUTTI_NVMESERVICE_ENABLED` 未定义，所有 RealHw 用例自动 Skip |
| GTest | 自动通过 FetchContent 下载（v1.15.2） |

---

## 运行测试

### CTest（推荐）

```bash
cd tutti/build

# 仅 Unit 层（无硬件，任意机器可运行）
ctest -R "NvmeBackendUnit"

# Unit + RealHw 层（需要真实 NVMe + 运行中的 daemon）
TUTTI_NVME_REAL_HW=1 ctest --output-on-failure -R "NvmeBackend"

# Unit + RealHw + 破坏性写入校验
TUTTI_NVME_REAL_HW=1 TUTTI_NVME_DESTRUCTIVE=1 ctest --output-on-failure -R "NvmeBackend"
```

### 直接运行二进制

```bash
# 所有用例
./bin/nvme_backend_test

# 指定用例
TUTTI_NVME_REAL_HW=1 ./bin/nvme_backend_test \
    --gtest_filter=NvmeBackendRealHw.GpuSubmitSingleBlock
```

---

## 环境变量

| 变量 | 值 | 作用 |
|------|----|------|
| `TUTTI_NVME_REAL_HW` | `1` | 启用 `NvmeBackendRealHw.*` 用例。未设置时全部 Skip。 |
| `TUTTI_NVME_ENDPOINT` | `host:port` | nvmeservice daemon 的 gRPC 地址。默认 `127.0.0.1:50051`。 |
| `TUTTI_NVME_DESTRUCTIVE` | `1` | 启用 `GpuWriteReadVerify`（会覆盖 LBA 0）。仅对 scratch 设备设置。 |

---

## 测试分层

### Unit 层（`NvmeBackendUnit.*`）

无需硬件，无需 daemon，任意机器可运行。使用 `DaemonNvmeDeviceDriver` 的 mock 模式：`d_qps=nullptr`，`blk_size=0`（后端内部使用默认值 4096 / 512 KiB 防止除零）。

| 用例 | 验证内容 |
|------|---------|
| `InitializeAndShutdownLifecycle` | initialize 开 roster → shutdown 归还 vdevice，幂等 |
| `InitializeRejectsNullDm` | null dm → false |
| `InitializeRejectsZeroCount` | vdevice_count=0 → false |
| `ShutdownIsIdempotent` | 第二次 shutdown 无崩溃 |
| `RosterAccessors` | 范围内 non-null + valid handle；范围外 null + invalid |
| `MetadataIdentity` | backend_type == LOCAL_NVME，capabilities == SUPPORTS_GPUDIRECT |
| `FactoryRegistration` | `BackendFactory::is_registered(LOCAL_NVME)` 为 true |
| `FactoryCreateReturnsNvmeBackend` | `create_backend(LOCAL_NVME)` 返回非 null |

### Real-HW 层（`NvmeBackendRealHw.*`）

需要：
1. `TUTTI_NVME_REAL_HW=1`
2. `gRPC` 编译支持（`TUTTI_NVMESERVICE_ENABLED` 已定义）
3. nvmeservice daemon 在 `TUTTI_NVME_ENDPOINT` 上运行（见下文启动方式）

| 用例 | 验证内容 | 破坏性 |
|------|---------|--------|
| `InitializeAcquiresLiveQueues` | d_qps != null，live GPU queue slice | 否 |
| `RosterHoldsNvmeVirtualDevice` | downcast 成功，queue_quota 匹配 | 否 |
| `MetadataPopulatedFromDevice` | namespace_id / blk_size / blk_size_log > 0 | 否 |
| `MultipleVdevicesDistinctQueueSlices` | 两个 vdevice 的 d_qps 指针不同 | 否 |
| `AcquireReleaseTargetHandle` | 合成 StorageTarget → acquire/release GPU handle 无泄漏 | 否 |
| `PrepareAndReleaseDescriptors` | DUAL-PRP descriptor 结构正确 | 否 |
| `GpuSubmitSingleBlock` | GPU 内核提交 NVMe READ，cudaStreamSynchronize 成功 | 否 |
| `GpuWriteReadVerify` | GPU 内核 WRITE + READ，逐字节校验数据一致 | **是**，需要 `TUTTI_NVME_DESTRUCTIVE=1` |

---

## 启动 nvmeservice daemon

Real-HW 用例依赖一个运行中的 nvmeservice daemon。启动脚本位于：

```
tutti/tests/device_manager/nvme/start_nvmeservice_daemon.sh
tutti/tests/device_manager/nvme/stop_nvmeservice_daemon.sh
```

daemon 的配置文件（设备 PCI 地址、CUDA 设备、队列参数等）：

```
tutti/tests/device_manager/nvme/sys_config.b1.yaml
```

**注意**：daemon 会独占绑定 NVMe 设备（unbind 内核 nvme 驱动）。只对专用的 scratch 设备操作，不要指向有数据的设备。

---

## 缓存一致性说明（`GpuWriteReadVerify`）

GPU-direct NVMe IO 涉及三层内存：GPU GDDR、host DRAM、NVMe 介质。缓冲区分配策略如下：

| 缓冲区 | 分配方式 | 原因 |
|--------|----------|------|
| WRITE buf | `cudaMallocHost` (WB) + `nvm_dma_map_data_host` | CPU `memcpy` 填模式；NVMe 控制器通过常规 PCIe DMA 读取，无 GPU↔NVMe P2P 依赖 |
| READ buf | `cudaHostAlloc(cudaHostAllocWriteCombined)` + `nvm_dma_map_data_host` | NVMe DMA 写 host DRAM；WC 属性使 CPU 读跳过 L3 直取 DRAM，消除 CPU 缓存陈旧问题 |

GPU device memory（`cudaMalloc`）不能用于 WRITE 缓冲区，原因是该测试机器上 NVMe 控制器与 GPU 之间不存在 PCIe P2P 路径；NVMe 会从 PCIe 总线读到全零。

WC 内存对 READ 缓冲区是必要的：若使用普通 WB pinned 内存，`memset` 产生的 CPU 缓存行不会因 NVMe DMA 写入 DRAM 而失效，CPU 读到的仍是旧的零值。

---

## 工厂注册与链接器注意事项

`NvmeBackend` 的工厂注册通过 `nvme_backend_registration.cpp` 中的静态对象完成。该 TU 在 `libtutti_backends_nvme.a` 中，链接器会因无符号被引用而将其丢弃。

`CMakeLists.txt` 通过将 `nvme_backend_registration.cpp` 直接加入测试可执行文件的源文件列表来解决这个问题（与 `backend_test` 对 `mock_backend.cpp` 的处理方式相同）。
