原始 prompt：

> 对，按这个顺序做前三点吧，不同点之间的修改要分不同的commit

# native i386 栈与 canary 前置修复计划

## 背景

i386 fortune 已经能走 GTIRB native 链路，并且 PLT/GOT 现在能解析到真实外部符号。但输出里还残留大量 `ESP.entry` 和一部分 `GS_OFFSET.entry`。按当前 SummarySSA pipeline 顺序看，栈帧重写和 canary 清理都发生在外部签名推断、签名重写之前，所以应该先修更靠前的栈语义，再做参数分析。

当前顺序是：

1. p-code lifting 生成 LLVM IR。
2. SummarySSA 内部先跑 `runNativeStackFrameRewrite()`。
3. 再跑 `runNativeStackCanaryCleanup()`。
4. 然后才跑 peephole、第一遍 register summary、外部签名推断、第二遍 summary、签名重写。

## 目标

按 pipeline 从前往后完成前三个前置修复，并拆成三个独立 commit：

1. i386 CALL/RET 隐式栈动作消除。
2. i386 stack frame rewrite 基础增强。
3. i386 GS canary / TLS canary 清理。

本次不直接做 i386 栈上传参推断。等 `ESP` 和 `GS_OFFSET` 噪音先压下去后，再做参数分析会更稳。

## 技术路线

### 阶段一：i386 CALL/RET 隐式栈动作

现有 `PcodeToLLVM.cpp` 只识别 x64 的 `RSP -= 8; store return address; call` 和 `load return target; RSP += 8; return`。i386 的 `ESP -= 4` / `ESP += 4` 应该复用同一类逻辑，只是栈指针寄存器、指针宽度和返回地址宽度不同。

实现时先确认 SLEIGH i386 p-code 形状，再把 x64 专用逻辑泛化为 x86-family 逻辑。目标是只消除 CALL/RET 指令自身的隐式 return-address 栈动作，不碰普通 `push` / `pop` / 栈上传参。

### 阶段二：stack frame rewrite

当前 `NativeStackFrame` 只把固定负偏移栈访问局部化，并把 ABI stack pointer 加入忽略集合。i386 上如果阶段一完成后仍有大量 `ESP.entry`，优先检查是否是栈基址不唯一、frame pointer 替换失败，或者正偏移/返回路径被误判。这里先只增强“函数内部局部栈帧”识别，不把 caller 参数槽混进去。

### 阶段三：GS canary

当前 canary cleanup 主要识别 x86-64 的 `FS_OFFSET + 40`。i386 glibc 常见 canary 是 `GS_OFFSET + 20`。应把 TLS base register 和 canary offset 做成 ABI/架构相关匹配：x86-64 继续支持 `FS_OFFSET+40`，i386 增加 `GS_OFFSET+20`。清理策略仍从 `__stack_chk_fail` fail block 反向确认，不泛化成普通 TLS 删除。

## 风险

- i386 的 `push` / `pop` 很常见，不能把普通栈操作误删成 CALL/RET 隐式动作。
- stack frame rewrite 不能把 caller 参数槽当成本地栈帧，否则后续栈传参会丢信息。
- `GS_OFFSET` 也可能用于普通 TLS，只有确认是 canary check 时才能删。

## 判断标准

- 每阶段单独提交。
- i386 fortune 回归通过，IR 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- x86-64 fortune / native smoke 不退化。
- 阶段一后 `ESP.entry` 残留应下降或至少不增加。
- 阶段三后 canary 相关 `GS_OFFSET.entry` 残留应下降。

## 阶段一实现记录：i386 CALL/RET 隐式栈动作

已完成并单独提交。

- `lib/PcodeToLLVM.cpp:111-112`：把 lowering 前的栈动作清理入口从 x64 专用函数改成 x86-family 函数。
- `lib/PcodeToLLVM.cpp:288-475`：把原来的 `RSP` / 8 字节固定匹配泛化为 `X86CallStackSpec`，同时支持 x86-64 `RSP/RIP/8` 和 i386 `ESP/EIP/4`。
- i386 CALL 匹配形状：`ESP -= 4; STORE fallthrough, [ESP]; CALL target`。
- i386 RET 匹配形状：`LOAD [ESP]; ESP += 4; RETURN target`。
- 普通 `push` / `pop` 不包含 `CALL` / `RETURN` p-code，不会被该规则删除。

验证：

- `cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm notdec-native-pcode -j4`：通过。
- `ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_llvm.realworld_fortune_i386 --output-on-failure`：通过。
- `ctest --test-dir external/NotDec-bin2llvm/build -R 'notdec.native_(discover.x86_64_smoke|llvm.x86_64_smoke|llvm.realworld_fortune_x86_64)' --output-on-failure`：通过。
- i386 fortune residue 当前仍主要是 `ESP` 27 条、`GS_OFFSET` 7 条；说明剩余问题主要在后续 stack frame / canary 层，不是 CALL/RET 隐式 return-address 栈动作本身。

评分：

- 实现效果：7/10。消除了 i386 CALL/RET 自身隐式 return-address 栈动作，未直接解决 caller 参数和普通栈帧残留。
- 复杂度：3/10。复用原 x64 结构，只把寄存器名和宽度参数化。
- 维护成本：3/10。后续 x86-family 行为集中在一个 spec 中，比复制一套 i386 逻辑更容易维护。

## 阶段二实现记录：i386 stack frame rewrite 基础增强

已完成并单独提交。

- `lib/passes/summary/NativeStackFrame.cpp:132-144`：新增 `isStackAlignmentMask()`，识别 `-power_of_two` 形式的栈对齐 mask。
- `lib/passes/summary/NativeStackFrame.cpp:179-191`：`stackOffsetFromBase()` 支持 `and stack_base, -alignment`，把它作为本地 aligned stack base 继续追踪负偏移。
- 该规则仍只让负偏移进入 `notdec_stack.native`，不会把 `ESP+4` / `ESP+8` 这类 caller 参数槽纳入本地栈帧。

验证：

- `cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm -j4`：通过。
- `ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_llvm.realworld_fortune_i386 --output-on-failure`：通过。
- `ctest --test-dir external/NotDec-bin2llvm/build -R 'notdec.native_(discover.x86_64_smoke|llvm.x86_64_smoke|llvm.realworld_fortune_x86_64)' --output-on-failure`：通过。
- i386 fortune 的 `stack_frame_accesses_rewritten` 从 693 增加到 704；`notdec_native_14d0` 这类 `and ESP, -16` 的 aligned frame 已生成 `notdec_stack.native`。
- residue 仍为 `ESP` 27 条、`GS_OFFSET` 7 条；剩余 `ESP` 多为正偏移 caller/返回槽，不属于本阶段要处理的本地负偏移栈帧。

评分：

- 实现效果：6/10。补上 aligned stack base 的本地栈帧重写，残留数量暂未下降。
- 复杂度：2/10。只扩展已有 offset 追踪，不改 pipeline。
- 维护成本：2/10。规则窄，只匹配常见对齐 mask。

## 阶段三实现记录：i386 GS canary 与指针宽度修正

已完成。

### canary 清理

- `lib/passes/summary/NativeStackCanaryCleanup.cpp:31-42`：把 TLS canary 位置改成 `TlsCanarySpec`，保留 x86-64 `FS_OFFSET+40`，新增 i386 `GS_OFFSET+20`。
- `lib/passes/summary/NativeStackCanaryCleanup.cpp:155-205`、`207-270`：新增对 TLS base 和 `base + const` 地址链的识别，支持 `zext/add/sub` 这类 cleanup 后更绕的表达式。
- `lib/passes/summary/NativeStackCanaryCleanup.cpp:1132-1225`：删除逻辑仍从 `__stack_chk_fail` fail block 反向看前驱边，只删除已确认的 canary compare 边。
- `lib/passes/summary/NativeStackCanaryCleanup.cpp:1228-1267`：shared fail block 逐个 predecessor 处理，避免一个 fail sink 里混入非 canary 边时误删。

### pointer width 修正

- `lib/PcodeToLLVM.cpp:96-130`、`1442-1444`、`2091-2100`、`2180-2186`、`2460-2464`：根据寄存器集合推断 32/64 位 DataLayout，内存地址、间接调用目标统一按 `module.getDataLayout().getPointerSize()` 调整。
- `lib/HeritageToLLVM.cpp:116-160`、`205-218`、`720-726`、`1737-1742`、`2188-2191`、`2445-2496`：Heritage lowering 同步设置 DataLayout，`long/ulong/T*` 和地址 lowering 不再固定 64 位。
- `lib/passes/summary/NativeStackFrame.cpp:212-221`：`notdec_stack.native` GEP index 改用 DataLayout 的 index type。
- `include/notdec-bin2llvm/NativeExternalPrototype.h:17-23`、`lib/NativeExternalPrototype.cpp:18-30`、`187-198`：新增 `PointerSized` prototype 类型，`ptr/pointer/size_t/ssize_t/long/ulong` 走 DataLayout；`__errno_location` 和 `__ctype_*_loc` 默认返回 pointer-sized。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1038-1048`、`1397-1428`、`1480-1503`：known external 的 typed 参数/返回类型从 module DataLayout 取 pointer 宽度。
- `tests/native_register_summary_ssa_test.cpp:127-147`、`2175-2220`、`7867-7869`：新增 i386 DataLayout 下 `__errno_location` 返回 i32 的回归测试。

### 验证

- `cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test notdec-native-llvm pcode_to_llvm_test heritage_to_llvm_test -j4`：通过。
- `./external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test`：通过。
- `./external/NotDec-bin2llvm/build/bin/pcode_to_llvm_test`：通过。
- `ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_llvm.realworld_fortune_i386 --output-on-failure`：通过。
- `external/NotDec-bin2llvm/build/native-fortune-i386-regression/fortune.native.ll`：`target datalayout = "e-p:32:32"`；`__errno_location` 为 `declare i32 @__errno_location()`；未再出现 `ptrtoint ptr %notdec_stack.native... to i64`。
- `external/NotDec-bin2llvm/build/native-fortune-i386-regression/run.stderr`：`stack_canary_checks_removed=7`，`stack_canary_fail_blocks_removed=7`。
- `./external/NotDec-bin2llvm/build/bin/heritage_to_llvm_test`：仍失败在既有 `stack input fallback was not reused`，这次改动没有修这个旧测试问题。

### 评分

- 实现效果：8/10。i386 canary 从 5 个提升到 7 个删除，`GS_OFFSET` 全局残留消失，i386 指针宽度从源头改成 DataLayout。
- 复杂度：4/10。canary 匹配增加了 TLS 地址链递归，但删除仍锚定 `__stack_chk_fail` 前驱边。
- 维护成本：4/10。pointer-sized prototype 和 DataLayout 路径集中化后，比继续散落硬编码 8 字节更容易维护；Heritage 旧 prototype recovery 仍有单独的 i64 遗留点，后续可单独处理。

### 补充：删除无调用者的 fail-local thunk

- `lib/passes/summary/NativeStackCanaryCleanup.cpp:902-1006`：`functionOnlyCallsStackCheckFail()` 判断出 fail-only 函数时，把对应函数记录到本轮候选集合。
- `lib/passes/summary/NativeStackCanaryCleanup.cpp:1240-1298`：canary fail edge 清完后，只删除 `internal/local + use_empty + fail-only body` 的候选函数。
- `include/notdec-bin2llvm/passes/summary/NativeStackCanaryCleanup.h:20-24`、`include/notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h:100-102`、`lib/passes/summary/NativeRegisterSummarySSA.cpp:6988-7109`：新增 `fail_functions_removed` / `stack_canary_fail_functions_removed` 统计。
- `tests/native_register_summary_ssa_test.cpp:5546-5590`、`7960-7962`：新增 `notdec_native_4430` 形状的 internal fail thunk 单测，确认 fail edge 删除后 thunk 本体也会删除。

验证：

- `cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test -j4`：通过。
- `./external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test`：通过。
- `cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm -j4`：通过。
- `ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_llvm.realworld_fortune_i386 --output-on-failure`：通过。
- i386 fortune 最终 IR 中 `notdec_native_4430` 已删除，只剩 `declare void @__stack_chk_fail()`；summary 输出 `stack_canary_fail_functions_removed=1`。

### 补充：i386 GS canary 回归测试

- `tests/native_register_summary_ssa_test.cpp:551-628`：把 `createStackCanaryCheckFunction()` 从 FS/64 位固定测试 helper 改成可传 TLS register 名和字宽，默认仍是 `FS_OFFSET` / 8 字节。
- `tests/native_register_summary_ssa_test.cpp:5554-5573`：新增 `testI386GsStackCanaryCheckIsRemoved()`，构造 `e-p:32:32`、`ESP` ABI、`GS_OFFSET+20` 的 i386 canary 形状，确认 SummarySSA 前段直接删掉 canary check 和 `GS_OFFSET` load。
- `tests/native_register_summary_ssa_test.cpp:7989`：把新回归接入 `native_register_summary_ssa_test`。

验证：

- `cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test notdec-native-llvm -j4`：通过。
- `./external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test`：通过。
- `ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_llvm.realworld_fortune_i386 --output-on-failure`：通过。
- `llvm-22.1.0.obj/bin/llvm-as external/NotDec-bin2llvm/build/native-fortune-i386-regression/fortune.native.ll -o /tmp/fortune-i386-check.bc`：通过。
- `llvm-22.1.0.obj/bin/opt -passes=verify /tmp/fortune-i386-check.bc -disable-output`：通过。

审计结果：

- i386 fortune 输出没有 `GS_OFFSET` global / load / `GS_OFFSET.entry`。
- `ESP.entry` 剩余 22 个，均非负偏移；主要是 `ESP+4` / `ESP+8` 的 caller 参数或返回槽，不属于本阶段的本地负偏移栈帧重写。
