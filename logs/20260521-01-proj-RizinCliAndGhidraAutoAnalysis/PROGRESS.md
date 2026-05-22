# Progress

## 当前状态

- [x] 阶段 1: State skeleton
  - [x] 已补 confirmed function / basic block / xref 的内存态和查询接口。
  - [x] 已补 instruction 的内存态和查询接口。
- [ ] 阶段 2: Entry discovery
  - [x] 已补 function seed 到 recursive decode worklist 的最小桥接。
  - [x] 已补 `.dynsym` 已定义函数符号的 seed 来源。
  - [x] 已补 Bench2 smoke 的入口 source baseline，覆盖 dynamic init/fini、init/fini array、`.eh_frame`、executable `elf-entry` 和 shared object 无 `elf-entry`。
  - [x] 已补 function seed JSON 查询，能直接查看入口地址、range、名字、alias、来源和 confidence。
- [ ] 阶段 3: Recursive disassembly CFG
  - [x] 已补从 function worklist 到 `NativeInstruction` 的最小 Sleigh 线性解码。
  - [x] 已补已解码 seed 到 confirmed function / 单 basic block 的保守落地。
  - [x] 已补直接 `CALL` / `BRANCH` / `CBRANCH` 的 xref 和 block successor。
  - [x] 已补按控制流指令切分 block，并为条件跳转补 fallthrough successor。
  - [x] 已补 direct call 目标到 function seed / worklist 的桥接。
  - [x] 已补本轮 bounded direct call seed 消费：初始 8 个 seed，同轮最多 decode 16 个 seed。
  - [x] 已补本轮 bounded direct branch successor 消费：branch target 作为同函数 block 入队。
  - [x] 已补已解码 direct branch target 对 basic block 起点的反向切分，并避免同函数 block 重叠。
  - [x] 已补 unresolved indirect flow 记录：`CALLIND` / `BRANCHIND` 进入 native state 和 report。
  - [x] 已补已知函数入口边界：direct branch 到其他 function seed 时不并入当前函数。
  - [x] 已补已知 function seed range 对 Sleigh decode 字节数的截断，减少跨函数线性 decode。
  - [x] 已补 `functionContaining(...)` 按 confirmed basic block 判断，避免 block 空洞误判。
  - [x] 已补来源可追到外部 `GLOB_DAT` GOT slot 的 `CALLIND` xref，并从 unresolved indirect call 中移除。
  - [x] 已补 direct `ram` GOT 命中 `NativePltEntry` 的 `BRANCHIND` xref，并从 unresolved indirect branch 中移除。
  - [x] 已补 `.plt.got` thunk byte-match 后的 `BRANCHIND` 解析，进一步减少 unresolved indirect branch。
  - [x] 已补来源可追到外部 `GLOB_DAT` GOT slot 的 `BRANCHIND` xref，并从 unresolved indirect branch 中移除。
  - [x] 已补 PLT0 resolver `BRANCHIND` xref，并从 unresolved indirect branch 中移除。
  - [ ] 尚未解析间接 branch/call 目标，也还没有完整函数边界判断。
- [ ] 阶段 4: CLI query
  - [x] 已补 `notdec-native-discover --summary-json` 最小 summary 输出。
  - [x] 已补 `notdec-native-discover --memory-json` 输出 load range 和 section 布局。
  - [x] 已补 `notdec-native-discover --relocations-json` 输出 relocation 表。
  - [x] 已补 `notdec-native-discover --notes-json` 输出 native 分析提示列表。
  - [x] 已补 `notdec-native-discover --eh-frame-json` 输出 `.eh_frame` / `.eh_frame_hdr` 解析统计和 FDE 列表。
  - [x] 已补 `notdec-native-discover --seeds-json` 输出 function seed 列表。
  - [x] 已补 `notdec-native-discover --functions-json` 最小 confirmed function 列表输出。
  - [x] 已补 `notdec-native-discover --blocks-json` 最小 basic block 列表输出。
  - [x] 已补 `notdec-native-discover --cfg-json` 按 confirmed function 入口输出单函数 CFG。
  - [x] 已补 `notdec-native-discover --callgraph-json` 输出 callsite 粒度调用图。
  - [x] 已补 `notdec-native-discover --xrefs-json` 最小 xref 列表输出。
  - [x] 已补 `notdec-native-discover --xrefs-kind-json` 按 flow/call/data/string 过滤 xref。
  - [x] 已补 `notdec-native-discover --xrefs-from-json` / `--xrefs-to-json` 按地址查 xref。
  - [x] 已补 `notdec-native-discover --instructions-json` 最小 instruction 列表输出。
  - [x] 已补 `notdec-native-discover --instructions-range-json` 按地址范围查 instruction。
  - [x] 已补 `notdec-native-discover --instructions-function-json` 按 confirmed function 入口查 instruction。
  - [x] 已补 `notdec-native-discover --plt-json` 输出 PLT stub / GOT / 外部符号映射。
  - [x] 已补 `.plt.got` 函数型 `GLOB_DAT` thunk 到 PLT 外部映射。
  - [x] 已补 `.plt.got` 机器码反查 GOT slot，覆盖 relocation 数量不对齐的外部 thunk。
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
  - [x] 已补 x86 `LOCK` / `UNLOCK` `CALLOTHER` 17/18 no-op lowering，保留普通内存交换语义。
  - [x] 已补来源可追到外部 `GLOB_DAT` GOT slot 的 `BRANCHIND` external tail jump lowering。
  - [x] 已补 `NativePltEntry::GotAddress` 的 `BRANCHIND` external tail jump lowering，并复用同一外部 LLVM symbol。
  - [x] 已补 direct `ram` varnode 输入到 `@notdec_ram` load，避免继续生成 `freeze poison`。
  - [x] 已补 PLT0 resolver `BRANCHIND` 到 `notdec_plt0_resolver` 的保守 tail jump lowering。
- [ ] 阶段 7: Bench2 regression
  - [x] 已补 `scripts/bench2-native-smoke.sh`，固定跑 `vsftpd`、`libuv`、`memcached` 的 native discovery 和 LLVM verify。
  - [x] 已补 Bench2 smoke 的固定 IR pattern 检查，覆盖内部 direct call 和 libuv PLT external call。
  - [x] 已补 libuv `.plt.got` external call 的 smoke pattern。
  - [x] 已补 libuv 单函数 `-f 0x9df0` 和 `-n uv_key_delete` 的 smoke 检查。
  - [x] 已补三个目标 `_init` 里 `__gmon_start__` GOT indirect call 的 smoke pattern。
  - [x] 已补 Bench2 smoke 禁止 `CALL` / `CALLIND` / `CALLOTHER` helper 回退。
  - [x] 已补 Bench2 smoke 禁止当前三目标保留 unresolved indirect call。
  - [x] 已补 Bench2 smoke 对当前 unresolved indirect branch 基线的上限检查。
  - [x] 已把当前 branch 基线收紧到 `vsftpd` / `memcached` <= 1，`libuv` <= 0。
  - [x] 已补三个目标 `_ITM_deregisterTMCloneTable` GOT external tail jump 的 smoke pattern。
  - [x] 已补 `vsftpd` / `memcached` `.plt.sec` GOT indirect branch 的 smoke pattern。
  - [x] 已把当前三目标 unresolved indirect branch 基线收紧到 0。
  - [x] 已补 Bench2 smoke 禁止 direct `ram` 输入重新 lower 成 `freeze poison`。
  - [x] 已补 Bench2 smoke 禁止当前三目标重新出现 `notdec_exit`，并检查 PLT0 resolver call。
  - [x] 已补 Bench2 smoke 的 `metrics.tsv` 汇总，记录函数、block、instruction、xref、unresolved 和耗时。
  - [x] 已补 Bench2 smoke 消费已有 heritage module，输出 `heritage-metrics.tsv` 和 `native-heritage-compare.tsv`。
  - [x] 已补 Bench2 smoke 的 native block CFG 不变量检查：同函数 block 不重叠，successor 不指向同函数 block 内部。

## 记录规则

1. 每完成一个小块，就更新这里的状态。
2. 每完成一个阶段，就同步更新 `ARCHITECTURE.md`。
3. 只记录真实完成的内容，不记空想和试错草稿。
