# Floating register ABI signature rewrite plan

用户原始要求：

> 话说Ghidra Sleigh那边的架构的ABI信息是不是涵盖了浮点相关的？另外，形成一个新的详细的规划文件，规划怎么支持浮点相关的寄存器的传参和返回值

## 当前目标和 native 状态

summary 链路已经能处理大量整数寄存器参数、返回值和 partial register 残留。

`20260625-01-partial-register-bit-lane-liveness-plan.md` 推进后，`fortune`、`wrk`、`memcached`、`redis-cli` 都能重新生成 IR，并通过 LLVM 22 verifier。`memcached` 的 ZMM/RCX/RDX raw access 已明显下降。

现在剩下一个不同性质的问题：`redis-cli` 的 `powerLawRand` 里，`pow` 仍然类似这样：

```llvm
call void @pow(i64 0, i64 0, ...)
%old = load i512, ptr @ZMM3
...
store i512 %new, ptr @ZMM3
```

这不是 partial write liveness 能解决的问题。这里的 ZMM/XMM 访问是在表达浮点 ABI 参数和返回值。正确方向应该是把确定原型的 external call 改成 typed LLVM call，例如：

```llvm
%ret = call double @pow(double %x, double %y)
```

然后由普通 SSA 和 partial demand cleanup 消掉不再需要的 ZMM register global 访问。

## Ghidra / Sleigh 侧 ABI 信息

结论：Ghidra 的 x86-64 cspec 确实包含浮点相关 ABI 信息。

相关文件：

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`
- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-win.cspec`
- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-swift.cspec`

以 `x86-64-gcc.cspec` 为例：

- `data_organization` 里有：
  - `float_size value="4"`
  - `double_size value="8"`
  - `long_double_size value="10"`
- default prototype 的 `<input>` 里先列出浮点参数寄存器：
  - `XMM0_Qa` 到 `XMM7_Qa`
  - `pentry minsize="4" maxsize="8" metatype="float"`
- default prototype 的 `<output>` 里列出浮点返回寄存器：
  - `XMM0_Qa`
  - `XMM1_Qa`
  - `pentry minsize="1" maxsize="8" metatype="float"`
- `<killedbycall>` 里有：
  - `XMM0`

这说明 Ghidra 的 ABI 模型不是只描述整数寄存器。它知道浮点值使用 XMM slot，也知道这些 slot 的 metatype 是 `float`。

## 当前 native 侧已有信息

我们当前已经能解析这些 cspec 信息。

相关文件：

- `include/notdec-bin2llvm/NativeAbi.h`
- `lib/NativeAbi.cpp`
- `tests/native_abi_cspec_test.cpp`
- `lib/passes/summary/NativeRegisterSummarySSA.cpp`

`NativeAbiParamEntry` 已经有：

```cpp
uint32_t MinSize;
uint32_t MaxSize;
std::string MetaType;
NativeAbiStorage Storage;
```

`lib/NativeAbi.cpp:paramEntryFromElement()` 会读取：

- `minsize`
- `maxsize`
- `align`
- `metatype`
- `register` / `addr`

所以 native 侧并不是丢了 Ghidra 的浮点 ABI 信息。

真正的问题在 `NativeRegisterSummarySSA.cpp:collectAbiFacts()`：

```cpp
if (entry.MetaType == "float") {
  continue;
}
```

当前 summary signature rewrite 故意跳过了浮点 input/output。原因是当时 `SignatureShape` 只用 `RegisterUnit*` 表示参数和返回值，LLVM 类型直接取 backing register global 的类型。对当前 lifting 来说，`XMM0_Qa` 会归一到 `ZMM0`，也就是 `i512`。如果直接把浮点 ABI slot 加进去，会把 `double` 错改成 `i512` 参数/返回。

这个 skip 是保守处理，不是 ABI 信息缺失。

## 需要复刻 Ghidra 哪些策略

这里不需要复刻 Ghidra trial/use。

需要借鉴的是 cspec 里的 storage + metatype：

- `metatype=float` 表示这是浮点 ABI slot。
- `XMMn_Qa` 表示低 64 bit，可承载 `double`。
- `XMMn_Da` 或 size 4 表示低 32 bit，可承载 `float`。
- x86-64 SysV 的普通浮点参数顺序是 `XMM0_Qa` 到 `XMM7_Qa`。
- 普通浮点返回优先是 `XMM0_Qa`。

第一版不跑 Ghidra 的完整 datatype rule engine，不处理 HFA、vector aggregate、long double、stack float 参数、vararg 浮点规则。

## 设计核心

当前 `SignatureShape` 太粗：

```cpp
std::vector<const RegisterUnit *> Params;
std::vector<const RegisterUnit *> Returns;
```

它只能表达“某个寄存器作为一个 LLVM 参数/返回值”，不能表达：

```text
storage: ZMM0 / low 64 bits
LLVM type: double
ABI name: XMM0_Qa
```

需要把参数/返回从 `RegisterUnit*` 扩成 typed ABI slot。

建议新增：

```cpp
enum class NativeSignatureSlotKind {
  IntegerRegister,
  FloatRegister,
};

struct NativeSignatureSlot {
  const RegisterUnit *Unit;
  std::string AbiName;        // XMM0_Qa / RDI / ...
  std::string MetaType;       // "", "float"
  unsigned OffsetBits;        // first bit inside backing register
  unsigned SizeBits;          // 32 / 64 first
  llvm::Type *LlvmType;       // i64 / double / float
};
```

然后：

```cpp
struct SignatureShape {
  std::vector<NativeSignatureSlot> Params;
  std::vector<NativeSignatureSlot> Returns;
  bool VarArg;
};
```

第一版只支持：

- integer register slot：保持当前行为。
- float register slot：
  - `SizeBits == 64` -> `double`
  - `SizeBits == 32` -> `float`

## typed slot 的取值和写回

### 1. 从 register value 取出 float 参数

当前 register backing value 可能是 `i512`。

如果 slot 是 `ZMM0` low 64 bits as `double`：

```llvm
%reg = ...
%bits = trunc i512 %reg to i64
%arg = bitcast i64 %bits to double
```

如果 slot 是 `ZMM0` low 32 bits as `float`：

```llvm
%reg = ...
%bits = trunc i512 %reg to i32
%arg = bitcast i32 %bits to float
```

如果后续支持非 low lane，再加：

```llvm
%shifted = lshr i512 %reg, offsetBits
%bits = trunc i512 %shifted to i64
```

但第一版只做低 lane。

### 2. 把 float 返回值放回 register value

如果 external `pow` 返回 `double`：

```llvm
%ret = call double @pow(...)
%bits = bitcast double %ret to i64
%wide = zext i64 %bits to i512
```

这可以作为 `XMM0_Qa` / `ZMM0` 的新 low-lane value。高位不要手工保留；如果后面确实 demand 高位，partial demand 会保留旧值，否则可以清掉。

第一版可以把 typed return value 接入现有 `ReturnHelpers` / call value 机制，让后续 `readValueBefore()` 读到 call 后的 `ZMM0` 时拿到这个 low-lane 组合值。

更简单的第一版实现是：

- 对 external typed float return，call rewrite 后直接记录一个 synthetic call value。
- 对读取 `ZMM0` full value 的场景，构造：

```llvm
%low = zext/bitcast ret to i512
```

如果需要 keep-high，由现有 partial demand 决定是否引入旧值。第一版可以保守只返回 low value，不保留高位，因为 x86 SSE scalar 返回对高位通常不是有意义返回；但这条需要结合 lifted p-code 语义验证，不能硬猜。

## external prototype 表扩展

当前 `KnownExternalPrototype` 只有：

```cpp
FixedArgs
VarArg
NoReturn
MaxReturnRegisters
```

需要扩成能描述 typed 参数和返回。

建议新增轻量结构：

```cpp
enum class KnownValueType {
  I64,
  Ptr,
  Float,
  Double,
  Void,
};

struct KnownPrototypeValue {
  KnownValueType Type;
};

struct KnownExternalPrototype {
  std::vector<KnownPrototypeValue> Params;
  std::optional<KnownPrototypeValue> Return;
  unsigned FixedArgs;
  bool VarArg;
  bool NoReturn;
};
```

为了少改，第一步可以保留 `FixedArgs`，额外加：

```cpp
std::vector<KnownValueType> TypedParams;
std::optional<KnownValueType> TypedReturn;
```

当 `TypedParams` 非空时，shape construction 按 typed prototype 走；否则保持现有整数 ABI 行为。

第一批只加 libm 中确定且收益明显的：

- `pow(double, double) -> double`
- `sqrt(double) -> double`
- `sin(double) -> double`
- `cos(double) -> double`
- `log(double) -> double`
- `exp(double) -> double`

`pow` 是必须覆盖的验证样例。

## 参数寄存器分配

x86-64 SysV 的重点是整数和 SSE 参数分别有自己的序列。

当前 `AbiFacts.InputsInOrder` 因为跳过 `metatype=float`，只保留：

```text
RDI, RSI, RDX, RCX, R8, R9
```

需要新增：

```cpp
FloatInputsInOrder: XMM0_Qa, XMM1_Qa, ...
FloatOutputsInOrder: XMM0_Qa, XMM1_Qa
```

对 typed known external：

- `double` / `float` 参数消耗 float input slot。
- integer / pointer 参数消耗 integer input slot。
- 返回 `double` / `float` 使用 float output slot。
- 返回 integer / pointer 使用 integer output slot。

这样 `pow(double,double)` 会用：

```text
param0 -> XMM0_Qa / ZMM0 low64
param1 -> XMM1_Qa / ZMM1 low64
return -> XMM0_Qa / ZMM0 low64
```

不是 `RDI/RSI`。

## call rewrite 流程变化

当前大致流程：

1. `buildInitialSignatureShapes()`
2. 每个函数 `FunctionBuilder::run()`
3. `rewriteLoads()`
4. `rewritePartialWrites()`
5. `collectSignatureCallArgs()`
6. `collectFunctionReturnValues()`
7. `rewriteSignatureShapes()`

新增 typed float 后：

### shape construction

`shapeForKnownExternal()`：

- 如果 external prototype 有 typed params：
  - 按 type 从 integer / float ABI input sequence 中分配 slot。
  - slot 的 LLVM 类型来自 known prototype。
- 如果没有 typed params：
  - 保持现有 fixed arity 行为。

### argument binding

`callArgStoreBindings()` 现在要求：

```cpp
value->getType() == unit->Global->getValueType()
```

typed float 需要改成：

- 先 `readValueBefore()` 拿 backing register value。
- 根据 slot 从 backing value 取 bit range。
- bitcast 成 `float` / `double`。
- 这个 converted value 作为 call operand。

如果 call 前有只服务于这个参数的 `store @ZMMn`，仍然应该标记为可删除。store 识别要按 backing Unit 找，不按 converted value 完全相等找。

### return rewrite

external float return：

- 新 call 的 LLVM return type 是 `float` / `double`。
- `replacementForOldCallUses()` 要能处理旧 call 是 void 的情况，因为当前 `pow` 旧 call 很可能是 `call void @pow(...)`。
- call 后 `notdec.register.summary_return.*` 或 register read 需要能映射到 typed return slot。

现有 return helper 机制主要按 register name 查 `shape.Returns`。typed slot 后仍然可以按 backing unit name / ABI name 查，但返回给 register SSA 的 value 需要是 backing register 类型或一个可组合的 low-lane value。

## 内部函数是否一起支持

第一版不建议支持 internal float signature rewrite。

原因：

- internal 函数的真实 ABI 可能被编译器改写，不一定遵守 external SysV 的 XMM 顺序。
- 当前 summary 的 `ReadEntry` / `ExitDemand` 能识别寄存器是否用到，但不能单独恢复 `float` vs `double`。
- 对 internal 直接引入 float 类型容易把类型恢复问题提前混进寄存器消除。

第一版只做 known external typed prototype。internal 仍保持当前整数寄存器策略。

## vararg 暂时不做

`printf`、`scanf` 这类 vararg 的浮点参数不要第一版支持。

原因：

- SysV vararg 涉及 `%al` 记录 vector register 参数数量。
- 源级 prototype 不能只从 fixed args 判断所有浮点参数。
- 当前目标残留来自 `pow` 这类 fixed libm 函数，不需要 vararg。

## 和 partial bit/lane demand 的关系

typed float signature rewrite 应该在 SummarySSA 里和现有流程配合，而不是另开旧链路 pass。

关系是：

- signature rewrite 把 `pow` 这类 call 改成 typed LLVM call。
- 参数/返回低 lane 通过 bitcast/trunc/zext 连接到 backing register value。
- partial bit/lane demand 继续负责删除不再被观察的 keep-high 旧值。

不要让 bit/lane demand 去猜 `double`。float 类型必须来自 known external prototype 或 ABI metadata。

## 阶段计划

### 阶段 1：补 ABI 测试，确认 Ghidra float slot 已解析

修改：

- `tests/native_abi_cspec_test.cpp`

目标：

- 验证 `x86-64-gcc.cspec` 解析结果包含：
  - input `XMM0_Qa`，`MetaType == "float"`，`MinSize == 4`，`MaxSize == 8`
  - output `XMM0_Qa`，`MetaType == "float"`
  - killedbycall `XMM0`

这一步不改行为，只把已有事实锁住。

### 阶段 2：引入 typed signature slot

修改：

- `lib/passes/summary/NativeRegisterSummarySSA.cpp`

目标：

- 把 `SignatureShape::Params/Returns` 从 `RegisterUnit*` 改成 slot 结构。
- 保持 integer slot 行为完全等价。
- 所有现有 summary SSA 测试仍通过。

风险：

- 这一步会碰到 signature rewrite 多个函数，必须小步做。

### 阶段 3：扩展 known external prototype

修改：

- `KnownExternalPrototype`
- `knownExternalPrototypes()`
- fixed arity 测试

目标：

- 支持 typed prototype。
- 先加 `pow(double,double)->double`。
- 暂时不改其他 libm。

### 阶段 4：实现 float 参数转换

修改：

- `functionTypeForShape()`
- `callArgStoreBindings()`
- call rewrite argument localization

目标：

- `pow` call operand 从 `ZMM0/ZMM1` low64 取出 `double`。
- 生成：

```llvm
call double @pow(double ..., double ...)
```

判断标准：

- 单测里构造 `store @ZMM0/@ZMM1; call @pow`，rewrite 后 call 参数是 `double`。

### 阶段 5：实现 float 返回转换

修改：

- return type construction
- call return mapping
- `replacementForOldCallUses()` / return helper mapping

目标：

- `pow` 返回 `double`。
- 后续对 `XMM0/ZMM0` low64 的读取能使用这个返回值。
- 不生成 `i512` 的 `pow` 返回。

判断标准：

- 单测里 call `pow` 后读取 `ZMM0` low64，能被替换为 `bitcast double ret to i64` 相关值。

### 阶段 6：真实目标验证

目标：

- 重新跑 `redis-cli`。
- `powerLawRand` 中 `pow` 不再是 `call void @pow(i64 0, ...)`。
- `redis-cli` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 检查 `ZMM3` 相关残留是否下降。

同时跑：

- `fortune`
- `wrk`
- `memcached`

确保已有整数和 partial demand 效果不退化。

## 判断标准

第一版完成时：

- `native_abi_cspec_test` 通过。
- `native_register_summary_ssa_test` 通过。
- `pow` 的 external declaration / call 被改成 `double(double,double)`。
- `redis-cli` 的 `powerLawRand` 不再用 `store @ZMM3` 表达 `pow` 参数/返回。
- `fortune`、`wrk`、`memcached`、`redis-cli` 全部通过 LLVM 22 verifier。
- 不支持 vararg 浮点参数，不支持 internal float signature rewrite，不算失败。

## 风险

### 1. backing register 是 ZMM，ABI slot 是 XMM

当前 lifting 把 `XMM/YMM/ZMM` 归一到最大 backing global，例如 `ZMM0`。typed slot 必须保存 ABI slot 的 bit range，不然会把 `double` 错当 `i512`。

### 2. call return 和 register SSA 的类型不一致

LLVM call 返回 `double`，register SSA 需要的是 backing register value。需要有明确转换边界，不能直接把 `double` RAUW 给 `i512` load。

### 3. vararg 浮点

暂时不做。强行支持容易引入错误 ABI。

### 4. internal 函数浮点类型

暂时不做。summary 能证明寄存器用没用，但不能证明是 `float` 还是 `double`。

### 5. long double / x87

x86-64 SysV 的 `long double` 常涉及 x87 / memory，不纳入第一版。

## 不做什么

第一版不做：

- Ghidra full datatype rule engine。
- HFA / vector aggregate。
- vararg 浮点参数。
- stack 浮点参数。
- internal function float prototype recovery。
- long double / x87。
- 旧 heritage 链路支持。

## 结论

Ghidra / Sleigh 的 ABI 信息已经覆盖浮点寄存器参数和返回值。当前 native summary 链路没有利用它，是因为 signature rewrite 的 shape 只能表达整个 backing register，不能表达 typed low-lane ABI slot。

下一步应该扩展 summary signature rewrite，让 known external prototype 可以声明 `double` / `float` 参数和返回。先从 `pow(double,double)->double` 做闭环，再逐步扩展到常见 libm 函数。

## 实现记录：known `pow(double,double)->double` 闭环

### 实现内容

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:63`：`AbiFacts` 新增 integer/float input/output slot 顺序，保留 cspec ABI 名、backing register 名、metatype、offset 和 size。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:178`：`KnownExternalPrototype` 新增 typed 参数和 typed 返回，第一版只落地 `I64` / `Float` / `Double`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:197`：新增 `NativeSignatureSlot`，让 signature shape 能表达 `XMM0_Qa -> ZMM0 low64 -> double`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:513`：为 `pow` 增加 known prototype：两个 `double` 参数，一个 `double` 返回。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:983`、`1000`：`collectAbiFacts()` 不再丢掉 `metatype=float`，而是放入 `FloatInputsInOrder` / `FloatOutputsInOrder`。internal summary 仍只使用整数 ABI register。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1096`、`1125`、`1242`、`1264`、`1285`：signature shape construction 改为 typed slot。known external 的 `double` 参数消耗 float ABI slot，整数参数仍走原整数 slot。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:2570`：`callArgStoreBindings()` 先读 backing register，再按 slot 转成 call operand。float slot 走 low-lane `trunc` + `bitcast`，原始 store 删除仍按 backing register 判断。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:2907`、`3048`：typed return helper 替换时把 `double` 返回转回 backing register 低 lane：`bitcast double -> i64` 后 `zext -> i512`。不把 `double` 直接 RAUW 给 i512 load。
- `tests/native_abi_cspec_test.cpp:27`、`91`：补 cspec 测试，确认 `XMM0_Qa` input/output 的 `MetaType == "float"`，并确认 `killedbycall XMM0` 被解析。
- `tests/native_register_summary_ssa_test.cpp:1973`、`2236`：补 `testKnownPowUsesFloatAbiSlots()`，构造 `ZMM0/ZMM1` 参数、旧 `void @pow()` call、后续 `ZMM0` 读取，验证 rewrite 后是 `call double @pow(double, double)`，且参数 ZMM store 被删。

### 验证

- `cmake --build build --target native_register_summary_ssa_test native_abi_cspec_test -j2`
- `./build/bin/native_abi_cspec_test /sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`
- `./build/bin/native_register_summary_ssa_test`
- `cmake --build build --target notdec-native-llvm notdec-native-discover -j2`
- `scripts/bench2-native-prototype-audit.sh --target redis:cli --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-float-pow`
  - 脚本最后因为 summary 链路 stderr 不再输出旧 prototype metric，报 `missing metric 'functions'`。
  - 但它已经生成并验证 `/tmp/notdec-bin2llvm-bench2-float-pow/redis-cli.signature-rewrite.ll`，对应 `llvm-as` 和 `opt -passes=verify` stderr 都是 0 字节。
- `/usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss=%M' build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /tmp/notdec-fortune-float-pow.summary.json -o /tmp/notdec-fortune-float-pow.ll`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-float-pow.ll -o /tmp/notdec-fortune-float-pow.bc`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-float-pow.bc -o /tmp/notdec-fortune-float-pow.opt.bc`
- `/usr/bin/time -f 'wrk elapsed=%e user=%U sys=%S maxrss=%M' build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk --all-confirmed --summary-json-out /tmp/notdec-wrk-float-pow.summary.json -o /tmp/notdec-wrk-float-pow.ll && /sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-wrk-float-pow.ll -o /tmp/notdec-wrk-float-pow.bc && /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-wrk-float-pow.bc -o /tmp/notdec-wrk-float-pow.opt.bc`
- `/usr/bin/time -f 'memcached elapsed=%e user=%U sys=%S maxrss=%M' build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached --all-confirmed --summary-json-out /tmp/notdec-memcached-float-pow.summary.json -o /tmp/notdec-memcached-float-pow.ll && /sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-memcached-float-pow.ll -o /tmp/notdec-memcached-float-pow.bc && /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-memcached-float-pow.bc -o /tmp/notdec-memcached-float-pow.opt.bc`

结果：

- `redis-cli` 的 `powerLawRand` 里 `pow` 已变成：
  - `declare double @pow(double, double)`
  - `call double @pow(double %1, double %0)`
  - `call double @pow(double %7, double 0x7FF8000000000000)`
- `powerLawRand` 里还剩一条 `store i512 0, ptr @ZMM3`，但已经不是 `pow` 参数或返回表达；`pow` 本身不再通过 ZMM register global 传参/返回。
- `fortune` 用时 `elapsed=8.70 user=8.67 sys=0.02 maxrss=169756`，通过 LLVM 22 verifier。之前同口径日志是 8.56 到 8.9 秒，未见明显退化。
- `wrk` 用时 `elapsed=46.47 user=46.42 sys=0.04 maxrss=181428`，通过 LLVM 22 verifier。之前同口径约 45.97 秒，未见明显退化。
- `memcached` 用时 `elapsed=120.15 user=119.96 sys=0.17 maxrss=245780`，通过 LLVM 22 verifier。之前同口径约 120.88 秒，未见明显退化。

### 暂不做

- 不支持 vararg 浮点参数。
- 不支持 internal float signature rewrite。
- 不补 long double / x87。
- 不把旧 heritage 链路接入这套 typed slot。
- 不把所有 libm 函数一次性加入 known prototype；第一版只闭 `pow`。

### 评分

- 实现效果：8/10。`pow(double,double)->double` 已闭环，redis-cli 的目标问题解决，Bench2 当前关注目标 verifier 通过。
- 复杂度：6/10。signature shape 从 register 指针扩成 typed slot，理解成本增加，但边界集中在 summary signature rewrite，没有扩散到 heritage。
- 后期维护成本：5/10。后续加 `sqrt/sin/cos/log/exp` 只需扩 known prototype；真正复杂的是 vararg、stack float、internal float，这次没有提前引入。

更好的方案是接入更完整的外部原型库和 Ghidra datatype rule engine，但当前目标只需要 fixed libm 的确定原型。先闭 `pow` 比一次性复刻完整 ABI 类型系统风险更低。

## 实现记录：扩展 fixed libm 和修复 Bench2 audit

### 实现内容

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:319`、`356`、`453`、`674`、`695`：known external prototype 表新增 `cos(double)->double`、`exp(double)->double`、`log(double)->double`、`sin(double)->double`、`sqrt(double)->double`。这些都走已有 typed float ABI slot，不新增 vararg、stack float 或 internal float 推断。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:3460`、`3494`：summary SSA 文本统计补 `calls_rewritten` 和 `functions_rewritten`，让脚本能检查 summary 链路自己的 rewrite 结果。
- `tests/native_register_summary_ssa_test.cpp:116`：新增 `attachTestFloatAbi()`，复用 `XMM*_Qa` 的 float ABI metadata。
- `tests/native_register_summary_ssa_test.cpp:2003`、`2064`、`2308`：`pow` 测试改用 helper，并新增 `testKnownUnaryLibmUsesFloatAbiSlots()`，覆盖 `sqrt/sin/cos/log/exp` 生成 `call double @name(double)`，且参数/返回不保留 ZMM store。
- `scripts/bench2-native-prototype-audit.sh:133`、`195`、`235`、`243`、`258`：audit 脚本打开 `--register-ssa-summary`，metrics 改为记录 summary SSA 的 `functions/loads/stores/calls_rewritten/functions_rewritten`，不再硬依赖旧 prototype recovery stderr 指标。

### `powerLawRand` 的 `ZMM3` 分类

`/tmp/notdec-bin2llvm-bench2-float-libm-audit/redis-cli.signature-rewrite.ll:14126` 仍有 `store i512 0, ptr @ZMM3`。它位于两次 `pow` 调用之间：

- `call double @pow(double %1, double %0)`
- `store i512 0, ptr @ZMM3`
- `call double @pow(double %7, double 0x7FF8000000000000)`

这条 store 的 metadata 是 `!notdec.register.access !400`，`!400 = !{!"base=ZMM3", !"space=register", !"offset=4800", !"size=64", !"name=ZMM3"}`。它是 `powerLawRand` 自身的 vector register 写入，不是 `pow` 参数或返回。`pow` 的声明和调用已经完全是 typed double。

### 验证

- `bash -n scripts/bench2-native-prototype-audit.sh`
- `cmake --build build --target native_register_summary_ssa_test native_abi_cspec_test notdec-native-llvm notdec-native-discover -j2`
- `./build/bin/native_register_summary_ssa_test`
- `./build/bin/native_abi_cspec_test /sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`
- `scripts/bench2-native-prototype-audit.sh --target redis:cli --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-float-libm-audit`
  - 通过，`all_confirmed=194s`，`signature_rewrite=192s`。
  - metrics：`summary_functions=537`，`summary_calls_rewritten=2883`，`summary_functions_rewritten=693`。
  - `redis-cli.signature-rewrite.llvm-as.stderr` 和 `redis-cli.signature-rewrite.opt.stderr` 都是 0 字节。
- `/usr/bin/time -f 'fortune elapsed=%e user=%U sys=%S maxrss=%M' build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /tmp/notdec-fortune-float-libm.summary.json -o /tmp/notdec-fortune-float-libm.ll && /sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-float-libm.ll -o /tmp/notdec-fortune-float-libm.bc && /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-float-libm.bc -o /tmp/notdec-fortune-float-libm.opt.bc`
  - `fortune elapsed=8.51 user=8.48 sys=0.03 maxrss=168124`，通过 verifier。上一轮同口径 `8.70s`，未见退化。
- `wrk elapsed=45.42 user=45.35 sys=0.06 maxrss=184536`，通过 LLVM 22 verifier。上一轮同口径 `46.47s`，未见退化。
- `memcached elapsed=118.15 user=118.03 sys=0.10 maxrss=242808`，通过 LLVM 22 verifier。上一轮同口径 `120.15s`，未见退化。
- `scripts/native-register-residue-audit.py /tmp/notdec-bin2llvm-bench2-float-libm-audit/redis-cli.signature-rewrite.ll`
  - 仍有 `vector store/access/full/full/no = 10`，其中 `powerLawRand` 的 `ZMM3` store 已分类为非 `pow` 参数/返回残留。
- `scripts/native-register-residue-audit.py /tmp/notdec-fortune-float-libm.ll`
  - 无残留行。

### 评分

- 实现效果：8/10。fixed unary libm 已接入 typed float ABI slot，redis audit 脚本恢复可用，当前 Bench2 关注目标 verifier 通过。
- 复杂度：5/10。新增原型复用已有 typed slot；脚本只是改统计来源。主要成本是 summary stderr 现在会更大。
- 后期维护成本：4/10。后续继续补 fixed external prototype 只需扩表和测试；真正复杂的 vararg float、stack float、internal float 仍未引入。
