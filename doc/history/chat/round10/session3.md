# TASK T-042 — Round 10 Session 3：snvme 共享 UAPI 头与 ABI 握手

## 前置条件

- Session 1 完成：snvme kernel module 与 userspace libnvm 的唯一事实源位置已确定。
- 阅读 `Roadmap.md` Phase 3 deliverable「`include/uapi/tutti_snvme.h` shared by userspace and kernel; fixed-width types, ABI version/capability handshake, compat ioctl strategy」。
- 现状：snvme 的 ioctl 号、struct 布局定义在 kernel module 自有头与 libnvm 内部头中，用户态/内核各自维护，无版本握手。

## 目标

建立唯一共享 UAPI 头，userspace（libnvm/device_manager）与 kernel（snvme module）都 include 它；加入 ABI version/capability 握手与固定宽度类型断言，为后续 kernel compat 隔离（Session 4）打底。本 session 只做头文件与握手，不改 kernel module 行为逻辑。

## 允许修改/创建

- `tutti/include/uapi/tutti_snvme.h`（新建；位置可按 Session 1 决议调整，但必须同时对 userspace 与 kernel 可 include）
- snvme kernel module 源码（仅改为 include 共享头 + 加握手实现，删除本地重复定义）
- libnvm / device_manager 中引用 snvme ABI 的源码（同上）
- `tests/` 下新增 hardware-free UAPI 契约测试目录
- `tutti/CMakeLists.txt`（测试接线）
- `chat/round10/result3.md`

## 禁止范围

- 不改变任何 ioctl 语义、命令号、struct 现有字段布局（只可搬运到共享头并加 static_assert 锁定；发现布局依赖内核内部类型时必须停下来在 result 中记录，不得擅自改字段）。
- 不新增 ioctl 命令；不实现 compat 转换层本体（属 Session 4）。
- 不执行 insmod/rmmod/daemon/mount；kernel module 只允许 compile-only。
- 不把 UAPI 头引入 `tutti/include/tutti/` public API（它走 private target include，属 Session 5 卫生检查范围）。
- 不提交 Git。

## 必须实现的行为

1. 共享头内全部类型为固定宽度（`uint32_t`/`uint64_t` 等），无 `long`/`size_t`/指针宽度依赖；每个 ioctl struct 有 `_Static_assert(sizeof(...)==...)` 与关键字段 offset 断言。
2. 定义 `TUTTI_SNVME_ABI_VERSION` 与 capability bitmask；libnvm/device_manager 打开设备时执行 handshake ioctl（或既有版本查询路径），版本不兼容时 fail-closed（明确 `Status`/errno，不静默继续）。
3. kernel 侧编译时校验自身 struct 与共享头一致（同款 static_assert）；userspace 侧同等断言。
4. 32/64-bit compat 策略以文档化注释+断言落地：列出哪些 struct 在 compat 下布局不变（应为全部），哪些 ioctl 需要 `compat_ptr` 处理；本 session 只保证断言与策略，转换实现留给 Session 4。
5. userspace 与 kernel 引用同一物理头文件（给出两边 include 路径证据）；删除所有本地重复 ABI 定义。

## 测试要求

- 新增 hardware-free 契约测试：静态断言生效（可用负例编译测试或 `static_assert` 单测 TU）、ABI version 常量唯一事实源、capability bit 定义稳定。
- kernel module 对当前内核 compile-only 通过（`make` 或既有 module 构建路径；不允许 insmod）。
- HOST/CUDA standalone build 不回归；libnvm 相关目标编译无新警告。

## 验收

- `chat/round10/result3.md` 含：UAPI struct/断言清单、握手失败路径证据（单测或代码走查）、kernel compile-only 日志、两边引用同一头的证据。
- 总指挥复核：抽查共享头断言与握手 fail-closed 路径，复跑 HOST/CUDA build + module compile-only。

## 后续依赖

- Session 4（compat 隔离）依赖本 session 的共享头与握手；Session 5 门禁依赖。
