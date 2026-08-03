# Round 15 重做基线（2026-08-03 总指挥核定）

## 当前树状态（已验证绿色）

- datapath 契约 **799/0**、runtime E2E **115/0**、非硬件 ctest **135/135**（build/r15base 全新构建）。
- 保留的已验收成果：R15 S1（多设备底座，tests 78-81）、R15 S2（StripedResolver+Binding，test #13）、R14 全部。
- 已回退的未验收改动（备份在 `~/tutti-r15-tangled-20260803.tar.gz`）：
  - Runtime 分组 (data_path,target)→data_path（S2b 半成品）
  - 批次容量参数化 + quota 16→512（S2b/S3）
  - StripedDataPath + fused kernel + 共享头抽离（S3，从未硬件验证，hang 来源）
  - tests/striped_datapath_contract/、tests/striped_local_nvme_contract/

## 缠结教训（重做必须遵守）

1. **每个 session 必须当次硬件验证**——"编译通过"不算交付，striped hang 就是从未真机运行导致的。
2. **一个 session 只动自己的文件清单**；跨 session 文件（storage_runtime.h、local_nvme_data_path.*、submit_one.cuh）串行交接。
3. **任何默认值变更（quota/arena/容量）单独列为显式工作项**，禁止夹带；默认值 16/256 保持。
4. **result 文件必须含改动文件清单**（供回退定位）。
5. **新测试编号从 82 起**（70-73 arena、76/77 defense、78-81 多设备已占用）。
6. 构建目录用 session 专属新目录，禁止复用他人 build 目录；二进制自带 rpath，无需 LD_LIBRARY_PATH。

## 死代码清理（2026-08-03 总指挥执行，已验证）

删除孤儿分层栈（生产侧零引用，生产 DataPath 直链 libnvm+CUDA）：`tutti/abstraction`、`tutti/accel`、`tutti/backends`、`tutti/io_engine`、`tutti/block_storage`、`tutti/coordinator`、`tutti/raw_device`、`tutti/tests/`（全部）、`tutti/data_paths/local_nvme/control/`（tutti_device_manager 无人消费）、`tutti/README.md`+`tutti/STRUCTURE_SUMMARY.md`（描述已删架构）。

**ctest 基线变化**：139 → **19**（消失的 120 个全是孤儿层 gtest 用例：MockBackend/NvmeBackendUnit/accel smoke/device_manager/backends/io_engine 等）。现存 19 个全为生产测试：#1 memfs、#2-12 硬件无关契约、#13 striped_resolver、#14 header_hygiene、#15 mock_kit、#16-19 hardware（resolver/datapath/runtime/layerwise）。

验证：非硬件 15/15、datapath 799/0、runtime 115/0、根构建 configure+daemon 重建正常（根构建 configure 需 `-DCMAKE_TOOLCHAIN_FILE=$PWD/third_pkgs/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_PREFIX_PATH=$PWD/third_pkgs/vcpkg/installed/x64-linux`）。
