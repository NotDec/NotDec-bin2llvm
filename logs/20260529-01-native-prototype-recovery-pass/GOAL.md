# /goal: Native Prototype Recovery Pass

## 原始用户需求

```text
最核心的任务是，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分，其次，规划一下如何做测试和验证。然后创建对应的子文件夹，指定具体的实现文件放到对应文件夹里面，同时把进度记录到PROGRESS.md里面。依然要求，根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## 目标

核心任务是逐步复刻 Ghidra prototype recovery 相关的数据结构和数据流判断，最终在 native 链路里形成一个可独立运行的参数和返回值恢复 Pass。

第一阶段不直接改 LLVM 函数签名。先把 ABI、函数寄存器效果、参数候选、返回候选用 metadata 标清楚。metadata 稳定后，再考虑把函数签名和 callsite 重写接上。

## 和当前 Register SSA 的关系

当前 `NativeRegisterSSA` 不是要推翻的部分，而是 prototype recovery 的数据流底座。它已经把“函数入口没有本函数定义但被使用的寄存器”标成 `notdec.register.external_input` / `notdec.register.external_inputs`，这和 Ghidra heritage 之后的 input varnode 概念能对应上。

路线要明确按 Ghidra 对应关系走：我们的 `NativeRegisterSSA` 对应 Ghidra 的 heritage SSA；后续 ABI、call effect、参数候选、返回值候选都按 Ghidra prototype engine 的数据结构继续复刻。不要把当前 LLVM 后处理方式理解成另一条独立路线。

Ghidra 的 prototype recovery 链路是：

- heritage 先在 P-Code 层给寄存器、栈、内存 varnode 建 SSA。
- 没有本函数定义的 varnode 变成 input varnode。
- call 点通过 `FuncCallSpecs::hasEffect(...)` 判断 `unaffected` / `killedbycall` / `unknown_effect`。
- prototype 阶段再用 `ParamActive` / `ParamTrial` 和 `ProtoModel` 规则筛出参数和返回值。

native 侧的映射关系是：

```text
Ghidra Heritage SSA
  -> NativeRegisterSSA
     - register varnode SSA -> register global load/store SSA
     - input varnode -> notdec.register.external_input
     - MULTIEQUAL -> LLVM PHI
     - killedbycall / unaffected guarding -> NativeCallEffectResolver + SSA clobber handling

Ghidra ProtoModel / FuncProto
  -> NativeAbiSpec / NativePrototypeModel
     - compiler spec prototype -> !notdec.abi
     - ParamEntry / ParamList -> NativeParamEntry / NativeParamList
     - hasEffect(...) -> NativeCallEffectResolver

Ghidra ParamActive / ParamTrial
  -> NativeParamActive / NativeParamTrial
     - input varnode trial -> external_input filtered by ABI storage
     - active trial -> value has real non-preserve use
     - used trial -> notdec.prototype.input_candidates

Ghidra output prototype recovery
  -> Native return candidate recovery
     - RETURN op input -> return block 前 ABI output register value
     - deriveOutputMap(...) -> notdec.prototype.return_candidates
```

需要注意的结合点：

- `external_input` 不能直接等于参数。保存恢复用的 `RBX/RBP/R12-R15` 也可能是 external input，必须先用 preserved 分析过滤。
- 当前 call barrier 太保守，后续要按 ABI 和 callee metadata 细分，否则会挡住 ABI preserved register 的传播。
- 返回值不从 LLVM 函数返回类型推断，第一版从 return block 前的 ABI output register SSA 值推断。
- 栈参数暂时不靠当前 SSA pass 解决。当前底座主要覆盖 register 参数和 register 返回值。

如果实现过程中发现 `NativeRegisterSSA` 缺少 Ghidra heritage 已经提供的信息，比如 call effect、return 前寄存器值、入口 input 的精确来源、PHI 合流或部分寄存器处理，就优先参考 Ghidra heritage 的做法补齐 SSA 底座。后续 prototype recovery 的其他部分也按这个原则调整：先找 Ghidra 对应数据结构和关键函数，再在 native 侧做最小可验证复刻。

## 工作规则

每次实现只能从某个阶段里选一小块做，必须按这个顺序：

1. 先在对应子目录写 markdown 规划文件。
2. 规划文件必须先介绍 Ghidra 中如何实现，明确源码文件和关键函数。
3. 再说明 native 侧如何复刻，优先跟着当前测试用例做最小可用部分。
4. 然后实现代码。
5. 实现完成后，把改动文件、函数、验证命令、结果和风险回写到同一个规划文件。
6. 同步更新本目录的 `PROGRESS.md`。

## 阶段划分

### 1. `01-cspec-abi-model`

复刻 Ghidra compiler spec / prototype XML 里的 ABI 事实。

主要数据结构：

- `NativeAbiSpec`
- `NativeAbiPrototype`
- `NativeAbiStorage`
- `NativeAbiEffect`

拟放源码：

- `include/notdec-bin2llvm/NativeAbi.h`
- `lib/NativeAbi.cpp`
- `lib/CMakeLists.txt`

目标：

- 复用 Ghidra C++ 的 `DocumentStorage` / `XmlDecode` 读取 `.cspec`。
- 解析 `stackpointer`、`returnaddress`、`default_proto`、`prototype`、`input`、`output`、`pentry`、`register`、`addr`、`unaffected`、`killedbycall`。
- 在 module 上写 `!notdec.abi` metadata。

### 2. `02-prototype-storage-model`

复刻 Ghidra `ParamEntry` / `ParamList` 的核心 storage 匹配能力。

主要数据结构：

- `NativeParamEntry`
- `NativeParamList`
- `NativeStorageMatch`
- `NativePrototypeModel`

拟放源码：

- `include/notdec-bin2llvm/NativePrototypeModel.h`
- `lib/NativePrototypeModel.cpp`

目标：

- 判断一个 register/stack storage 是否可能是 ABI input/output。
- 保留参数顺序、size 范围、metatype、stack 起点。
- 先不做完整 protorule 类型系统，只支持 Bench2 x86-64 SysV 需要的规则。

### 3. `03-function-register-effects`

扩展当前 register SSA 结果，复刻 Ghidra 对函数 preserved / killed register 的判断。

主要数据结构：

- `NativeFunctionRegisterEffects`
- `NativeRegisterPreserveResult`
- `NativeRegisterClobberResult`

拟放源码：

- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h`
- `lib/passes/NativeRegisterSSA.cpp`
- 后续如果变大，再拆到 `include/notdec-bin2llvm/passes/NativeRegisterEffects.h`
- 后续如果变大，再拆到 `lib/passes/NativeRegisterEffects.cpp`

目标：

- 标注 `notdec.register.clobbers`。
- 标注 `notdec.register.preserves`。
- 基于 SSA 判断 ABI unaffected register 是否在所有 return 前恢复为入口值。
- 不依赖 `push/pop` 字节模式作为正确性前提。

### 4. `04-callsite-effects`

复刻 Ghidra `FuncCallSpecs::hasEffect(...)` 的 callsite 副作用判断。

主要数据结构：

- `NativeCallEffectResolver`
- `NativeCallsiteEffects`
- `NativeCalleeSummary`

拟放源码：

- `include/notdec-bin2llvm/passes/NativeCallEffects.h`
- `lib/passes/NativeCallEffects.cpp`
- `lib/passes/NativeRegisterSSA.cpp`

目标：

- 本模块 direct call 使用 callee 的函数级 metadata 判断 clobber。
- 外部导入函数使用 ABI metadata 判断 clobber 和返回寄存器。
- 间接 call / 未解析 call 仍按 ABI caller-saved 保守处理。
- 支持两轮或小型 fixpoint，避免单轮过保守。

### 5. `05-input-output-candidates`

复刻 Ghidra `ParamActive` / `ParamTrial` 的候选参数和返回值筛选。

主要数据结构：

- `NativeParamTrial`
- `NativeParamActive`
- `NativePrototypeRecoverySummary`

拟放源码：

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h`
- `lib/passes/NativePrototypeRecovery.cpp`
- `tools/notdec-native-llvm.cpp`

目标：

- 从 `notdec.register.external_inputs` 推出 `notdec.prototype.input_candidates`。
- 从 return block 前的 ABI output register 推出 `notdec.prototype.return_candidates`。
- 过滤 saved register 噪声。
- 参数和返回值先只写 metadata，不改函数签名。

### 6. `06-pass-pipeline-and-verification`

把前面各块串成稳定 Pass，并建立 Bench2 验证流程。

主要数据结构：

- `NativePrototypeRecoveryOptions`
- `NativePrototypeRecoveryReport`

拟放源码：

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h`
- `lib/passes/NativePrototypeRecovery.cpp`
- `tools/notdec-native-llvm.cpp`
- `scripts/bench2-native-smoke.sh`
- `/sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py`

目标：

- `notdec-native-llvm` 默认跑 ABI metadata + LLVM canonicalization + register SSA + prototype recovery。
- 做 saved register、参数、返回值这类具体模式识别前，先跑一层保守的 LLVM canonicalization。第一候选是 `instcombine`，目的是把局部表达式、冗余 cast、简单等价值先规整，减少后续匹配分支。
- `.ll` / `.bc` 输入也能只跑 pass。
- Bench2 selected native 全量 `llvm-as` + `opt -passes=verify` 通过。
- 抽查真实函数，确认参数/返回候选没有明显误标。

`instcombine` 放进链路前要先做一小步验证：

- register access metadata 不能被误删到影响 `NativeRegisterSSA` 识别。
- 不能把寄存器 global load/store 优化成无法追踪的形态。
- pass 前后 `NativeRegisterSSA` 的 external input / replaced load 数量不能出现异常下降。
- 如果直接放在 `NativeRegisterSSA` 前风险偏大，就先放在 prototype recovery 的模式识别前，只对已经 SSA 化后的函数做 canonicalization。

## 测试和验证

### 单元测试

先加纯数据结构测试，不依赖 ELF：

- cspec 子集解析：用 x86-64 gcc cspec 的小片段验证 input/output/unaffected/killedbycall。
- storage 匹配：验证 `RDI` 是 input slot 0，`RAX` 是 output slot 0，`RBX` 是 unaffected。
- trial 筛选：验证 active 的 ABI input 会被标成候选，preserved-only register 不会被标成参数。

可放位置：

- `tests/native_abi_spec_test.cpp`
- `tests/native_prototype_model_test.cpp`
- `tests/native_prototype_recovery_test.cpp`

### IR 小样例测试

用手写 `.ll` 覆盖 Pass 行为：

- 无 call 的寄存器参数候选。
- direct call 后 ABI caller-saved 被 clobber。
- preserved register 写后恢复。
- 多 return path 返回寄存器一致 / 不一致。
- `instcombine` 前后 metadata 和参数/返回候选保持稳定。

可放位置：

- `tests/ir/native-prototype/*.ll`

### ELF smoke

沿用当前 smoke：

- `/bin/ls -a 0x6aa0 -l 1024`
- Bench2 `vsftpd`
- Bench2 `libuv`
- Bench2 `memcached`

判断：

- 输出有 `!notdec.abi`。
- 输出有函数级 clobber/preserve/input/return metadata。
- LLVM 22 `llvm-as` 和 `opt -passes=verify` 通过。

### Bench2 全量

使用：

- `/sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py`

判断：

- `selected-targets-native/summary.tsv` 全部 ok，或失败项有明确归类。
- 至少抽查 20 个函数。
- 对比 pass 前后运行时间，避免明显性能退化。
