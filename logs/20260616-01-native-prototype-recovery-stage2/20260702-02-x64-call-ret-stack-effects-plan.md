# 原始 prompt

对，我的意思就是只消除CALL/RET 指令自身的隐式栈动作。按这个推进试试。因为lifting过来，估计大概率会有这种模式，如果没有匹配到的话，要报warning提示可能存在漏匹配的栈操作。另外，其他的架构会有这种问题吗？是否应该把这个逻辑限定到x64架构？

# 背景

fortune 的 x64 native IR 里，CALL 的返回地址压栈被保留下来，导致 CALL 后继块里的 RSP 变成 `RSP - 8`。后面栈 canary 匹配按固定栈帧地址找值时，会看到错误的 RSP PHI。

Sleigh 的 x64 p-code 里，CALL 通常是：

- `RSP = RSP - 8`
- `STORE [RSP], fallthrough`
- `CALL/CALLIND target`

RET 通常是：

- `tmp = LOAD [RSP]`
- `RSP = RSP + 8`
- `RETURN tmp`

LLVM `call` / `ret` 本身已经表达函数调用和返回，不需要再保留这两个返回地址栈动作。

# 目标

只在 x86-64 lifting 阶段消除 CALL/RET 指令自身的返回地址栈动作：

- CALL：删掉固定 8 字节的返回地址压栈。
- RET：删掉固定 8 字节的返回地址出栈和返回目标读取。
- 不删除普通 push/pop、栈参数、栈对齐、callee saved 保存恢复。
- 如果 x64 CALL/RET 没匹配到预期形状，输出 warning，方便后续排查漏匹配。

# 路线

在 `PcodeOpView` 记录机器指令长度，Sleigh decode 后给同一条机器指令产生的 p-code 填上长度。`PcodeToLLVM` 在 lower 前按同一地址的 p-code 分组，只有检测到 x64 `RSP`/`RIP` 寄存器时启用规则。

匹配 CALL 时要求同一组内有 `RSP = RSP - 8`，并且后面有 `STORE [RSP], address + instruction_size`。匹配 RET 时要求 `RETURN` 的输入来自 `[RSP]` load，并且后面有第一处 `RSP = RSP + 8`。匹配到的 p-code op 在正常 lowering 里跳过。

# 风险和判断标准

风险是误删手写跳转或非标准 ABI 的真实栈操作，所以先限定 x64，并且 CALL 要求写入值等于 fallthrough 地址。`ret imm` 只删返回地址 pop，不删额外参数清理。

判断标准：

- `pcode_to_llvm_test` 覆盖 x64 CALL/RET 消除和非 x64 不消除。
- fortune native 链路能生成可验证 IR。
- 关注函数里 CALL 后继 RSP 不再因为返回地址压栈变成 `RSP - 8`。

# 实现记录

## 已完成

- `include/notdec-bin2llvm/Pcode.h:101`：`PcodeOpView` 增加 `InstructionSize`，用于识别同一条机器指令里的 fallthrough 返回地址。
- `lib/SleighLift.cpp:262`：新增 `setInstructionSize(...)`，Sleigh decode 后给本条指令产生的 p-code 填入指令长度。
- `lib/SleighLift.cpp:273`、`lib/SleighLift.cpp:464`：`collectSleighPcode*` 和复用 decoder 路径都记录当前指令新增 p-code 的长度。
- `lib/PcodeToLLVM.cpp:230` 到 `lib/PcodeToLLVM.cpp:408`：新增 x64 CALL/RET 返回地址栈动作匹配。只在检测到 8 字节 `RSP` 和 `RIP` 时启用；CALL 要求 `RSP -= 8` 且 store 值等于 `Address + InstructionSize`；RET 要求返回目标来自 `[RSP]` load，且只跳过第一处 `RSP += 8`。
- `lib/PcodeToLLVM.cpp:94`：正常 lowering 前跳过已匹配的 p-code op。
- `tests/pcode_to_llvm_test.cpp:43` 到 `tests/pcode_to_llvm_test.cpp:138`：补 x64 register/p-code 构造 helper。
- `tests/pcode_to_llvm_test.cpp:1219` 到 `tests/pcode_to_llvm_test.cpp:1317`：补 x64 CALL、x64 RET、非 x64 同形状不消除三组测试。

## 验证

- `cmake --build build --target pcode_to_llvm_test -j4 && ./build/bin/pcode_to_llvm_test`：通过。
- `cmake --build build --target notdec-native-llvm -j4`：通过。
- fortune native 单目标：
  - 输出：`/tmp/notdec-bin2llvm-fortune-x64-callret-skip-runtime-20260702162424/fortune.native.ll`
  - 命令：`build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --skip-runtime --summary-json-out ... -o ...`
  - `llvm-as` 和 `opt -passes=verify` 均通过。
  - confirmed functions：12。
  - `notdec_native_31f0` refs：0。
  - `RSP` refs：0。
  - `FS_OFFSET` refs：3，均为 metadata。
  - native-llvm 时间：6.35s。

## 结果判断

fortune 新 IR 里 `notdec_native_5270` 不再出现 CALL 返回地址压栈带来的 `RSP - 8` 后继漂移。带 `--skip-runtime` 后 `_start` 形状的 `notdec_native_31f0` 已被过滤，代码里没有 `RSP` 残留。剩余 `FS_OFFSET` 只在 metadata 里。

## 复杂度和维护判断

- 实现效果：8/10。解决当前 fortune 里 CALL 返回地址压栈污染 RSP 的问题，同时不触碰真实 push/pop 和 `_start` 栈整理。
- 复杂度：4/10。新增规则集中在 p-code lowering 前的同地址分组扫描，没有改 register SSA 或 prototype recovery。
- 维护成本：4/10。当前只支持 x64；后续如果要支持 x86-32 或其他栈返回架构，需要按架构单独加规则。AArch64、RISC-V 这类 link-register 架构不应复用这条规则。
