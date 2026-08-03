# TASK T-052 — Round 11 Session 3：真异步 DEVICE_EXECUTION 与 stream 顺序证明

## 前置条件

- Session 1/2 完成；阅读 `Roadmap.md` Phase 4 异步相关 deliverable 与 gate、「`TUTTI_TARGET_ARCHITECTURE.md` progress/退避模型」。
- 现状（已核实）：submit 在 caller stream 上 launch IO kernel + `cudaEventRecord`；kernel 在 device 侧 doorbell+CQ poll，IO 推进不依赖 host query；`progress()` 仅 harvest（`cudaEventQuery` + D2H status）。隐藏同步点：`local_nvme_data_path.cpp:142`（test drain helper）与 `:1192`（event-record 失败回退的 `cudaStreamSynchronize`）。

## 目标

形式化 stream-ordered 异步执行：`submit()` 返回前 IO kernel 与完成 fence 均已入 caller stream，无任何隐藏 `cudaStreamSynchronize`；同 stream compute→IO→compute 与跨 stream producer-event→IO→consumer 顺序可证明；host 不 poll 时 stream 照样越过 IO 完成点。

## 允许修改/创建

- `tutti/data_paths/local_nvme/**`
- `tests/local_nvme_datapath_contract/**`、`tests/storage_runtime_local_nvme_contract/**`
- `chat/round11/result3.md`

## 禁止范围

- 不修改 public/SPI 语义（`HostSubmitContext`/event 语义不变）；不引入新 public 类型。
- 不改变 error/timeout 分类与 has_timeout 规则（Round 9 已验收）。
- 不做 kernel strategy 变更（Session 4）。
- 不执行模块/daemon/mount 操作；不提交 Git。

## 必须实现的行为

1. 同步点审计：DataPath 内全部 `cudaStreamSynchronize`/`cudaDeviceSynchronize`/`cudaMemcpy`（同步版）列出清单；生产路径只允许明确例外（event-record 失败回退，须注释论证）；test helper 不算违规但须标注。
2. 完成 fence：IO kernel 后在同一 stream 记录完成事件（现有 event 机制形式化为 fence 语义）；文档化「fence 之后入队的 stream 工作Happens-After IO 完成」。
3. 同 stream 顺序：compute kernel → IO kernel → compute kernel 依次入队，无 host 干预，后序 compute 读到 IO 结果（READ 后校验数据、WRITE 前数据被正确消费）。
4. 跨 stream 顺序：producer stream 填数据 + record event → IO stream `cudaStreamWaitEvent` → IO → record event → consumer stream wait 后校验；全程无 `cudaStreamSynchronize`。
5. host 不 poll 推进证明：submit 后 host 不调用 query/wait/progress，直接 `cudaStreamSynchronize(stream)`（caller 侧同步属于 caller 语义），stream 越过 IO 完成点且结果正确；op 终态随后由一次 progress 正确 harvest。

## 测试要求

保留全部既有断言并新增：

- 同 stream compute→IO→compute 数据正确性（GPU 侧 fill kernel 替代 host memset）；
- 跨 stream 三 stream（producer/IO/consumer）event 链顺序与数据正确性；
- 无 host poll 的 stream 推进（submit 后直接 stream sync，校验数据）；
- 同步点审计清单与例外论证写入 result；
- 真实 SINGLE/DUAL/LIST 与 S5 E2E 无回归。

## 验收

- `chat/round11/result3.md`：同步点审计表、fence 语义文档、三种顺序证明的测试输出、全量回归。
- 总指挥复跑两硬件契约，审查审计表完整性与 fence 论证。
