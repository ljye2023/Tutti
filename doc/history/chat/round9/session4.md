# TASK T-034 — Round 9 Session 4：LocalNvmeDataPath 有界完成与真实错误传播

## 前置条件

- Session 1 已确认 standalone CUDA target 使用 `tutti/device_manager/nvme/libnvm/` 的唯一 standalone source；Session 2 已定义 Runtime 如何消费 DataPath failure。
- 阅读 `MAIN_IO_PATH.md`、`MAIN_MEMORY_PRP_PATH.md`、`chat/round8/result4.md`、`chat/round8/result5.md` 与现有 local-NVMe contract tests。
- 当前 SINGLE/DUAL/LIST、跨 extent、mixed request、双 stream 与 public Runtime happy path 已有真实验证；但 SQE 全零初始化、每 request NVMe/CQ 错误状态、bounded CQ polling 仍未闭合。

## 目标

在不改 public/SPI、不中断已有 happy path 的情况下，使 `LocalNvmeDataPath` 能把 device-side command failure 变为可观察 terminal failure，并消除无界 CQ polling 对 stream/shutdown 的永久阻塞风险。

## 允许修改/创建

- `tutti/data_paths/local_nvme/**`
- `tutti/device_manager/nvme/libnvm/include/nvm_parallel_queue.h`（仅在没有安全私有 wrapper 可实现有界 polling 时）
- `tutti/device_manager/nvme/libnvm/include/nvm_cmd.h`（仅为明确 zero-init helper 时）
- `tests/local_nvme_datapath_contract/**`
- 必要时 Session 1 新增的 local-NVMe private CMake target
- `chat/round9/result4.md`

## 禁止范围

- 不修改 snvme UAPI、kernel module、根目录 legacy libnvm 或旧 IO engine。
- 不执行模块/daemon/mount/bind/unbind/format/raw LBA IO。
- 不加入 reset/retry/failover/persistent kernel/metadata cache/性能策略。
- 不把 CUDA/NVMe completion 类型暴露进 `tutti/include/tutti/**` 或 SPI。
- 不把“event 完成”误报为“所有 NVMe command 成功”。
- 不提交 Git。

## 必须实现的行为

1. 每次 SQE 发送前，完整零初始化 `nvm_cmd_t`，再设置 CID/opcode/nsid/DPTR/SLBA/NLB；保留字段不可携带栈垃圾。
2. `resolve_lba` 失败、queue acquire 异常、CQ completion status 非成功、device poll timeout 都必须写入**每 entry 私有 completion/status storage**；kernel 不只 `printf` 后继续把 op 标成功。
3. DataPath op 汇总 completion storage：
   - 所有 entry 成功才是 `COMPLETED`；
   - 任一 entry 失败，DataPath op 为 `FAILED`，`Status` 保留第一项稳定错误摘要；
   - `bytes_transferred` 只计已确认成功的 bytes，不能盲目写 total。
4. CQ poll 必须有可配置、可测试的硬上限（计数或等价单调 device 时间预算）。到达预算后返回 timeout failure；不得无界 `while(true)`。
5. timeout/failure 后在没有确认 queue/DMA quiesce 前，operation、PRP-list DMA、entry/status workspace、memory/target 仍受 BUSY/lifetime 保护。只有安全终态后才允许 `release/unregister/close`。
6. CUDA launch/event/stream 状态仍按 Round 8 的 issued-vs-unissued 不变量处理；不得把 launch 后错误变为 `op == nullopt`。

## 测试要求

保留全部 501 个现有断言并增加：

- SQE zero-init 的可重复 source/unit evidence；
- resolve_lba failure injection → `FAILED` 而非成功；
- NVMe completion error injection → `FAILED`、bytes 不虚报；
- CQ timeout injection/可控 bounded poll → timeout terminal 或严格的 non-terminal isolation（必须说明选择及 release 规则）；
- LIST PRP timeout 仍验证 raw/aligned/DMA mapping 不早释放；
- 真实受控文件 SINGLE/DUAL/LIST WRITE/READ 无回归；
- 两个 host thread 在不同 stream 上 submit/query/release 的 race regression，验证内部 table/token/workspace 不竞态。

故障 branch 可使用 private test seam，但至少一条真实 READ/WRITE 回归必须经真实 queue/module/mount 执行。不得用 synthetic success 替代 happy-path 回归。

## 验收

使用 Session 1 的正式 target/CTest 开关构建。hardware test 只显式运行，且前后确认 daemon/mount/module 未被本任务改变。输出必须含：

- 每 entry completion/status 的内部表示与 op 聚合规则；
- CQ budget 的单位、默认值和 timeout 后资源状态；
- 注入 failure 的 `Io/DataPathSnapshot` 实际结果；
- 真实 SINGLE/DUAL/LIST 回归；
- 临时文件清理、`git diff --check`、linter 0 diagnostics；
- 最终 assertions 计数与 `PASS`/`BLOCKED`。

写入 `chat/round9/result4.md`，不要提交 Git。