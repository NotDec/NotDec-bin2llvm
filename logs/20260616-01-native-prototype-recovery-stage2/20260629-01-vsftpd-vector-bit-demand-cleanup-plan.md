# 原始 prompt

那个matcher是准备弃用，被bit demand 分析替代掉的，直接删掉那部分matcher可以吗，避免误导。同时规划一下怎么处理函数签名重写ABI，以及基于big demand分析消除这些寄存器

将当前规划写成一个新的logs/下的规划文件。然后，按照AGENTS.md的规范启动一个goal实现它

# 背景

当前默认 native 链路是 summary：

- 代码在 `include/notdec-bin2llvm/passes/summary/` 和 `lib/passes/summary/`。
- `NativeRegisterSummarySSA` 负责 summary SSA、寄存器消除、call/internal signature rewrite。
- 本次不碰 heritage 链路。

`vsftpd` 最新 selected-targets-native IR：

- `/sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.ll`

当前 `vsftpd` 的 register residue 只剩 8 条 vector store，全是 `ZMM0/ZMM2`：

- `notdec_native_18550` 里 1 条 `ZMM2`
- `notdec_native_e4c0` 里 7 条 `ZMM0`

这些残留多是 lifted SSE/XMM 低 lane 操作落到全宽 `ZMM` global 后留下的状态写回。例如：

```llvm
%old = load i512, ptr @ZMM0
%new = and i512 %old, -340282366920938463463374607431768211456
store i512 %new, ptr @ZMM0
```

这表示清低 128 bit、保留高位。后续如果没有真实观察点需要这些高位，就应该能被 bit demand 清掉。

# 当前问题

`NativeRegisterSummarySSA` 里已经有 bit demand 分析：

- `computePartialDemands()`
- `rewritePartialWrites()`

但旧逻辑还保留了面向固定形状的 matcher：

- `matchKeepHighPartialStore()`
- `rewriteDeadKeepHighParts()`

这个 matcher 主要覆盖 `load old -> and keep-high -> or low -> store`。`vsftpd` 里常见的是裸 `and keep-high -> store`，没有 `or low`，所以没有完全吃掉。

旧 matcher 也容易误导后续开发：看起来 partial register cleanup 仍靠形状匹配，而目标方向应该是 bit demand 统一决定哪些 bit 还需要保留。

# 目标

第一阶段目标：

- 删除旧 partial-write matcher 的主路径。
- 让 `rewritePartialWrites()` 直接基于 bit demand 改写寄存器 store 的 value。
- 覆盖 `load @ZMM -> and keep-high-mask -> store @ZMM` 这类裸 keep-high 形状。
- 尽量让 `vsftpd` 的 8 条 ZMM residue 归零。

第二阶段目标：

- 规划并逐步补 internal function signature rewrite 对 XMM/ZMM ABI slot 的支持。
- 外部函数先保守，只对已知 prototype 的 float/double 参数使用 XMM slot，不对 unknown external call 泛化假设。

# 技术路线

## 1. bit demand cleanup

把 `rewritePartialWrites()` 改成只依赖 `computePartialDemands()`：

- register store 本身不是真实 observer。
- 普通 memory store、call 参数、return、branch/switch 条件、icmp/fcmp 操作数才作为真实需求来源。
- 对每个 register store 的 value，根据真实需求 mask 递归改写。
- 如果某个 operand 的需求为 0，用同类型 0 断开数据流，让 DCE 删除旧 register load 链。
- 重点支持 `and/or/xor/trunc/zext/sext/shl/lshr/bitcast/freeze/select/phi` 的基本传播。

这样 `load @ZMM -> and keep-high-mask -> store @ZMM` 里，如果 high bits 没有真实需求，`load @ZMM` 应该被断开。

## 2. 函数签名重写 ABI

分阶段做，不在第一阶段一次完成：

- 内部函数先允许把 whole `ZMMn` 当作 `i512` 参数/返回。这比较粗，但语义保守，可以先吃掉 internal call 前的 `store @ZMMn`。
- 后续再用 ABI slot 信息把 `XMMn_Qa`、`XMMn`、`YMMn` 映射到 backing `ZMMn` 的低 lane，签名类型逐步精细化成 `float`、`double` 或 vector 类型。
- 外部声明只对已知 prototype 启用 float/double XMM 参数。unknown external call 不直接假设会读取全部 XMM/ZMM。

# 风险

- 如果 bit demand 的真实 observer 集合太窄，可能误删仍有语义的寄存器状态。
- 如果 observer 集合太宽，`vsftpd` 的 ZMM 残留可能降不下来。
- whole `ZMM i512` 参数适合内部函数第一步，不适合作为最终 ABI 表达。
- XMM/ZMM 低 lane 和高 lane 的关系要保守处理，不能把高位清零当成无条件可删。

# 判断标准

- `native_register_summary_ssa_test` 通过。
- `ctest -R notdec.native_register_summary.ssa` 通过。
- `vsftpd` 重新生成后 `scripts/native-register-residue-audit.py` 中 vector store residue 明显下降，理想为 0。
- `wrk` 非 x87 residue 不回退。
- `fortune` 保持干净，并记录同口径耗时。
- 生成的 IR 能通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。

# 本次实现记录

完成第一阶段 bit demand cleanup，没有实现第二阶段 ABI 签名重写。

改动文件：

- `lib/passes/summary/NativeRegisterSummarySSA.cpp`
  - `FunctionBuilder::run()`：第 1467-1469 行，把 `collectSignatureCallArgs()` 放到 `rewritePartialWrites()` 前，避免 bit demand 在 call rewrite 前删掉已记录的参数值。
  - `FunctionBuilder::removeDeadStoresAfterSignatureRewrite()`：第 1483-1487 行，在 post rewrite cleanup 里也调用 `rewritePartialWrites()`，让 signature rewrite / InstCombine 后暴露的新死链也能被断开。
  - 删除旧 `matchKeepHighPartialStore()` 和 `rewriteDeadKeepHighParts()`。
  - 新增 `demandedBits()`、`zeroDemandReplacement()`、`eraseTriviallyDeadNonPhiTree()`、`rewriteZeroDemandOperands()`：第 1569-1657 行。现在寄存器 store 的 value 直接按 bit demand 断开 zero-demand operand，不再靠固定 `and/or` matcher。
  - `rewritePartialWrites()`：第 1969-2000 行，先收集 store，再逐个基于 `computePartialDemands()` 改写；pre-signature 阶段跳过已记录的 call arg store/value，避免破坏后面的 call rewrite。
- `tests/native_register_summary_ssa_test.cpp`
  - 新增 `testPartialZmmNakedKeepHighStoreIsDemandRewritten()`：第 3286-3334 行，覆盖 `load @ZMM0 -> and keep-high-mask -> store @ZMM0` 这种 `vsftpd` 形状。
  - `main()` 第 3443 行加入该测试。

实现时遇到一次崩溃：直接用 LLVM 的递归 DCE 删除 zero-demand operand 会删掉还记录在 `PendingPhi` 里的 PHI，`finalizePendingPhis()` 后续访问悬空指针。最后改为 `eraseTriviallyDeadNonPhiTree()`，显式跳过 PHI。

验证：

```bash
cmake --build build --target native_register_summary_ssa_test notdec-native-llvm
build/bin/native_register_summary_ssa_test
ctest --test-dir build -R notdec.native_register_summary.ssa --output-on-failure

/usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss=%M' \
  build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd \
  --all-confirmed \
  --summary-json-out /sn640/NotDec-Exp/Bench2/bin2llvm-native-projects/selected-targets-native/vsftpd-executable/summary.json \
  -o /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.ll

scripts/native-register-residue-audit.py \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.ll

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.ll \
  -o /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.bc \
  -o /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.verified.bc

/usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss=%M' \
  build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed \
  --summary-json-out /tmp/notdec-fortune-vsftpd-bit-demand.summary.json \
  -o /tmp/notdec-fortune-vsftpd-bit-demand.ll

scripts/native-register-residue-audit.py /tmp/notdec-fortune-vsftpd-bit-demand.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-vsftpd-bit-demand.ll -o /tmp/notdec-fortune-vsftpd-bit-demand.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-vsftpd-bit-demand.bc -o /tmp/notdec-fortune-vsftpd-bit-demand.verified.bc

scripts/native-register-residue-audit.py \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/wrk/executable/module-all.ll
```

结果：

- `vsftpd` 重新生成成功，耗时 `elapsed=80.27 user=80.15 sys=0.11 maxrss=211516`。
- `vsftpd` residue audit 为空，原来的 8 条 `ZMM0/ZMM2` vector store 已清掉。
- `vsftpd` LLVM 22 `llvm-as` 和 `opt -passes=verify` 通过。
- `fortune` 生成耗时 `elapsed=8.34 user=8.30 sys=0.03 maxrss=167676`，residue audit 为空，LLVM 22 verify 通过。
- `wrk` 没有非 x87 回退，仍是 4 load + 2 store 的 `ST*` 残留。

评分：

- 实现效果：8/10。解决了 `vsftpd` 当前 ZMM residue，保留了后续 ABI 重写空间。
- 复杂度：6/10。比旧 matcher 更统一，但新增了 zero-demand operand rewrite 和非 PHI DCE，需要继续靠测试约束。
- 维护成本：5/10。逻辑仍在 `NativeRegisterSummarySSA` 内，没有新 pass；后面做 XMM/ZMM ABI 时要复查 call arg 保护逻辑。

后续更好的方案：

- 做第二阶段 internal XMM/ZMM signature rewrite，让内部 call 前的 SIMD 参数不再依赖寄存器 global store。
- 把 bit demand 的 observer 和 rewrite 规则继续补成更系统的 value rewrite，而不是只处理 integer operand 置 0。

# 第二阶段实现记录：internal XMM/ZMM signature rewrite

本阶段先做 conservative 版本：只让 summary 链路的 internal signature rewrite 支持 ABI float slot 对应的 backing `ZMMn`，并按 whole register `i512` 传递。外部 unknown call 规则不变；外部已知 prototype 仍走原来的 float/double slot。

改动文件：

- `lib/passes/summary/NativeRegisterSummarySSA.cpp`
  - `collectAbiFacts()`：第 1032-1036 行和第 1048-1052 行，把 `MetaType=float` 的 ABI input/output 映射后的 backing register 加入 internal param/return 候选集合。这样 `XMM0_Qa` / `XMM0` / `YMM0` 能通过已有 fallback 对到 `ZMM0`。
  - 新增 `isFloatAbiOutputUnit()`：第 1210-1216 行，用于识别一个 internal return 候选是否来自 float ABI output。
  - `shapeForInternalFunction()`：第 1243-1273 行继续用 summary facts 判断 `ReadEntry` / `MayNonEntry` / `ExitDemand`，但允许 float backing register 进入候选；同时限制 whole-ZMM return 只用于原本 `void` 的 lifted helper。已有 LLVM 返回值的函数不额外加 `ZMM` 返回，避免覆盖原来的 public return shape。
- `tests/native_register_summary_ssa_test.cpp`
  - 新增 `testInternalSignatureRewriteUsesZmmArgAndReturn()`：第 2424-2499 行，覆盖内部 callee 从 `ZMM0` 读入、写回 `ZMM0`，caller 通过 call 前 store 和 call 后 load 传值。期望 rewrite 后 callee 变成 `i512 (i512)`，caller 里 `ZMM0` store/load 被清掉。
  - `main()`：第 3497 行加入该测试。

实现时踩到一个边界：最初直接把 float output backing register 全部加入 internal return，会让 `pow/sqrt/sin/cos/log/exp` 这类测试 wrapper 被改成 `i512` 返回，导致已有 `ret i64` 语义被 signature rewrite 覆盖。最后把 whole-ZMM return 限制在原本 `void` 的 lifted helper 上；参数侧不需要这个限制。

验证：

```bash
cmake --build build --target native_register_summary_ssa_test
build/bin/native_register_summary_ssa_test
ctest --test-dir build -R notdec.native_register_summary.ssa --output-on-failure
cmake --build build --target notdec-native-llvm

/usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss=%M' \
  build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd \
  --all-confirmed \
  --summary-json-out /sn640/NotDec-Exp/Bench2/bin2llvm-native-projects/selected-targets-native/vsftpd-executable/summary.json \
  -o /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.ll

scripts/native-register-residue-audit.py \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.ll

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.ll \
  -o /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.bc \
  -o /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/vsftpd/executable/module-all.verified.bc

/usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss=%M' \
  build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed \
  --summary-json-out /tmp/notdec-fortune-xmm-zmm-signature.summary.json \
  -o /tmp/notdec-fortune-xmm-zmm-signature.ll

scripts/native-register-residue-audit.py /tmp/notdec-fortune-xmm-zmm-signature.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-xmm-zmm-signature.ll -o /tmp/notdec-fortune-xmm-zmm-signature.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-xmm-zmm-signature.bc -o /tmp/notdec-fortune-xmm-zmm-signature.verified.bc

scripts/native-register-residue-audit.py \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/wrk/executable/module-all.ll
```

结果：

- `native_register_summary_ssa_test` 通过。
- `ctest -R notdec.native_register_summary.ssa` 通过。
- `vsftpd` 重新生成成功，耗时 `elapsed=82.63 user=82.53 sys=0.08 maxrss=211476`。
- `vsftpd` residue audit 为空，LLVM 22 `llvm-as` 和 `opt -passes=verify` 通过。
- `fortune` 生成耗时 `elapsed=8.20 user=8.16 sys=0.03 maxrss=167876`，residue audit 为空，LLVM 22 verify 通过。
- `wrk` 没有非 x87 回退，仍是 4 load + 2 store 的 `ST*` 残留。

评分：

- 实现效果：7/10。internal `ZMMn` 参数/返回第一步打通了，但返回侧还保守限制在原 `void` helper。
- 复杂度：4/10。改动集中在 ABI facts 和 shape 构造，没有改 call rewrite 主体。
- 维护成本：4/10。whole `i512` 是过渡表达，后续还需要把 `XMMn_Qa` 低 lane 精细化成 float/double/vector slot。

后续更好的方案：

- 支持 internal signature 的 typed SIMD slot，不再把所有 `XMM`/`YMM` backing 都表达成 whole `i512`。
- 明确处理已有 LLVM 返回值和寄存器返回同时存在的情况，而不是简单跳过非 `void` 函数的 whole-ZMM return。
