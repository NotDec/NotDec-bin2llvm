# native register final cleanup 实现记录

## 背景

fortune native 输出里，寄存器 SSA 已经消除了大部分寄存器访问，但输出 IR 还残留两类非语义内容：

- 无引用的 `!notdec.register` 全局变量、`llvm.*.with.overflow.*` declaration、`notdec.register.summary_*` helper declaration。
- 已经没有寄存器访问的函数上还带 `notdec.register.summary*` / `notdec.register.summary_ssa` metadata。

单独验证后，`GlobalDCEPass` 可以删掉第一类内容；函数 metadata 不属于 dead global，仍需要自己清。

## 实现

- `include/notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h:12`：新增 `NativeRegisterFinalCleanupOptions` 和 `NativeRegisterFinalCleanupSummary`。
- `include/notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h:28`：声明 `runNativeRegisterFinalCleanup(...)` 和 summary 打印函数。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:19`：定义只清理 summary 相关的函数 metadata key，不清 `external_inputs` / `preserves` / `clobbers`。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:56`：新增 `functionHasRegisterResidue(...)`，函数内还有 register global load/store 或 `notdec.register.*` helper call 时保留 metadata。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:92`：封装 `GlobalDCEPass`，交给 LLVM 删除无引用 register global、overflow intrinsic declaration 和 helper declaration。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:113`：最终 cleanup 流程为 `GlobalDCE -> 清函数 summary metadata -> GlobalDCE -> 统计剩余 register access`。
- `lib/CMakeLists.txt:14`：把 `NativeRegisterFinalCleanup.cpp` 加入 `notdec-bin2llvm-core`。
- `tools/notdec-native-llvm.cpp:952`：新增 `runFinalCleanupPass(...)`，cleanup 后再 `verifyModule`。
- `tools/notdec-native-llvm.cpp:996`、`tools/notdec-native-llvm.cpp:1119`：IR 输入和 ELF 输入两条 `notdec-native-llvm` 链路都在输出前运行 final cleanup。
- `tests/native_register_summary_ssa_test.cpp:270`：新增测试 helper 检查 summary metadata 和 unused declaration。
- `tests/native_register_summary_ssa_test.cpp:3493`：覆盖干净函数会删除 dead register global 和 summary metadata。
- `tests/native_register_summary_ssa_test.cpp:3526`：覆盖还有 register load 时保留 metadata 和 global。
- `tests/native_register_summary_ssa_test.cpp:3559`：覆盖 GlobalDCE 删除 unused overflow intrinsic declaration 和 summary helper declaration。

## 验证

```bash
cmake --build build --target native_register_summary_ssa_test notdec-native-llvm -j4
ctest --test-dir build -R notdec.native_register_summary.ssa --output-on-failure
```

结果：通过，`notdec.native_register_summary.ssa` 用时约 `0.71s`。

fortune smoke：

```bash
/usr/bin/time -f 'TIME native-fortune-cleanup-globaldce2 %e' \
  build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --register-ssa-summary \
  -o /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.ll \
  -o /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.bc \
  -o /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.verified.bc
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.ll
```

结果：`llvm-as` 和 `opt -passes=verify` 通过；native register residue audit 只有表头。运行时间 `8.32s`，和修改前 fortune 同口径 `8.16s` 接近，未看到明显性能回退。

输出清理效果：

- `llvm.*.with.overflow.*` declaration 已清掉。
- `notdec.register.summary_*` helper declaration 已清掉。
- `!notdec.register` global 从原来的 27 个降到 4 个，剩余 `RSP/RAX/RBX/RDX` 对应仍有真实 load。
- 只有还存在寄存器 load 的 `notdec_native_27f0` 和 `notdec_native_31f0` 保留 summary metadata。

## 评估

- 实现效果：8/10。能清掉输出 IR 里的无用 global/declaration，并只在干净函数上删除 summary metadata。
- 复杂度：2/10。新增 pass 很小，主要依赖 LLVM GlobalDCE，自己只做保守 metadata 清理。
- 维护成本：2/10。后续如果新增 summary metadata key，只需要补 key 列表；不需要维护自定义 global DCE 逻辑。

更好的方案是未来把 register residue audit 接成更正式的 C++ 或 lit 检查，但当前先用现有 native summary SSA 单测和 fortune smoke 覆盖主链路即可。
