# T-035 — Round 9 Session 5: Public Runtime → File → Local-NVMe Acceptance Gate — Result

## 0. 结论

**PASS**。建立了独立、正式、只使用公开 Runtime API 的 hardware acceptance gate
`tests/storage_runtime_local_nvme_contract/`。115 assertions / 0 failed，连跑两次稳定
（0.54s）。所有数据面操作只走 `StorageRuntime::create / register_memory / open /
submit / query / wait / release_io / close / unregister_memory / shutdown`；私有
`LocalNvmeDataPath`+`LocalFileResolver` 仅作注入 fixture，从不被直接调用做 IO。

## 1. 前置条件

- Round 9 result1–4 全部 PASS：
  - result1：build/test target 正式接线（10 个可执行，test 53 端到端 4KiB）。
  - result2：Runtime 并发/失败语义闭合（`registry_mutex_`/`io_cv_`/`progress_gates_`/
    submit credit，20× repeat）。
  - result3：resolver fail-closed（FIEMAP/fd-lease/payload 兼容）。
  - result4：LocalNvmeDataPath 有界错误传播（entry result 码、FAILED、timeout 资源保护）。
- 公开 `StorageRuntime` API 完整（create/register_memory/open/submit/query/wait/
  release_io/close/unregister_memory/shutdown + `RuntimeComponents` 注入 + 懒注册
  `registration_for_`）。
- 并发检查：无其他构建进程；单独执行。

## 2. 注入装配与公共 API 调用链

fixture（私有，仅构造，不做 IO）：

```cpp
LocalNvmeDataPath dp("/dev/ssnvme0", 16384, 0, 2, 64, 1, 4096);
LocalFileResolver resolver("0000:08:00.0", 1, 4096, BackingDeviceConfig{"/dev/snvme0n1", 0});
RuntimeComponents components;
components.resolvers.push_back({"file", &resolver});
components.data_paths.push_back({"local-nvme-ext4", &dp, DataPathConfig{"local_nvme"}});
auto rt = StorageRuntime::create({}, std::move(components)).value();
```

数据面调用链（每个场景）：
`open(file://...) → register_memory → submit → wait → release_io → close → unregister_memory → shutdown`。
不调用 `LocalNvmeDataPath::open/register_memory/submit/progress/query/release`，不手工
`nvm_dma_map_data_*()`。

## 3. SINGLE / DUAL / LIST / cross-segment 真实回读

| 场景 | 大小 | WRITE pattern | READ-back | 结果 |
| --- | --- | --- | --- | --- |
| SINGLE | 4 KiB | 0x51 | 0x51 | ✓ |
| DUAL | 8 KiB | 0x52 | 0x52 | ✓ |
| LIST | 1 MiB | 0x53 | 0x53 | ✓ |
| cross-segment | 8 KiB @4MiB-4KiB | 0x54 | 0x54 | ✓ |

cross-segment：fallocate A 4MiB + B 4MiB 占位 + 扩展 A 到 8MiB + 全量写+fsync，强制
2 段物理分配；公共 API 在 4MiB 边界前后各 4KiB 提交 8KiB IO，host lowering 按 segment
边界 fan-out，逐字节回读 0x54 正确。

## 4. batch / partial / mixed direction/target/memory 证据

- mixed batch：`WRITE t1(0x61) + READ t2` 一批提交，`initial_states.size()==2`，
  `io.has_value()`，COMPLETED；t2 READ 回读 0xAB（方向未被 batch bool 覆盖），t1 回读 0x61。
- partial commit：1 合法 WRITE + 1 越界 WRITE：
  - `io.has_value()`（IoHandle 保留）
  - `!status.ok()`（总体非 OK）
  - `initial_states[0].state==ACCEPTED, status.ok()`
  - `initial_states[1].state==REJECTED, !status.ok()`
  - 已接受 request 数据真实完成并可读回（0x77）

## 5. 顺序与并发

- 同 stream producer→IO→consumer：fill 0x55 → WRITE → READ → 回读 0x55 ✓
- 两 stream 同时在飞：op1(s1,t1,0x37) + op2(s2,t2,0x73) 均提交后才 drain；独立 read buffer
  分别回读 0x37 / 0x73 ✓（per-op workspace 不覆盖）
- 两个 host thread 经同一 Runtime submit/query/release：thread1(t1,0x81) +
  thread2(t2,0x82)，各自 write+read+verify，`failures=0` ✓（Runtime 线程安全）

## 6. 失败与 timeout

- resolver reject：`open("noscheme://...")` → 非 OK；`open(missing file)` → 非 OK；
  DataPath key mismatch（注入 wrong-key DataPath）→ 非 OK。无 private target 泄漏。
- DataPath error：越界 WRITE → `!io.has_value()` + `!status.ok()`（无主 IO）。
- observation timeout：submit WRITE → `wait(io, 0)` → `!observation_status.ok()`，
  `!result.has_value()`（TIMEOUT，op 仍 IN_FLIGHT 且可 `query`）；`wait(io, 5000)` → COMPLETED。
- shutdown timeout→retry：submit WRITE → `shutdown(0)` → 非 OK（TIMEOUT，资源保留）→
  `wait` drain → `release_io` → `shutdown(1000)` → OK。
- 所有已发出 IO 在 public IoHandle 下可观察，无无主 IO。

## 7. lazy registration 与 teardown 证据

- `register_memory` 在任何 submit 前成功（懒：尚无 data-path DMA map）；
  `query_memory.inflight_count == 0`。
- 首次 submit 触发懒 data-path 注册（成功）；第二次 submit 复用同一 domain 注册（成功）。
- teardown 顺序：`release_io → close → unregister_memory（释放懒 DMA map）→ shutdown`。
- 重复 lifecycle：2 轮完整 create→open→register→submit→release→close→unregister→shutdown，
  无残留临时文件/handle/DMA mapping（temp dir 为空）。

## 8. CMake label / default-off 行为

- 测试在 `tutti/CMakeLists.txt` 的 `if(TUTTI_BUILD_HARDWARE_TESTS)` 块内 `add_subdirectory`，
  仅 `TUTTI_BUILD_HARDWARE_TESTS=ON` 且 CUDA/local-NVMe private target 可用时注册。
- CTest labels：`hardware;local_nvme;runtime_e2e`（`Label Time Summary` 确认三个 label 均生效）。
- `TUTTI_BUILD_HARDWARE_TESTS=OFF`（HOST/default）：`ctest -N` 中该测试计数 = 0（不配置、
  不访问 CUDA/libnvm/mount）。HOST CTest 10/10 全绿。

## 9. CUDA compile/link gate

`cmake -S tutti -B build/round9-session5 -DTUTTI_ACCELERATOR=CUDA -DBUILD_TESTING=ON
-DTUTTI_BUILD_HARDWARE_TESTS=ON -DTUTTI_BUILD_HARDWARE_STACK=ON`：configure 5.7s 成功；
`libnvm` + `tutti_local_nvme_datapath` + `tutti_storage_runtime_local_nvme_contract_test`
编译/链接通过，零告警。

## 10. 显式 hardware CTest

```
1/1 Test #13: tutti_storage_runtime_local_nvme_contract_test ... Passed 0.54 sec
Label Time Summary:
  hardware       = 0.54 sec (1 test)
  local_nvme     = 0.54 sec (1 test)
  runtime_e2e    = 0.54 sec (1 test)
```

二进制直跑：7 场景全部执行，`passed: 115 / failed: 0 / RESULT: PASS`，连跑两次稳定。

## 11. public consumer 边界

测试 `.cpp` 中数据面逻辑只用公开类型（`IoRequest`/`IoHandle`/`IoSubmitOutcome`/
`IoSnapshot`/`WaitOutcome`/`MemoryView`/`TargetHandle`/`MemoryHandle`/`HostSubmitContext`/
`IoState`/`IoRequestState`）。私有头（`local_nvme_data_path.h`、`submit_one.cuh`、
`resolver.h`）仅用于 fixture 构造。

```
$ grep -nEi 'libnvm|PRP|extent|LBA|[^a-zA-Z]fd[^a-zA-Z]' .../storage_runtime_local_nvme_contract_test.cpp
(empty, grep_exit=1)
```

不出现 libnvm、PRP、extent、LBA、fd。

## 12. 环境与清理

```
pgrep tutti_daemon: 3386944 ./build/bin/tutti_daemon --config sys_config.yaml (不变)
findmnt /mnt/nvme1: /mnt/nvme1 /dev/snvme0n1 ext4 rw,relatime (不变)
nvidia-smi -L: GPU 0: NVIDIA H20 (不变)
ls /mnt/nvme1/GPU0/resolver_test/: (empty — 所有临时文件清理)
```

daemon/mount/module/RAID 未变；未操作模块/bind/unbind/mkfs/mount/raw block device；
未接触 `/mnt/nvme4`；未提交 Git。

## 13. 修改/创建文件

- `tests/storage_runtime_local_nvme_contract/CMakeLists.txt`（创建）
- `tests/storage_runtime_local_nvme_contract/storage_runtime_local_nvme_contract_test.cpp`（创建）
- `tutti/CMakeLists.txt`（修改：`if(TUTTI_BUILD_HARDWARE_TESTS)` 块内新增 add_subdirectory，
  纯新增，未重构既有内容）
- `chat/round9/result5.md`（创建）

未修改 `tutti/include/tutti/storage_runtime.h`、`tutti/data_paths/local_nvme/**`、
`tutti/resolvers/local_file/**`（公开 API 与私有实现均完整，无需最小修复）。未触碰
public nouns/SPI、binding、main/旧 source、其他 tests/CMake、根目录参考文档。

## 14. 文件边界、whitespace、linter

```
git diff --check -- tutti/CMakeLists.txt : exit 0 (clean)
trailing whitespace (new files)         : empty
EOF newline                              : all OK
read_lints (test .cpp)                  : 0 diagnostics
```

## PASS

## 总指挥验收

验收结论：**PASS（条件生效）**。独立复跑：CTest `1/1 Passed`（9.99s，labels `hardware;local_nvme;runtime_e2e` 全部生效），直跑 `115 passed / 0 failed`，7 个场景输出与报告一致；HOST 默认 CTest `10/10` 通过且不包含本测试。装配只用公开 Runtime API 做数据面操作，fixture 不直接调用 DataPath IO 方法；SINGLE/DUAL/LIST/cross-segment 真实回读、mixed/partial commit、同/跨 stream、双 host thread、wait(0)/shutdown(0) TIMEOUT 与 retry、两轮 lifecycle 均满足 prompt 验收清单。临时目录为空，环境未变。

**生效条件（已满足，2026-08-01）**：S4 follow-up 已通过总指挥验收且 S4 升级为最终 PASS（`result4b.md`）。因 session4b 修改了 `release()` 语义，本门禁已按约定复跑：**115 passed / 0 failed**，无回退。本结果转为正式无条件 PASS，Round 9 关闭。
