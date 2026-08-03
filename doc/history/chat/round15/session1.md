# TASK T-076 — Round 15 Session 1：多设备底座实测（双 LocalNvmeDataPath 经 Runtime）

## 前置条件

- 架构事实：StorageRuntime 支持多 DataPath 注入、按 `recommended_data_path_key` 路由、batch 按 (DataPath,target) 分组（Round 8 验收）；但双真实设备从未实测。
- 环境：snvme1（`0000:4b:00.0`）已绑定；**需要 operator 前置操作**：`sudo mkfs.ext4 /dev/snvme1n1`（若未建）+ `sudo mount -t ext4 /dev/snvme1n1 /mnt/nvme2`。agent 不代跑。
- 阅读 `chat/round8/result5.md`（multi-stream/target 证据）与 S5 契约测试结构。

## 目标

用两块真实 NVMe（snvme0+snvme1）证明 Runtime 级多设备路径：两个 `LocalNvmeDataPath` 实例 + 两个 `LocalFileResolver`（各自 backing device），经公开 StorageRuntime 完成跨设备 IO。

## 允许修改/创建

- `tests/local_nvme_datapath_contract/**`（新增多设备测试用例；环境缺第二盘时显式 skip，不得 fail）
- `chat/round15/result1.md`

## 禁止范围

- 不修改生产代码（本 session 是实测验证；若发现真实缺陷，停下记录走 follow-up）。
- 不执行 mkfs/mount/模块/daemon 操作；不提交 Git。

## 必须实现的行为

1. 双实例装配：`LocalNvmeDataPath` ×2（pci 08/4b，ns1），resolver ×2（backing `/dev/snvme0n1`/`/dev/snvme1n1`），经 `RuntimeComponents` 注入同一 Runtime（不同 DataPath key）。
2. 跨设备用例：
   - 两设备各一个文件 target，分别 WRITE/READ 回读校验；
   - **一个 batch 跨两设备**（2 请求、不同 target），验证 group-by-target 下发与聚合；
   - 双 stream 双设备并发（设备 A stream 1、设备 B stream 2），无串扰；
   - 单设备异常路径隔离（对设备 B 的非法请求拒绝不影响设备 A 的正常 IO）。
3. 环境检测：第二盘不可用时（无 /mnt/nvme2 挂载）测试 SKIP 并打印明确原因。

## 测试要求

- 新增用例在双盘环境全过；既有 735+115 零回归；临时文件双盘目录均清空。
- 运行后 `ls /mnt/nvme2/GPU0/resolver_test/` 为空（如测试使用该目录结构）。

## 验收

- `chat/round15/result1.md`：环境前置确认、用例输出、回归输出。
- 总指挥复跑双设备契约与既有基线。

## 后续依赖

- S2（StripedResolver）依赖本 session 的多设备装配模式。
