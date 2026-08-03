# TASK T-026

你是一名资深 CUDA/NVMe C++ 工程师。你的任务是**分层抽象 + 代码搬运**：把 main 分支典型 IO 路径中的 `NvmeQueueGroup` 和 GPU-resident `NvmeFileDeviceHandle` 搬进当前 `LocalNvmeDataPath` 私有 package，为后续真实 submit 准备 `d_qps` 与 device target handle。

# 任务定位

**你在搬代码，不是在设计新队列模型。**

- 分析基线：`main@4862157d50c8a7004cdeb166dda630ab1ef4561a`。
- 典型链路说明：`MAIN_IO_PATH.md`（必读）。
- queue 搬运源：
  - `main:device_manager/include/nvme_queue_group.h`
  - `main:device_manager/src/nvme_queue_group.cu`
- target handle 搬运源：
  - `main:nvme_storage/include/nvme_file_device_handle.h`
  - `main:nvme_storage/src/host_fs_backed_nvme_storage_device.cu` 的 `build_handle_template_` / acquire/release 相关代码
- 当前落点（Round 7 已实现骨架 + target + DMA registration）：
  - `tutti/data_paths/local_nvme/local_nvme_data_path.h/.cpp`

**原样搬运 source 的 queue 资源所有权、创建顺序、doorbell 映射、GPU handle 布局与释放顺序。** 只做目录、命名空间、现有 `LocalNvmeTargetState` 数据源等新接口必需的机械适配。

禁止评审/修改 source 行为；禁止修改 main/旧代码。若 source 有你不喜欢的策略，照搬。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 前置条件与顺序

本任务是 Round 8 第一个任务，必须在 Session 2/3 之前完成。需要负责人保持 daemon 与 ext4 环境：

```bash
pgrep -af tutti_daemon | head -1
ls -l /dev/ssnvme0
findmnt /mnt/nvme1 | tail -1
nvidia-smi -L | head -1
```

若不就绪，报告 `BLOCKED`，不要自己启动 daemon、bind、mount。

# 1. 搬运 `NvmeQueueGroup`

在 `tutti/data_paths/local_nvme/io/` 新建私有实现（建议文件名）：

```text
nvme_queue_group.h
nvme_queue_group.cu
```

命名空间移到 `tutti::data_paths::local_nvme`，其余结构尽量保持 source：

```text
borrowed nvm_ctrl_t*
→ nvm_create_group
→ N × QueuePair
→ nvm_dma_map_ring_device
→ nvm_add_user_queue
→ BAR0 doorbell host VA → cudaHostGetDevicePointer GPU VA
→ cudaMemcpy QueuePair[] → d_qps
```

析构顺序保持 source：

```text
nvm_destroy_group → cudaFree(d_qps) → delete host QueuePair[]
```

controller 是 borrowed；`LocalNvmeDataPath` 必须先析构 queue group，再 `nvm_ctrl_free_client(ctrl_)`。

## 机械适配

main 的 SERVICE_CLIENT 路径在 `device_manager/src/nvmeservice_backed_registry.cpp:237-260` 合成 `struct disk`：

```cpp
d.page_size  = ctrl/page info;
d.ns_id      = namespace_id;
d.block_size = target block_size;
d.disk_name  = snvme chrdev name + "n<nsid>";
```

当前 DataPath 不复活 registry；在 `initialize()` 里按相同代码从已注入的 controller/config 合成 `disk`。不要引入 `DeviceManager`。

扩展 `LocalNvmeDataPath` 构造参数（保持已有默认构造兼容）：

```text
snvme_dev_path, bar0_size, cuda_device, num_user_queues, queue_depth, namespace_id/block_size（如 target open 后才能确认，可按 source 采用配置值并在 open 校验一致）
```

默认空 path 仍保留 Round 7 的 skeleton 模式；真实 path 才 attach + queue group。

## 异常处理

source 构造函数抛 `std::runtime_error`。DataPath `initialize()` 捕获并转为结构化 `Status`（建议 `NOT_READY` 或 `DEVICE_ERROR`，说明理由），然后释放已 attach 的 client ctrl，不能留下半初始化资源。

# 2. 搬运 GPU target handle

在同一 private package 新建（建议）：

```text
device_target.h
device_target.cu
```

搬运 `NvmeFileDeviceHandle` 的布局语义：

```text
file/token identity
logical_size_bytes
header_bytes = 0
nvme_block_size + log2
namespace_id
num_extents
inline extents[8]
overflow extents pointer（>8 时 cudaMalloc）
d_qps + num_d_qps（borrowed from NvmeQueueGroup）
```

旧 `LbaExtent` 只有 `{start_lba,length_blocks}`。当前 `LocalNvmeTargetState::LbaExtent` 还含 `logical_offset_bytes`；device handle 按 source 布局只需 block-unit extent，保持 source。

## `open()` 机械扩展

当前 `open()` 已：

- 经 binding `view_payload()`；
- 字节 extent → block extent；
- 保存 host target state；
- mint `DataPathTarget`。

在其后搬入 device handle 构造：

1. 从 host state 填 source handle；
2. inline 前 8 个 extent；
3. >8 时分配/上传 overflow；
4. 填 `queue_group_->d_qps()` / `n_qps()`；
5. `cudaMalloc` device handle + H2D；
6. 将 device handle 与 overflow owner 存入 `LocalNvmeTargetState`。

## `close()` / `shutdown()`

- `close()` 先安全释放该 target 的 device handle + overflow，再 erase identity；
- `shutdown()` 先释放所有 targets，再销毁 queue group，再 unmap memory registrations/释放 controller（实际顺序要保证任何 target/kernel 不再引用 d_qps；本任务没有 IO in-flight）。
- 重复 close 仍按 Round 7 返回错误，不引入旧 P0-8 的 stale cache。

# 3. Capabilities

本任务仍没有 submit，所以：

- `supports_read/write/direct/device_execution` 仍保持 false；
- memory registration true 保持；
- queue group/device target 存在**不等于**已支持 IO，禁止虚报。

# 4. 测试

扩展 `tests/local_nvme_datapath_contract/`，保留 Round 7 的全部 98 断言，新增至少：

1. 真实 initialize 创建 queue group：`group_id != 0`、`d_qps != nullptr`、`n_qps == requested`。
2. open 合成单 extent target 后，device handle 非空；拷回 host 检查 size/block/nsid/extent/d_qps 字段与输入一致。
3. >8 extent target：验证 overflow 分配、拷回的第 9+ extent 正确。
4. close 后 device handle 被释放且 identity 失效（用 test accessor 状态或计数证明，不访问已 free 指针）。
5. shutdown 自动释放未 close target，再次 initialize/open 成功。
6. queue group 构造失败时 initialize 结构化失败且 ctrl 无泄漏（可用非法 queue_depth=0 或 num_queues=0 的受控用例；不要破坏 daemon）。
7. 既有 memory registration 仍可用（HOST/DEVICE ioaddrs 非 0）。

测试不发数据面 NVMe IO。

# 5. 你只能修改/创建

- `tutti/data_paths/local_nvme/local_nvme_data_path.h`
- `tutti/data_paths/local_nvme/local_nvme_data_path.cpp`
- `tutti/data_paths/local_nvme/io/nvme_queue_group.h`
- `tutti/data_paths/local_nvme/io/nvme_queue_group.cu`
- `tutti/data_paths/local_nvme/io/device_target.h`
- `tutti/data_paths/local_nvme/io/device_target.cu`
- `tests/local_nvme_datapath_contract/CMakeLists.txt`
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`
- `chat/round8/result1.md`

构建只能写 `build/round8-session1*`。

禁止修改：main/旧 source、`nvme_storage/**`、`device_manager/**`、`backends/**`、`tutti/include/**`、binding/resolver、libnvm 源码、其他测试/CMake。

# 6. 安全限制

禁止 `sudo`、模块操作、bind/unbind、mkfs/mount/umount、启停 daemon、数据面 block IO。允许 client attach、queue group/ring 创建、CUDA 分配/H2D、DMA ring/data map。不得触碰 `/dev/md0`/`/mnt/nvme4`。

# 7. 验收

```bash
rm -rf build/round8-session1
cmake -S tests/local_nvme_datapath_contract -B build/round8-session1 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round8-session1 -j8
ctest --test-dir build/round8-session1 --output-on-failure
```

并验证：

- `ldd` 使用 `build/lib/libnvm.so`；
- queue group/target handle 真实 GPU pointer 非空；
- source 与 port 的创建/销毁步骤对照表逐项一致；
- Round 7 既有测试全部保留通过；
- daemon/pci/mount/module/RAID 状态不变；
- 文件边界、空白、EOF 正常。

# 成功标准

1. Queue group 创建/销毁顺序忠实搬运 main；
2. device target 布局与 source 语义一致，inline/overflow 都覆盖；
3. DataPath 资源释放顺序正确（target → queue → ctrl），无半初始化泄漏；
4. 仍不虚报 IO capability；
5. 所有既有+新增测试通过，零新增告警；
6. 未发数据面 IO，环境未改变；
7. 未修改允许列表外文件。

# 结果落盘

写入 `chat/round8/result1.md`，至少包含：source→port 文件/符号对照、所有机械适配、资源创建/销毁顺序、capabilities、测试完整输出、真实 pointer/字段证据、环境与边界核验、最终 PASS/BLOCKED。

不要寒暄、不要提交 Git commit、不要写「总指挥验收」。
