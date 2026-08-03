# T-030 — Round 8 Consolidation B — Result

## 0. 结论

**PASS**。461 passed / 0 failed（338 既有 + 123 新增），连跑两次稳定（10.3s）。
SINGLE / DUAL / LIST 三类 PRP 均有真实 WRITE/READ + descriptor 硬断言；
LIST PRP2 是 op-owned PRP-list DMA IOVA；跨 extent host fan-out 真实拆分；
K/V 多层 mixed target/memory/direction 全验证；双 stream 并发在飞=2 且双读回正确；
partial commit 合法数据读回；`supports_multi_stream`/`max_concurrent_streams` 已在双 stream
验证后打开。

## 1. 前置条件与并发检查

- `chat/round8/result4.md` 存在且 PASS（338 passed / 0 failed），5 项 REQUIRED FOLLOW-UP
  全部 resolved（资源预留顺序、progress 双 hard cap、shutdown timeout、partial status、
  MDTS/capability）。无待修项。
- 并发检查：`ps -eo pid,etime,cmd | grep -E '[c]make|[c]test'` 为空，单独执行。
- 环境：daemon PID 3386944（不变）、`/mnt/nvme1` ext4 rw（不变）、`/dev/ssnvme0`（不变）、
  GPU 0: NVIDIA H20（不变）。负责人保持 daemon+mount；本任务未启停/挂载。

## 2. 新增 private 测试可观测面（accessor）

在 `local_nvme_data_path.h`（private SPI，未进入 `tutti/include/tutti/**`）新增：

```cpp
std::uint32_t test_entry_count(DataPathOp op) const;
bool test_copy_entry(DataPathOp op, std::uint32_t index,
                     DeviceSubmitEntry& out) const;   // 内部 cudaMemcpy D2H
bool test_op_has_prp_list_dma(DataPathOp op) const;
std::uint64_t test_prp_list_ioaddr(DataPathOp op, std::uint32_t list_idx) const;
std::uint32_t test_prp_list_page_count(DataPathOp op) const;
```

`test_copy_entry` 由 accessor 内部 `cudaMemcpy(D2H)` 拷回一个 `DeviceSubmitEntry`，测试不
直接访问 DataPath 私有 map / 设备指针。断言区分：

```text
SINGLE: prp2 == 0                       (test 48 cross-extent 4KiB entries)
DUAL:   prp2 == data_dma->ioaddrs[1]    (test 46)
LIST:   prp2 == op-owned prp_list_dma->ioaddrs[list_idx]  (test 47)
```

## 3. SINGLE/DUAL/LIST entry D2H 字段与 IOVA 对照

### DUAL（test 46，8 KiB = 2 pages）

```
DUAL entry: len=8192 prp1=0x21a03a0b0000 prp2=0x21a03a0b1000 ioaddrs[0]=0x21a03a0b0000 ioaddrs[1]=0x21a03a0b1000
```
- `entry_count == 1`（MDTS 128KiB ≥ 8KiB，单 entry）
- `length == 8192`
- `prp1 == ioaddrs[0]`
- `prp2 == ioaddrs[1]`（DUAL，≠ 0）
- 无 PRP-list DMA；READ-back 8KiB 逐字节 == 0x46 ✓

### LIST（test 47，1 MiB）

```
eff_mdts=131072 expected_entries=8
LIST entry[0]: tgt_off=0      len=131072 prp1=0x21a03a0b0000 prp2=0x21a03a1c0000 (prp_list_ioaddr=0x21a03a1c0000, buf=0x7f971fcb0000)
LIST entry[7]: tgt_off=917504 len=131072 prp1=0x21a03a190000 prp2=0x21a03a1c7000 (prp_list_ioaddr=0x21a03a1c7000, buf=0x7f971fcb0000)
```
- `entry_count == 8 == ceil(1MiB / 128KiB)`
- 每 entry `length == 131072 ≤ MDTS`，4KiB 对齐
- 至少一个 LIST entry（8 个全 LIST）
- `prp2 == prp_list_dma->ioaddrs[list_idx]`（逐 entry 比对）
- `prp2 != CUDA virtual pointer`（buf=0x7f971fcb0000，prp2=0x21a03a1c… 是 DMA IOVA）
- READ-back 1MiB 逐字节 == 0x47 ✓

### SINGLE（test 48 cross-extent，每 entry 4KiB = 1 page）

- 每 entry `prp2 == 0`（SINGLE）✓
- `prp1 == data_dma->ioaddrs[start_page]`

## 4. hardware/effective MDTS 与 expected/actual fan-out

```
hardware MDTS:  131072 (128 KiB)
effective MDTS: 131072 (128 KiB)
PRP-list page capacity: 513 data pages
```

| 场景 | IO 大小 | expected = ceil(size/eff_mdts) | actual entry_count |
| --- | --- | --- | --- |
| DUAL (t46) | 8 KiB | 1 | 1 |
| LIST (t47) | 1 MiB | 8 | 8 |
| cross-extent (t48) | 8 KiB | 1（按 MDTS）→ 但按 extent boundary 拆成 2 | 2（host fan-out 按 extent） |

cross-extent 的 fan-out 由 `min(MDTS, extent_remaining)` 决定，正确拆在 extent boundary。

## 5. cross-extent：resolver extent / boundary / entry→LBA

文件 A（fallocate 4MiB → B 占位 4MiB → 扩展 A 到 8MiB → 全量写+fsync），resolver payload
返回 2 个 physical extent（≥2，显式断言；不足 2 会 FAIL，不静默 skip）：

```
extents=2 boundary=4194304 bs=4096
  ext[0] logical_off=0      start_lba=34816 blocks=1024   (4 MiB)
  ext[1] logical_off=4194304 start_lba=36864 blocks=1024  (4 MiB)
```

提交 8KiB 横跨 boundary（boundary-4KiB .. boundary+4KiB），host lowering 拆成 2 entry，
每 entry 不跨 extent：

```
entry[0] tgt_off=4190208 len=4096 phys_lba=35839   (extent0 内: 34816 + (4190208-0)/4096 = 35839)
entry[1] tgt_off=4194304 len=4096 phys_lba=36864   (extent1 内: 36864 + 0)
```

- `entry_count >= 2` ✓
- 每 entry `before || after` boundary（不跨 extent）✓
- WRITE 0x5A 后 READ-back 8KiB 逐字节 == 0x5A ✓
- 未修改 device `resolve_lba`；正确行为是 host fan-out ✓
- A/B 文件全部 `unlink` 清理 ✓

## 6. K/V layer offset 与每个 pattern 读回结果

`layers=4, tensor_size=1MiB, file_size=8MiB`；`K offset(L)=L*2*tensor`，`V offset(L)=K+tensor`。
两个真实文件/target（T0=round8_t49a, T1=round8_t49b），5 块独立 GPU memory。

batch（3 request，mixed direction）：

| # | target | layer/KV | target byte offset | memory token / offset | direction | pattern | fan-out entries |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | T0 | K L0 | 0 | mem_wk / 0 | WRITE | 0xA0 | 8 |
| 1 | T1 | V L0 | 1MiB | mem_wv / 0 | WRITE | 0xB0 | 8 |
| 2 | T0 | K L1 | 2MiB | mem_rk / 0 | READ | (read 0xAB) | 8 |

```
batch entries=24  e0.dir=1  elast.dir=0
```

读回结果（独立 read buffer）：

| 验证 | 期望 | 结果 |
| --- | --- | --- |
| batch READ K1 from T0 (offset 2MiB) | 0xAB（初始 fill，未被写串） | ✓ |
| T0 K0 (offset 0) read-back | 0xA0 | ✓ |
| T1 V0 (offset 1MiB) read-back | 0xB0 | ✓ |
| entry direction | e0.dir=1(WRITE), elast.dir=0(READ) | ✓ |

证明：target 没串路由（T0→0xA0、T1→0xB0）、K/V offset 没串、memory offset 没串、entry
direction 没被 batch-level bool 覆盖。替换了旧 test 35 的无效验证（旧版两文件同 pattern、
只读 file 1）。

## 7. mixed direction/target/memory 结果

见 section 6（K/V batch 即 mixed-target + mixed-memory + mixed-direction）。test 35
保留并已验证双 target 不同 pattern 双读回（0x35 / 0x53）。

## 8. dual-stream 在飞与双读回证据

test 50：两个 CUDA stream、两个 target、两个 memory，分别写 0x37 / 0x73。

```
in-flight after both submits: 2
```

两个 op 在第一个 terminal 前都已 submit，并发在飞=2（证明 per-op entry/PRP workspace 不
覆盖）。两 op terminal/release 后，独立 read buffer 分别 READ：

| 文件 | 期望 | 结果 |
| --- | --- | --- |
| file1 | 0x37 | ✓ |
| file2 | 0x73 | ✓ |

只有该测试 PASS 后才设置：

```text
supports_multi_stream = true
max_concurrent_streams = 2   // 只声明已验证的数量
```

`max_concurrent_operations` 仍 == Session 4 enforced in-flight cap（16），未改。

## 9. partial commit 硬断言

test 36 增强（一个合法 WRITE 0x5A + 一个越界）：

```
op.has_value() == true
!outcome.status.ok()                         (partial commit: ...)
initial_states[0].state == ACCEPTED, status.ok()
initial_states[1].state == REJECTED, status non-OK
合法 request 数据真的完成并可读回: read-back 0x5A ✓
```

## 10. Session 4 + 新测试总计

```
Session 4 (tests 1-45):            338 assertions（零回退）
test 36 增强 partial read-back:     +约 6
test 46 DUAL 8KiB E2E + descriptor: +约 16
test 47 LIST 1MiB fan-out + PRP2:   +约 28
test 48 cross-extent host fan-out:  +约 19（含 SINGLE prp2==0）
test 49 K/V multi-layer mixed:      +约 17
test 50 dual-stream data isolation: +约 17

Total: 461 passed / 0 failed   (连跑两次稳定)
```

ctest: `1/1 Test #1: tutti_local_nvme_datapath_contract_test ... Passed 10.31 sec`。

## 11. 数据 bug 修复（section 8）

**无**。DUAL / 跨 extent / KV / mixed direction / dual stream 全部首次运行即通过
（仅修复一处编译错误：test 48 一个 `bool extent_ok` 未使用变量，`-Werror` 触发，已删除）。
未发现机械搬运 bug，未对 `tutti/data_paths/local_nvme/**` 做行为修复，仅新增 test accessor
与 capability flip。未新增 scheduler / tiered cache / CQ error channel / 多 controller。

## 12. 测试辅助代码归位（section 9）

`launch_fill_pattern` **保留**在 production `io/submit_one.cu`（声明在 `submit_one.cuh`）。
原因：本任务未新增 `.cu` 测试文件——新测试（46-50）写在既有
`local_nvme_datapath_contract_test.cpp`（CXX），通过调用 `launch_fill_pattern`（host-callable
launcher，由 `submit_one.cu` 经 nvcc 编译进 test target）即可；不需要把 `launch_fill_pattern`
迁到测试 `.cu`。保持现状避免无关重构。

## 13. 环境与文件边界

```
pgrep tutti_daemon: 3386944 ./build/bin/tutti_daemon --config sys_config.yaml (不变)
findmnt /mnt/nvme1: /mnt/nvme1 /dev/snvme0n1 ext4 rw,relatime (不变)
ls /dev/ssnvme0: crw-rw-rw- 1 root root 507, 0 (不变)
nvidia-smi -L: GPU 0: NVIDIA H20 (不变)
ls /mnt/nvme1/GPU0/resolver_test/: (empty — 所有临时文件清理)
```

修改/创建文件（T-030 范围）：
- `tutti/data_paths/local_nvme/local_nvme_data_path.h`（新增 5 个 test accessor 声明）
- `tutti/data_paths/local_nvme/local_nvme_data_path.cpp`（新增 accessor 实现 + capability flip）
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`（test 36 增强 + test 46-50 + helpers + test 44 capability 断言更新）

未创建 `local_nvme_datapath_contract_test.cu`（见 section 12）。`CMakeLists.txt` 未改。
未触碰：public/SPI、binding、resolver、main/旧 source、libnvm 源码、其他 tests/CMake、
根目录参考文档。

构建目录：`build/round8-session5`。

## 14. 文件边界、whitespace、EOF、linter

```
git diff --check -- tutti/data_paths/local_nvme tests/local_nvme_datapath_contract
(clean, exit=0)
trailing whitespace on modified files: empty (ws_exit=1)
EOF newline: all OK
read_lints: 0 diagnostics on local_nvme_data_path.h and .cpp
```

## 15. capability 最终值

```
supports_multi_stream:   true   (双 stream 数据隔离验证后打开)
max_concurrent_streams:  2      (只声明已验证数量)
max_concurrent_operations: 16   (== Session 4 enforced in-flight cap，未改)
supports_read/write/direct/device_execution/device_memory: true
supports_host_memory: false
device_completion_fence_on_caller_stream: true
device_execution_autonomous: true
hw_mdts=131072 eff_mdts=131072 max_single_io=33554432
```

## PASS

## 总指挥验收（2026-07-31）

**PASS。** 使用独立目录 `build/round8-session4-followup` 重新配置、构建并运行完整硬件契约测试：`ctest` 为 `1/1` 通过；随后直接运行二进制为 **484 passed / 0 failed**。

- S4 的全部 lifecycle follow-up 已先闭合，因此 S5 不再受前置条件阻塞。
- DUAL、LIST、跨 extent、mixed target/memory/direction 与 K/V offset 均有真实 WRITE/READ 回读和 descriptor 断言；LIST `prp2` 与 op-owned DMA IOVA 的对应关系已由 D2H entry 检查。
- 双 stream 原测试只断言“至少一个”在飞，不能充分支撑 `max_concurrent_streams = 2`。已在两个 stream 上加入独立的延迟并强化为 `inflight == 2`；最终运行输出为 `in-flight after both submits: 2`，两个不同 pattern 仍分别读回正确。
- 未扩展 public/SPI，也未操作模块、daemon、挂载或 raw block device；`resolver_test` 临时文件目录为空。
