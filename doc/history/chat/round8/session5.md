# TASK T-030 — Round 8 Consolidation B

你是一名资深 CUDA/NVMe C++ 工程师。你的任务是对已经完成生命周期收口的 `LocalNvmeDataPath` 补齐 Round 8 缺失的**真实数据面证据**：DUAL、跨 extent、K/V-like offset、mixed target/direction、PRP2 IOVA、fan-out entry 和双 stream 数据隔离。

# 任务定位

**这是验证驱动的机械补齐，不是新设计。**

- main 基线固定为：`4862157d50c8a7004cdeb166dda630ab1ef4561a`。
- 必读：
  - `MAIN_IO_PATH.md`
  - `MAIN_MEMORY_PRP_PATH.md`
  - `chat/round8/result3.md` 的「总指挥验收」
  - `chat/round8/result4.md` 及其总指挥验收
- 搬运/验证对象保持 private：`AddressDescriptor`、PRP、entry、LBA、CUDA event 不进入 public/SPI。
- 不搬 tiered PRP cache；本轮继续验证 owned fallback。
- 不恢复旧 Coordinator/MemoryRegion/BlockStorage public API。

# 前置条件与顺序

**必须在 Session 4 完成、独立验收 PASS 且无 REQUIRED FOLLOW-UP 后串行执行。**

若 `chat/round8/result4.md` 不存在、未 PASS，或生命周期/capability 仍有待修项，报告 `BLOCKED`，不要开始。

本任务必须单独执行，不与其他 daemon/controller/CMake 任务并发。负责人保持 daemon + mount；你不启停、不挂载。

# 1. 增加最小 private 测试可观测面

当前结果声称 PRP2/fan-out，但日志没有硬证据。允许在 `LocalNvmeDataPath` 增加**仅用于 private contract test** 的 accessor，至少能在 op terminal/release 前取得：

```text
entry_count
某 entry 的 target pointer / target_offset / length / direction / prp1 / prp2
op 是否拥有 PRP-list DMA
某 LIST page 的 DMA IOVA（或 prp_list_dma->ioaddrs[index]）
```

推荐由 accessor 内部 `cudaMemcpy D2H` 拷回一个 `DeviceSubmitEntry`，测试不直接访问 DataPath 私有 map。

这些 accessor 只存在于 private `local_nvme_data_path.h`，不得进入 `tutti/include/tutti/**`。

输出/断言必须区分：

```text
SINGLE: prp2 == 0
DUAL:   prp2 == data_dma->ioaddrs[start_page+1]
LIST:   prp2 == op-owned prp_list_dma->ioaddrs[list_idx]
```

# 2. 补 DUAL（8 KiB）真实 E2E

新增独立测试：

1. 创建至少 8 KiB 的真实临时文件，初始 pattern A；
2. 64 KiB aligned GPU buffer，填不同 pattern B；
3. submit 8 KiB WRITE；
4. 断言 fan-out `entry_count == 1`（本环境 MDTS ≥8 KiB）；
5. 拷回 entry，断言：
   - `length == 8192`
   - `prp1 == data_dma->ioaddrs[0]`
   - `prp2 == data_dma->ioaddrs[1]`
6. 清空/改写 read buffer，再 READ 8 KiB，逐字节验证 pattern B。

该测试必须明确输出 `DUAL`、PRP1、PRP2 与两个 data IOVA。

# 3. 补 MDTS fan-out + LIST PRP2 硬证据

保留现有 1 MiB LIST roundtrip，并增强断言：

- 从 DataPath effective hardware MDTS 计算 expected entries：

```text
ceil(1 MiB / effective_mdts)
```

- `test_entry_count(op) == expected`；
- 每 entry length ≤ effective MDTS，block aligned；
- 至少一个 LIST entry；
- LIST entry 的 `prp2` 等于该 op-owned PRP-list DMA 的对应 `ioaddrs[list_idx]`；
- `prp2` 不等于 PRP CUDA virtual pointer；
- 输出每个 entry（或至少首/尾）的 target offset、length、kind、PRP1、PRP2；
- 逐字节 WRITE/READ pattern 仍通过。

不要只打印 data buffer `ioaddrs[0]` 冒充 PRP2。

# 4. 补真实跨 extent request

使用已验证的确定性方法：

```text
A: fallocate 4 MiB
B: fallocate 4 MiB（占住 A 后面的物理空间）
扩展 A 到 8 MiB
全量实际写入 A + fsync
```

要求：

1. `filefrag -v` 或 resolver payload 证明 A 至少 2 个 physical extent；若不足 2，测试显式 FAIL，禁止静默 skip/pass；
2. 找到第一个 extent 的 logical end；
3. 提交一个 block-aligned request，范围横跨该 boundary（例如 boundary 前 4 KiB + 后 4 KiB）；
4. host lowering 必须拆成至少 2 entries，且每 entry 不跨 extent；
5. 拷回 entry，输出/断言：
   - target logical offset
   - 解析后的 physical LBA（可用 target host state 计算）
   - length
6. WRITE distinct pattern 后 READ 回逐字节验证；
7. A/B 文件全部清理。

不要修改 device `resolve_lba` 让单 command 跨 extent；正确行为是 host fan-out。

# 5. 补 K/V-like 多层、mixed-target/memory/direction

按 main adapter 公式，但不引入 Adapter 类型：

```text
layers      = 4
tensor_size = 1 MiB
file_size   = 2 * layers * tensor_size
K offset(L) = L * 2 * tensor_size
V offset(L) = K offset + tensor_size
```

至少两个真实文件/target，并使用多块独立 GPU memory。测试要求：

1. 选择至少两个 layer；
2. K/V 使用不同 pattern（例如按 target/layer/KV 编码）；
3. 一个 batch 同时包含：
   - 两个 target；
   - 多个 memory identity / memory_offset；
   - K 与 V offset；
   - 至少一个 READ 与一个 WRITE（mixed direction）；
4. WRITE 部分完成后，用**独立 read buffers**读回两个 target 的每个已写范围；
5. 逐字节验证每个 pattern，证明：
   - target 没串路由；
   - K/V offset 没串；
   - memory offset 没串；
   - entry direction 没被 batch-level bool 覆盖。

替换旧 test 35 的无效验证：不得两个文件写同一 pattern、不得只读 file 1。

输出：

```text
target token
layer / K-or-V
target byte offset
memory token / offset
fan-out entries
pattern
verify result
```

# 6. 补双 stream 数据隔离后再打开 capability

替换旧 test 37 的“只看 completion”验证：

1. 两个 CUDA stream；
2. 两个 target、两个 memory，分别写不同 pattern（例如 0x37、0x73）；
3. 两个 op 在第一个 terminal 前都已 submit，证明并发在飞；
4. 两个 op 均 terminal/release；
5. 使用独立 read buffer 分别 READ 两个文件；
6. 逐字节验证 0x37 与 0x73，证明 per-op entry/PRP workspace 不覆盖；
7. 只有该测试 PASS 后才设置：

```text
supports_multi_stream = true
max_concurrent_streams = 2   // 只声明已验证的数量，除非额外验证更多
```

`max_concurrent_operations` 仍等于 Session 4 已 enforced 的 in-flight cap。

# 7. 补 mixed-target 与 partial-commit 硬断言

- mixed-target 测试必须两个 target 不同 pattern并双读回；
- partial commit 必须断言：

```text
op.has_value()
!outcome.status.ok()
accepted request → ACCEPTED + OK
rejected request → REJECTED + non-OK
合法 request 数据真的完成并可读回
```

# 8. 只允许外科手术式修复测试暴露的数据 bug

本任务以补测试为主。若 DUAL/跨 extent/KV/mixed direction/dual stream 暴露当前机械搬运 bug，允许修改 `tutti/data_paths/local_nvme/**`，但必须：

- 在结果中列出 failing test → root cause → 最小修复；
- 修复只能是适配冻结 SPI/已搬 main 行为所必需；
- 不新增 scheduler、tiered cache、CQ error channel、多 controller 或 framework 类型；
- 不改变 public/SPI。

# 9. 测试辅助代码归位

若新增 `.cu` 测试文件，建议把 `launch_fill_pattern` 从 production `submit_one.*` 移到 contract-test `.cu`，并删除 production 声明/实现；若保持现状，结果中说明原因。

不要为了清理而重构无关代码。

# 10. 你只能修改/创建

- `tutti/data_paths/local_nvme/**`
- `tests/local_nvme_datapath_contract/CMakeLists.txt`
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp`
- `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cu`
- `chat/round8/result5.md`

构建只能写 `build/round8-session5*`。

禁止修改：public/SPI、binding、resolver、main/旧 source、libnvm、其他 tests/CMake、根目录参考文档。

# 11. 安全限制

禁止 sudo、模块操作、bind/unbind、mkfs/mount/umount、启停 daemon、打开 raw block device、固定 LBA IO。只对 resolver 产生的临时文件 extent 做 IO。禁止触碰 `/mnt/nvme4`。

# 12. 验收

```bash
rm -rf build/round8-session5
cmake -S tests/local_nvme_datapath_contract -B build/round8-session5 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round8-session5 -j8
ctest --test-dir build/round8-session5 --output-on-failure
```

必须满足：

1. Session 4 的全部断言零回退；
2. SINGLE/DUAL/LIST 三类均有真实 WRITE/READ + descriptor 硬断言；
3. LIST PRP2 是 DMA IOVA，fan-out count/length 与 effective MDTS 一致；
4. 跨 extent request 真正拆分并逐字节验证；
5. K/V-like 多层、multi-target、multi-memory、mixed direction 全部验证；
6. partial commit 总体非 OK 且合法数据完成；
7. 双 stream 两个不同 pattern 都读回正确，capability 才可 true；
8. 所有临时文件清理；
9. daemon/mount/module/RAID 不变；
10. 文件边界、whitespace、EOF、linter 正常。

# 结果落盘

写入 `chat/round8/result5.md`，至少包含：

- SINGLE/DUAL/LIST entry D2H 字段与 IOVA 对照；
- hardware/effective MDTS 与 expected/actual fan-out；
- cross-extent 的 filefrag/resolver extent、boundary、entry→LBA；
- K/V layer offset 与每个 pattern 的读回结果；
- mixed direction/target/memory 结果；
- dual-stream 在飞与双读回证据；
- Session 4 + 新测试总计；
- 环境与边界；
- 最终 `PASS` / `BLOCKED`。

不要寒暄、不要提交 Git commit、不要写「总指挥验收」。
