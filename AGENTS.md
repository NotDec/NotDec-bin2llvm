# bin2llvm 工作规范

保持白话和具体。实现前先说明假设、取舍和最小修改方案，讨论一致后再改；不要顺手重构相邻代码或删除无关内容。

## 记录、验证和性能

- 新数据结构前写注释说明设计原因。
- 复杂代码工作使用 `logs/`：plan 顶部保留用户原始 prompt，说明背景、目标、路线、风险和判断标准；实现记录列出文件、行、函数、验证命令和结果。实现已有 plan 时更新原 plan。复杂修改还要评估实现效果、增加的理解成本和维护成本。
- 已讨论一致且验证通过的修改默认直接提交。每次修改都检查性能；涉及类型恢复、结构体合并、pointer analysis 或 pipeline 时，按相同条件对比 fortune 用例。
- IR 只能用 `/sn640/NotDec/llvm-22.1.0.obj` 下的 LLVM 22 工具验证，不能用系统 `llvm-as` 或 `opt`。

## native 路线

目标是为 Bench2 真实二进制生成语义正确的 LLVM IR；能被 `llvm-as` 接受只是底线。数据和产物位于 `/sn640/NotDec-Exp/Bench2`：`rootfs/`、`manifest/benchmark-targets.tsv`、`manifest/benchmark-needed.tsv`、`bin2llvm-ir/`。需要对照 Ghidra 时直接读 `/sn640/ghidra`。

native 计划必须说明已有 native 状态，列出 Ghidra 源码文件和关键函数，区分需要复刻的策略与保守暂缓的部分，再写阶段、判断标准、风险和不做什么。本格式只用于 native bin2llvm。

## 寄存器消除路线

当前默认且后续重点是 `summary`：

- 代码在 `include/notdec-bin2llvm/passes/summary/`、`lib/passes/summary/`。
- `NativeRegisterSummary` 与 `NativeRegisterSummarySSA` 负责寄存器 SSA、寄存器消除和 native 函数签名重写。
- `notdec-native-llvm` 默认使用该路；只有传 `--heritage-register-ssa-pass` 才走旧路。
- 新的寄存器消除、call/internal signature rewrite 都基于 summary 结果，不依赖 Ghidra `notdec.prototype.*` metadata。相关 plan/记录在 `logs/20260616-01-native-prototype-recovery-stage2/`。

`heritage` 仅用于对照、历史测试和编译维护：代码在 `include/notdec-bin2llvm/passes/heritage/`、`lib/passes/heritage/`，核心为 `NativeHeritageSSA` 和 `NativePrototypeRecovery`。不要把 summary 新功能写回旧路，除非保持现有测试或构建所必需。

## EVM PHI 约束

EVM PHI 修复不能退回旧的 slot + mem2reg 模式。优先检查真实 CFG/SSA 语义或修复 Gigahorse 导出；语义不明确时先记录归类，不用 slot fallback 掩盖问题。

本目录的调试参数、Bench2 命令和产物位置见 `DEBUG.md`；规则变化时同步更新本文件。
