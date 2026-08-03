# TASK T-023

你是一名资深 C++ 测试工程师。你只负责一件事：修复 `tests/resolver_contract/resolver_contract_test.cpp` 中 test 14（多轮 FIEMAP）的**空洞验证** —— 让它真正走多轮 FIEMAP 循环，而不是在单 extent 文件上空转。

你看不到任何其他上下文，本 prompt 已包含完整现状、已验证可行的修复方法和验收标准。

# 项目位置

`/data/home/ryeqiu/Tutti`

# 执行时机

本任务需要在已挂载的 snvme + ext4 环境上运行测试。**必须单独执行，不与任何其他 session 并发**（环境是独占硬件）。

开始前确认：

```bash
ps -eo pid,etime,cmd | grep -E '[c]make|[c]test|[t]utti_daemon' | head
```

环境就绪自检（负责人已挂好）：

```bash
findmnt /mnt/nvme1
test -w /mnt/nvme1/GPU0/resolver_test && echo 'writable: OK'
```

若不就绪，报告 `BLOCKED`，**不要**自己挂载。

# 背景：为什么 test 14 是空洞的

当前 test 14（`resolver_contract_test.cpp` 的 `test_multi_round`，约第 554-608 行）：

1. 创建一个 4 MiB 文件（fallocate + pwrite + fsync）；
2. 用 `exts_per_call=2` 的 resolver 解析；
3. 与默认 `exts_per_call=256` 的结果比对，断言 extent 集合一致。

**问题**：4 MiB 文件在全新 ext4 上只有 **1 个 extent**。`exts_per_call=2` 时，第一次 `FS_IOC_FIEMAP` 就返回 1 个 extent（`< 2`）且带 `FIEMAP_EXTENT_LAST`，于是：

- 多轮循环的游标推进（`logical_cursor = fe_logical + fe_length`）执行了，但循环体只跑**一轮**；
- 第二次 ioctl、以及把多轮结果**拼接**起来的路径，**从未被执行**。

所以「extent 集合完全一致」虽然为真（两个 1-extent 集合相同），但它**没有验证任何多轮行为**。上一轮验收判定这是一个空洞声明，必须返工。

# 修复方法（已在真实环境验证可行）

要让 `exts_per_call=2` 真正触发第二轮，文件必须有 **≥2 个 extent**。在全新 ext4 上，普通的 fallocate 文件总是连续的（1 个 extent）。用下面的技巧可以**确定性**地造出 2 个 extent：

```text
1. fallocate 文件 A 为 4 MiB
2. fallocate 文件 B 为 4 MiB（ext4 顺序分配，B 紧跟在 A 后面）
3. 把 A 扩展到 8 MiB
   → A 无法在原处连续扩展（B 占了紧邻的块），ext4 只好给 A 的第二段
     分配在别处，于是 A 变成 2 个物理 extent，logical 覆盖 [0, 8 MiB)
4. 对 A 和 B 都 pwrite 全量 + fsync（得到 written extent，非 unwritten）
```

实测结果（`/mnt/nvme1`，ext4）：

```text
fa.bin (A):
 ext 0: logical 0..1023    physical 34816..35839   (4 MiB)
 ext 1: logical 1024..2047 physical 36864..37887   (4 MiB, last)
        ^ physical 35840..36863 是 fb.bin (B)，A 被拆成两段
```

**注意几点**：

- 扩展 A 用 `fallocate(fd, 0, 4MiB, 4MiB)`（在偏移 4 MiB 处追加分配 4 MiB），或等价地在 C++ 里 `fallocate(fd, 0, 4UL*1024*1024, 4UL*1024*1024)`。
- A 的最终 `file_size` 是 8 MiB，logical 完整覆盖 `[0, 8 MiB)`，无空洞，因此能通过 binding 的 `validate()`。
- 必须 pwrite 全量数据，否则 extent 是 unwritten（虽然本 resolver 接受 unwritten，但 written 更贴近真实用法，且避免混淆测试意图）。
- B 只是占位，迫使 A 分段；测完一起删掉。
- 这个分配行为依赖 ext4 的「顺序分配」启发式。在当前全新文件系统上实测可靠。但它**不是 100% 契约保证**（ext4 理论上可以把 B 分到别处）。因此测试必须**先断言 A 确实产生了 ≥2 个 extent**，若因环境变化只得到 1 个 extent，应明确报错（说明未触发多轮），而不是静默通过。

# 精确改动

只改 `test_multi_round`（或你重命名的等价函数），**其余 13 个测试一律不动**。

改后的 test 14 必须做到：

1. 用上述技巧创建 A（2+ extent）和占位文件 B；
2. 对 A 做全量 pwrite + fsync；
3. **先验证前提**：直接调用 FIEMAP（或复用测试里已有的采集路径）确认 A 的 extent 数 **≥ 2**。若 < 2，打印「未能造出多 extent 文件，多轮路径未触发」并判失败 —— 不要静默通过；
4. 用 `exts_per_call=1` 的 resolver 解析 A —— 这样每个 extent 都需要一轮 ioctl，**必然**走多轮（≥2 轮）。`exts_per_call=1` 比 `=2` 更能强制多轮，因为即使文件只有 2 个 extent 也会跑 2 轮；
5. 用默认 `exts_per_call=256` 的 resolver 解析同一个 A；
6. 断言两者解析成功，且 extent 集合**逐项一致**（数量、`logical_offset`、`device_offset`、`length` 全等）；
7. 断言 extent 数 **≥ 2**（这是多轮真正发生的证据）；
8. 在输出中打印两个 resolver 各自采集到的 extent 数量与每个 extent 的三元组（运行时真实值）；
9. 清理 A 和 B。

**关于 `exts_per_call=1` 的一个细节**：`LocalFileResolver` 的构造函数把 `exts_per_call==0` 映射回默认值 256（见 `resolver.h`）。`exts_per_call=1` 是合法且有效的（不是 0），请直接使用 1。

# 你只能修改

- `/data/home/ryeqiu/Tutti/tests/resolver_contract/resolver_contract_test.cpp`
- `/data/home/ryeqiu/Tutti/chat/round7/result1.md`

构建产物只能写入 `/data/home/ryeqiu/Tutti/build/round7-session1*`。

禁止修改或创建任何其他文件。尤其禁止：

- 修改 `tutti/resolvers/**`（resolver 本身没问题，不要动）
- 修改 `tests/resolver_contract/CMakeLists.txt`
- 修改 `nvme_storage/**`、`tutti/bindings/**`、`tutti/include/**`
- 修改 `tests/` 下其他目录
- bind / unbind / mkfs / mount / umount / 启停 daemon / 打开块设备节点
- 修改 `.gitignore`、`chat/**` 中除 `chat/round7/result1.md` 外的文件

禁止提交 Git commit。

# 安全限制

绝对禁止 `sudo` / `insmod` / `rmmod` / `modprobe` / bind / unbind / mkfs / mount / umount / 启停 daemon；禁止打开 `/dev/snvme*`、`/dev/nvme*` 或任何块设备节点；禁止触碰 `/dev/md0`、`/mnt/nvme4`（生产数据）；禁止在 `/data` 上做 FIEMAP 测试。

只允许在测试目录 `/mnt/nvme1/GPU0/resolver_test/` 下创建/删除普通文件，以及只读运行 `filefrag`、`findmnt`、`df`。

# 验收步骤

在 `/data/home/ryeqiu/Tutti` 下执行。

## 1. 重编并运行

```bash
rm -rf build/round7-session1
cmake -S tests/resolver_contract -B build/round7-session1 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round7-session1 --target tutti_resolver_contract_test -j8 2>&1 | tail -10
TUTTI_RESOLVER_TEST_DIR=/mnt/nvme1/GPU0/resolver_test \
  ctest --test-dir build/round7-session1 --output-on-failure -R '^tutti_resolver_contract_test$'
```

要求 `-Werror` 零告警、`1/1 Passed`、14 个测试全部通过。

## 2. 多轮真正发生的证据

在结果中给出 test 14 的完整输出，必须显示：

- A 文件的 extent 数 **≥ 2**（小缓冲与大缓冲两侧都打印）；
- 每个 extent 的 `logical_offset` / `device_offset` / `length` 真实值；
- 两个 extent 的 `logical_offset` 是**连续**的（第二个的 logical = 第一个的 logical + length），证明无空洞；
- 可加一段 `filefrag -v` 输出佐证 A 确有 2 个 extent。

## 3. 其余 13 个测试未受影响

确认完整输出中 14 个测试全部 PASS，且除 test 14 外其余测试的行为与改动前一致（你只改了 test 14）。

## 4. Hygiene

```bash
git diff --check -- tests/resolver_contract/resolver_contract_test.cpp
git diff --stat -- tests/resolver_contract/resolver_contract_test.cpp
git status --short --untracked-files=all | grep -vE '^\?\? (chat/|build/|tutti/|tests/|TUTTI_)' | head
```

文件尾随空白与 EOF newline；确认只改了允许的文件。

## 5. 环境未被改动

```bash
findmnt /mnt/nvme1 | tail -1
pgrep -af tutti_daemon | head -1
grep -E '^(snvme|snvme_core|phoenixfs) ' /proc/modules
findmnt /mnt/nvme4 | tail -1
```

挂载仍在、daemon 仍在、模块状态不变、生产 RAID 完好。

# 成功标准

报告 `PASS` 需同时满足：

1. test 14 用多 extent 文件（≥2 extent）+ `exts_per_call=1` 真正走多轮 FIEMAP；
2. 前提断言生效：若文件 < 2 extent，测试明确失败而非静默通过；
3. 结果中含多轮发生的硬证据（extent 数 ≥2、连续 logical、真实三元组、可选 filefrag 佐证）；
4. 其余 13 个测试行为未变、全部通过；
5. 只改了 `resolver_contract_test.cpp`（test 14 一处）；
6. 未执行任何禁止的硬件/系统操作，环境未被改动；
7. 空白与 EOF newline 检查通过。

# 结果落盘要求

写入 `/data/home/ryeqiu/Tutti/chat/round7/result1.md`，至少包含：

1. 环境就绪自检
2. 修复方法说明（如何造多 extent 文件）
3. test 14 的精确改动（改了什么、为什么这样改能真正触发多轮）
4. test 14 完整输出（含 extent 数、三元组、filefrag 佐证）
5. 14 个测试全部通过的完整输出
6. 「其余 13 个测试未受影响」的说明
7. hygiene 检查
8. 环境未被改动的核验
9. 最终 `PASS` / `BLOCKED`

规则：

- 结果必须由你自己写入，禁止要求用户复制聊天输出。
- 命令失败就写真实错误与 `BLOCKED`，不得伪造 PASS。
- 不得预留或编写「总指挥验收」内容；总指挥会在你结束后追加。
- 最终聊天回复只需给出状态和路径，例如：`PASS — 结果已写入 chat/round7/result1.md`。

不要寒暄、不要提交 Git commit、不要修改其他文件。
