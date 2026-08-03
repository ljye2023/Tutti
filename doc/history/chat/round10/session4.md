# TASK T-043 — Round 10 Session 4：snvme kernel compat 与 GPU pinning 隔离

## 前置条件

- Session 3 完成：共享 UAPI 头与 ABI 握手已就位。
- 阅读 `Roadmap.md` Phase 3 deliverable「Kernel-version differences isolated in compat ops; GPU pinning isolated in `peer_memory_ops`」及 gate「at least two supported kernel baselines compile-only CI」。
- 现状：snvme kernel module 源码中内核版本相关分支（如有）与 NVIDIA P2P/pin 逻辑（`nvidia_p2p_get_pages`/`nv_p2p_*` 或 peer_memory 路径）散布在模块主体中。

## 目标

把内核版本差异收进显式 compat ops 表，把 GPU pinning 收进 `peer_memory_ops` 风格的隔离单元；模块主体只调用稳定内部接口。全程 compile-only，不加载模块。

## 允许修改/创建

- snvme kernel module 源码树（`tutti/device_manager/nvme/kernel_modules/**` 或 Session 1 决议位置）
- 新增 `compat.h/.c`、`peer_memory.h/.c`（命名可协商，职责必须单一）
- `scripts/`（两个内核 baseline 的 compile-only 脚本）
- `chat/round10/result4.md`

## 禁止范围

- 不改变模块对外 ABI（共享 UAPI 头中的 struct/ioctl 一字节不动）。
- 不改变 pinning 行为的语义（pin/unpin 时机、引用计数、错误路径保持原样；只移动与封装）。
- 不执行 insmod/rmmod/modprobe/daemon/mount；不在本机加载任何新编译的模块。
- 不引入对 NVIDIA 驱动源码的新依赖路径（若现有代码引用 `/usr/src/nvidia-*`，保持原样并在 result 中记录该脆弱点）。
- 不提交 Git。

## 必须实现的行为

1. 所有 `LINUX_VERSION_CODE`/`KERNEL_VERSION` 条件分支集中在 compat 单元；模块主体零散落版本判断（`git grep` 证明）。
2. GPU pin/unpin、P2P 页表获取全部经 `peer_memory` 单元的函数指针/ops 调用；主体不出现直接的 `nvidia_p2p_*`/`nv_p2p_*` 调用。
3. compat/peer_memory 各自可独立编译为对象；缺失 NVIDIA 头时 peer_memory 能以编译期开关降级为明确 `#error` 或 stub（不允许静默编出无 pin 能力的模块）。
4. 提供 `scripts/` 下 compile-only 脚本：对当前内核 + 至少第二个 baseline（容器/chroot/不同 headers 目录均可，实在没有第二个真实 baseline 时用 `make -C` 对另一套 kernel headers 树）构建模块，输出成功/失败矩阵。

## 测试要求

- 当前内核 compile-only 通过，无新警告。
- 第二 baseline compile-only 结果记录在案（允许失败，但失败必须归因到 compat 单元内具体位置——这正是隔离价值的证明）。
- userspace（libnvm/device_manager/LocalNvmeDataPath）build 不回归；不需要硬件 IO 测试。

## 验收

- `chat/round10/result4.md` 含：compat/peer_memory 接口清单、主体零版本判断的 grep 证据、两 baseline compile 矩阵、NVIDIA 头依赖风险记录。
- 总指挥复核：抽查隔离边界，复跑 compile-only 脚本。

## 后续依赖

- Session 5（门禁）依赖本 session 完成。
