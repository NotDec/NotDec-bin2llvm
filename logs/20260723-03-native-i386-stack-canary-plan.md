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
