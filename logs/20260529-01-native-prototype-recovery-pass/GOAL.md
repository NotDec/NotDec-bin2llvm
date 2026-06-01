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

如果当前工作只是 Bench2 数据集测试、skip reason 归类、真实函数抽查、收敛判断或目标文档调整，而不是复刻新的 Ghidra 模块/数据结构，可以不新建功能复刻 plan。但仍要把结论和进度记到 `PROGRESS.md` 或对应审计日志里。

## 大块任务识别规则

后续每轮不要直接从一个单独 CFG 变体开始。先看 Bench2 当前 skip reason 和真实函数样本，再按 Ghidra 数据结构和 Bench2 blocker 识别 2-5 个候选大块任务。

大块任务是能推进主线能力的一类问题，通常满足至少一条：

- 对应 Ghidra 一个明确数据结构或算法，例如 `FuncCallSpecs`、`ParamActive`、`ParamTrial`、`FuncProto`、call effect、return output map。
- 能减少 Bench2 中一类真实 blocker，例如 `unsafe callsite input value`、`unsafe callsite return load`、`function has uses`。
- 能替换当前分散 helper，让 pass 更接近稳定结构，例如统一 register current-value 查询、统一 callsite rewrite plan。
- 能提高语义正确性，而不只是让 LLVM verifier 通过。
- 能把一类 skip reason 拆细、统计清楚，帮助判断后续是否继续做。

每轮 plan 或审计日志里应先写：

- 候选大块任务；
- Ghidra 对应源码文件和关键函数；
- native 当前缺口；
- 影响哪些 Bench2 skip reason 或真实样本；
- 做完后怎么判断收敛；
- 本轮为什么选这一块。

只有大块任务确定后，才从里面切可验证的小步。小步服务于大块任务，不能自己变成主线。

## 实现粒度和提交规则

每次任务和 commit 不应默认以“一个极小 CFG 变体”为粒度。同一类语义问题应合并成一个小阶段处理，同类问题尽量多修几个再合并提交。

推荐粒度：

- 新增或修改一个明确能力，例如 callsite input 当前值查询、return load 查找、multi-return binding、call effect 判断。
- 解决或拆清一类 skip reason。
- 覆盖同一策略下 2-4 个代表性 CFG 形状，包含正例和负例。
- 对 Bench2 一个真实 blocker 做定位、实现、验证，并记录剩余 blocker 分类。

不推荐单独作为一次任务：

- 一个 shared successor / multi-return / input 数量排列组合。
- 一个已有逻辑理论上已经支持的测试变体。
- 一个只增加断言但不改变能力、不减少真实 blocker 的测试。
- 一个没有 Bench2 blocker、也没有明确语义风险的小 CFG 形状。

如果确实要做很小的回归测试，必须说明它属于哪个大块任务，以及为什么不和同类测试合并。

## 阶段停止标准

当前 native prototype recovery 不以“覆盖所有可能 CFG 形状”为停止标准，而以 Bench2 和语义风险收敛为标准。

第 6 阶段阶段性完成需要满足：

1. Bench2 selected 目标稳定完成，生成 `.ll` / `.bc`，并通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
2. signature rewrite 的 skip reason 已分类，至少包括 `declaration`、`already matches`、`missing recovered prototype`、`unsafe callsite input value`、`unsafe callsite return load`、`function has uses`。
3. 非合理 skip reason 有处理结论：能安全实现的按一类问题实现；暂不做的写明原因，例如 indirect call、栈参数、复杂 alias、已有函数指针 use。
4. 对 vsftpd、libuv、memcached 的 rewritten 函数做真实样本抽查，确认参数顺序、返回值和 callsite 替换语义合理。
5. 后续不再追逐零散 CFG 组合。只有 Bench2 暴露新 blocker，或已有能力存在明确回归风险时，才继续补小测试。

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
