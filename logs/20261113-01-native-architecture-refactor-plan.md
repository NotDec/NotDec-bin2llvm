# native 架构与冗余代码整理计划（2026-11-13）

## 用户原始 prompt

> 这套代码由你来负责，做什么、怎么搭都由你决定，以后也由你来继续维护。
>
> 从这一点考虑，看看代码架构上，以及冗余代码合并上是否还有改进的空间
>
> 代码文件名，类名，文件位置也可以优化
>
> 但是先讨论规划，不要直接做
>
> 启动一个goal，把这些都处理一下吧

## 决策（讨论时留白，按维护者授权自定）

1. 目录迁移：全量分目录（core/elf/abi/intrinsics/discovery/passes/*）。
2. 重命名：破坏性改干净，不保留旧名转发头；CLI flag 同步改，脚本/文档一起更新。
3. 死代码：删除 `notdec-bin2llvm` demo 工具与 `ModuleBuilder`；`notdec-sleigh-*` /
   `notdec-native-pcode` 保留为调试工具，在 `DEBUG.md` 标注用途。

## 阶段

- [ ] P0 删除 demo 工具/ModuleBuilder；修正 `ARCHITECTURE.md` 第 6 节文件清单；
      在 `DEBUG.md` 标注调试工具。
- [ ] P1 抽共享底座：`RegisterModel`（RegisterUnit / 单位解析 / load-store 识别 /
      x87 窗口读）、`NotDecIntrinsics`（unknown helper）、`NotDecMetadata`（mdField）。
      不合并 `AbiFacts`（两边语义已分叉：mask vs slot）。
- [ ] P2 拆分 `NativeRegisterPeephole` 为 pre-summary / post-rewrite 两个 pass；
      `NativeFunctionPointerPromotion` 归位到 passes。
- [ ] P3 拆 `NativeRegisterSummarySSA` 单体：orchestrator + entry state /
      external call shapes / signature rewrite / range materialization / value source audit。
      先搬不改，公共 API 不变。
- [ ] P4 按 analyzer 边界拆 `NativeAnalysis`；pipeline 下沉为 `NativePipeline`，
      tool 只留 CLI。
- [ ] P5 命名与目录迁移：
      `NativeRegisterSummarySSA` → `NativeRegisterElimination`；
      `NativeRegisterSummarySSASummary` → `NativeRegisterEliminationSummary`；
      `NativeAnalysis` → `NativeDiscovery`；
      include/lib 按 core/elf/abi/intrinsics/discovery/passes 分层。
- [ ] P6 拆 13.8k 行 SSA 测试为多个可执行文件，共享 `tests/support/RegisterTestHarness`。

## 验证标准

- 每个重构提交行为零变化：同一输入 `.ll` 逐字节相同；metadata 顺序差异必须消除或单独说明。
- `ctest --test-dir build` 全绿（含 fortune x86_64/i386 realworld）。
- 同一 fortune 二进制 pass 时间 3 次取中位数，不劣化超过噪声（基线 ~3.2s）。
- 拆分后单文件目标：orchestrator < 1k 行，其余 stage 各 < 3k 行。

## 基线（2026-11-13）

- ctest：12/12 通过，总耗时 41.5s。
- fortune x86_64 IR：`/tmp/refactor-baseline/x86_64.ll`（3.20–3.24s）。
- fortune i386 IR：`/tmp/refactor-baseline/i386.ll`。

## 进度记录

（按阶段追加）
