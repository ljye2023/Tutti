# TASK T-032 — Round 9 Session 2：StorageRuntime 并发、错误与 shutdown 硬化

## 前置条件

- 先阅读 `TUTTI_TARGET_ARCHITECTURE.md` 的 Runtime/registry/shutdown 契约、`TUTTI_REFACTOR_TAKEOVER.md`、`chat/round8/result4.md`、`chat/round8/result5.md`。
- Session 1 完成后执行；若 standalone CMake target/contract test 接线未通过，报告 `BLOCKED`。
- 当前 `StorageRuntime` 已具备组件注入、resolver→DataPath open、惰性 registration、target grouping、partial commit 和 public local-NVMe E2E，但 registry 和 `DataPath::progress()` 尚未具备明确并发保护与错误终态语义。

## 目标

在不把 NVMe/CUDA/PRP 私有类型放入公开 Runtime 的前提下，完成最小 host control-plane 安全契约：

1. public `StorageRuntime` 方法可被多 host thread 安全调用；
2. 同一个 `DataPath` 同时最多一个 `progress()` 调用；
3. `open/close/register/unregister/submit/query/wait/release_io/shutdown` 与 in-flight IO 的竞争只产生安全成功、明确 `BUSY`、`NOT_READY` 或可观察终态，绝不能 use-after-free、计数丢失或 handle 复活；
4. `DataPath::progress()`、`query()`、`release()` 的失败不能让 Runtime IoHandle 永远停在 `IN_FLIGHT`；
5. `shutdown(timeout)` 失败时保留 `DRAINING` 和全部资源，后续 `wait/query/shutdown` 可继续推进；析构不得为了释放内存绕过这个规则。

## 允许修改/创建

- `tutti/include/tutti/storage_runtime.h`
- 仅在确有 API 语义缺口时修改：`tutti/include/tutti/status.h`、`tutti/include/tutti/io_types.h`、`tutti/include/tutti/spi/data_path.h`
- `tests/storage_runtime_contract/**`
- `tests/data_path_contract/**`
- 若 Session 1 的接线要求，`tutti/CMakeLists.txt`
- `chat/round9/result2.md`

## 禁止范围

- 不修改 `LocalNvmeDataPath`、resolver、binding、libnvm、kernel。
- 不新增后台永久 polling thread、cancel API、priority、notification、transport enum。
- 不在 Runtime include 或暴露 CUDA/libnvm/NVMe/PRP 类型。
- 不用锁覆盖 DataPath 虚调用导致死锁或长时间持锁；不以“DataPath 必然线程安全”代替 Runtime 的 registry 安全。
- 不做真实硬件测试；本 session 只用 HOST fake resolver/DataPath。
- 不提交 Git。

## 实施约束

- 采用最小清晰的所有权模型：Runtime 只借用 injected resolver/DataPath；调用者必须使其活过 Runtime 的成功 shutdown。不得悄悄转为 Runtime delete/own injected components。
- Runtime registry 的锁与 DataPath call 的锁分离。调用 DataPath 前必须拥有足以阻止同一 target/memory 被 close/unregister 的 Runtime strong reference/inflight credit；DataPath call 后再以 generation 检查提交结果。
- 为每 DataPath 建立 progress serialization gate；不同 DataPath 可并发 progress。
- DataPath progress/query 失败时，将对应 public op 终结为 `FAILED` 并保留可观察 `Status`；只在 DataPath op 已可安全 release 时调用 release。
- 保持现有 stub mode（无组件注入）的所有语义；`StorageRuntimeTestAccess` 只能继续用于 stub mode。
- 不在未经必要性证明的情况下扩张 public `IoResult`。如果 per-request terminal result 是必需阻塞点，先将缺口与最小 proposed shape 记录在 result，不要暗自设计大型 API。
- 析构如遇仍在飞的 component-backed IO，只能采取与 DataPath 安全契约一致的保守策略；不允许提前 unmap/free。若 C++ 析构无法传播 timeout，宁可保守泄漏并清晰记录，不得 UAF。

## 必测场景

在 `tests/storage_runtime_contract` 的 fake DataPath 中新增确定性测试：

1. 多 thread 同时 `submit/query/wait/release`；
2. close/unregister 与 submit 竞争；
3. 同一 fake DataPath 两个 Runtime IoHandle 并发 query，验证 progress 最大并发为 1；
4. fake `progress/query/release` 返回错误，Runtime op 不永久 IN_FLIGHT，wait 能观察 FAILED；
5. shutdown(0) → DRAINING/TIMEOUT → 后续 progress/wait → 再次 shutdown → STOPPED；
6. terminal result 上限/backpressure、generation 与跨 Runtime 隔离无回归；
7. 所有现有 component route、multi-target grouping、partial commit、lazy registration 缓存继续通过。

可使用 `std::thread`、barrier/condition variable 和 deterministic fake seam；必须避免 sleep-only 的脆弱竞态测试。

## 验收

```bash
cmake -S tutti -B build/round9-session2-host \
  -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON \
  -DTUTTI_BUILD_HARDWARE_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round9-session2-host -j8
ctest --test-dir build/round9-session2-host --output-on-failure
```

附加：对 Runtime/DataPath contract 至少连续运行 20 次；若项目环境支持 ThreadSanitizer，可另建不提交的 build 运行该二进制。验收需证明 HOST profile 不引入 CUDA/libnvm 依赖，diagnostics 为 0，`git diff --check` clean。

## 结果落盘

写 `chat/round9/result2.md`，包含：

- 锁/credit/progress gate 的精确模型；
- DataPath 失败如何映射为 Runtime terminal failure；
- shutdown/析构 timeout 的资源策略；
- 并发测试清单与重复运行结果；
- 保留的 public API 缺口（如有）；
- 修改文件与最终 `PASS`/`BLOCKED`。

不要提交 Git。