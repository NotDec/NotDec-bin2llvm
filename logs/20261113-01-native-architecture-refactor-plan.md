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
- [x] P2 拆分 `NativeRegisterPeephole` 为 pre-summary / post-rewrite 两个 pass；
      `NativeFunctionPointerPromotion` 归位到 passes。
- [x] P3 拆 `NativeRegisterSummarySSA` 单体：orchestrator + entry state /
      external call shapes / signature rewrite / range materialization / value source audit。
      先搬不改，公共 API 不变。
- [x] P4 按 analyzer 边界拆 `NativeAnalysis`；pipeline 下沉为 `NativePipeline`，
      tool 只留 CLI。
- [x] P5 命名与目录迁移：
      `NativeRegisterSummarySSA` → `NativeRegisterElimination`；
      `NativeRegisterSummarySSASummary` → `NativeRegisterEliminationSummary`；
      `NativeAnalysis` → `NativeDiscovery`；
      include/lib 按 core/elf/abi/intrinsics/discovery/passes 分层。
- [x] P6 拆 13.8k 行 SSA 测试为多个可执行文件，共享 `tests/support/RegisterTestHarness`。

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

### P4（2026-11-14）

`NativeAnalysis.cpp` 从 6.2k 行按 analyzer 边界拆成：

| 文件 | 行数 | 内容 |
|---|---|---|
| `NativeAnalysis.cpp` | 817 | NativeProgramState 方法、runtime 判定、manager |
| `NativeAnalysisInternal.h` | ~250 | x86 pattern 类型、helper 声明、factory 声明 |
| `NativeAnalysisSupport.cpp` | 1523 | ELF/reloc/eh_frame/x86 pattern helper |
| `NativeElfFacts.cpp` | 383 | ElfLoad / RelocationPlt / ElfEntry / ElfSymbol |
| `NativeEhFrame.cpp` | 758 | .eh_frame 解码 |
| `NativeGtirbFacts.cpp` | 398 | GTIRB 导入 |
| `NativeSleighSeeds.cpp` | 1241 | SLEIGH seed 指令解码 |
| `NativeX86Facts.cpp` | 880 | x86 pc-thunk / jump table / flow normalizer |
| `NativeReport.cpp` | 213 | report analyzer |

- 每个 analyzer 的 factory 跟着类走，声明进内部头。
- 验证：ctest 16/16；x86_64 IR 逐字节相同；i386 只有 `%ST1.clobberNNN`
  后缀编号不同（LLVM 同名计数器受构建布局影响），去掉数字后逐字节相同，
  指令与 metadata 内容一致。

### P6（2026-11-14）

- 13.8k 行、213 个 test 的单文件拆成 `tests/RegisterTestHarness.h`（helpers，2006 行）
  + 5 个可执行文件：core(107) / vararg(21) / stack(54) / peephole(30) / audit(1)。
- 每个文件自带 `main` 串联本组 test；CMake 用 foreach 注册
  `notdec.register_elimination.<group>`。
- 验证：ctest 16/16；fortune x86_64/i386 IR 逐字节相同。

### P5（2026-11-14）

- 命名：`NativeRegisterSummarySSA*` → `NativeRegisterElimination*`
  （文件、options/summary/warning 类型、`runNativeRegisterElimination`）。
  CLI flag `--register-ssa-summary` / `--summary-register-ssa-pass` 暂保留兼容。
- 目录分层：
  - `include|lib/core/`：Pcode、PcodeToLLVM、SleighLift、RegisterStorage、
    NotDecIntrinsics、NotDecMetadata
  - `include|lib/elf/`：LiefElfLoadImage、NativeRelocationMetadata
  - `include|lib/abi/`：NativeAbi、NativeExternalPrototype
  - `include|lib/intrinsics/`：partial read/write、value range、x87
  - `include|lib/discovery/`：NativeAnalysis
  - `include|lib/passes/register|stack|vararg|function-pointer/`
- `NativeFunctionPointerPromotion` 归位到 `passes/function-pointer/`。
- 空目录删除，`ARCHITECTURE.md` 第 6 节与 `AGENTS.md`、docs 路径同步。
- 验证：ctest 12/12；fortune x86_64/i386 IR 逐字节相同。

### P3（2026-11-13）

`NativeRegisterSummarySSA.cpp` 从 9.6k 行拆成：

| 文件 | 行数 | 内容 |
|---|---|---|
| `NativeRegisterSummarySSA.cpp` | 391 | pass 入口编排 + 公开 audit 入口 |
| `RegisterEliminationInternal.h` | 1420 | 内部类型、free function 声明、FunctionBuilder 声明 |
| `RegisterEliminationModel.cpp` | 719 | unknown helper / noreturn 截断 / 死 helper 清理 / audit / ABI facts |
| `RegisterSignatureSlots.cpp` | 805 | 签名 slot 与 shape 构造 |
| `RegisterExternalShapes.cpp` | 1434 | 外部调用形状、变参推断、间接调用证据 |
| `RegisterSignatureRewrite.cpp` | 1446 | 函数/调用点签名重写辅助 |
| `FunctionBuilderCore.cpp` | 944 | demand 计算、partial write、metadata |
| `FunctionBuilderRanges.cpp` | 1150 | 区间边界、planning、liveness |
| `FunctionBuilderSsa.cpp` | 830 | 区间 SSA、phi、range transfer |
| `FunctionBuilderSignature.cpp` | 1351 | 调用点实参、栈槽、返回值 |

- FunctionBuilder 的 157 个成员函数改为 out-of-line 定义，类声明与成员变量进内部头。
- 嵌套类型 `RangeLossCause` 提到 namespace 作用域，out-of-line 定义可以直接使用。
- 拆分是纯搬移：free function 与成员函数体逐字未改，只补 `FunctionBuilder::`、
  去掉定义处的默认实参、把 `static` 成员定义前的 `static` 去掉。
- 验证：ctest 12/12；fortune x86_64/i386 IR 与 P2 基线逐字节相同；
  pass 时间 3.16–3.18s。

### P2（2026-11-13）

- `NativeRegisterPeephole.{h,cpp}` 拆成
  `NativeRegisterPreSummaryPeephole.{h,cpp}`（整寄存器 load 收窄、按观察宽度收窄读、
  死标志位）与 `NativeRegisterPostRewritePeephole.{h,cpp}`（常数除法还原、
  value-range glue 化简）。
- 两边共用的 `constantShiftAmount` 放进 `PeepholeUtils.h`；`LowBitsView` /
  `stripWidthRoundTrip` / `simplifyInsertedValueExtract` 只被 post 使用，随 post 走。
- `NativeFunctionPointerPromotion` 的目录归位推迟到 P5，避免和目录迁移重复搬动。
- 验证：ctest 12/12；fortune x86_64/i386 IR 与 P1 基线逐字节相同。

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
