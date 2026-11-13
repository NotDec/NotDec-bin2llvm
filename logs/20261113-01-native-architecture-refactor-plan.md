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

- [x] P0 删除 demo 工具/ModuleBuilder；修正 `ARCHITECTURE.md` 第 6 节文件清单；
      在 `DEBUG.md` 标注调试工具。
- [x] P1 抽共享底座：`RegisterModel`（RegisterUnit / 单位解析 / load-store 识别 /
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

### P0（2026-11-13，commit 7b398b6）

- 删除 `tools/notdec-bin2llvm.cpp`、`include/notdec-bin2llvm/ModuleBuilder.h`、
  `lib/ModuleBuilder.cpp`（三者只互相引用，产出 demo IR）。
- `ARCHITECTURE.md` 第 6 节删掉已并入 `NativeAnalysis.cpp` 的
  `GtirbNativeAnalysis.cpp` / `NativeEhFrame.cpp` / `NativeRuntime.cpp`。
- `DEBUG.md` 第 5 节标注 `notdec-native-pcode` / `notdec-sleigh-pcode` /
  `notdec-sleigh-llvm` / `notdec-lief-elf` 为调试工具。
- ctest 12/12。

### P1（2026-11-13）

- 新增 `lib/passes/summary/RegisterModel.{h,cpp}`：RegisterUnit / 单位解析 /
  collectRegisterUnits / storageUnitName / registerLoad / registerStore /
  isX87WindowShiftLoad / isNotDecRegisterHelperCall / isAnalyzableCall /
  intersectBitMasks。`registerLoad` 用 `RegisterReadOptions` 统一两边差异。
- 新增 `lib/NotDecIntrinsics.{h,cpp}`：llvmTypeName /
  getOrInsertUnknownValueHelper / unknownValueAt（含无 block 时的 poison 兜底），
  PcodeToLLVM 与 SSA 共用。
- 新增 `lib/NotDecMetadata.h`：mdField（原 4 份）。
- summary 与 SSA 各自删除本地副本；`AbiFacts` 保持两份（mask 模型 vs slot 模型，
  语义不同）。
- **顺带修可复现性**：`notdec.register.summary*` 元数据原先按
  `std::map<GlobalVariable*>` 的指针顺序输出，换一个构建产物就会重排。新增
  `unitsInNameOrder()`，metadata 与 summary JSON 都按 (name, space, offset) 排序。
- 验证：ctest 12/12；fortune x86_64/i386 指令流逐行相同、metadata 图规范化后
  相同（顺序变化仅来自上面的确定性修复）；重复构建产物逐字节相同；
  pass 时间 3.16–3.17s（基线 3.20–3.24s）。
- 基线已按新输出刷新。
