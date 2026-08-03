# TASK T-031 — Round 9 Session 1：Standalone 构建图与测试分层

## 背景与事实基线

- `chat/round8/result4.md`、`chat/round8/result5.md` 已完成验收；`LocalNvmeDataPath` 直接硬件契约目前为 `501 passed / 0 failed`，其中 test 53 已覆盖公开 `StorageRuntime → LocalFileResolver → LocalNvmeDataPath` 的 4 KiB WRITE/READ。
- `tutti/` 的 HOST profile 已可 clean configure/build/CTest（5 项），但 local-NVMe、resolver、binding 与多数 contract tests 仍是独立 mini-project，存在硬编码 `/data/home/ryeqiu/Tutti/...` include/link 路径。
- 本 session 的目标是让**新的 standalone `tutti/` 构建图**成为可重复的 refactor build owner；不在此 session 合并根目录 legacy 树与 `tutti/device_manager/nvme/` 的 libnvm/NVMeService/snvme 双树。

## 目标

1. 为新架构的私有 package 建立正式 CMake target：
   - `tutti_local_file_binding`（header-only，私有）；
   - `tutti_local_file_resolver`（header-only，私有）；
   - `tutti_local_nvme_datapath`（CUDA/private target，包含 `local_nvme_data_path.cpp` 与 `io/*.cu`）。
2. standalone CUDA profile 仅使用 `tutti/device_manager/nvme/libnvm/` 所构建的 `libnvm` target；local-NVMe 测试不得再手写根目录 `build/lib/nvm`、根目录 libnvm include 或绝对 workspace 路径。
3. 完整区分 hardware-free 与 hardware contract tests：
   - HOST + `BUILD_TESTING=ON` 默认只构建/运行无 CUDA、无 libnvm、无 mount 的 tests；
   - `TUTTI_BUILD_HARDWARE_TESTS` 默认为 `OFF`；开启后才注册 resolver/local-NVMe hardware CTest，并加 `LABELS hardware;local_nvme` 或 `LABELS hardware;resolver`；
   - 不允许普通 CTest 自动访问 `/dev/ssnvme*`、GPU、`/mnt/nvme1`。
4. 将 `status`、`memory_types`、`io_types`、`storage_target_resolver`、`binding`、`data_path`、`storage_runtime` 等 hardware-free contract tests 纳入 standalone `tutti/CMakeLists.txt` 的 CTest 图。
5. 更新 `Roadmap.md` 的事实快照：只标记已由 Round 8/当前 Runtime 实现闭合的项；明确保留未闭合项（双 source owner、CQ timeout/NVMe status、resolver fail-closed、Runtime 线程安全）。不得把目标架构误写成已完成。

## 允许修改/创建

- `tutti/CMakeLists.txt`
- `tutti/cmake/**`
- `tutti/bindings/ext4_local_nvme/CMakeLists.txt`
- `tutti/resolvers/local_file/CMakeLists.txt`
- `tutti/data_paths/local_nvme/CMakeLists.txt`
- `tests/*/CMakeLists.txt`（仅为接线、去绝对路径、CTest labels）
- 必要的 package/test CMake 新文件
- `Roadmap.md`
- `chat/round9/result1.md`

除为 CMake target 使用现有 source 外，不改 Runtime、resolver、binding、DataPath、libnvm 或 kernel 业务逻辑。

## 禁止范围

- 不修改根 `CMakeLists.txt` 的 legacy production graph；不宣称 root 与 standalone 已统一 source owner。
- 不移动、复制、合并 libnvm/NVMeService/snvme/kmod 源码。
- 不修改 public/SPI header。
- 不执行 `insmod`、`rmmod`、daemon 启停、bind/unbind、mount/umount、raw block IO。
- 不运行 hardware tests；本 session 只完成 configure/build/registration 验证。
- 不提交 Git。

## 实施约束

- 所有路径使用相对 `${PROJECT_SOURCE_DIR}` / `$<BUILD_INTERFACE:...>` / target usage requirements；测试不得再出现硬编码 workspace 绝对路径。
- private local-NVMe/resolver target 不能传播 libnvm、CUDA、PRP 或 kernel include 到 `tutti_api` / `tutti_spi` / install public headers。
- HOST profile 绝不能 evaluate CUDA、libnvm、gRPC 或 package CMake 的 CUDA source。
- 若 CUDA target 依赖当前 standalone 的 `libnvm` target 名称，复用该 target；不得为了方便重新定义同名 target。
- 先读取现有 CMake，再做外科式接线；保留当前 legacy layer target 的既有状态，不顺手重构。

## 验收

至少执行：

```bash
cmake -S tutti -B build/round9-session1-host \
  -DTUTTI_ACCELERATOR=HOST -DBUILD_TESTING=ON \
  -DTUTTI_BUILD_HARDWARE_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/round9-session1-host -j8
ctest --test-dir build/round9-session1-host --output-on-failure
```

并验证：

1. HOST configure log 不出现 CUDA/libnvm/gRPC 查找；
2. 所有 hardware-free CTest 通过；
3. `ctest -N` 不含 resolver/local-NVMe hardware test；
4. CUDA configure/build 至少完成 local-NVMe target的 compile/link gate（不运行 hardware test）；
5. `grep`/CMake 审查确认相关 tests 不再硬编码 `/data/home/ryeqiu/Tutti`、根 `build/lib` 或根 libnvm include；
6. `git diff --check` 与改动文件 diagnostics 为 0。

## 结果落盘

写入 `chat/round9/result1.md`：

- 新 target 与依赖方向；
- HOST/CUDA configure/build/CTest 结果；
- hardware test 开关、labels 与默认行为；
- 仍未解决的 root/standalone 双 source owner 边界；
- `Roadmap.md` 已更新的事实项；
- 修改文件列表与最终 `PASS`/`BLOCKED`。

不要写“总指挥验收”，不要提交 Git。