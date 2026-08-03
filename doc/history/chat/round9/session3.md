# TASK T-033 — Round 9 Session 3：Fail-closed LocalFileResolver

## 前置条件

- Session 1 的 package/test CMake 接线已通过；若没有正式 resolver/binding target 或 hardware label，报告 `BLOCKED`。
- 阅读 `TUTTI_TARGET_ARCHITECTURE.md` 的 file target 约束、`TUTTI_REFACTOR_TAKEOVER.md`、`tutti/resolvers/local_file/resolver.h`、`tutti/bindings/ext4_local_nvme/binding.h`。
- 当前 resolver 能通过 FIEMAP 生成 ext4-local-NVMe payload，但仍接受不安全 extent 状态，并默认将 `fe_physical` 当作 namespace offset。它仅适合现有受控测试环境，不能作为一般文件直写承诺。

## 目标

把 `LocalFileResolver` 收紧为**受控 file target 的 fail-closed resolver**：只有能明确证明其 backing block device、namespace offset、FIEMAP 稳定性和 extent 状态均符合 local-NVMe 直写前提时才返回 `ResolvedTarget`；否则返回结构化失败。

## 允许修改/创建

- `tutti/resolvers/local_file/**`
- `tutti/bindings/ext4_local_nvme/**`
- `tests/resolver_contract/**`
- `tests/binding_contract/**`
- Session 1 必需的 package/test CMake 小修
- `chat/round9/result3.md`

## 禁止范围

- 不修改 public `StorageTargetResolver` SPI、`StorageRuntime`、`LocalNvmeDataPath`、libnvm、kernel。
- 不新增 public create/remove/list/WAL/striping/raw-device API。
- 不把 advisory `flock` 或“fd 仍打开”宣称为能防止其他进程 truncate/hole-punch/reflink/COW 的 layout lease。
- 不接触 `/mnt/nvme4`，不执行 mount/umount、mkfs、raw block IO、模块或 daemon 操作。
- 不提交 Git。

## 必须实现的安全规则

1. resolver 私有配置必须显式声明已验证的 backing block device 与 namespace byte base（whole namespace 为 0；partition/其他映射只有在 deployment 明确提供可验证 offset 时才允许）。不能再静默假设 `fe_physical == namespace offset`。
2. 对目标文件 `stat`、对 backing device `stat`；只接受 regular file 与已配置 block device，且文件所在 filesystem device identity 与配置相符。无法证明时拒绝。
3. FIEMAP extent 必须完整、连续、block-aligned，且拒绝至少：`UNKNOWN`、`DELALLOC`、`UNWRITTEN`、`ENCODED`、`NOT_ALIGNED`、`SHARED`、hole/zero-length/overflow/非单调 extent。`LAST` 仅作为结束标记允许。
4. 解析返回的 device offset 必须显式应用经验证的 namespace base，并检查无溢出、block alignment、file coverage。
5. 保留 `ResolvedTarget` 的 fd owner lease；将“只支持 Tutti 预分配且在 handle 生命周期中禁止 layout mutation 与普通 buffered filesystem IO”的契约写入准确注释/错误信息，但不得声称 resolver 能单独强制它。
6. 现有 fallocate-only/UNWRITTEN 正例必须改为明确拒绝；正例必须先完整写入并 fsync。

## 测试

必须新增或调整：

- 正例：受控 backing device + 完整写入/fsync 后 resolve 成功，payload 的 offset 等于 `fiemap physical + configured namespace base`；
- 配置 backing device 不匹配、namespace base 溢出、非 block-aligned 配置拒绝；
- hole、UNWRITTEN/fallocate-only、sparse/不完整 coverage、异常 flag 的拒绝（可使用 test-only parser seam 或受控 FIEMAP fixture；不可依赖不稳定 filesystem 行为）；
- payload 仍持有 fd lease、type/version/key 仍兼容 `LocalNvmeDataPath`；
- 现有真实 ext4 resolver contract 在已声明的受控环境下通过。

## 验收

1. HOST hardware-free binding tests 通过；
2. resolver hardware test 仅在 `TUTTI_BUILD_HARDWARE_TESTS=ON` 下注册/执行，并使用受控测试目录；
3. 执行 resolver contract 后清理所有临时文件；
4. 失败路径明确是 `INVALID_ARGUMENT`、`UNSUPPORTED`、`NOT_READY`、`DATA_LOSS` 或 `OUT_OF_RANGE`，不能 silent fallback；
5. `git diff --check`、linter 0 diagnostics。

## 结果落盘

写入 `chat/round9/result3.md`：

- backing identity/namespace base 的验证方式；
- 接受与拒绝的 FIEMAP flag 表；
- file mutation/layout 的明确边界；
- 正反测试与清理结果；
- public/SPI 未变的证明；
- 最终 `PASS`/`BLOCKED`。

不要提交 Git。