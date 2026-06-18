# Work guidelines

## 0. 语言风格

即使是在说话和思考的时候，也要保持简洁，不造抽象层次的风格。**这一点非常重要，必须从头到尾始终贯彻，即使是在自己思考的过程中**
1. 说白话，使用更简洁务实的说法，不要过度抽象，不要引入自己造的名词，不要用新术语把问题重新命名。
2. 不要在特别简单的，比如命名，比如用户已经意识到的，或者肯定知道的问题上大费笔墨，而是思考那些真正关键的地方，真正和当前事情相关，更重要的地方。

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

# 项目规范

2. 代码一定要多写注释，特别是新引入的数据结构前，说明背后的设计理念。
3. （非Goal模式下的）工作流程：收到需求 -> 思考后告诉用户打算怎么更改 -> 讨论一致后再开始实现。复杂代码修改实现完后再写文档到 `logs/`；简单文档修改、注释修改、错别字修正不需要写日志。
   实现完计划并完成验证后，默认必须直接提交，不要停在未提交状态；涉及 submodule 时，先在 submodule 内提交，再提交顶层指针和日志。
4. 写修改日志时，必须明确指出修改了哪个文件的哪一行，涉及哪些函数。
5. 尽量复用并改进之前的日志，最好每个功能都单独一个日志。
6. plan日志重点写问题背景、目标、期望效果、大致技术路线、风险和判断标准，要让没有上下文的人也能看懂；不要过早写成具体实现清单、命令清单或行号清单。实现记录才需要明确写修改了哪个文件的哪一行、涉及哪些函数、验证命令和性能结果。只有复杂代码修改需要从实现效果、复杂度（增加其他人对项目的理解成本）、后期维护成本三个角度评分，并思考有没有更好的方案。
   `logs/` 下的 plan 文档顶部必须先保留本次用户的原始 prompt，然后再写背景、目标、路线、风险和判断标准。
7. 如果当前的任务是对之前的plan日志的实现，则不需要单独创建日志，而是将实现情况写入之前的计划日志，比如将计划的步骤在标题中标记为已完成，记录实现细节，以及调整计划时考虑不全而实现时有所改变的部分。同时也不要使得日志文件过于冗长，简洁一些，包括语言风格上，以及没有真正实现，或者试错的思路都尽量简写。
8. 每次改动后都要关注是否造成性能下降。涉及类型恢复、结构体合并、pointer analysis、pass pipeline 时，至少对比 fortune 当前关注用例的同口径运行时间。
9. evm2llvm 的 PHI 修复不能退回旧的 slot 模式 + mem2reg 思路。遇到 `PHIIncoming`
   语义问题时，要优先确认真实 CFG/SSA 语义，或者修复 Gigahorse 侧导出；如果问题复杂，
   先记录和归类，不要用 slot fallback 掩盖问题。

## 6. 近期目标

bin2LLVM 子项目近期目标：围绕 Bench2 这些真实项目生成 LLVM IR，并且语义要对。
“能被 `llvm-as` 接受”只是底线，不能代替语义正确。

## 6.1 native 寄存器消除两条链路

当前 native 寄存器消除有两条历史不同的链路，开发时必须先确认自己在改哪一条。

### 新链路：summary

这是当前默认链路，也是后续开发重点。

- 代码目录：
  - `include/notdec-bin2llvm/passes/summary/`
  - `lib/passes/summary/`
- 当前核心 pass：
  - `NativeRegisterSummary`
  - `NativeRegisterSummarySSA`
  - `NativeExternalCallSignatureRewrite`
- 入口行为：
  - `notdec-native-llvm` 默认运行 `NativeRegisterSummarySSA`。
  - 不传 `--heritage-register-ssa-pass` 时，不走旧链路。
  - 默认 summary 链路不运行 `NativePrototypeRecovery`。
- 开发要求：
  - 新的寄存器消除、call signature rewrite、internal function signature rewrite 都应基于 summary 链路的结果。
  - 不依赖 Ghidra trial/use 风格的 `notdec.prototype.*` metadata。
  - 计划和实现记录统一放到 `logs/20260616-01-native-prototype-recovery-stage2/`。

### 旧链路：heritage

这是之前模仿 Ghidra heritage/trial/use 思路写出来的链路，保留用于对照和历史测试。

- 代码目录：
  - `include/notdec-bin2llvm/passes/heritage/`
  - `lib/passes/heritage/`
- 当前核心 pass：
  - `NativeHeritageSSA`
  - `NativePrototypeRecovery`
- 入口行为：
  - 只有显式传 `--heritage-register-ssa-pass` 才运行 `NativeHeritageSSA`。
  - `NativePrototypeRecovery` 也只在该模式下运行。
- 开发要求：
  - 不要把新 summary 链路的新功能写回这里。
  - `NativePrototypeRecovery` 属于旧链路，不作为新链路 internal signature rewrite 的实现基础。

Bench2 真实项目集合在 `/sn640/NotDec-Exp/Bench2`：

- `rootfs/`：已收集的真实项目二进制和依赖。
- `manifest/benchmark-targets.tsv`：当前选中的 ELF / shared object 目标。
- `manifest/benchmark-needed.tsv`：目标的动态依赖。
- `bin2llvm-ir/`：bin2llvm 相关 JSON、`.ll`、`.bc`、日志和 Ghidra project。

本地 Ghidra 源码在 `/sn640/ghidra`。需要查 Ghidra 实现时直接从这里找，不要全盘搜索。

bin2llvm native 链路写计划时，优先按这个结构写，范围只限
`external/NotDec-bin2llvm` 的 native 路线：

1. 先说明当前目标和已有 native 状态。
2. 再介绍 Ghidra 相关实现，明确写出源码文件和关键函数。
3. 然后说明 native 侧要复刻哪些策略，哪些地方要保守处理或暂时不做。
4. 最后写阶段计划、判断标准、风险和不做什么。

这个写法只用于 bin2llvm native 链路；主 NotDec pass、evm2llvm、wasm2llvm、
llvm2c 等其他任务仍按普通项目规范写计划。

## 7. 构建

当前仓库依赖本地 LLVM 22：

- `/sn640/NotDec/llvm-22.1.0.obj`

不要用系统 `/usr/bin/llvm-as`、`/usr/bin/opt` 验证当前 IR；它们可能仍是旧 LLVM。
需要直接使用：

- `llvm-22.1.0.obj/bin/llvm-as`
- `llvm-22.1.0.obj/bin/opt`

## 11. 本文件维护原则

当本文件涉及的内容变化时，应同步更新本文件：
