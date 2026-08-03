# T-024 Session Result

## 1. 交付文件列表与角色

| 文件 | 角色 |
|------|------|
| `tutti/data_paths/local_nvme/local_nvme_data_path.h` | `LocalNvmeDataPath` 类声明 + `LocalNvmeTargetState` / `LbaExtent` 值类型 |
| `tutti/data_paths/local_nvme/local_nvme_data_path.cpp` | 全部 13 个 SPI 方法实现 |
| `tests/local_nvme_datapath_contract/CMakeLists.txt` | 独立 CMake 构建，`-Werror`，无 CUDA/libnvm |
| `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp` | 10 类 contract test，57 个断言 |

## 2. 类的形状

- **继承**：`class LocalNvmeDataPath : public DataPath`（公开继承全部 13 个纯虚方法）
- **命名空间**：`tutti::data_paths::local_nvme`（与目录结构一致，`data_paths` 区分于旧 `backends/`）
- **构造/析构**：默认构造在构造体中预填 capabilities；析构做 best-effort cleanup
- **成员**：
  - `DataPathCapabilities caps_` — 构造时定型
  - `bool initialized_` — lifecycle 状态
  - `std::uint64_t next_token_` — 递增 token 计数器
  - `std::unordered_map<uint64_t, LocalNvmeTargetState> targets_` — token → 状态表
  - `const LocalNvmeTargetState* find_(DataPathTarget) const` — 按 token+generation 查找

## 3. capabilities 逐项的值及依据

| 字段 | 值 | 依据 |
|------|-----|------|
| `name` | `"local_nvme"` | 数据路径标识 |
| `source_api_version` | `1` | SPI 版本 1 |
| `supports_host_execution` | `true` | 骨架控制逻辑在 host 上运行 |
| `supports_device_execution` | `false` | 无 CUDA |
| `supports_host_memory` | `false` | registration 未实现 |
| `supports_device_memory` | `false` | 无 CUDA |
| `supports_direct` | `false` | 无 IO 路径 |
| `supports_staged` | `false` | 无 IO 路径 |
| `supports_read` | `false` | 无 IO |
| `supports_write` | `false` | 无 IO |
| `target/memory/length_alignment_bytes` | `1` | 骨架无法强制 NVMe block 对齐 |
| 所有 limits | `0` | 无 IO 容量 |
| `supports_scatter_gather` | `false` | 未实现 |
| `registration_scope` | `PER_TARGET` | 每个 target 有独立 domain |
| `progress_model` | `HOST_POLL` | 骨架是 host 侧 |
| `device_completion_fence_on_caller_stream` | `false` | 无设备执行 |
| `device_execution_autonomous` | `false` | 无设备执行 |
| `supports_multi_stream` | `false` | 未实现 |
| `supports_multi_gpu` / `supports_cross_device` | `false` | 未实现 |

**无虚报**：所有 `false`/`0` 的项确实是骨架做不到的。唯一的 `true` 是 `supports_host_execution`（骨架本身在 host 上运行）。

## 4. lifecycle / open / close / registration_domain 实现要点

### `initialize(config, resources)`
- 记录 config.name（若非空），置 `initialized_ = true`
- **不打开设备、不链 libnvm、不 CUDA**
- `ResourceProvider&` 被忽略（前向声明类型，骨架不使用——Device Manager 接入是后续 control-plane 任务）

### `shutdown(timeout_ns)`
- 幂等：清空 `targets_`，置 `initialized_ = false`
- 重复调用安全返回 OK

### `open(target)`
1. `view_payload(target)` 取得 `const Ext4LocalNvmePayload*`；失败返回 payload view 的错误
2. 校验 `block_size != 0`、`controller_pci_addr` 非空
3. 字节→block 转换（见下节）
4. `SpiIdentityMint::mint<DataPathTargetTag>(token, 1)` 铸造身份（token 递增，generation 从 1 起）
5. 存入 `targets_[token]`，返回身份

### `close(target)`
- 校验 `target.valid()` → 否则 `INVALID_ARGUMENT`
- 查找 `targets_` 中 token+generation 匹配项 → 否则 `NOT_FOUND`
- **从 map 中 erase** → 完全使身份失效
- 重复 close 已关闭身份 → `NOT_FOUND`（不静默成功）

### `registration_domain(target)`
- 查找 target（`find_`），失败返回 `NOT_FOUND`
- 派生字符串键：`"local_nvme:<pci_addr>:ns<namespace_id>"`
- 无裸指针，同一 target 两次调用返回相同键

## 5. 字节→block 转换及 logical_offset 保留选择

### 转换公式

```
start_lba      = device_offset / block_size
length_blocks  = length / block_size
```

### logical_offset 保留：是

**选择**：保留 `logical_offset_bytes` 在 `LbaExtent` 结构中。

**理由**：旧 `NvmeFileDeviceHandle` 的 `LbaExtent` 没有 logical offset（靠 extent 顺序 walk 推出逻辑位置），但未来 IO 提交路径需要将 file-relative byte range 映射到正确的 extent。丢弃 logical_offset 会需要重新从顺序 walk 推算，而 payload 已提供该信息。保留它是无损转换，不增加复杂度。

### block_size_log 计算

额外计算 `log2(block_size)` 存入 `LocalNvmeTargetState::block_size_log`，与旧 `NvmeFileDeviceHandle::nvme_block_size_log` 对应，供未来快速 LBA 转换使用。校验 block_size 是 2 的幂（NVMe LBA 大小保证）。

## 6. 如何避免旧代码 P0-8 悬空问题

**旧代码 P0-8**：`release_target_handle` 只从 `target_handles_` 删除并 `cudaFree`，但没清 `target_handle_cache_`，导致后续 acquire 可能返回已释放的指针。

**骨架的避免方式**：

1. **单一存储**：`targets_` map 是 target 状态的唯一存储，没有单独的缓存层。
2. **close 完全移除**：`close()` 从 `targets_` 中 `erase`，使 token+generation 对完全消失。
3. **后续查找失败**：`find_()` 按 token+generation 查找；close 后查找返回 `nullptr` → 调用方得到 `NOT_FOUND` 错误。
4. **无 token 复用**：`next_token_` 单调递增，即使 close 后 token 也不会被重新分配。

## 7. 显式失败各方法的返回与理由

| 方法 | 返回 | 理由 |
|------|------|------|
| `register_memory` | `Result<DataPathMemory>::Failure(UNSUPPORTED)` | registration 是后续任务 |
| `unregister_memory` | `Status(UNSUPPORTED)` | 同上 |
| `submit` | `SubmitOutcome{status=UNSUPPORTED, op=nullopt, initial_states=[count×REJECTED+UNSUPPORTED]}` | 无 IO 路径 |
| `progress` | `Result<ProgressResult>::Failure(UNSUPPORTED)` | 骨架无提交的工作可推进；返回零进度 ProgressResult 会误导为「操作正常但空闲」 |
| `query` | `Result<DataPathSnapshot>::Failure(NOT_FOUND)` | 骨架从不 mint op |
| `release` | `Status(NOT_FOUND)` | 同上 |

### submit 不变量

- `op == nullopt` ✓（零发出）
- `initial_states.size() == count` ✓
- 每项 `state == REJECTED`、`status` 非 OK ✓

### progress 选择

选择返回 `UNSUPPORTED` 失败而非零进度 `ProgressResult`。理由：骨架不能提交工作，因此永远没有工作可推进。返回零进度会暗示数据路径正常运行但空闲，是误导。

## 8. 显式推迟的部分及理由

| 推迟项 | 理由 |
|--------|------|
| Device Manager / vdevice roster 接入 | 这是 control-plane 任务；骨架的 `initialize` 是「无设备」的 |
| Device-resident `NvmeFileDeviceHandle` 分配 (cudaMalloc + H2D) | 依赖 CUDA 与 queue pair；骨架不引入 CUDA |
| Memory registration (PRP cache, DMA mapping) | 依赖 libnvm 与 CUDA；是独立的后续任务 |
| IO 提交 (queue pairs, doorbell, completion polling) | 依赖以上全部前置 |

## 9. 10 类测试用例逐一的实现与结果

### 1. capabilities honest
- 验证 name 非空、version ≥ 1
- 逐项验证 `supports_host_execution=true`、其余 `false`/`0`
- 打印 capabilities 供人工核对
- **PASS** (8 checks)

### 2. lifecycle
- `initialize` → OK
- `shutdown` → OK
- 再 `shutdown` → OK (幂等)
- **PASS** (3 checks)

### 3. open success
- 合成 `ResolvedTarget` → `open()` 返回有效 `DataPathTarget`
- `valid()` 为真
- **PASS** (3 checks)

### 4. open rejects wrong payload
- 用 `ResolvedTarget::make<int, int>` 造错误 payload type 的 target
- `open()` 失败，错误信息含 "payload type mismatch"
- **PASS** (2 checks)

### 5. registration_domain
- 对打开的 target 返回非空键
- 键以 `"local_nvme:"` 开头，包含 PCI addr 和 ns id
- 同一 target 两次调用返回相同键
- **PASS** (6 checks)

### 6. close invalidates identity
- `close()` → OK
- 再 `close()` 同身份 → 失败
- `registration_domain()` 同身份 → 失败
- **PASS** (3 checks)

### 7. close unknown identity
- 默认构造 `DataPathTarget`（`valid() == false`）
- `close()` → 失败，不 crash
- **PASS** (2 checks)

### 8. explicit failure
- `register_memory` → UNSUPPORTED
- `unregister_memory` → UNSUPPORTED
- `submit` → status 非 OK、op == nullopt、initial_states.size() == 2、每项 REJECTED + 非 OK status
- `progress` → UNSUPPORTED
- `query` → 非 OK
- `release` → 非 OK
- **PASS** (12 checks)

### 9. open multiple targets
- 连续 open 3 个，token 递增（1, 2, 3）
- registration_domain 不同
- **PASS** (5 checks)

### 10. byte→block conversion
- 已知 extent: device_offset=0/8192, length=4096, block_size=4096
- 验证 start_lba=0/2, length_blocks=1/1, logical_offset=0/4096
- **PASS** (12 checks)

## 10. 测试完整输出

```
--- 1. capabilities honest ---
  PASS (×8)
  name=local_nvme version=1
  host_exec=1 dev_exec=0
  host_mem=0 dev_mem=0
  direct=0 staged=0 read=0 write=0
  align: tgt=1 mem=1 len=1
--- 2. lifecycle ---
  PASS (×3)
--- 3. open success ---
  PASS (×3)
  token=1 gen=1
--- 4. open rejects wrong payload ---
  PASS (×2)
  error: open: payload view failed: payload type mismatch
--- 5. registration_domain ---
  PASS (×6)
  domain: local_nvme:0000:08:00.0:ns1
--- 6. close invalidates identity ---
  PASS (×3)
--- 7. close unknown identity ---
  PASS (×2)
  error: close: target identity is invalid (never minted)
--- 8. explicit failure ---
  PASS (×12)
  register_memory: not yet implemented (skeleton); memory registration is a subsequent task
--- 9. open multiple targets ---
  PASS (×5)
  tokens: 1, 2, 3
--- 10. byte→block conversion ---
  PASS (×12)
  extent[0]: lba=0 blocks=1 log_off=0
  extent[1]: lba=2 blocks=1 log_off=4096

=== Summary ===
  passed: 57
  failed: 0
RESULT: PASS
```

CTest: `1/1 Test #1: tutti_local_nvme_datapath_contract_test ... Passed 0.00 sec`

## 11. 依赖约束核验结果

```
# include 列表:
local_nvme_data_path.h: <tutti/status.h>, <tutti/io_types.h>,
  <tutti/spi/data_path.h>, <tutti/spi/storage_target_resolver.h>,
  <tutti/bindings/ext4_local_nvme/binding.h>, <cstdint>, <string>,
  <unordered_map>, <vector>
local_nvme_data_path.cpp: "tutti/data_paths/local_nvme/local_nvme_data_path.h",
  <cstring>

# 禁止依赖 grep:
仅注释中出现 "cuda"、"libnvm"（描述推迟的功能），无实际 #include 依赖。
no forbidden dependency: PASS
```

## 12. 无 CUDA / libnvm 链接证据

```
grep -nE 'target_link_libraries|find_package|cuda|libnvm|nvm' CMakeLists.txt
```

命中仅为注释（`# No CUDA, no libnvm, no hardware.`）和 `project(... LANGUAGES CXX)`。无 `target_link_libraries` 链接 CUDA 或 libnvm，无 `find_package(CUDA)`。

## 13. 头文件独立编译 + 共存编译结果

### 独立编译

```
echo '#include "tutti/data_paths/local_nvme/local_nvme_data_path.h"' | c++ -std=c++17 -Wall -Wextra -Werror -DTUTTI_USE_HOST=1 -I.../tutti/include -I.../Tutti -x c++ -fsyntax-only -
EXIT=0
```

### 共存编译（与 memory_types.h + data_path.h + storage_target_resolver.h 同 TU）

```
EXIT=0
```

无重复定义、无命名冲突 ✓

## 14. 三个既有 contract test 结果

```
data_path_contract: 100% tests passed, 0 tests failed out of 1
storage_target_resolver_contract: 100% tests passed, 0 tests failed out of 1
binding_contract: 100% tests passed, 0 tests failed out of 1
```

全部通过 ✓

## 15. hygiene 检查

```
tutti/data_paths/local_nvme/local_nvme_data_path.h: OK
tutti/data_paths/local_nvme/local_nvme_data_path.cpp: OK
tests/local_nvme_datapath_contract/CMakeLists.txt: OK
tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp: OK
```

所有文件无尾随空白，EOF newline OK ✓

### 文件边界

本 session 创建的文件：
- `tutti/data_paths/local_nvme/local_nvme_data_path.h`
- `tutti/data_paths/local_nvme/local_nvme_data_path.cpp`
- `tests/local_nvme_datapath_contract/CMakeLists.txt`
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`
- `chat/round7/result2.md`

未修改允许列表外的任何文件 ✓

### 安全限制

未执行 sudo/insmod/rmmod/modprobe ✓
未启动 daemon/client ✓
未访问 /dev/nvme* ✓
未执行 CUDA 调用或硬件 IO ✓

## 16. 最终结论

PASS

## 总指挥验收

验收结论：`PASS`。DataPath SPI 的第一个具体实现骨架成立，干净、诚实、无遗留。

独立核验结果：

- **SPI 方法全覆盖。** `LocalNvmeDataPath` 公开继承 `DataPath` 并实现全部 13 个纯虚方法；`open()` 经 `view_payload()` 取得 payload（不绕 binding），字节→block 转换（`device_offset/block_size`、`length/block_size`）正确。
- **capabilities 诚实。** 唯一为 `true` 的是 `supports_host_execution`；registration/IO/设备执行全部如实 `false`/`0`，无虚报。这是 `DataPathCapabilities` 作为硬约束的正确姿态。
- **P0-8 悬空问题被正确规避**（我重点核了这条）：`close()` 从 `targets_` map 中 `erase`，`find_()` 按 token+generation 查找，close 后查找返回 `NOT_FOUND`；`next_token_` 单调递增不复用。没有旧代码那种「release 只删一半、cache 残留悬空指针」的结构。
- **显式失败且保持 SPI 不变量。** `submit` 返回 `op == nullopt`、`initial_states.size() == count`、每项 `REJECTED`+`UNSUPPORTED`；`progress` 返回 `UNSUPPORTED` 而非零进度（worker 的理由正确：骨架没有可推进的工作，返回零进度会误导为「正常运行但空闲」）。
- **logical_offset 保留**：worker 选择把它存进 `LbaExtent`，理由成立（未来 IO 需要 file-relative 映射，payload 已提供，无损）。这是「适配新接口所必需」的合理保留，不是顺手改进。
- **依赖干净**：`#include` 清单无禁止项；`cuda`/`libnvm` 仅出现在注释（说明推迟项），无真实依赖；未 link CUDA/libnvm（grep + CMakeLists 确认）。
- **我独立重跑 CTest：`1/1 Passed`（57 断言）。** 头文件独立编译、与三个既有契约头共存编译、三个既有 contract test 均通过。
- 交付文件尾随空白与 EOF newline 均 OK；文件边界干净（只新增允许列表内文件）；未执行任何硬件操作。

非阻塞观察（记录，不返工）：`supports_host_memory = false` 与 Session 3 实现 registration 后需要翻转为 `true` —— 骨架的诚实填写意味着 Session 3 必须同步更新 capabilities，否则会出现「能注册但 capabilities 说不能」的矛盾。这不影响本 session 的判定（骨架阶段如实填 false 是对的），但 Session 3 应处理。

后续决定：T-024 完成，DataPath SPI 骨架落地，为 Session 3 的 registration 提供了正确的扩展点。
