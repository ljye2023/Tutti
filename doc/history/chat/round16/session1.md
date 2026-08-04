# TASK T-086 — Round 16 Session 1：P0 正确性修复（handle cache UAF + striped unregister UAF + partial-commit 警告）

> 三项均为 Roadmap Phase A（doc/review/ 四份评审发现，总指挥已对实际代码实证）。修复后跑全量硬件回归。**环境前提**：snvme 模块 + daemon + 双盘挂载（`insmod→daemon→mount`）。

## P0-1：HandleWorkspaceCache reopen→eviction 悬空 GPU handle

**已实证的机理**（勿重复调查）：`handle_workspace_cache.h` 的 `get_or_build` 命中路径（:122-128）只做 `touch_lru_`，**不恢复 `in_use=true`**。序列 `open(A)→close(A)→open(A)→open(B)→submit(A)`：close(A) 后 entry 可逐出（`release_entry` 置 `in_use=false` 并入 LRU）；重开 A 命中但 `in_use` 仍 false；open(B) 触发 evict → `destroy_handle()`；submit(A) 经 `tstate->dev_handle` 使用已释放显存 → UAF。同文件并发 open 共享 cache_key（extent 签名，`local_nvme_data_path.cpp:648`），`in_use` 布尔扛不住并发，正解是 open 引用计数。cache 默认 OFF（cap=0），仅 cache ON 触发。

**修复要求**：
- `Entry` 引入 `open_refcount`（替代/补充 `in_use` 语义）：`open()` 命中与新建均 `++`；`close()` `--` 且仅在 0 时入 LRU；evict 条件 `pin_count==0 && open_refcount==0`；`shutdown()` 按 open_refcount 结清（>0 时保守保留 handle 不 destroy，与 timeout 语义一致）。
- 同模式核查 `PrpPageCache`（pin/evict 生命周期）——若有同类"命中不恢复占用标记"缺陷一并修复。
- **回归测试**（cache ON，`TUTTI_HANDLE_CACHE_CAP`/`TUTTI_PRP_CACHE_CAP` 环境变量或构造参数）：
  1. `open(A)→close(A)→open(A)→open(B)→submit(A)`（cap=1 强制逐出路径）WRITE+READ 字节级正确；
  2. 同文件两个并发 open target，close 其一后另一 target submit 仍正确（open_refcount>0 不被逐出）；
  3. cache OFF 默认路径零行为变化（820 断言全绿）。

## P0-2：StripedDataPath op 未记录 memory token

**已实证**：`OpEntry::memory_token`（`striped_data_path.h:228`）全文件无赋值点，`memory_has_inflight_ops_()`（:1215）恒 false → `unregister_memory()` 在 striped op 在飞时直接 `nvm_dma_unmap`。

**修复要求**：
- `submit()` 按 `DataPathMemoryToken` 收集 accepted 请求的 token 存入 `OpEntry`（集合或单 token 视 op 内请求是否同 memory 而定——batch 可跨 memory，须为集合）；`memory_has_inflight_ops_()` 检查该集合。
- **回归测试**（striped 契约目录）：
  1. striped op 在飞时 `unregister_memory()` 返回 BUSY/被拒绝（不 unmap），wait 后数据完整；
  2. drain 后 unregister 成功；
  3. 既有 46 断言零回归。

## P0-3：partial-commit 语义声明处警告

`storage_runtime.h` `submit()` 声明注释增加醒目契约说明：返回的 `SubmitOutcome.io` 只覆盖**本轮被接受**的请求；调用方必须逐一检查 `initial_states`，被拒请求（`RESOURCE_EXHAUSTED` 等）须重投，否则数据静默丢失（附 R14 S4 事故一行引用）。

## 边界

- 只准改：`metadata/handle_workspace_cache.h`、`metadata/prp_page_cache.{h,cpp}`（仅核查发现时）、`striped_data_path.{h,cpp}`、`storage_runtime.h`（注释）、契约测试文件。
- 禁止改任何默认值/配额；禁止动 submit_one.cuh / fused kernel / Runtime 分组。
- 硬件回归：820+46+137 断言全绿（新增断言另计）；非硬件 ctest 15/15；`git diff --check` clean；双盘测试目录清空。

## 结果文件

`chat/round16/result1.md`：两项修复的 diff 摘要与语义论证、新回归测试输出、全量回归证据、**改动文件清单**。
