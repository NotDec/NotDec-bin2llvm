# Stage 2: Entry Discovery

## 这一步做什么

从 ELF entry、symbols、PLT、init/fini array、eh_frame FDE 生成 function seed，并进入轻量队列。

## 先写什么

1. Ghidra `EntryPointAnalyzer` 相关源码和关键函数。
2. native 侧可以复刻哪些入口发现策略。
3. 哪些入口必须保守处理。

## 后续要求

这里只放这一阶段的规划和实现记录。

