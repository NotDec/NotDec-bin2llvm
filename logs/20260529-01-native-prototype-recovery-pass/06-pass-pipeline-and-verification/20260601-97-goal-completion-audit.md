# 97. Goal 完成度审计

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 95 步确认第 6 阶段 direct signature rewrite 在当前 Bench2 样本上阶段性收敛。第 96 步进一步分类了空 prototype。当前需要区分两件事：

- 第 6 阶段是否阶段性完成；
- 整个 native prototype recovery 长期 Goal 是否可以标完成。

本轮只做完成度审计，不实现代码。

## 显式要求对照

| 要求 | 当前证据 | 结论 |
| --- | --- | --- |
| 阅读并遵守 `GOAL.md` | 后续日志都引用 `GOAL.md`；第 90-97 步按 Bench2 skip reason 和真实样本推进 | 满足 |
| 按 Ghidra 数据结构划分工作 | `GOAL.md` 分为 ABI、storage、register effects、callsite effects、candidate recovery、pipeline/verification 六块 | 满足 |
| 创建对应子文件夹 | `01-cspec-abi-model` 到 `06-pass-pipeline-and-verification` 均已存在 | 满足 |
| 指定实现文件 | `GOAL.md` 各阶段列出拟放源码；实现落在 `NativeAbi`、`NativePrototypeModel`、`NativeRegisterSSA`、`NativePrototypeRecovery`、CLI 和 smoke 脚本 | 满足 |
| 实现前写规划，并写 Ghidra 源码/关键函数 | 151 个相关 markdown 记录；功能实现类日志按阶段放置并包含 Ghidra 对应关系 | 基本满足 |
| 实现后回写过程 | `PROGRESS.md` 和阶段日志记录改动文件、函数、验证方式 | 满足 |
| 数据集审计可不走功能复刻流程 | 第 90-97 步都是数据集/收敛审计，记录在 06 目录和 `PROGRESS.md` | 满足 |
| 每轮先看 Bench2 skip reason / 真实样本 | 第 90 步之后都先基于 Bench2 输出和样本审计 | 满足 |
| 先识别大块能力，再切小步 | 第 90-97 步日志都列出候选大块任务和本轮选择 | 满足 |
| 不再以极小 CFG 变体为默认粒度 | 第 90 步后没有继续追加 CFG 排列组合实现 | 满足 |

## 代码能力对照

| Ghidra 对应块 | native 证据 | 当前状态 |
| --- | --- | --- |
| compiler spec / `ProtoModel` ABI 事实 | `NativeAbiSpec`、`NativeAbiStorage`、`NativeAbiEffect`、`!notdec.abi` | Bench2 x86-64 SysV 子集可用 |
| `ParamEntry` / `ParamList` storage 匹配 | `NativePrototypeModel`、register/stack storage tests | register input/output 和基础 stack matching 可用 |
| heritage SSA / external input | `NativeRegisterSSA`、`notdec.register.external_input(s)` | 作为 prototype recovery 底座可用 |
| preserved / clobbered register effect | `notdec.register.preserves`、`notdec.register.clobbers` | direct callee metadata + ABI fallback 可用 |
| `FuncCallSpecs::hasEffect(...)` | call clobber 查询、intrinsic 不 clobber、declaration 回落 ABI unaffected | 保守可用，未做完整外部 prototype database |
| `ParamActive` / `ParamTrial` | `notdec.prototype.input_candidates`、`return_candidates` | register 参数/返回候选可用 |
| recovered `FuncProto` | `notdec.prototype.recovered`、metadata readback、FunctionType 构造 | register prototype 可用 |
| 函数签名 / callsite rewrite | `--rewrite-prototype-signatures`、direct callsite rewrite、多返回 struct return | 当前 Bench2 direct call 样本收敛 |

## 测试和验证对照

| 验证层 | 证据 | 当前状态 |
| --- | --- | --- |
| 单元测试 | `native_abi_cspec_test.cpp`、`native_prototype_model_test.cpp`、`native_register_effects_test.cpp`、`native_prototype_recovery_test.cpp` | 已覆盖核心数据结构和 IR 形状 |
| instcombine 安全 | `native_instcombine_metadata_test.cpp` | 已覆盖 register metadata、call barrier、prototype candidates |
| CLI smoke | `native-llvm-cli-signature-rewrite-smoke.sh`、`cli-signature-rewrite.ll` | 覆盖 `.ll` / `.bc` 和多种签名形状 |
| Bench2 smoke | `/tmp/notdec-bin2llvm-bench2-multi-input-multi-return-partial-shared-smoke` | vsftpd/libuv/memcached 有当前证据 |
| LLVM 22 verify | 第 95 步记录 signature rewrite `llvm-as` / `opt` stderr 为 0 | 当前代码在该 smoke 上通过 |
| 真实样本抽查 | 第 91-96 步 | 已覆盖 rewritten 函数、空 prototype、multi-return、declaration |

## 不能标长期完成的原因

当前不能把整个 Goal 标成完成，原因是 Goal 的长期目标是“不断复刻 Ghidra 对应数据结构，最终实现参数和返回值恢复的完善 Pass”。当前证据只证明当前 Bench2 阶段的 register 参数/返回和 direct signature rewrite 收敛，不能证明完整 Ghidra prototype recovery 已实现。

明确未覆盖或只保守处理的部分：

- 栈参数暂时不靠当前 SSA pass 完整解决。
- 完整 `protorule` / 类型系统没有复刻。
- 外部函数 prototype database 没有做，declaration call effect 只按 ABI fallback。
- 间接 call / 未解析 call 仍保守处理。
- multi-return 的真实调用方消费返回分量，在当前 Bench2 样本里没有真实 `extractvalue { i64, i64 }` 证据。
- 当前 Bench2 只覆盖 vsftpd、libuv、memcached selected，不等于所有真实项目全量语义正确。

## 当前可确认的完成边界

可以确认：

- 阶段划分、目录、规划、进度记录已经建立。
- Ghidra 对应的核心 native 数据结构已经有可运行子集。
- 第 6 阶段 direct signature rewrite 在当前 Bench2 样本上阶段性收敛。
- 后续不应该继续追第 6 阶段的零散 CFG 组合。

不能确认：

- 整个 native prototype recovery 已经达到 Ghidra 完整能力。
- 所有 Bench2 / 真实项目都语义正确。
- 栈参数、复杂 alias、外部 prototype、间接调用已经完善。

## 后续建议

如果继续推进，建议不要再从第 6 阶段 direct call rewrite 里切小步。更合理的大块任务是：

1. 扩大 Bench2 selected 范围或重新跑全量 smoke，寻找新的真实 blocker。
2. 若出现栈参数相关 blocker，再进入 `ParamEntry` / stack storage 和 SSA memory 相关能力。
3. 若出现外部 call effect 造成的候选误判，再复刻更细的 `FuncCallSpecs::hasEffect(...)`。
4. 若出现真实 multi-return 消费样本，再补 callsite 返回分量语义验证。

因此，本轮不调用 goal complete。当前状态是：第 6 阶段阶段性完成，长期 Goal 继续保持 active。
