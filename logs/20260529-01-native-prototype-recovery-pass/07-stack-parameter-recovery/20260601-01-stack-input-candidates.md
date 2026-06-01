# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，当前 register 参数、register 返回值和 direct callsite signature rewrite 已经在现有 Bench2 真实样例上阶段性收敛。后续不再把扩大audit 覆盖面当作主线，也不要求继续把所有大目标全量跑完。

之后的重点改为：

1. 用已经选定的真实样例做回归验证，防止已有能力退化。只有改了生产代码、出现新 blocker，或连续新增 1-2 批真实样例暴露问题时，才继续扩大 audit。
2. audit 以自动 gate 为主：跑 all-confirmed / signature-rewrite，确认 LLVM 22 assemble/verify 通过，检查 skip reason 只剩合理类别；只抽查少量代表函数的 IR 转换前后语义，不再大量手工看IR。
3. 下一阶段参考 Ghidra 对 stack parameter 的处理，规划并实现native 侧第一版栈上传参恢复。暂不处理复杂alias、varargs、动态栈调整和完整类型恢复。

实现新功能或数据结构时，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

另外，注意commit不要过于冗余，同类问题多修复一些再合并commit。连续的audit commit可以合并。
```

# 背景

register 参数、register 返回值和 direct callsite signature rewrite 已经阶段性收敛。下一步不继续扩大 audit，而是补 native 第一版 stack 参数候选。

当前已有两块基础：

- `NativePrototypeModel::findInputStack()` 已能按 cspec `pentry` 匹配 stack storage。
- `HeritageToLLVM` 会把 Ghidra stack varnode 放进函数入口的 `notdec_stack` alloca，并通过常量 GEP + load 读取。之前主要覆盖负 offset local stack，本轮需要把正 offset address-tied input 也纳入。

缺口是 `NativePrototypeRecovery` 只从 `notdec.register.external_inputs` 生成参数候选，没有读取 stack varnode 的候选。

# Ghidra 对应实现

Ghidra 的相关数据结构在：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamEntry`：描述一个 ABI 参数存储范围，包含 address space、base、size、minsize、alignment、slot 规则。
  - `ParamTrial`：描述一个候选参数，保存地址、大小、slot、匹配到的 `ParamEntry` 和 active/used 状态。
  - `ParamActive`：保存当前函数或 callsite 的 `ParamTrial` 集合。
  - `ParamList` / `ParamListStandard`：提供 `possibleParam()`、`possibleParamWithSlot()`、`unjustifiedContainer()`。
  - `FuncProto::deriveInputMap()`：把 active trials 按 ABI model 变成最终输入参数。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandard::possibleParamWithSlot()`：通过 `findEntry()` 判断某个 storage 是否属于 ABI 参数范围，并返回 slot。
  - `FuncCallSpecs::checkInputTrialUse()`：对 callsite 输入 trial 做 active 判断。对 stack spacebase，会用 alias/local range/ancestor use 判断；复杂 alias 会保守。
  - `FuncCallSpecs::buildInputFromTrials()`：把 used 的 trial 写回 call 输入；stack 参数会按 stackoffset 从 spacebase 翻译到调用者栈。

Ghidra 的关键点是：stack 参数不是特殊类型，而是 `ParamEntry` 能匹配到的 storage trial。alias、局部变量范围、callee pop、ancestor use 会参与后续 active 判断。

# native 复刻策略

本轮只做第一版候选，不做 signature rewrite。

1. 保留现有 register 参数路径。
2. 给 recovered/input metadata 补 storage kind，默认兼容旧的 `name=RDI` register metadata。
3. 在 `HeritageToLLVM` 里给 address-tied stack input load 标 `notdec.stack.input` metadata。
4. 扫描 `notdec_stack` 上的简单 load：
   - load 指针必须是常量 GEP；
   - GEP base 必须来自当前函数入口的 `notdec_stack` alloca；
   - Ghidra stack offset 从 `notdec.stack.input` metadata 读取；
   - 只接受 active use，也就是 load 有真实 use；
   - 用 `NativePrototypeModel::findInputStack("stack", offset, size)` 匹配 ABI slot。
5. stack trial 写入 `notdec.prototype.input_candidates` 和 `notdec.prototype.recovered`，形式使用 `storage=stack`、`space=stack`、`offset=N`。
6. 现有 signature rewrite 只支持 register storage，遇到 stack 输入时明确返回 unsupported，不做误 rewrite。

第一版暂不做：

- 非 address-tied input 的正 offset stack varnode。
- alias 分析。
- varargs。
- 动态栈调整。
- 栈对象类型恢复。

# 判断标准

- 单测里简单 stack load 会产生 input candidate 和 recovered prototype。
- 未使用的 stack load 不产生候选。
- 有 stack 输入时不会被现有 register signature rewrite 当成可重写函数。
- 现有 register 参数、返回值、direct callsite rewrite 单测不退化。

# 实现记录

## 改动文件和函数

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:31`
  - `NativeParamTrial` 新增 `StorageKind`、`StackSpace`、`StackOffset`、`Size`。
  - 设计意图：沿用现有 `ParamActive` 排序和去重路径，但把 stack trial 和 register trial 明确分开，避免后续 rewrite 误把栈参数当寄存器参数。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:62`
  - `NativeRecoveredPrototypeParam` 新增同样的 storage 字段。
  - 设计意图：`notdec.prototype.recovered` metadata 能稳定表达 stack storage，同时旧的无 `storage=` metadata 仍按 register 读回。
- `lib/HeritageToLLVM.cpp:492`
  - `createStackFrame()` 开始纳入正 offset 的 address-tied input stack varnode。
  - `readAddressTiedInput()` 在 stack load 上写 `notdec.stack.input`，字段为 `space=stack`、`offset=N`、`size=N`。
- `lib/passes/NativePrototypeRecovery.cpp:50`
  - `inputCandidateMetadata()` 和 `recoveredParamListMetadata()` 写出 `storage=register/stack`。
- `lib/passes/NativePrototypeRecovery.cpp:252`
  - 新增 `stackInputTrials()`，扫描当前函数 `notdec_stack` 上带 `notdec.stack.input` 的 active load，用 `NativePrototypeModel::findInputStack()` 匹配 ABI input slot。
- `lib/passes/NativePrototypeRecovery.cpp:1772`
  - `readRecoveredParamList()` 支持读回 stack storage，并继续校验 slot 顺序、重复 register name、重复 stack storage。
- `lib/passes/NativePrototypeRecovery.cpp:2056`
  - `buildNativeRecoveredPrototypeFunctionType()` 遇到非 register input/return 返回 `nullopt`，让现有 signature rewrite 明确保守跳过。
- `tests/native_prototype_recovery_test.cpp:59`
  - 测试 ABI 增加 `inputStack()` 和 stack input pentry。
- `tests/native_prototype_recovery_test.cpp:1359`
  - 新增 `createStackInputFunction()`，覆盖 used / unused stack load。
- `tests/native_prototype_recovery_test.cpp:2528`
  - 更新 summary 计数，并验证 stack input candidate、recovered stack metadata、stack prototype 不进入 register rewrite。

## 验证

```bash
cmake --build build --target native_prototype_recovery_test notdec-native-llvm -j2
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)' --output-on-failure
ctest --test-dir build -R 'signature_rewrite|native_llvm\.cli' --output-on-failure
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-stack-param-regression-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
```

结果：

- `native_prototype_recovery_test` 编译通过。
- 全量 `ctest` 9/9 通过。
- CLI signature rewrite smoke 通过。
- Bench2 固定回归 gate 通过，LLVM 22 `llvm-as` / `opt -passes=verify` stderr 全部 0 字节。

Bench2 回归耗时和 skip reason：

| target | all-confirmed | signature-rewrite | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `vsftpd:executable` | 41s | 41s | 139 | 139 | 97 | `already matches=48`, `declaration=49` |
| `libuv:shared-library` | 106s | 107s | 338 | 338 | 233 | `already matches=147`, `declaration=86` |
| `memcached:executable` | 59s | 60s | 188 | 188 | 127 | `already matches=71`, `declaration=56` |

性能判断：本轮只在 prototype recovery 阶段多扫函数内 stack load，固定 Bench2 回归耗时仍和之前同口径在同一量级；没有出现新的非合理 skip reason。

## 风险和后续

- 第一版只恢复 stack input candidate metadata，不做 signature rewrite。
- 当前依赖 `HeritageToLLVM` 的 `notdec.stack.input` metadata，不从任意 GEP 推断 stack 参数。
- 暂不处理 alias、varargs、动态栈调整、栈对象类型恢复。

评分：

- 实现效果：7/10。能表达和读回第一版 stack 参数候选，且不破坏 register rewrite。
- 复杂度：6/10。给 recovered metadata 增加 storage kind，会增加后续读写约束，但这是支持 stack 参数必须补的结构。
- 维护成本：6/10。当前路径保守，后续真正做 stack rewrite 时需要扩展 `NativePrototypeInputBinding`，但不会推翻本轮 metadata 形状。
