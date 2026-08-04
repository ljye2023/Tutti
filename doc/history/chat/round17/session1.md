# TASK — Round 17 Session 1：daemon 生命周期（自动挂载/卸载 + 占用诊断）

**日期：** 2026-08-03（预生成，启动前总指挥复核）
**前置依赖：** R16 S4（性能专项）完成之后——本 session 的硬件验证需要重启 daemon，与 S4 的硬件运行互斥。
**模式：** 源码在 `tutti/device_manager/nvme/nvmeservice/`（与 S4 零文件交集）；硬件验证阶段独占 daemon。

---

## 背景（maintainer 需求原话）

当 daemon 通过 sys_config 启动时，直接挂载好；退出时尝试取消挂载；当卸载失败时告诉为什么（谁占用），用户处理（关掉占用的 session）后继续退出程序。

## 已对齐的设计决策（不要重新辩论）

1. **启动自动挂载**：按 sys_config 每个 nvme 条目的 `mount_path` 挂载（ext4）。挂载失败（已在别处挂载、fs 脏、设备不存在）：**打印原因 + 该设备继续运行但不挂载**（不直接退出 daemon）。
2. **退出自动卸载**：SIGTERM/SIGINT → 停止接受新请求 → drain 客户端 → 逐设备 umount。
3. **EBUSY 诊断**：umount 失败时扫描 `/proc/*/fd`、`/proc/*/maps`、`/proc/*/cwd`，列出占用该挂载点的 PID+进程名+占用类型，打印给用户；**按间隔重试**（如 1s × 可配置次数），期间收到第二次信号则强退（保留挂载并明确报告）。
4. 实现用 `mount(2)`/`umount2(2)` 系统调用（不 fork 外部命令）；日志走 daemon 现有日志通道。

## 工作项

1. sys_config schema：每 nvme 条目加 `auto_mount: true`（默认 true）与全局 `unmount_retry: {interval_ms: 1000, max: 30}`；解析 + 校验。
2. 挂载管理器（daemon 内）：启动挂载、状态跟踪（哪台设备是 daemon 挂的——只卸自己挂的）、退出卸载、EBUSY PID 扫描器（/proc 遍历，注意权限与 namespace）。
3. 信号处理：第一次 SIGTERM/SIGINT 走优雅流程（含卸载重试），第二次强退。
4. **主机侧单测**（不碰运行中 daemon）：/proc 解析、占用扫描（用临时 mount namespace 或 mock /proc 视图）、配置解析失败路径。
5. **硬件验证**：启动→确认 4 盘自动挂载→跑一个契约冒烟→SIGTERM→确认全部卸载→注入占用（shell `cd /mnt/nvme3` 或持 fd）→SIGTERM→确认诊断输出列出该 PID→释放占用→确认重试后卸载成功退出。
6. **新增（2026-08-04 总指挥）**：resolver 契约 "not regular file (directory)" 失败排查——连续 3 个 session 出现（1024 reload + 重挂载后的环境），确认是 ext4 UNWRITTEN 标志的环境语义变化还是真回归；是环境问题则修测试的环境适配，是真回归则修复并补防御。
7. **新增**：bring-up 文档/SOP 写明生产模块参数 `insmod snvme.ko io_queue_depth=1024`（64 是防呆默认）；daemon 启动前置检查可考虑读取 /sys/module/snvme/parameters/io_queue_depth 并 WARN（不强制）。

## 硬约束

- 只卸 daemon 自己挂载的设备（状态记录），绝不 umount 启动前已存在的挂载。
- 诊断输出必须含 PID、进程名（/proc/PID/comm）、占用类型（fd/maps/cwd）。
- O_DIRECT 政策、防缠结规则（改动文件清单写入 result）。
- ~~sys_config.yaml 注释复制错误修正~~（总指挥 2026-08-04 已顺手修掉：三条目更正为 #1/#2/#3 → device_id=1/2/3）。
- libnvm ABI/UAPI 头零改动；daemon 与内核模块接口零改动。

## 验收

1. 主机单测全过；2. 硬件验证 5 步全过（含占用诊断实证）；3. 全量契约一次（842/137/66 + 非硬件 15）确认无回归；4. result：改动清单 + 验证记录。

## 启动前总指挥复核点

- S4 是否已落地（硬件互斥）；当时 daemon 源码是否被其他 round 动过；config/ 目录（R20）是否已存在——若存在，挂载配置键位要与 R20 的 tutti_config 对齐。
