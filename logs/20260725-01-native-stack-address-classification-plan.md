# 原始 prompt

```text
内部有一个 8232 字节本地栈 buffer，不应该不在范围内吗，应该是RSP - xxx。把寄存器参数 spill 到本地栈槽，后面再 load，但是当前不是主要关注RSP+xx的地方吗，难道spill到的地方是RSP - xxx？

因为我们寄存器参数也是按照函数的使用判断是否作为参数的，所以还是可以直接扫描使用点作为强证据的。signedOffset < 0可能可以单独扫描，或者单独收集作为参数相关的证据

我觉得store matcher应该支持所有的偏移，不区分什么Offset < 0，但是后续处理的时候，根据ABI cspec的栈传参的范围，去把参数相关区域的load单独拿出来。如果是参数识别就处理拿出来的这些，如果是其他的就去掉参数识别的这些？

详细规划一下具体怎么做，形成一个具体的规划文件
```

# 背景

上一轮 SummarySSA 已经能从 cspec 读取 i386 / x64 的 stack pentry，并能把一部分 `SP.entry + 正偏移` load 识别成 stack 参数。但 fortune 讨论暴露出两个问题：

1. 参数识别、caller stack store binding、本地栈帧访问现在混在一起，容易把“地址偏移正负”误用成“是不是参数”的判断。
2. caller 侧 store matcher 对 stack 地址形状太窄。i386 fortune 里有 call 前已经写入栈参数的场景，但 matcher 没认出来，最后生成 `stack+*.arg_unknown`。

正确方向是把问题分成两层：

- 先统一识别“这是哪个栈地址，偏移多少”，这里不按正负过滤。
- 再按 cspec 的 ABI stack pentry 判断哪些访问属于传参区域。

也就是说，`Offset < 0` 不应该让 store matcher 直接放弃；它只说明这个地址落在当前入口 SP 坐标的下方，可能是本地栈、寄存器 spill、outgoing call arg，也可能是 rewrite 后的 native stack alloca 地址。

# 当前实现状态

相关代码集中在 `lib/passes/summary/NativeRegisterSummarySSA.cpp`：

- `entryStackOffsetFromValue()` / `entryStackOffsetFromPointer()`：只追 `SP.entry +/- 常量` 形状。
- `stackInputSlotForLoad()`：只接收 `signedOffset >= 0`，并用 cspec stack pentry 过滤 callee 侧入口参数 load。
- `appendInternalStackParams()`：扫描 callee 侧 load，并把匹配的 stack slot 加入 internal function signature。
- `findNearestStackStoreBeforeCall()`：从 call 往前找 matching stack store，但只认 `entryStackOffsetBefore()` 能解释的 store 地址。
- `refineInternalStackParamShapes()`：如果 direct callsite 不能绑定足够参数，就裁剪 internal stack params，避免 x64 fortune 误扩成大量 `stack+*.arg_unknown`。

当前最直接的问题不是 `stackInputSlotForLoad()` 的 `<0` 检查本身，而是 stack 地址识别能力太弱，并且 caller store binding 一遇到无法解释的 store 就停止。

# 目标

做一个 SummarySSA 内部的 stack address 分类层，让 callee 参数识别和 caller store binding 共用同一套地址解释，但后续消费规则分开：

1. store matcher 支持所有可归一化的 signed offset，不因为 offset 正负直接放弃。
2. 参数识别只消费 cspec stack pentry 覆盖的入口参数区域。
3. 本地栈帧、寄存器 spill、outgoing call arg 可以被分类和记录，但不能直接扩 internal signature。
4. i386 fortune 中 call 前已写栈参数但仍变成 `stack+*.arg_unknown` 的例子要下降。
5. x64 现有寄存器参数恢复不能退化，尤其不能重新引入 `FUN_5270` / `FUN_3470` 这类误扩 stack 参数。

# 技术路线

## 1. 引入统一 stack address 分类

在 SummarySSA 内部新增一个小 helper，用来解释 load/store 指针，返回一个简单分类结果：

- `KnownEntrySpOffset`：地址能归一化到 `SP.entry + signedOffset`。
- `KnownNativeFrameOffset`：地址来自 `notdec_stack.native` alloca 或其 `ptrtoint + 常量`，记录 native frame offset。
- `Unknown`：暂时解释不了。

结果里只需要保留：

- address kind。
- signed offset。
- access size。
- base stack pointer register 名。
- 原始 instruction/value，用于 warning 和 audit。

第一版只支持这些形状：

- `load @ESP/@RSP` 作为 entry SP。
- `entry SP + const` / `entry SP - const`。
- `inttoptr(entry SP +/- const)`。
- `ptrtoint %notdec_stack.native`。
- `ptrtoint %notdec_stack.native.ptrN +/- const`。

不做 alias 分析，不跨 basic block 推复杂表达式。

## 2. ABI 参数区域只从 cspec 得到

保留现在 `AbiFacts::StackInputsInOrder` 的来源：只从 `NativeAbiSpec::Inputs` 里的 stack pentry 读取。

然后增加一个明确的判断函数：

- 输入：`StackAddressClass`、访问 size。
- 输出：是否落在 ABI stack input pentry 范围内，以及对应 `NativeSignatureSlot`。

规则：

- 只有 `KnownEntrySpOffset` 能作为 callee entry stack 参数。
- offset 必须落在 cspec stack pentry 范围。
- size 必须满足 pentry 的 minsize/maxsize。
- offset 必须满足 pentry align。
- i386 `stack+4`、x64 sysv `stack+8`、未来 x64 MS `stack+40` 都只能来自 cspec metadata，不能按架构名硬编码。

`KnownNativeFrameOffset` 不作为 ABI 参数输入，但可以作为本地栈或 outgoing arg 的候选证据。

## 3. callee 侧参数扫描

`appendInternalStackParams()` 继续扫描 callee 侧 load，但不再自己解析地址。它只做：

1. 调用统一 stack address classifier。
2. 只保留 `KnownEntrySpOffset`。
3. 再用 ABI pentry 判断是否是参数区域。
4. 有真实 use 才加入 internal signature。

这样可以保持和寄存器参数类似的原则：函数确实读了入口参数位置，才作为参数候选。

但它不能消费：

- `SP.entry - const`。
- native frame alloca。
- rewritten local stack slot。
- call 前 outgoing 参数临时槽。

这些进入 audit / local stack 证据，不扩 signature。

## 4. caller 侧 store matcher 支持所有 signed offset

`findNearestStackStoreBeforeCall()` 改为：

1. 根据 callee 参数 slot 和 cspec `StackShift` 算出 callsite expected offset。
2. 从 call 往前扫描 store。
3. 对每个 store 调用统一 classifier。
4. 如果能归一化并且 offset 等于 expected，就绑定这个 store。
5. 如果 store 地址解释不了，不要立即失败；只有可能写到同一栈区域或是 unknown memory write 时才停止。

这里不能按正负过滤。因为 outgoing call arg 在当前函数的入口 SP 坐标下常常可能是负偏移，尤其函数有自己的 frame 或 aligned stack 后。

第一版仍只在同一 basic block 内向前扫描，跨 block 留到后续数据流。

## 5. 参数识别和其他栈处理分开消费

需要明确消费顺序：

1. callee entry stack load：如果落在 ABI pentry 范围，作为函数参数候选。
2. caller stack store：如果匹配某个 call 参数 slot，作为 call argument binding，并且只有绑定成功后才允许删除这个 store。
3. native frame load/store：继续给 stack frame rewrite / dead store cleanup 处理，不参与参数识别。
4. 其他 `SP.entry - const` 访问：先保守保留，记录 audit，不做参数 rewrite。

重点是不让“参数识别”吞掉本地栈，也不让“本地栈清理”删掉已经被识别成 call arg 的 store。

## 6. 调整 internal stack param 裁剪

当前 `refineInternalStackParamShapes()` 用 direct callsite 绑定前缀裁剪 internal stack 参数。保留这个保护，但判断要更细：

- callee 侧 ABI stack load 是强候选。
- direct callsite 有 matching store 时，确认该参数。
- direct callsite 暂时没有 matching store 时，不马上证明它不是参数；先根据函数是否有 direct callers、是否外部可见、是否递归来决定。

第一版建议：

- 有 direct callers 的 internal 函数：仍按已绑定前缀裁剪，避免 x64 fortune 误扩。
- 没有 direct callers 或外部可达的函数：保留 callee-side ABI stack load 候选，但生成 warning。
- 递归函数：至少要求自身递归 callsite 能绑定对应参数，否则保守裁剪。

这个策略比现在更清楚：caller 证据用于避免误扩，不替代 callee 侧使用证据。

# fortune 里的目标例子

## i386：call 前 store 已存在但 binding 失败

`notdec_native_3510` 中，`__fprintf_chk` 前已经把 3 个值写到当前栈上的连续位置，随后 call 仍然变成 `stack+4/8/12.arg_unknown`。这说明 callsite stack store matcher 没认出这些 store。

计划目标是让这些 store 被绑定成 `__fprintf_chk` 的真实参数，而不是用 unknown 补洞。

同类场景还包括 `fseek`、`fgets`、`fwrite`、`exit` 等 i386 外部调用。

## x64：避免 internal stack 参数误扩

x64 fortune 目前最终 IR 中：

- `FUN_5270()` 没有参数。
- `FUN_3470()` 是 6 个 register 参数。
- `FUN_3eb0()` 是 2 个 register 参数。

这些函数内部有本地栈 buffer 或 register spill，但不应该变成 stack 参数。新分类层必须把 native frame / local spill 和 `RSP.entry + ABI stack pentry` 区分开。

如果后续真的出现 x64 第 7 个参数或 vararg stack tail，它应该通过 cspec `stack+8` pentry 和 caller store 绑定进入参数，而不是靠本地栈 load 猜。

# 阶段计划

## 阶段一：只加 audit，不改 rewrite 结果

先实现 stack address classifier，并在 SummarySSA warning / audit 中记录：

- callee 侧 ABI stack load 候选。
- callee 侧 negative stack load 候选。
- native frame load/store。
- caller 侧 expected stack arg offset。
- caller 侧扫描到的 matching / non-matching / unknown store。

判断标准：

- fortune i386 / x64 输出不变。
- audit 能解释当前 `stack+*.arg_unknown` 是因为 expected offset 没找到 store，还是 store 地址无法归一化。

## 阶段二：改 caller store matcher

让 `findNearestStackStoreBeforeCall()` 使用 classifier，并支持 signed offset / native frame 形状。

判断标准：

- i386 fortune 外部调用的 `stack+*.arg_unknown` 明显下降。
- `call_arg_binding_missing` warning 下降。
- 只删除被绑定的 store。
- x64 fortune 不退化。

## 阶段三：整理 callee 参数扫描

把 `stackInputSlotForLoad()` 改成调用 classifier + ABI pentry filter。保持 `KnownEntrySpOffset` 才能进参数，不把 native frame / negative offset 作为参数。

判断标准：

- i386 internal stack 参数仍能恢复。
- x64 第 7 参数单测仍通过。
- `FUN_5270`、`FUN_3470` 不出现额外 stack 参数。

## 阶段四：调整 internal stack param 裁剪

基于 audit 结果决定是否放宽 `refineInternalStackParamShapes()`。如果 caller matcher 已经能绑定 i386/x64 主要场景，裁剪可以继续保守；如果发现 callee 侧有真实 ABI stack load 但 caller 侧因为间接调用或跨 block 暂时无法证明，再单独引入 warning 和保守保留策略。

判断标准：

- 不重新引入 x64 fortune `stack+*.arg_unknown` 扩散。
- i386 不因为裁剪丢掉已经有明确 `ESP.entry + ABI offset` use 的内部函数参数。

## 阶段五：补测试和回归

新增 SummarySSA 单测：

- i386 caller store 在 `ESP.entry - const` 坐标下也能绑定到 `stack+4` 参数。
- i386 rewritten native frame alloca store 能绑定 outgoing call arg，但不会作为 callee input 参数。
- i386 callee `ESP.entry+4` load 变成 stack formal。
- i386 callee `ESP.entry-4` load 不变成 stack formal。
- x64 第 7 个参数仍从 cspec `stack+8` 进入。
- x64 local alloca / register spill 不扩 internal signature。

回归：

- `native_register_summary_ssa_test`
- i386 fortune
- x64 fortune
- x64 native smoke

# 风险

最大风险是 native frame alloca 和 entry-SP offset 的坐标混淆。尤其 i386 函数里会有 aligned stack、call 前 outgoing args、本地 spill 混在一起。实现时必须让 classifier 明确说出自己识别的是哪种坐标，不能只返回一个裸 offset。

第二个风险是 store matcher 过度跳过 unknown store。遇到无法解释的 memory write 时，如果继续往前找，可能跨过真正 clobber；如果直接停止，又会漏掉 fortune 里的参数 store。第一版可以只对“明确不是栈相关”的 store 继续，对 unknown memory write 仍停止。

第三个风险是 internal stack param 裁剪过强。callee 侧使用 ABI stack slot 本身是强证据，但 caller 侧绑定失败可能只是 matcher 暂时弱。需要先用 audit 区分 matcher 问题和真实误扩。

# 不做什么

本轮不做：

- 跨 basic block stack arg 数据流。
- 动态栈指针表达式。
- 完整 alias 分析。
- struct/byval/sret 参数恢复。
- stdcall callee pop 差异。
- x87 / float stack 参数恢复。
- heritage prototype recovery 路线维护。

# 判断标准

最终完成后应满足：

- stack 地址识别不再用 offset 正负直接决定是否参与 store matcher。
- 参数识别只消费 cspec stack pentry 覆盖的入口参数区域。
- i386 fortune 的 `stack+*.arg_unknown` 和 `call_arg_binding_missing` 明显下降。
- x64 fortune 不出现新的 internal stack 参数误扩。
- SummarySSA 单测覆盖 i386 和 x64 两套 ABI stack pentry。
- IR 仍通过 LLVM 22 `llvm-as` / `opt -passes=verify`。

# 实现记录（2026-07-25）

## 已完成

本次实现了阶段二的核心路径，并补了一处后置清理：

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1407` 新增 `isNativeStackAlloca()` / `nativeStackAllocaSize()`，用于从 `notdec_stack.native` alloca 还原 native frame 的最低偏移。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5983` 新增 `stackSlotIndex()`，只按 cspec `StackInputsInOrder` 展开的 stack slot 计算参数序号，没有硬编码 i386/x64 偏移。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:6003` 到 `6087` 新增 native frame offset 解析，支持 `alloca/gep/ptrtoint/inttoptr/add/sub`，并在每层先走 SummarySSA 的 `resolve()`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:6090` 到 `6178` 新增 `ESP/RSP.range_summary_ssa + const` 解析。fortune i386 的真实形状是 `%ESP.range_summary_ssa527 - 12`，不是最终 IR 里看到的 native alloca 直连形状。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:6180` 到 `6275` 改写 `findNearestStackStoreBeforeCall()`：保留原有 entry-SP 精确匹配；额外收集 native-frame store 和 stack-pointer-SSA-relative store；按最低 outgoing 地址 + cspec slot index * step 绑定栈参数；未知普通内存写和可分析 call 仍截断扫描。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:7945` 到 `7950` 增加 rewrite 后的死 summary helper 兜底清理，避免签名 rewrite 消掉最后 use 后，`summary_clobber` helper 留在最终 IR。
- `tests/native_register_summary_ssa_test.cpp:50` 新增测试用 summary phi metadata helper。
- `tests/native_register_summary_ssa_test.cpp:2187`、`2289`、`2367` 新增/调整 i386 caller stack store 用例，覆盖 native-frame 单参数、native-frame vararg prefix、以及 fortune 里的 `ESP.range_summary_ssa + const` 形状。
- `tests/native_register_summary_ssa_test.cpp:8456` 到 `8458` 挂载新增测试。

## fortune 实例结果

`notdec_native_3510` 的 `__fprintf_chk` 之前是：

- `stack+4.arg_unknown`
- `stack+8.arg_unknown`
- `stack+12.arg_unknown`

实现后变成直接实参：

- `%unique_1e780_4355`
- `1`
- `%storemerge`

这对应 i386 push 顺序：最低 outgoing 地址是第一个栈参数。该 call 目前只剩 vararg tail `stack+16` 缺证据 warning，固定前三个参数已绑定。

## 验证

通过：

```bash
cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test -j4
./external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test
cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test notdec-native-llvm -j4
ctest --test-dir external/NotDec-bin2llvm/build -R 'notdec.native_(llvm.realworld_fortune_i386|discover.x86_64_smoke|llvm.x86_64_smoke|llvm.realworld_fortune_x86_64)' --output-on-failure
```

结果：

- `native_register_summary_ssa_test` 通过。
- `notdec.native_discover.x86_64_smoke` 通过。
- `notdec.native_llvm.x86_64_smoke` 通过。
- `notdec.native_llvm.realworld_fortune_x86_64` 通过。
- `notdec.native_llvm.realworld_fortune_i386` 通过。

## 计划调整

阶段一的 audit 没单独落地。实现时直接通过 fortune 实例定位到了缺失形状：SummarySSA 中间态保留的是 `ESP.range_summary_ssa + const`，最终 IR 才显示成 native alloca。阶段二因此同时支持 native-frame 地址和 stack-pointer SSA 相对地址。

阶段三/四暂未展开。callee 侧参数扫描仍保持原来的 `KnownEntrySpOffset + cspec pentry` 过滤，不把 native frame 或负偏移 load 当作 internal stack 参数。x64 fortune 回归确认 `FUN_3470` 仍是 6 个寄存器参数，`FUN_5270` 没有重新误扩 stack 参数。

## 评分

- 实现效果：8/10。修掉 fortune i386 的目标 `__fprintf_chk` 固定栈参数，并保持 x64 回归不退化。
- 复杂度：6/10。新增了两类地址解析，代码比单纯 entry-SP matcher 复杂，但范围集中在 caller store binding。
- 维护成本：6/10。后续如果要支持跨 basic block 或动态栈调整，需要把现在的同块回扫提升成小型数据流；当前实现没有提前做这层扩展。

更好的后续方案：把 native-frame / stack-pointer-relative / entry-SP 三类结果收敛成一个明确的 `StackAddressClass` 结构，给 callee load、caller store、audit 共用。当前先保持局部 helper，避免一次性重构 SummarySSA 前段。

# 实现记录（2026-08-03）

## 已完成：`and SP, -16` 作为独立 caller 栈基址

本次完成阶段二的对齐栈补充，不把对齐后的 SP 伪装成 entry SP：

- `lib/passes/summary/NativeRegisterSummary.cpp:67` 到 `:100` 新增
  `StackPointerAddress` 和 `-16` mask 识别。`Base == nullptr` 只表示真正的
  entry SP；`and SP, -16` 的 SSA value 是一个新的 base。
- `NativeRegisterSummary.cpp:145` 到 `:161` 将 summary 的 SP 状态拆为
  `CurrentStackPointer` 和每个 load 的 `StackPointerAddresses`。因此保存寄存器、
  caller store 和 entry register origin 不再共用一个裸 offset。
- `NativeRegisterSummary.cpp:1416` 到 `:1431`、`:1817` 到 `:1917` 按当前 SP
  base 和 cspec `StackShift` 计算 callsite slot。callee 侧仍只认真正的 entry-SP
  地址，aligned SP 不会落进 `stack+4` / `stack+8` 的 callee 参数区域。
- `NativeRegisterSummary.cpp:1966` 到 `:1997` 在每个真实 call 后清空
  `StackSlotOrigins`，保留 `StackSlots` 给 saved-register tracking。这样前一个 call
  的 `[ESP+4]` 证据不会扩宽下一个 call 的 arity。
- `NativeRegisterSummary.cpp:1326` 到 `:1345` 是本次计划外的必要修正：外部
  callsite evidence 预分析原来只按寄存器 lattice 判断 CFG 收敛，call 后清空栈证据
  时可能提前停止。仅在 `CollectExternalCallsiteEvidence` 模式改为比较完整 `State`；
  普通 summary 继续使用原来的 register lattice，避免扩大分析成本。
- `NativeRegisterSummarySSA.cpp:69` 到 `:76`、`:6131` 到 `:6205` 只把对齐结果
  当 caller store matcher 的独立 base；`entryStackOffsetFromValue()` 仍不会把它解释
  成 entry SP。
- `tests/native_register_summary_ssa_test.cpp:2187` 到 `:2307` 新增 i386 对齐 SP
  参数绑定和 call 后旧 evidence 失效测试；`:8576` 到 `:8577` 挂载测试。
- `tests/native_register_summary_ssa_test.cpp:2628` 到 `:2694` 新增 x64 对齐 RSP
  的第七个栈参数测试；`:8649` 挂载测试。
- `tests/native_register_summary_test.cpp:142` 到 `:151` 显式构造 register input
  slot，适配已有的 `Kind` 字段，避免旧 aggregate initializer 把寄存器名当成 enum。

## fortune 结果

i386 fortune 的目标调用现在是：

```llvm
call i32 @recode_new_outer(i32 1)
call i32 @recode_new_request(i32 %2)
call i32 @setlocale(i32 6, i32 %...)
call i32 @nl_langinfo(i32 14)
```

`recode_new_outer` 的 warning 为 `arity=1..1/final=1`。这证明前一个 call 的
`[ESP+4]` 没有被错误复用为它的第二个参数。x64 fortune 的 residue audit 只有表头，
没有 `notdec.register.summary_clobber` 残留。

## 验证

通过：

```bash
cmake --build build --target native_register_summary_ssa_test \
  native_register_summary_test notdec-native-llvm -j4
build/bin/native_register_summary_ssa_test
build/bin/native_register_summary_test
scripts/native-fortune-i386-regression.sh build/bin/notdec-native-llvm \
  /sn640/NotDec/llvm-22.1.0.obj/bin "$PWD" "$PWD/build"
scripts/native-fortune-x86_64-regression.sh build/bin/notdec-native-llvm \
  /sn640/NotDec/llvm-22.1.0.obj/bin "$PWD" "$PWD/build"
ctest --test-dir build -R '^notdec\\.native_llvm\\.x86_64_smoke$' \
  --output-on-failure
```

两个 fortune 脚本均以 LLVM 22 的 `llvm-as` 和 `opt -passes=verify` 验证输出。i386
同口径单次性能对比：修改前 `15.81s / 169792 KB`，修改后 `16.11s / 168600 KB`；
0.30s 差异在一次运行的波动范围内，内存没有增长。

## 计划调整与评分

原计划只考虑了地址分类，没有明确要求 external evidence 数据流也随栈证据收敛。
call 后使 evidence 失效后，这个遗漏会直接影响 CFG join；已限定在 evidence 预分析中
修正，没有把完整状态比较放进常规 summary。

- 实现效果：9/10。i386 对齐栈参数可绑定，旧参数证据不会串到下一 call，x64 回归保持通过。
- 复杂度：6/10。增加一个小地址结构和两张状态表，但没有重构 callee 参数扫描。
- 维护成本：6/10。`and SP, -16` 仅覆盖当前 ABI 常见对齐形状；动态对齐或非 `-16` mask
  仍应在有真实样本时单独扩展。

# 实现记录（2026-08-03，合并栈地址分析）

## 已完成

前两次实现分别在 `NativeRegisterSummary`、`NativeRegisterSummarySSA` 维护当前 SP
状态；`NativeStackFrame` 又单独从入口 SP 找负偏移。这三者都在回答“该地址相对哪个
栈基址”，但结果不能互用。本次将其收敛为 `NativeStackAddress`：

- `include/notdec-bin2llvm/passes/summary/NativeStackAddress.h:20` 到 `:115` 定义三类
  坐标：入口 SP、相对 SP、`notdec_stack.native` frame。只有第一类可以是 callee ABI
  参数；对齐后的 SP 和本地 frame 即使数值偏移相同也不能混用。
- `lib/passes/summary/NativeStackAddress.cpp:190` 到 `:414` 新增函数内 SP 数据流，识别
  直接 SP load/store、常量 add/sub、`and SP, -16` 一类对齐掩码和 native frame
  `alloca/gep/ptrtoint/inttoptr`。前驱地址不一致时返回 unknown，不猜测合并结果。
  `:51` 到 `:64` 还避免对 `INT64_MIN` 对齐掩码做有符号取负。
- `NativeStackFrame.cpp:342` 到 `:498` 改用共享分析，只把可证明的入口 SP 负偏移改为
  `notdec_stack.native`；对齐 SP 仍保守保留，因其低位相对入口 SP 不可知。
- `NativeRegisterSummary.cpp:1129` 到 `:1134`、`:1374` 到 `:1459`、`:1744` 到 `:1800`
  缓存同一分析给 preliminary external callsite evidence 使用，按当前 SP 和 cspec
  `StackShift` 找 caller 栈槽；删除原来的 `StackPointerAddress` 状态副本。
- `NativeRegisterSummarySSA.cpp:1362` 到 `:1435` 和 `:6811` 到 `:6853` 用共享分析扫描
  callee load，但仍须同时满足 entry-SP、非负偏移和 cspec `StackInputsInOrder` 的
  size/alignment 范围。`:5835` 到 `:5953` 用当前 SP 计算 caller expected slot，支持
  正负偏移和相对/native-frame store，却不将它们扩成 internal formal。
- `lib/CMakeLists.txt:14` 将新实现编入 core library。`tests/native_register_summary_test.cpp:644`
  覆盖静态 summary 的对齐 ESP evidence；`tests/native_register_summary_ssa_test.cpp:2751`
  到 `:2942` 覆盖跨 block 的 `ESP-20+24 == stack+4`、i386 负偏移/对齐 local，以及
  x64 对齐后的 `RSP+8` local。既有 x64 第七个参数及对齐 RSP caller 测试继续覆盖
  cspec `stack+8` 路径。

## 结果和边界

i386 fortune 中 `notdec_native_1cb0`、`notdec_native_1ce0` 都保留为 `stack+4`；
`notdec_native_2a50` 从伪 stack formal 收敛到 `stack+4`、`stack+8`。warning 行数为
43。x64 fortune 中 `FUN_5270()` 仍无参数，`FUN_3470` 仍是 6 个寄存器参数，residue
audit 只有表头。

`notdec_native_2a50 -> fwrite` 的四个 push 分散在不同 basic block，`notdec_native_36c0
-> close` 的 push 后有未知别名的普通 store；两者仍会产生 `call_arg_binding_missing`。
本轮不跨 CFG 合并 stack value，也不跳过未知内存写，避免把错误 store 绑定成参数。

## 验证

通过：

```bash
cmake --build build --target native_register_summary_test \
  native_register_summary_ssa_test notdec-native-llvm -j4
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
ctest --test-dir build -R '^notdec\\.native_llvm\\.x86_64_smoke$' --output-on-failure
scripts/native-fortune-i386-regression.sh build/bin/notdec-native-llvm \
  /sn640/NotDec/llvm-22.1.0.obj/bin "$PWD" "$PWD/build"
scripts/native-fortune-x86_64-regression.sh build/bin/notdec-native-llvm \
  /sn640/NotDec/llvm-22.1.0.obj/bin "$PWD" "$PWD/build"
```

两个 fortune 脚本均使用 LLVM 22 的 `llvm-as` 和 `opt -passes=verify`。i386 同口径
单次为 `15.65s / 168464 KB`，上一版为 `15.87s / 168116 KB`，无可见性能退化。

## 评分

- 实现效果：9/10。统一了三处坐标解释，同时保住 i386/x64 的 ABI 参数边界。
- 复杂度：6/10。增加一个小型函数内数据流，删除两份重复状态，整体理解成本略降。
- 维护成本：6/10。后续只有在真实样本需要时，才为跨 block 参数值或 alias 增加独立分析。

# 实现记录（2026-08-03，第一遍 summary 的 native-frame evidence 坐标回退）

## 已完成

阶段二改完后，i386 fortune 的 unknown external 仍全部被推断成 arity=0。根因在
第一遍 summary：`fixedStackSlot()` 记录 outgoing store 证据时用的是
`{notdec_stack.native alloca, allocaOffset}`（`addressForPointer` 对 alloca GEP
返回 NativeFrame），而 `callsiteStackSlot()` 查询时用 entry-SP 坐标
`{@ESP, entryOffset}`，两者永远不匹配，16 个 evidence 槽全部 Unknown，
`localDefinitionPrefix` 得到 0。本次给第一遍的 evidence 查询补 NativeFrame 回退：

- `lib/passes/summary/NativeRegisterSummary.cpp:78` 新增 `nativeFrameOrigin()`，
  从 `notdec_stack.native` alloca 的 `[N x i8]` 大小还原 `frameLow = -N`。
- `:110` 新增 `entrySlotToNativeFrameKey()`，把 entry-SP 偏移换算成
  `entryOffset - frameLow` 的 alloca 偏移。
- `:1429` 的 `callsiteOrigin()` 先按原 entry-SP key 查，miss 后再按换算出的
  alloca key 查 `StackSlotOrigins` / `StackSlots`，其余逻辑不变。
- `tests/native_register_summary_test.cpp:691` 新增
  `testI386RewrittenFrameStackEvidenceMatchesAllocaSlot()`：手工构造
  `notdec_stack.native` alloca + outgoing store + `ESP-4` push，验证证据能匹配；
  去掉回退后该测试失败。

## 结果

i386 fortune：

- `__xstat` 4 个调用点 arity 0 -> 3，最终 IR 为 `call i32 @__xstat(i32 3, ...)`。
- `_IO_putc` arity 0 -> 2，`recode_string` 0 -> 2，`recode_delete_request` /
  `recode_new_outer` 0 -> 1。
- `unresolved_unknown_external_signature` 从 7 条降到 2 条；剩下
  `notdec_native_10a0` / `notdec_native_1839` 是内部零参数函数，修改前已存在。
- 新增 `non_contiguous_vararg_evidence`（`__fprintf_chk` / `__snprintf_chk` /
  `__sprintf_chk` / `snprintf`）和个别 `call_arg_binding_missing` warning：这是
  arity 推断从 0 变成真实值后暴露的后续问题，vararg tail（`stack+16` 等）绑定
  尚未完全对齐，留待下轮。

x64 fortune 无退化，`unresolved_unknown_external_signature` 仍为 0。

## 验证

```bash
cmake --build build --target native_register_summary_test \
  native_register_summary_ssa_test notdec-native-llvm -j$(nproc)
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
scripts/native-fortune-i386-regression.sh build/bin/notdec-native-llvm \
  /sn640/NotDec/llvm-22.1.0.obj/bin "$PWD" "$PWD/build"
scripts/native-fortune-x86_64-regression.sh build/bin/notdec-native-llvm \
  /sn640/NotDec/llvm-22.1.0.obj/bin "$PWD" "$PWD/build"
```

两个 fortune 脚本均用 LLVM 22 的 `llvm-as` 和 `opt -passes=verify` 验证输出。

## 评分

- 实现效果：9/10。unknown external 的栈参数 arity 恢复正确，x64 无退化。
- 复杂度：5/10。只加两个小 helper 和一个回退查询，没有改动 rewrite。
- 维护成本：5/10。坐标换算依赖 alloca 大小等于 `-frameLow` 这一构造不变式，
  后续改 rewrite 布局时需要同步留意。

# 实现记录（2026-08-03，过滤栈槽 non_contiguous vararg evidence 误报）

## 已完成

上一轮新增的 `non_contiguous_vararg_evidence` warning 是误报：栈槽 evidence
窗口（`addStackEvidenceSlots` 固定补 16 个槽，`VarArgInputs` 一直延伸到局部
变量区）里，参数槽后面排着局部变量 store，来源同样是 `LocalDefinition`，
`hasLocalDefinitionAfter` 就误报"参数区不连续"。实际每个告警点的 tail 都与
IR 里真实 push 数一致（i386：`snprintf` tail=1、`__sprintf_chk` tail=1、
`__snprintf_chk` tail=2/3、`__fprintf_chk` tail=1，共 9 条；x64 4 条），
只是栈上 evidence 无法靠来源区分参数槽和局部变量槽。

改法：`lib/passes/summary/NativeRegisterSummarySSA.cpp:1938`
`hasLocalDefinitionAfter()` 只检查非栈槽（`Kind != Stack`）。栈上"不连续"
基本是 outgoing 区与本地帧重叠造成，不可靠；寄存器槽（x64 SysV vararg 的
`RDI..R9` 顺序）仍保留这个信号。

## 结果

- i386 fortune：`non_contiguous_vararg_evidence` 9 -> 0。
- x64 fortune：基线对比只有 4 条 `non_contiguous_vararg_evidence` 消失，
  其余 warning 完全一致，无退化。
- 该函数只在告警分支调用，不影响 vararg tail 推断和 rewrite。

## 验证

```bash
cmake --build build --target notdec-native-llvm -j$(nproc)
scripts/native-fortune-i386-regression.sh build/bin/notdec-native-llvm \
  /sn640/NotDec/llvm-22.1.0.obj/bin "$PWD" "$PWD/build"
scripts/native-fortune-x86_64-regression.sh build/bin/notdec-native-llvm \
  /sn640/NotDec/llvm-22.1.0.obj/bin "$PWD" "$PWD/build"
```

## 评分

- 实现效果：10/10。误报全部消除，真实 tail 推断未受影响。
- 复杂度：2/10。只改一个判断条件加注释。
- 维护成本：3/10。语义清楚（栈槽不连续不可靠），后续若要重新启用需先解决
  局部变量区混入 evidence 的问题。

# 实现记录（2026-08-03，i386 PIC get_pc thunk 识别与折叠）

## 已完成

fortune 是 PIE，几乎每个函数入口都有 `call 1740; add $imm, %ebx`
（0x1740 = `mov (%esp),%ebx; ret` 的 `__x86.get_pc_thunk.bx`，0x1839 是
`mov (%esp),%edx; ret` 的 dx 变体）。这两个地址不在 confirmed 函数里，被当成
unknown external：30+1 个调用点的返回地址 push 被误当栈参数，签名推成
`notdec_native_1740(i32 %EBX.arg)`，几乎每个函数入口都多一个 EBX.arg，
EBX 基址（GOT）完全没建模。修复分两层：

1. thunk 识别（NativeAnalysis）：新增 `X86PcThunkAnalyzer`
   （`lib/NativeAnalysis.cpp:4522`，priority 70），从已解码 CALL 的
   `DirectCallTargets` 收集未确认目标，按 `8b ?? 24 c3` 字节模式匹配
   `mov (%esp),%reg; ret`，写入 `NativeFunctionSeed` / `NativeFunction` 新字段
   `IsPcThunk` / `PcThunkRegister`（`include/notdec-bin2llvm/NativeAnalysis.h:72`
   与 `:198`）。0x1740 / 0x1839 因此进入 `functions()`。
2. call+add 折叠（PcodeToLLVM）：`PcodeLoweringConfig` 新增
   `ThunkCallTargets`（address -> register，`PcodeToLLVM.h:57`）；
   `prepareX86PcThunkSuppression()`（`lib/PcodeToLLVM.cpp:525`）对 thunk call
   预扫描后续 `add $imm, %reg`，把 CALL 指令组和 ADD 都加入
   `SuppressedPcodeOpIndices`，并在被抑制的 call 位置写
   `%reg = fallthrough + imm` 常量（fallthrough = call 地址 + 指令大小）。

工具层同步：`tools/notdec-native-llvm.cpp:459` 注册 analyzer；
`:883` 收集 thunk map 传入 config；`:900` 跳过 thunk 函数本身提升；
`tools/notdec-native-discover.cpp:1431` 注册 analyzer。

## 结果

i386 fortune：

- `notdec_native_1740` / `notdec_native_1839` 从 IR 完全消失，
  `EBX.arg` 参数全部消失（main 从 `(i32 %EBX.arg)` 变回 0 参数）。
- GOT 基址变成常量，`load [ebx+off]` 解析成绝对地址
  （`inttoptr (i32 N to ptr)` 共 69 处），例如 2a50 的
  `add %EBX,21631` 折叠后 EBX = 0x1496 + 0x6B3A = 0x7FD0。
- `notdec_native_43c0`（init-array 处理）从 `(EBX.arg, stack+4/8/12)`
  变为 `(stack+4, stack+8, stack+12)`（argc/argv/envp），循环里
  `.init_array` 地址 0x7D98 成为常量。
- warnings 从 55 条降到 41 条：1740 的 arity=0..1 / inconsistent、
  1839 的 unresolved、相关 clobber 全部消失；剩 10a0（`.init` 入口）
  unresolved 和 call_arg 栈绑定问题留待后续。

x64 fortune 与改动前 warnings 完全一致，无退化。

## 验证

```bash
cmake --build build --target notdec-native-llvm notdec-native-discover \
  pcode_to_llvm_test native_register_summary_ssa_test -j$(nproc)
./build/bin/pcode_to_llvm_test
./build/bin/native_register_summary_ssa_test
./build/bin/native_analysis_facts_test
./build/bin/native_register_summary_test
./build/bin/native_external_call_signature_rewrite_test
scripts/native-fortune-i386-regression.sh build/bin/notdec-native-llvm \
  /sn640/NotDec/llvm-22.1.0.obj/bin "$PWD" "$PWD/build"
scripts/native-fortune-x86_64-regression.sh build/bin/notdec-native-llvm \
  /sn640/NotDec/llvm-22.1.0.obj/bin "$PWD" "$PWD/build"
```

新增单测 `testX86PcThunkCallFoldsToConstantBase()`
（`tests/pcode_to_llvm_test.cpp:1366`）：构造 `call 0x1740` +
`add $0x547f, %ebx` 的 i386 pcode，验证 call 被折叠掉、EBX 被写常量
`0x1005 + 0x547f`。顺带修了上一轮遗留：`native_register_summary_ssa_test`
里 `__snprintf_chk` 用例 FixedArgs 4 -> 5
（`tests/native_register_summary_ssa_test.cpp:3794`），该用例在原型表
改成 5 后一直挂。

## 评分

- 实现效果：10/10。thunk 全部消解，GOT 基址常量，函数签名不再被返回
  地址污染。
- 复杂度：4/10。A 是一个 analyzer + 两个字段；B 复用现有 pcode 抑制
  机制，新增一个 prepare 函数。
- 维护成本：4/10。thunk 字节模式只覆盖 i386 `mov (%esp),%reg; ret`，
  其它架构（x64 RIP 相对）不需要；后续若遇到 thunk 后不跟 `add` 的
  变体，当前会退化成保留 call（不折叠），行为安全。

## 已完成：caller 侧回溯扫描不再被非栈 store 打断

### 背景

剩余 14 条 call_arg 绑定告警里，3fe0→3d30 stack+4、36c0→close stack+4
（含其 rewrite_missing）是同一类：push store 就在 call 同一块里，但更靠近
call 的位置有非栈 store（3fe0 的 `mov %ax,(%esi)`、36c0 bswap 后对
`inttoptr(ESI+k)` 的 store），`findNearestStackStoreBeforeCall`
遇到非栈 store 直接 break，够不到更早的 push。

### 实现

`lib/passes/summary/NativeRegisterSummarySSA.cpp`
`findNearestStackStoreBeforeCall()`（约 5920 行）：非栈、非寄存器 store
由 `break` 改为 `continue`。语义与第一遍 dataflow 对齐：第一遍只跟踪
concrete stack slot，非栈 store 不使槽位失效；call 和其余
mayWriteToMemory 指令仍然限制扫描范围（对应第一遍 call 后
`consumeCallerStackArgEvidence` 清空 outgoing evidence）。

### 验证

- i386 fortune：41 条 warning → 38 条；call_arg 15 → 11 条，3fe0/36c0
  的 3 条（2 binding_missing + 1 rewrite_missing）消失。
- x64 fortune：call_arg 仍 0 条，无回归。
- 5 个 native 单测全过。

### 评分

- 实现效果：10/10，3 条告警消失，无新告警。
- 复杂度：2/10，一行语义改动。
- 维护成本：2/10，注释说明了与第一遍 dataflow 的一致性。

剩余 call_arg 告警 11 条：跨块 push 7 条（2170 递归、2a50→2170、
2a50→fwrite 及其 3 条 rewrite_missing、3510→__fprintf_chk），
fp vararg 2 条（19b0），clobber 值 2 条（3a90）。
