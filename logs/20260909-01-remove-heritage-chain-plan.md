# 原始 prompt

```text
bin2llvm之前是不是有过heritage链路弃用了。规划一下什么完全删掉这部分链路

按这个开始推进吧。注意保证当前的基于ddisasm+SummarySSA 的代码正常且不受影响
```

# 背景

2026-06-17 的 `222b00b` 已将 SummarySSA 设为 native 默认寄存器 SSA 路线，
2026-06-18 的 `8f8b743` 又把旧 heritage 和 summary pass 明确拆开。当前仓库仍保留
两套旧入口：一套是 Ghidra HighFunction heritage JSON 导出/降低链路，另一套是
native `NativeHeritageSSA` 加 `NativePrototypeRecovery` 的旧寄存器和原型恢复链路。

当前 native 主链路是 DDISASM/GTIRB discovery、SLEIGH p-code、`PcodeToLLVM`、
`NativeRegisterSummarySSA` 和 summary cleanup。删除 heritage 不应改变这条链路的
分析顺序、ABI metadata、签名改写或最终寄存器清理。

# 目标

- 删除所有可执行的 heritage 实现、CLI、JSON schema、测试和文档入口。
- 保留并验证 DDISASM/GTIRB discovery、SLEIGH native lowering、SummarySSA、外部
  prototype provider、stack/canary/x87 处理和 LLVM 22 验证路径。
- 旧 CLI 选项和 `notdec.heritage-*.v0` JSON schema 不提供兼容空壳。
- 保留 `logs/` 及父仓库 archive 中的历史记录，不把退休旧链路的测试失败改写成修复。

# native 现状和 Ghidra 对照

当前 `notdec-native-llvm` 默认使用 `NativeRegisterSummarySSA`，并且 SummarySSA 已
同时负责寄存器 SSA、call/internal signature rewrite 和 residue cleanup。旧分支只在
显式 `--heritage-register-ssa-pass` 下执行。

旧路线来源于 Ghidra decompiler 的 `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
和 `fspec.cc`，关键机制包括 `ParamEntry`、`ParamTrial`、`ParamActive`、
`ParamListStandard::possibleParamWithSlot()`、`FuncProto::deriveInputMap()`、
`FuncCallSpecs::checkInputTrialUse()` 和 `buildInputFromTrials()`。这些机制只作为
历史背景记录，本次不再复刻或迁移；SummarySSA 保持现有静态 summary/SCC/demand
策略。

# 实施路线

## 1. 删除 native 旧 pass

删除 `include/notdec-bin2llvm/passes/heritage/` 和 `lib/passes/heritage/` 下的两个
旧 pass，连同只被 `NativePrototypeRecovery` 使用的 `NativePrototypeModel` 及其测试。
从 `notdec-native-llvm.cpp` 删除旧选项、冲突检查、旧 pass 调度和 prototype recovery
调度。保留 SummarySSA 的默认执行和 `--summary-register-ssa-pass` 兼容别名。

删除只覆盖旧 API 的测试目标；SummarySSA 现有测试和 CLI signature rewrite smoke
继续作为主链验收，不把旧 trial/use 测试改名冒充新链路测试。

## 2. 删除 Ghidra JSON heritage 链路

删除 `HeritagePcode`、`HeritageToLLVM`、四个 `notdec-heritage-*` 工具、两个
`ExportHeritage*.java` 脚本及其 CMake 和测试注册。删除父仓库中仅用于读取
`notdec.heritage-module.v0` 的 `scripts/bin2llvm-dump-module-pcode.py` 需要单独在
父仓库处理；当前扫描未发现非归档的 Bench2 heritage module 消费者。

## 3. 清理验证脚本和文档

`bench2-native-summary-ssa-audit.sh` 只保留 summary modes，删除 heritage/old mode
和旧 prototype flag。`bench2-native-smoke.sh` 删除 heritage module 对照指标和旧
prototype recovery 两次运行，改为使用 SummarySSA summary 中的 rewrite 指标，继续
用 LLVM 22 `llvm-as` 和 `opt -passes=verify` 检查 native IR。

更新 `ARCHITECTURE.md`、`DEBUG.md`、`AGENTS.md` 和 Arch2 的当前状态说明；历史日志
只保留原状，必要时在本文件追加实现记录。

## 4. 共享 Pcode 边界审计

不删除 `Pcode.h`、`PcodeToLLVM` 或 native SLEIGH lowering。只清理明确指向已删除
JSON producer 的注释；x87 的无 mnemonic/offset 兼容逻辑先确认没有 raw SLEIGH 或
测试用途后再决定是否移除，避免把通用 Pcode 行为和 heritage 实现混删。

# 风险

- 删除旧 CLI 和 schema 会破坏外部调用者；这是本次“完全删除”的有意 API 变化。
- 旧测试中有 heritage 专属行为，不能直接用 SummarySSA 输出替代并声称等价。
- `bench2-native-smoke.sh` 当前仍引用旧 prototype summary 选项，更新时必须改用
  SummarySSA 的真实统计字段，不能只删掉检查导致 coverage 下降。
- `PcodeToLLVM` 的 heritage 注释和 x87 fallback 与通用 Pcode API 有交集，需要按实际
  producer/consumer 逐项判断。

# 不做什么

- 不改 SummarySSA 分析算法、DDISASM/GTIRB discovery、ABI 解析或 native IR 语义。
- 不把旧 `NativePrototypeRecovery` 逻辑迁入 summary。
- 不删除 `NativeAbi`、`NativeExternalPrototype`、native stack/canary/x87 pass。
- 不删除历史 logs、归档结果或 git 历史。

# 验收标准

- 活跃源码和构建配置不再提供 heritage pass、工具、脚本、schema 或 CLI 入口。
- 删除旧目标后重新配置构建；当前 17 个 CTest 中 5 个旧 heritage/prototype 测试
  目标移除，剩余 12 个测试全部通过。
- native 默认 pipeline 仍完成 DDISASM/GTIRB discovery、SummarySSA、signature
  rewrite 和 residue cleanup。
- x86-64 和 i386 fortune 回归均通过，使用 `/sn640/NotDec/llvm-22.1.0.obj/bin`
  下的 `llvm-as` 和 `opt`，且 warning、IR 规模和耗时没有明显退化。
- active C++、CMake、脚本和测试不再出现旧 heritage/prototype API；当前文档可以
  保留明确的“已删除”历史说明，logs 保留原始历史文字；负向测试断言仍可用于
  确认 `notdec.prototype.*` 不残留。

# 实现记录（2026-09-09）

## 修改文件和边界

- 删除 `include/notdec-bin2llvm/passes/heritage/NativeHeritageSSA.h`、
  `lib/passes/heritage/NativeHeritageSSA.cpp`、
  `include/notdec-bin2llvm/passes/heritage/NativePrototypeRecovery.h` 和
  `lib/passes/heritage/NativePrototypeRecovery.cpp`；同时删除只服务于旧原型
  恢复的 `NativePrototypeModel` 头文件、实现和测试。
- 删除 `HeritagePcode` / `HeritageToLLVM` 的头文件、实现、
  `heritage_to_llvm_test`，以及四个 `notdec-heritage-*` 工具。删除
  `ExportHeritageModule.java`、`ExportHeritagePcode.java` 及其 README。
- `CMakeLists.txt`、`lib/CMakeLists.txt`、`tools/CMakeLists.txt` 移除上述目标和
  测试注册；同时清理只依赖旧 pass 的 `native_register_effects_test`、
  `native_instcombine_metadata_test` 和 `bench2-native-prototype-audit.sh`。
- `tools/notdec-native-llvm.cpp` 的 `CliOptions`、`parseArgs()`、
  `runRegisterSSAPassIfEnabled()` 和主 pipeline 删除 heritage pass、prototype
  recovery 及旧 CLI 选项。默认唯一运行 `runNativeRegisterSummarySSA()`；
  `--summary-register-ssa-pass` 只保留兼容别名。
- `bench2-native-summary-ssa-audit.sh`、`bench2-native-scale-audit.sh`、
  `bench2-native-smoke.sh` 和 CLI signature smoke 清理旧 flags、prototype
  metrics 和 heritage module 对照，保留 SummarySSA rewrite/residue 指标、
  discovery/CFG 检查和 LLVM 22 verify。
- `NativeAbi.h`、`Pcode.h`、`PcodeToLLVM.cpp` 只清理过时的 producer 注释；没有
  修改 SummarySSA、DDISASM/GTIRB discovery、SLEIGH lowering、ABI 解析、
  stack/canary 或 x87 算法。
- 更新 `AGENTS.md`、`ARCHITECTURE.md`、`Arch2.md`、`DEBUG.md`，明确 SummarySSA
  是唯一 native 主链。历史 logs 保持原样。本仓库外的
  `/sn640/NotDec/scripts/bin2llvm-dump-module-pcode.py` 仍是 heritage JSON
  读取器，因不属于本仓库写入范围，本次只记录，不修改。

## 验证命令和结果

- `cmake -S . -B build -DLIEF_USE_CCACHE=OFF`：使用 LLVM 22.1.0 配置成功。
- `cmake --build build -j4`：当前源码全量构建成功。
- `ctest --test-dir build --output-on-failure`：12/12 通过。测试集合包含
  Summary fixpoint/SSA、native discovery/LLVM smoke、CLI signature rewrite、
  x86-64 fortune 和 i386 fortune；删除前的 5 个旧 heritage/prototype 目标不再
  注册。
- `cmake --build build --target help` 和 `ctest --test-dir build -N`：没有
  heritage/prototype 目标或测试。运行旧的
  `--heritage-register-ssa-pass`、`--rewrite-prototype-signatures` 均返回
  `unknown flag`，没有兼容空壳。
- `scripts/bench2-native-summary-ssa-audit.sh --build-dir build` 对
  `libuv:shared-library` 和 `memcached:executable` 完整运行
  `summary-no-residue`、`summary-residue`；已有同口径 `vsftpd` 结果一起记录在
  `/tmp/notdec-bin2llvm-bench2-summary-ssa-20260909/metrics.tsv`。四次生成均通过
  `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as` 和
  `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify`。
- Bench2 SummarySSA 关键结果如下；两种模式的 `calls_rewritten` /
  `functions_rewritten` 相同，residue 模式只额外移除死寄存器访问：

  | target | mode | seconds | IR lines | functions | calls rewritten | functions rewritten |
  | --- | --- | ---: | ---: | ---: | ---: | ---: |
  | vsftpd | no-residue | 186 | 98,630 | 366 | 2,903 | 541 |
  | vsftpd | residue | 223 | 58,058 | 366 | 2,903 | 541 |
  | libuv | no-residue | 272 | 141,505 | 635 | 2,071 | 844 |
  | libuv | residue | 315 | 78,914 | 635 | 2,071 | 844 |
  | memcached | no-residue | 387 | 43,477 | 420 | 3,508 | 586 |
  | memcached | residue | 523 | 26,001 | 420 | 3,508 | 586 |

- 当前 CTest fortune 产物：x86-64 为 10,134 行 IR、29 条 warning、14.48 秒；
  i386 为 6,193 行 IR、8 条 warning、16.63 秒；均通过 LLVM 22 assembly、verify
  和现有 residue 检查。仓库中 2026-08-03 的旧构建快照为 x86-64 10,263/61、
  i386 6,691/42（IR 行数/warnings），差异来自此前 SummarySSA 持续改进，删除
  heritage 本身未触碰这些文件；本次只把当前值作为删除后的回归基线，不宣称
  Bench2 长耗时是性能改善。
- `scripts/bench2-native-smoke.sh` 仍在 discovery 阶段对 `vsftpd` 报告已有的
  12 个 unresolved indirect calls；这是删除前脚本已有的硬门槛，未为让脚本变绿
  而放宽 discovery 语义。专门的 SummarySSA audit 和 CTest 均已完成。

## 实现效果与维护成本

- 实现效果：9/10。native 默认路径、SummarySSA signature rewrite、residue
  cleanup、DDISASM/GTIRB discovery 和 LLVM 22 验证均保留；旧 heritage 可执行
  入口、schema producer、CLI 和测试注册全部消失。
- 理解成本：2/10。入口只剩一条 SummarySSA native pipeline，文档和审计脚本不再
  需要在 summary、heritage、prototype 三套指标之间切换；logs 仍提供历史背景。
- 维护成本：2/10。删除了两套 pass、JSON lowering、exporter 和旧 audit 的同步
  负担；后续寄存器消除和 signature rewrite 只需维护 summary 路径。代价是旧
  heritage CLI/schema 是有意的不兼容变更，父仓库脚本仍需另一个范围明确的清理。
