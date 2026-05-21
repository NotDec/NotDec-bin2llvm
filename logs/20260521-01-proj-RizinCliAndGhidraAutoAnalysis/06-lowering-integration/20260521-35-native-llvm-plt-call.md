# Native LLVM PLT Call Lowering

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

native discovery 已经能识别 ELF `.plt` stub，并且 direct `CALL` 命中 PLT stub 时不会再把 stub
当内部函数种子。当前 libuv 里 `0x2153a -> 0x9350` 已被标成
`sleigh-pcode-plt-call`，`--plt-json` 也能查到 `0x9350` 对应 `pthread_key_delete`。

但 `notdec-native-llvm --all-confirmed` 还不知道这张表，direct `CALL` 到 PLT stub 仍会降级成
`notdec_pcode_CALL_void(i64 stub)` helper。这次目标只补这个缺口：已知 PLT stub 直接 lower 成
外部 LLVM 函数调用。

## Ghidra 相关实现

Ghidra / heritage 路线先把外部符号纳入模块符号计划，再 lower call：

- `ghidra_scripts/ExportHeritageModule.java::rememberExternal(...)`
  - 记录外部地址和外部符号名。
- `ghidra_scripts/ExportHeritageModule.java::writeExternals(...)`
  - 导出外部函数信息。
- `ghidra_scripts/ExportHeritageModule.java::directCallTargetName(...)`
  - direct call 目标如果能解析到函数名，就把符号名写入 JSON。
- `lib/HeritageToLLVM.cpp::planModuleSymbols(...)`
  - 先规划内部函数和外部函数 symbol。
- `lib/HeritageToLLVM.cpp::lowerCall(...)`
  - direct `CALL` 通过符号表 lower 成 LLVM call；解析不了的才走保守路径。

## native 侧复刻策略

1. 在 `PcodeLoweringConfig` 里增加一张 PLT stub 地址到外部函数名的表。
2. `notdec-native-llvm --all-confirmed` 从 `NativeProgramState::pltEntries()` 生成这张表。
3. 生成外部函数名时复用当前 LLVM name sanitizer 和唯一名逻辑，避免和内部函数重名。
4. `PcodeLowerer::lowerCall(...)` 遇到无返回值 direct `CALL` 时，先查外部 PLT 表；命中就发
   `call void @symbol()`。
5. 没命中时保持现有逻辑：查内部 confirmed function；再不行继续走 helper。

暂时不做：

- 不恢复外部函数参数和返回值。
- 不处理 indirect call / `CALLIND`。
- 不把 PLT stub 本身生成成 wrapper 函数。
- 不改现有 xref JSON 格式。

## 判断标准

1. libuv 里 `CALL 0x9350` lower 成 `pthread_key_delete` 的外部 LLVM call。
2. 内部 direct call 仍继续 lower 成 `notdec_native_*`。
3. 未匹配的 call 仍保留 helper，不静默丢失。
4. Bench2 三个 smoke 目标继续通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。

## 风险

1. 外部符号名可能和内部函数名重名，所以必须用同一套 `usedNames` 去重。
2. 当前统一按 `void ()` 声明外部函数，只适合当前无原型 native lowering；后续要接 ABI/原型恢复。
3. 如果某个 PLT stub 也被误收成内部函数，外部表应该优先，避免把动态链接调用伪装成内部函数。

## 实现记录

### 修改文件和函数

1. `include/notdec-bin2llvm/PcodeToLLVM.h:26`
   - 在 `PcodeLoweringConfig` 增加 `ExternalCallTargets`。
   - 这张表专门记录 PLT stub 地址到外部 LLVM symbol 的映射，和内部 `DirectCallTargets` 分开。
2. `lib/PcodeToLLVM.cpp:643`
   - 修改 `PcodeLowerer::lowerCall(...)`。
   - 无返回值 direct `CALL` 先查 `ExternalCallTargets`，命中时生成 `call void @external()`。
   - 没命中时继续查内部 `DirectCallTargets`，再不行走原 helper。
3. `tools/notdec-native-llvm.cpp:290`
   - 新增 `externalFunctionLlvmName(...)`，复用 sanitizer 和 `uniqueFunctionName(...)`。
4. `tools/notdec-native-llvm.cpp:330`
   - 修改 `buildConfirmedModule(...)`，从 `state.pltEntries()` 建立 `externalNamesByStub`。
5. `tools/notdec-native-llvm.cpp:362`
   - 把 `externalNamesByStub` 写入每个函数的 `PcodeLoweringConfig`。
6. `ARCHITECTURE.md:115`
   - 记录 `--all-confirmed` 对已知 PLT stub 的 direct `CALL` 会 lower 成外部 LLVM call。
7. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:37`
   - 更新 Stage 6 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-35
rg -n "pthread_key_delete|notdec_pcode_CALL_void|call void @notdec_native" \
  /tmp/notdec-bin2llvm-bench2-smoke-20260521-35/libuv.all-confirmed.ll
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --plt-json \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  | python3 -m json.tool > /tmp/notdec-libuv-plt-20260521-35.pretty.json
rg -n '"stub": "0x9350"|"symbol": "pthread_key_delete"|"count"' \
  /tmp/notdec-libuv-plt-20260521-35.pretty.json
```

### Bench2 结果

三个目标都通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as`、LLVM 22 `opt -passes=verify`。

```text
vsftpd ok elapsed=7s
libuv ok elapsed=8s
memcached ok elapsed=7s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=297, unresolved_indirect_flows=7
libuv: confirmed_functions=9, basic_blocks=29, instructions=85, xrefs.total=24, unresolved_indirect_flows=2
memcached: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=179, unresolved_indirect_flows=7
```

libuv IR 关键结果：

```text
call void @notdec_native_9d80()
call void @pthread_key_delete()
declare void @pthread_key_delete()
call void (...) @notdec_pcode_CALL_void(i64 36288)
```

`0x9350` 已经从 helper call 变成外部函数调用。另一个 `36288 == 0x8dc0` 仍未匹配，
继续保留 helper。`--plt-json` 仍显示 libuv PLT count 为 215，并包含
`stub=0x9350, symbol=pthread_key_delete`。

vsftpd / memcached 的内部 direct call 仍在：

```text
vsftpd: call void @notdec_native_5ba0(), call void @notdec_native_8290()
memcached: call void @notdec_native_5b80(), call void @notdec_native_b950()
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只在 `--all-confirmed` 构建阶段多生成一张 PLT stub 名字表，并在每个 direct `CALL`
lowering 时多查一次 hash map。Bench2 三目标 smoke 总耗时约 22 秒，和上一轮同口径接近。

实现效果：4/5。libuv 里已知 PLT direct call 现在能落成外部 LLVM call。
复杂度：1/5。只增加一张配置表和一处分支。
维护成本：2/5。后续接 ABI/函数原型时，需要把当前 `void ()` 外部声明扩展成真实签名。
