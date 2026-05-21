# Progress

## 当前状态

- [x] 阶段 1: State skeleton
  - [x] 已补 confirmed function / basic block / xref 的内存态和查询接口。
  - [x] 已补 instruction 的内存态和查询接口。
- [ ] 阶段 2: Entry discovery
  - [x] 已补 function seed 到 recursive decode worklist 的最小桥接。
  - [x] 已补 `.dynsym` 已定义函数符号的 seed 来源。
- [ ] 阶段 3: Recursive disassembly CFG
  - [x] 已补从 function worklist 到 `NativeInstruction` 的最小 Sleigh 线性解码。
  - [x] 已补已解码 seed 到 confirmed function / 单 basic block 的保守落地。
  - [x] 已补直接 `CALL` / `BRANCH` / `CBRANCH` 的 xref 和 block successor。
  - [x] 已补按控制流指令切分 block，并为条件跳转补 fallthrough successor。
  - [x] 已补 direct call 目标到 function seed / worklist 的桥接。
  - [x] 已补本轮 bounded direct call seed 消费：初始 8 个 seed，同轮最多 decode 16 个 seed。
  - [x] 已补本轮 bounded direct branch successor 消费：branch target 作为同函数 block 入队。
  - [x] 已补 unresolved indirect flow 记录：`CALLIND` / `BRANCHIND` 进入 native state 和 report。
  - [x] 已补已知函数入口边界：direct branch 到其他 function seed 时不并入当前函数。
  - [ ] 尚未解析间接 branch/call 目标，也还没有完整函数边界判断。
- [ ] 阶段 4: CLI query
  - [x] 已补 `notdec-native-discover --summary-json` 最小 summary 输出。
  - [x] 已补 `notdec-native-discover --functions-json` 最小 confirmed function 列表输出。
- [ ] 阶段 5: XRef enhancement
- [ ] 阶段 6: Lowering integration
- [ ] 阶段 7: Bench2 regression

## 记录规则

1. 每完成一个小块，就更新这里的状态。
2. 每完成一个阶段，就同步更新 `ARCHITECTURE.md`。
3. 只记录真实完成的内容，不记空想和试错草稿。
