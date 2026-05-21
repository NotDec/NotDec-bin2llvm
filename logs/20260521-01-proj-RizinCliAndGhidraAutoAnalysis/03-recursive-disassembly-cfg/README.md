# Stage 3: Recursive Disassembly CFG

## 这一步做什么

从 function seed 出发递归 decode，建立最小 basic block 和 CFG。

## 先写什么

1. Ghidra disassembler / function creation 相关源码和关键函数。
2. native 侧如何复刻 direct jump、conditional jump、direct call、ret。
3. unresolved indirect flow 如何记录。

## 后续要求

只记录这一阶段的规划和实现。

