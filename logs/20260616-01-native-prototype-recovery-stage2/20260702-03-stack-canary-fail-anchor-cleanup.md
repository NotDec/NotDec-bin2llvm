# Stack canary fail-anchor cleanup implementation

## 背景

fortune 当前 native summary SSA 链路里，stack canary 检查有时会在寄存器消除后留下 `FS_OFFSET` / `RSP` / `__stack_chk_fail` 残留。之前的 canary cleanup 以条件分支为主入口，遇到共享 fail block、ZF 旗标中转、late cleanup 后的 `RSP.entry` 形状时容易漏删。

## 修改

- `lib/passes/summary/NativeStackCanaryCleanup.cpp:83`：`loadReadsRegister` 支持 `notdec.register.summary_ssa.entry`，让 late cleanup 能识别 `RSP.entry` / `FS_OFFSET.entry`。
- `lib/passes/summary/NativeStackCanaryCleanup.cpp:99`：新增 `storeWritesRegister`，用于识别比较结果写入 `ZF` 的 lifted x64 形状。
- `lib/passes/summary/NativeStackCanaryCleanup.cpp:446`：新增通用 `offsetFromRegister`，并在 `stackPointerSavedCanaryPointer` 中识别 `RSP +/- offset` 保存 canary slot。
- `lib/passes/summary/NativeStackCanaryCleanup.cpp:604`：新增 ZF-backed 条件匹配，先匹配 `ZF == 0` 再走普通 `icmp`，避免把外层 flag compare 误认为 canary compare。
- `lib/passes/summary/NativeStackCanaryCleanup.cpp:828`：cleanup 改为从 `__stack_chk_fail` fail block 反向检查 predecessor，只删除真实 canary edge；共享 fail block 中非 canary predecessor 会保留。
- `tests/native_register_summary_ssa_test.cpp:439`：新增 raw `RSP + offset` + ZF canary case。
- `tests/native_register_summary_ssa_test.cpp:765`：新增共享 fail block 的 mixed predecessor case，确认非 canary fail path 不被误删。
- `tests/native_register_summary_ssa_test.cpp:3163`、`:3229`、`:3944`、`:3948`：接入新增回归测试。

## 验证

```bash
cmake --build build --target native_register_summary_ssa_test -j4
./build/bin/native_register_summary_ssa_test
```

通过。

```bash
out=/tmp/notdec-bin2llvm-fortune-canary-final-20260702174749
./build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --skip-runtime --register-ssa-summary \
  --summary-json-out "$out/summary.json" \
  -o "$out/fortune.native.ll"
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as "$out/fortune.native.ll" -o "$out/fortune.native.bc"
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify "$out/fortune.native.bc" -o /dev/null
```

结果：

- `stack_canary_checks_removed=3`
- `stack_canary_fail_blocks_removed=3`
- `RSP refs: 1`，只剩 ABI metadata：`!"name=RSP"`
- `FS_OFFSET refs: 3`，只剩 summary metadata
- `stack_chk_fail refs: 0`
- `notdec_native_31f0 refs: 0`
- `native-llvm time: 6.22s`

## 影响

- 实现效果：8/10。fortune 关注用例里的 canary fail path 已清干净，`__stack_chk_fail` 和实体 `RSP` / `FS_OFFSET` 访问消失。
- 复杂度：6/10。匹配逻辑比原来多，但入口改成 fail block 反查后，误删风险更低。
- 维护成本：5/10。后续如果出现其它 flag 中转形状，可以继续在 `matchCanaryCondition` 里局部扩展。

## 风险

- early cleanup 仍可能因为 raw saved slot 形状不稳定而不删，但 late cleanup 已能处理 register summary SSA 后的稳定形状。
- 当前只处理 `FS_OFFSET + 40` 的 x64 canary 形状，没有扩到其它 ABI 或 TLS canary 偏移。
