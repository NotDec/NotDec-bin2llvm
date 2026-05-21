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
  - [x] 已补已知 function seed range 对 Sleigh decode 字节数的截断，减少跨函数线性 decode。
  - [x] 已补 `functionContaining(...)` 按 confirmed basic block 判断，避免 block 空洞误判。
  - [ ] 尚未解析间接 branch/call 目标，也还没有完整函数边界判断。
- [ ] 阶段 4: CLI query
  - [x] 已补 `notdec-native-discover --summary-json` 最小 summary 输出。
  - [x] 已补 `notdec-native-discover --functions-json` 最小 confirmed function 列表输出。
  - [x] 已补 `notdec-native-discover --blocks-json` 最小 basic block 列表输出。
  - [x] 已补 `notdec-native-discover --xrefs-json` 最小 xref 列表输出。
  - [x] 已补 `notdec-native-discover --xrefs-from-json` / `--xrefs-to-json` 按地址查 xref。
  - [x] 已补 `notdec-native-discover --instructions-json` 最小 instruction 列表输出。
  - [x] 已补 `notdec-native-discover --plt-json` 输出 PLT stub / GOT / 外部符号映射。
  - [x] 已补 `.plt.got` 函数型 `GLOB_DAT` thunk 到 PLT 外部映射。
  - [x] 已补 `notdec-native-discover --unresolved-json` 输出 indirect call / branch 未解析样本。
- [ ] 阶段 5: XRef enhancement
  - [x] 已补 direct `ram` P-Code 数据访问到 `data` xref 的保守记录。
  - [x] 已补 direct `ram` 数据引用到只读 C 字符串时的 `string` xref 分类。
  - [x] 已补 ELF relocated pointer 到 `data` / `string` xref 的记录。
  - [x] 已补 direct `CALL` 命中 PLT stub 时的 `sleigh-pcode-plt-call` xref 分类，并避免把 PLT 当内部函数 seed。
- [ ] 阶段 6: Lowering integration
  - [x] 已补 `notdec-native-llvm -f <entry>`，按 native confirmed function 入口生成 LLVM IR。
  - [x] 已补 `notdec-native-llvm -n <name>`，按 native confirmed function 名字生成 LLVM IR。
  - [x] 已补 `notdec-native-llvm --all-confirmed`，把可验证 confirmed functions 输出到同一 LLVM module。
  - [x] 已补 P-Code lowering 的 LLVM entry 跳板，允许 CFG 回边跳到机器入口 block。
  - [x] 已补 `--all-confirmed` 内 confirmed function 之间 direct `CALL` 到 LLVM direct call 的最小连接。
  - [x] 已补 forward direct call 先创建 declaration、后续再补函数体的处理。
  - [x] 已补 direct `CALL` 命中已知 PLT stub 时 lower 成外部 LLVM function call。
  - [x] 已补 `-f` / `-n` 单函数模式复用 confirmed function 和 PLT 外部符号映射。
  - [x] 已补来源可追到外部 `GLOB_DAT` GOT slot 的 `CALLIND` 到外部 LLVM function call。
- [ ] 阶段 7: Bench2 regression
  - [x] 已补 `scripts/bench2-native-smoke.sh`，固定跑 `vsftpd`、`libuv`、`memcached` 的 native discovery 和 LLVM verify。
  - [x] 已补 Bench2 smoke 的固定 IR pattern 检查，覆盖内部 direct call 和 libuv PLT external call。
  - [x] 已补 libuv `.plt.got` external call 的 smoke pattern。
  - [x] 已补 libuv 单函数 `-f 0x9df0` 和 `-n uv_key_delete` 的 smoke 检查。
  - [x] 已补三个目标 `_init` 里 `__gmon_start__` GOT indirect call 的 smoke pattern。

## 记录规则

1. 每完成一个小块，就更新这里的状态。
2. 每完成一个阶段，就同步更新 `ARCHITECTURE.md`。
3. 只记录真实完成的内容，不记空想和试错草稿。
