# ddisasm GTIRB CFG 间接跳转修复记录

## 背景

fortune 的 `main` 里有 jump table：

```asm
2872: movslq (%rbx,%rax,4),%rax
2876: add    %rbx,%rax
2879: notrack jmp *%rax
```

之前 ddisasm 没给出正确 CFG，pcode lifting 只能看到 `BRANCHIND`，于是把它降成未知
indirect tail call，后面 SummarySSA 会留下 `summary_return`。

这次先确认本机 Capstone 问题：旧 `/usr/local/lib/libcapstone.so.5.0.0` 能反汇编文本，
但 operand detail 是坏的，ddisasm 把很多正常指令记成 invalid opcode。替换为
GrammaTech 官方 `libcapstone-dev_5.0.1_gtdev_amd64.deb` 里的 `libcapstone.so.5` 后，
ddisasm 能识别 fortune 的 jump table CFG。

## 修改

- `lib/NativeAnalysis.cpp:4479`：
  `FlowFactNormalizer::run()` 在块级 successor 归一化后调用
  `resolveCfgBackedIndirectBranches(...)`。
- `lib/NativeAnalysis.cpp:4485`：
  新增 `resolveCfgBackedIndirectBranches(...)`。如果 unresolved indirect branch
  所在 basic block 已经有 GTIRB/ddisasm successor，并且这些 successor 都是当前函数里的块，
  就把这些 successor 补到该指令的 `DirectFlowTargets`，同时删除对应 unresolved flow。

这样做只同步已有 CFG 事实，不改 jump table 识别策略，也不在 pcode lowering 里猜目标。

## 验证

Capstone 修复后重新跑 fortune discovery：

```bash
OUT=/tmp/notdec-ddisasm-after-capstone-fix-20260706115459
notdec-native-discover --notes-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
notdec-native-discover --block-json 0x2872 /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
```

结果：

- GTIRB frontend 导入 `84` 个函数、`899` 个块、`1068` 条 CFG edge。
- `0x2872` block successor 包含 `0x287c`、`0x2886`、`0x28c5`、`0x28cf`、
  `0x28e5`、`0x28f2`、`0x2913`、`0x292a`、`0x2940`、`0x2949`、
  `0x2956`、`0x2963`、`0x2970`、`0x2efe`。

本次代码修复后验证：

```bash
cmake --build external/NotDec-bin2llvm/build --target notdec-native-discover notdec-native-llvm -j4

OUT=/tmp/notdec-bin2llvm-ddisasm-cfg-clean-20260706121220
external/NotDec-bin2llvm/build/bin/notdec-native-discover \
  --unresolved-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
external/NotDec-bin2llvm/build/bin/notdec-native-discover \
  --instructions-range-json 0x2872 0x287c \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" --all-confirmed --skip-runtime
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -disable-output
```

结果：

- `--unresolved-json`：`count=0`。
- `0x2879` 指令的 `direct_flow_targets` 已补齐 14 个 jump table target。
- IR：`/tmp/notdec-bin2llvm-ddisasm-cfg-clean-20260706121220/fortune.native.ll`。
- `summary_return refs=0`。
- `summary_clobber refs=0`。
- `load .*@RAX refs=0`。
- `load .*@RBX refs=0`。
- `tail call void % refs=0`。
- `llvm-as` 和 `opt -passes=verify` 通过。
- fortune native run `elapsed=12.10s`；这次包含 ddisasm/GTIRB discovery 修复后的完整 native 链路，
  未看到新的 IR verify 或寄存器 helper 回退。

## 评估

- 实现效果：8/10。fortune 的 jump table 不再变成未知 indirect tail call，相关
  `summary_return` 残留清零。
- 复杂度：2/10。只把 GTIRB 已有块级 CFG 同步到指令级 flow facts，没有引入新分析。
- 维护成本：2/10。逻辑局限在 flow fact normalization；后续如果 ddisasm 直接提供指令级目标，
  这段代码仍只是幂等补齐。

风险是：如果某个间接跳转的 block successor 跨函数，这里会保守跳过，不会强行清 unresolved。
这符合当前目标：只修已经能由 GTIRB/ddisasm 明确落在当前函数 CFG 里的 jump table。

## 追加：final cleanup 清理指令级 SummarySSA metadata

继续检查 fortune 最新 IR 时发现，真实寄存器全局访问、`summary_return`、`summary_clobber` 都已清零，
但仍有大量 `!notdec.register.summary_ssa.*` 指令级 metadata。原因是 final cleanup 只清理了
函数级 metadata 和死全局，没有清理 PHI、entry value、zero-demand 这类指令上的调试标注。

修改：

- `include/notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h:25`：
  `NativeRegisterFinalCleanupSummary` 增加 `InstructionMetadataCleared` 统计。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:31`：
  增加 `InstructionMetadataKeys`，覆盖 `call_value`、`entry`、`phi`、`range_entry`、`replaced`、
  `zero_demand_operand`。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:92`：
  增加 `clearInstructionMetadata(...)`，只在函数已经没有寄存器 load/store/helper 残留时清理这些标注。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:159`：
  final cleanup 清函数级 metadata 后继续清指令级 metadata。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:178`：
  summary 输出增加 `instruction_metadata_cleared`。

验证：

```bash
cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm -j4
OUT=/tmp/notdec-bin2llvm-fortune-final-metadata-clean-20260706123424
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" --all-confirmed --skip-runtime
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -disable-output
```

结果：

- IR：`/tmp/notdec-bin2llvm-fortune-final-metadata-clean-20260706123424/fortune.native.ll`。
- `register_global_refs=0`。
- `register_global_load_store=0`。
- `summary_return=0`。
- `summary_clobber=0`。
- `summary_ssa_metadata_uses=0`。
- `summary_ssa_named_metadata_defs=0`。
- `llvm-as` 和 `opt -passes=verify` 通过。
- fortune native run `elapsed=12.17s`，和上一轮 `12.10s` 同级。

这次清理的是已经消费完的调试/分析标注，不改变 IR 的计算语义。

## 追加：折叠 ddisasm 拆出的 eh_frame cold fragment

继续看 fortune 剩余 `@RBX` 残留时，发现 `FUN_27f0` 不是源码函数，而是
`FUN_4750` 的 cold error block：

```asm
47d9: js 0x27f0
27f0: mov rdi, qword ptr [rbx + 0x28]
```

`0x27f0` 有独立 `.eh_frame` FDE，ddisasm/GTIRB 也把它作为函数导入。这个输入
不能简单算 ddisasm 错；native 侧应把 `.eh_frame` 函数范围当 boundary hint，
对只有函数内 direct flow 进入、没有 call 进入的 cold fragment 折回 owner。

修改：

- `lib/NativeAnalysis.cpp:4620`：
  `foldEhFrameOnlyBranchTargets(...)` 不再只接受
  `gtirb-seed-range-fallback` + 单一 `eh-frame` source，而是调用统一判断函数。
- `lib/NativeAnalysis.cpp:4672`：
  新增 `isFoldableEhFrameBranchTarget(...)`。保留旧的 seed-range fallback 行为；
  对 `gtirb-ddisasm` 函数，只在目标 seed 带 `eh-frame`、不是 runtime 函数、
  没有 call xref、且所有 direct-flow xref 都来自同一个 owner 函数时折叠。

验证：

```bash
cmake --build external/NotDec-bin2llvm/build --target notdec-native-discover notdec-native-llvm -j4

OUT=/tmp/notdec-fortune-fold-ehframe-cold-tight-20260707040700
external/NotDec-bin2llvm/build/bin/notdec-native-discover \
  --native-decode-mode gtirb --function-json 0x27f0 \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
external/NotDec-bin2llvm/build/bin/notdec-native-discover \
  --native-decode-mode gtirb --function-json 0x27b2 \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" --all-confirmed --skip-runtime \
  --register-ssa-warning-out "$OUT/register-ssa-warnings.tsv"
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -disable-output
```

结果：

- `0x27f0`：`function-json found=false`，已折入 `FUN_4750`。
- `0x27b2`：`function-json found=false`，已折入 `FUN_3470`。
- `FUN_27f0 refs=0`，`FUN_27b2 refs=0`。
- `@RBX refs=0`，`@R14 refs=0`。
- 当前 fortune 只剩之前的 `@RCX` 高 32 位 range-entry 残留：`REG_REF RCX=2`。
- `summary_return=0`，`summary_clobber=0`。
- `llvm-as` 和 `opt -passes=verify` 通过。
- fortune native run `elapsed=11.50s`，和前面 `12.10s` / `12.17s` 同级。

评估：

- 实现效果：8/10。解决了 ddisasm/FDE cold fragment 被当作独立源码函数导致的
  `RBX/R14` 入口寄存器残留。
- 复杂度：3/10。只扩展已有 folding pass，并用 xref owner 限制避免跨函数误折叠。
- 维护成本：3/10。后续如果要更严格，可以继续接入 unwind state 兼容性判断；
  当前先用 no-call + single-flow-owner 约束覆盖已见问题。
