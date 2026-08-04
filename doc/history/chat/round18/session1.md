# TASK — Round 18 Session 1：GPU 可移植框架（cuda_like 三层 + Mooncake 模式）

**日期：** 2026-08-03（预生成，启动前总指挥复核）
**前置依赖：** R16 S4 与 R19 都落地之后——本 session 要改 `data_paths/local_nvme/io/*.cuh` 与 striped fused kernel 的 include，必须等 kernel 文件安静。
**合作伙伴：** 沐曦（MUSA/MACA）。框架我们搭，MUSA 具体实现他们填。

---

## 目标（maintainer 需求）

和 Mooncake 一样：GPU kernel 可动态选择目标厂商编译器编译（**编译期单选**：一次构建一个厂商，`TUTTI_ACCELERATOR=CUDA|MUSA|HOST`，不是运行时并存）；PTX 级特殊要求通过宏定义替换，其他厂商可接入。Mooncake 代码可直接参考（Apache 2.0 与 Tutti 兼容，已核实 `third_pkgs/Mooncake/LICENSE`），README 加参考说明。

## 三层工作

### 层 a：厂商选择框架
- `tutti/include/tutti/gpu_vendor/musa.h`：与现有 `cuda.h`/`host.h` 同构的框架 stub（类型/函数签名就位，实现标注 `// TODO(Metax): ...`，编译期明确报错不静默）。
- `cmake/accelerators/MUSA.cmake` 模板：编译器/ flags/源文件后缀（.mu 或 .cpp+宏）选择逻辑，照 `CUDA.cmake`/`HOST.cmake` 的形。
- `TUTTI_ACCELERATOR=MUSA` 档位接入根 CMake（configure 可见、明确提示 stub 状态）。

### 层 b：kernel 原语宏层（PTX 替换点）
- 新建 `tutti/data_paths/local_nvme/io/tutti_gpu_primitives.cuh`（或并入 cuda_like 体系，启动时按现状定）：把 IO kernel 里所有 CUDA 味原语收敛为宏/inline——`__threadfence_system`（GPU↔NVMe DMA 一致性命根，必须逐厂商实现并写明语义要求）、`__threadfence`、atomic 系列、warp 原语、`__nanosleep`、clock。
- 每个原语标注语义契约（内存序、范围 system/device），沐曦按契约填 MUSA 版。
- libnvm device 头（`ctrl.h/queue.h/nvm_cmd.h`）的 CUDA 依赖梳理：能隔开的隔开，隔不开的写入移植指南。

### 层 c：直引改走 cuda_like
- `data_paths/` 内 6 处 `#include <cuda_runtime.h>` 直引全部改走 `cuda_like.h`（Mooncake 同款）；kernel 里的 CUDA API 调用（cudaMallocHost 等）经 cuda_like 抽象。

### 文档
- README 加 Mooncake 参考/NOTICE（Apache 2.0）。
- `doc/gpu-porting-guide.md`：给沐曦的移植指南——三层结构、原语语义契约表、MUSA 接入步骤、验证方法。

## 验收

1. CUDA profile 全量回归（842/137/66 + 非硬件 15）零变化——行为零变更，纯结构改造。
2. HOST profile 15/15。
3. `TUTTI_ACCELERATOR=MUSA` configure 成功、编译到 stub 处明确报错信息（不静默）。
4. 移植指南评审通过（总指挥）。

## 硬约束

- **行为零变更**：CUDA 路径下任何 kernel 逻辑/原语语义不得改变（宏只是转发）。
- 不为 MUSA 写真实实现（那是沐曦的活）；stub 必须编译期显式可见。
- 防缠结：改动清单；串行前置（S4/R19 已落地）。
- 启动前总指挥复核：当时 `io/*.cuh` 与 striped kernel 的最终状态（S4 根因修复可能改过结构，宏层按现状设计）。**2026-08-04 现状备注**：kernel 已是单路径 descriptor 形态（R16 S6 REQUIRED 0——永远从 `prp_entry` 解引用 prp1/prp2/data_length，无双路径分支）；launch tpb=32（对齐 legacy）；`io_granularity` 注册期预构建是主路径。宏层（fence/atomic/warp 原语）按此形态设计。
