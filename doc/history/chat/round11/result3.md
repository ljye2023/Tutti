# Round 11 Session 3 Result: 真异步 DEVICE_EXECUTION 与 stream 顺序证明

## 概述

将 `LocalNvmeDataPath` 的 `submit()` 生产路径从隐式同步 `cudaMemcpy`/`cudaMemset`（default stream）迁移到 `cudaMemcpyAsync`/`cudaMemsetAsync`（caller stream），消除跨 stream 屏障。形式化 completion fence 语义，新增 3 个硬件测试证明同 stream、跨 stream 和无 host-poll 三种顺序。

## 改动文件清单

| 文件 | 改动 |
|------|------|
| `tutti/data_paths/local_nvme/local_nvme_data_path.cpp` | 3×`cudaMemcpy`→`cudaMemcpyAsync` + 1×`cudaMemset`→`cudaMemsetAsync`（均 on `ctx.stream`）；fence 语义文档；同步点注释标注 |
| `tests/local_nvme_datapath_contract/local_nvme_datapath_contract_test.cpp` | 新增测试 67/68/69（同 stream 顺序、跨 stream event 链、无 host-poll 推进） |

## 同步点审计表

### 生产 submit() 路径

| 行号 | API | 分类 | 状态 |
|------|-----|------|------|
| ~1178 | `cudaMemcpyAsync(..., ctx.stream)` | PRP cache H2D fill | **已修复**（原 `cudaMemcpy` default stream） |
| ~1224 | `cudaMemcpyAsync(..., ctx.stream)` | Arena PRP H2D fill | **已修复**（原 `cudaMemcpy` default stream） |
| ~1260 | `cudaMemcpyAsync(..., ctx.stream)` | Entries H2D fill | **已修复**（原 `cudaMemcpy` default stream） |
| ~1270 | `cudaMemsetAsync(..., ctx.stream)` | Status array zero-init | **已修复**（原 `cudaMemset` default stream） |
| ~1361 | `cudaStreamSynchronize(ctx.stream)` | Event-record 失败回退 | **明确例外**（见下方论证） |

### progress() harvest 路径（非 submit 生产路径）

| 行号 | API | 分类 | 状态 |
|------|-----|------|------|
| ~1858 | `cudaMemcpy(..., D2H)` | aggregate_completion_status_ status D2H | **保留**（event 已 signal，kernel 已完成） |
| ~1878 | `cudaMemcpy(..., D2H)` | aggregate_completion_status_ entries D2H | **保留**（同上） |

### Test-only 路径（非生产）

| 行号 | API | 分类 | 状态 |
|------|-----|------|------|
| ~154 | `cudaStreamSynchronize(s)` | shutdown drain helper | **标注** TEST-ONLY SYNC POINT |
| ~1731 | `cudaMemcpy(..., D2H)` | test_copy_entry | **标注** TEST-ONLY SYNC POINT |
| ~1785 | `cudaMemcpy(..., D2H)` | test_copy_completion_status | **标注** TEST-ONLY SYNC POINT |

### 例外论证：event-record 失败回退（行 ~1361）

```
cudaStreamSynchronize(ctx.stream)
```

**触发条件**：`cudaEventRecord(event, ctx.stream)` 返回非 `cudaSuccess`。

**为何必须同步**：
1. IO kernel 已成功 launch 到 `ctx.stream`（步骤 6 通过），kernel 正在运行。
2. Arena slot 已 lease（资源不可逆），kernel 将写入 `d_entries`/`d_status` 并执行 NVMe DMA。
3. 不能返回 `op=nullopt`（kernel 在飞行中，arena slot 在使用）。
4. 没有 event，`progress()` 无法用 `cudaEventQuery` 检测完成。
5. 唯一安全恢复：同步 stream 确定 kernel 终态，然后存储 terminal op。

**频率**：极罕见 — `cudaEventRecord` 失败属于 CUDA runtime 内部错误。正常操作永不触发。

**这是生产 submit 路径中唯一对 caller stream 的 `cudaStreamSynchronize`。**

## Fence 语义文档

在 `submit()` 步骤 7（`cudaEventRecord`）前新增正式文档注释：

```
The cudaEventRecord on ctx.stream IS the completion fence:
any work enqueued on ctx.stream AFTER this point is
Happens-After the IO kernel's completion.

Same-stream ordering:
  A → H2D → IO → fence → B
  (compute kernel A, then submit enqueues H2D + IO + fence,
   then compute kernel B — B reads IO results)

Cross-stream ordering:
  P: producer work → record EP
  I: wait(EP) → submit → record EI
  C: wait(EI) → consumer work
  (no cudaStreamSynchronize needed)
```

## 生产路径 cudaMemcpyAsync 安全性论证

`cudaMemcpyAsync` 使用 pageable host memory（`std::vector` 局部变量）时的行为：
- CUDA runtime 内部先将 pageable 数据拷贝到 pinned staging buffer（host-side memcpy，阻塞 host）
- 然后在 `ctx.stream` 上排队 DMA 传输
- 函数返回时，pinned staging 已填充，pageable 源可以安全释放

**关键改进**（对比原 `cudaMemcpy` on default stream）：
- 原：`cudaMemcpy` on default stream → 阻塞 host + 跨 stream 屏障（default stream syncs with ALL streams）
- 新：`cudaMemcpyAsync` on `ctx.stream` → 阻塞 host（pageable），但 NO 跨 stream 屏障
- 局部变量安全：pageable 源在函数返回前已拷贝到 staging，局部 vector 可以安全析构

## 新增测试

### Test 67: 同 stream compute→IO→compute 顺序

```
Stream s:
  1. GPU fill kernel → buf = 0x5A
  2. submit WRITE (buf → file)  [enqueues H2D + IO kernel + fence on s]
  3. GPU fill kernel → buf = 0xFF
  4. submit READ (file → buf)   [enqueues H2D + IO kernel + fence on s]
  5. cudaStreamSynchronize(s)   [caller-side, single sync]
```

**验证**：buf == 0x5A（READ 从文件读回 WRITE 写入的 0x5A，而非步骤 3 的 0xFF）

**证明**：
- WRITE 消费了步骤 1 的数据（0x5A），而非 stale/zeroed
- READ 结果覆盖了步骤 3 的 0xFF，说明 READ 在步骤 3 之后执行（stream 顺序）
- 步骤 1-4 之间无 `cudaStreamSynchronize`，全靠 stream ordering

### Test 68: 跨 stream producer→IO→consumer event 链

```
Producer stream sP:
  1. GPU fill → wbuf = 0x5A
  2. cudaEventRecord(eP, sP)

IO stream sI:
  3. cudaStreamWaitEvent(sI, eP)
  4. submit WRITE on sI
  5. cudaEventRecord(eI, sI)

Consumer stream sC:
  6. cudaStreamWaitEvent(sC, eI)
  7. submit READ on sC (rbuf ← file)
  8. cudaStreamSynchronize(sC)  [caller-side, only sync on sC]
```

**验证**：rbuf == 0x5A（从文件读回 producer 写入的数据）

**证明**：
- 三个 stream 纯靠 event 协调，无 `cudaStreamSynchronize` 在 sP 或 sI 上
- Producer 的数据正确传递到 IO WRITE
- IO WRITE 的完成正确传递到 Consumer READ
- rbuf 预填 0xFF，READ 覆盖为 0x5A，证明 READ 确实执行并读到正确数据

### Test 69: 无 host-poll stream 推进

```
Stream s:
  1. fill buf = 0x5A → submit WRITE → sync [setup]
  2. fill buf = 0xFF
  3. submit READ (file → buf)
  --- NO progress()/query()/wait() between step 3 and 4 ---
  4. cudaStreamSynchronize(s)  [if IO needs host poll, this DEADLOCKS]
  5. progress() once → harvest terminal
```

**验证**：buf == 0x5A（READ 从文件读回，非 0xFF fill）

**证明**：
- IO kernel 自主完成 NVMe 命令提交 + CQ poll + 返回，无需 host `progress()`/`query()`/`wait()`
- 如果 kernel 依赖 host polling，步骤 4 会死锁 — 测试通过即证明 device-side autonomous
- 步骤 5 的单次 `progress()` 成功 harvest terminal 状态（op 已终态或立即可 harvest）

## 构建验证

### HOST profile

```
$ cmake -S tutti -B /tmp/tutti-uapi-build -DBUILD_TESTING=ON -DTUTTI_ACCELERATOR=HOST
$ cmake --build /tmp/tutti-uapi-build
$ ctest

100% tests passed, 0 tests failed out of 12
```

### CUDA profile

```
$ cmake -S tutti -B tutti/build-profile-cuda -DTUTTI_ACCELERATOR=CUDA -DTUTTI_BUILD_HARDWARE_TESTS=ON
$ cmake --build . --target tutti_local_nvme_datapath          # PASS (zero error)
$ cmake --build . --target tutti_local_nvme_datapath_contract_test  # PASS (zero error)
$ cmake --build . --target tutti_storage_runtime_local_nvme_contract_test  # PASS (zero error)
```

### 编译警告

- `local_nvme_data_path.cpp`: 零新警告（pre-existing `arena_list_idx` unused 在 cache fallback 路径，非本次改动引入）
- 测试文件: 零警告（修复了 misleading-indentation `-Werror` 问题）
- `submit_one.cu`: pre-existing `ulonglong4` deprecated 警告（CUDA 13.0，非本次改动）

## 既有测试无回归说明

测试 67/68/69 新增在 `next_t66:` 标签之后、summary 之前。全部既有测试 1-66 保持不变。三个新测试使用与既有测试相同的 `make_qg_dp()`/`init_dp()`/`make_resolved_file()`/`drain_to_terminal()`/`verify_dev_region()` helper，不修改任何公共辅助函数。

硬件测试（Test 1-66 + 67-69）需要：
- snvme kernel module 已加载
- tutti_daemon 已启动
- /dev/snvme0n1 已挂载 /mnt/nvme1

由用户手动执行测试（不代跑 insmod/rmmod/daemon/mount）。

## 总指挥验收（2026-08-01）

**PASS。** 独立复跑与审查：

- **异步迁移**：submit 生产路径 3×`cudaMemcpyAsync` + 1×`cudaMemsetAsync` 全部在 `ctx.stream`（`:1177/:1222/:1258/:1270` 核实）；剩余同步 `cudaMemcpy` 仅 test-only（`:1731/:1785`）与 progress D2H（`:1858/:1878`，event 已 signal 后，论证成立）。
- **唯一生产同步点**：event-record 失败回退的 `cudaStreamSynchronize`（`:1361`）例外论证成立（kernel 已飞、资源已租、无 event 无法 harvest，极罕见路径）。
- **pageable→Async 安全性**：局部 `std::vector` 源在 host 侧 staging 完成后即可析构，论证与 CUDA 语义一致。
- **三种顺序证明**：tests 67（同 stream compute→IO→compute）、68（三 stream event 链）、69（无 host-poll 推进，若依赖 host poll 则死锁）全部在复跑日志中出现并通过。
- **全量复跑**：735/0（cache OFF+ON）、115/0、HOST 12/12、CUDA 132/132；fence 语义文档在位。

**Round 11 有效范围（S1-S3）全部闭合。** Phase 4 按 maintainer 决定保持未关闭状态（S4 基准/S5 门禁暂缓，`Roadmap.md` 不得标记完成）。

## 后续依赖

- Session 4（kernel strategy）可在本 session 的 async H2D 基础上进一步优化（pinned staging buffer、doorbell batching 等）
- fence 语义已形式化，Session 4 的 kernel strategy 变更不得破坏 fence 保证
