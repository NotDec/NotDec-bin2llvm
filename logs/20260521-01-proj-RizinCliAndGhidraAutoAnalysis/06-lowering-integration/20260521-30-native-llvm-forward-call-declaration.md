# Native LLVM Forward Call Declaration

## 原始 prompt

```text
在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## 当前目标和已有 native 状态

上一小块已经让 `--all-confirmed` 里的 direct `CALL` 可以连到本 module 的 confirmed function。
但现在还有一个边界：如果函数 A 先 lower，并直接调用还没 lower 的函数 B，LLVM 会先创建
`B` 的 declaration；后面 lower `B` 的函数体时，`appendPcodeFunction(...)` 当前会把这个
已有 declaration 当成重复函数名直接失败。

这次只补这个边界：已有同名函数如果只是空 declaration，且类型匹配，就复用它并填 body。

## Ghidra 相关实现

Ghidra / heritage 路线在模块 lowering 时也先处理符号，再处理函数体：

- `lib/HeritageToLLVM.cpp::planModuleSymbols(...)`
  - 先为所有内部函数和外部函数规划稳定 symbol。
- `lib/HeritageToLLVM.cpp::declareInternalFunction(...)`
  - 先创建内部函数 declaration。
- `lib/HeritageToLLVM.cpp::buildHeritageModuleWithBodies(...)`
  - 先声明所有函数，再逐个 lower body。
- `lib/HeritageToLLVM.cpp::lowerCall(...)`
  - `CALL` 用模块符号表解析目标，再发 LLVM call。

这次 native 侧不照搬完整 heritage module builder，只补同一个关键点：call 先创建的 declaration
不能阻止后面填函数体。

## native 侧复刻策略

1. `appendPcodeFunction(...)` 先看 module 里是否已有同名函数。
2. 如果不存在，就按现在逻辑新建函数。
3. 如果存在并且没有 body、类型也是 `void ()`，就复用这个 declaration。
4. 如果存在且已经有 body，仍然报 duplicate。
5. 如果存在但类型不匹配，报类型不匹配。

暂时不做：

- 不把 `--all-confirmed` 改成先显式声明所有函数。
- 不做参数/返回值类型恢复。
- 不改变 helper call。

## 判断标准

1. 已有 declaration 可以被后续 `appendPcodeFunction(...)` 填成 definition。
2. 已有 definition 仍然不能被覆盖。
3. Bench2 smoke 继续通过。

## 风险

1. 复用 declaration 时必须确认没有 body，否则会覆盖已有函数。
2. 目前只支持 `void ()`，后续有函数签名后要扩展类型匹配逻辑。

## 实现记录

### 修改文件和函数

1. `lib/PcodeToLLVM.cpp:750`
   - 修改 `appendPcodeFunction(...)`。
   - 如果同名函数不存在，保持原逻辑新建函数。
   - 如果同名函数已存在但没有 body，并且类型是当前 native lowering 使用的 `void ()`，复用这个 declaration 并继续填 body。
   - 如果同名函数已有 body，仍报 `duplicate function name`。
   - 如果 declaration 类型不匹配，报 `function declaration type mismatch`。
2. `ARCHITECTURE.md:111`
   - 记录 forward call 创建的空 declaration 可以在后续 lowering 中补成函数体。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:33`
   - 更新 Stage 6 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
c++ -std=c++17 -I/sn640/NotDec/external/NotDec-bin2llvm/include \
  $(/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-config --cxxflags) \
  /tmp/notdec-forward-call-test.cpp \
  /tmp/notdec-bin2llvm-build/lib/libnotdec-bin2llvm-core.a \
  $(/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-config --ldflags --system-libs --libs core) \
  -o /tmp/notdec-forward-call-test
LD_LIBRARY_PATH=/sn640/NotDec/llvm-22.1.0.obj/lib /tmp/notdec-forward-call-test
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-30
```

### 最小复现

`/tmp/notdec-forward-call-test.cpp` 构造两个函数：

1. `caller` 先 lower，里面 direct `CALL` 到 `0x2000`，并通过 `DirectCallTargets` 解析成 `callee`。
2. `callee` 后 lower。

验证结果：`caller` 创建的 `callee` declaration 被后续 `appendPcodeFunction(...)` 填成 body，
整个 module 通过 LLVM verify。

### Bench2 结果

三个目标都通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as`、LLVM 22 `opt -passes=verify`。

```text
vsftpd ok elapsed=7s
libuv ok elapsed=8s
memcached ok elapsed=8s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=31, instructions=80, xrefs.total=297, data=149, string=139
libuv: confirmed_functions=10, basic_blocks=32, instructions=93, xrefs.total=24, data=13, string=0
memcached: confirmed_functions=9, basic_blocks=31, instructions=80, xrefs.total=179, data=68, string=102
```

IR 里 direct call 仍然存在：

```text
vsftpd: call void @notdec_native_5ba0(), call void @notdec_native_8290()
libuv: call void @notdec_native_9d80(), call void @notdec_native_9350()
memcached: call void @notdec_native_5b80(), call void @notdec_native_b950()
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只在 `appendPcodeFunction(...)` 增加 declaration 复用判断。Bench2 三目标 smoke 总耗时约 23 秒，
和上一轮同口径接近。

实现效果：4/5。补上了 forward direct call 的必要边界。
复杂度：1/5。只改一个函数的创建逻辑。
维护成本：1/5。后续有真实函数签名时，只需要扩展类型匹配。
