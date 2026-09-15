# 原始 prompt

```text
wrk的IR中是否还有什么明显的问题吗？看看下一个修什么
```

后续选择先做本轮审计中的第一点：最终 IR 只保留 direct-call 闭包，address-taken
函数大量消失。

# 背景

RSP.entry 残留修完后，wrk 的 register residue 已经归零，但对比 discovery 和最终 IR
有一个更明显的问题：

- discovery `confirmed_functions=248`；
- 最终 `wrk.native.ll` 只有 39 个 `define`；
- `main` 里 `pthread_create(..., i64 42448, ...)`，`42448 = 0xa5d0`，
  debug symbol 是 `thread_main`，但最终 IR 没有 `@thread_main` 定义；
- `sock_connect`、`sock_close`、`sock_read`、`response_complete`、
  `script_stats_*` 等回调也缺失。

根因分两层：

1. 代码里的函数地址常量（`lea reg, [rip+...]` 对应的 P-Code `COPY const`）
   被 `PcodeToLLVM::read()` 直接降低成裸 `ConstantInt`，IR 中完全没有对 LLVM
   `@function` 的 use。最终 `GlobalDCEPass()` 只按 direct call graph 存活，
   address-taken 函数体全部被删。
2. SummarySSA 做签名 rewrite 时用 `createReplacementFunction()` 新建函数并
   `takeName(&oldFunction)`。如果裸整数已经改成 `ptrtoint @function` 常量，
   旧函数对象还有 ConstantExpr user，旧函数无法删除，且会以空 body + internal
   linkage 留在 module 里，触发 verifier
   `Global is empty, but doesn't have external/weak linkage`。

# 目标

1. 代码中的已知函数入口立即数变成 `ptrtoint @function`，保留函数 body 和 use。
2. SummarySSA 重写函数签名时，`ptrtoint` 形式的 address-taken use 跟随到新函数。
3. relocation 指向的函数指针无法直接改写内存数据，第一版先收集其目标函数集合，
   在最终 cleanup 前用 `@llvm.used` 保留，避免 GlobalDCE 删除。

# 不做什么

- 本轮不改写 `.data.rel.ro` / `.data` 中 relocation slot 的 load；把 memory load
  直接常量化成函数指针会在可写 slot 上破坏 store/load 语义。因此 `sock_*` 等
  通过数据表间接调用的函数只保留 body，目标解析留到第二阶段。
- 不保留 `_start` / init/fini / frame_dummy 等 runtime glue；这些函数即使被
  relocation 指向，也会在最终输出制造无意义 register residue。

# 实现

## 1. 代码 immediate 函数地址实体化

- `include/notdec-bin2llvm/PcodeToLLVM.h:43`
  `PcodeLoweringConfig` 新增 `CodeAddressTargets`。它比 `DirectCallTargets` 窄：
  过滤 PC thunk 和 PLT stub，避免把外部 stub 当成内部函数体。
- `lib/PcodeToLLVM.cpp:3096` 新增 `PcodeLowerer::functionForCodeAddress()`；
  `read()` 遇到 pointer-size 的 `const` varnode 命中函数入口时，返回
  `Builder.CreatePtrToInt(function, type)`，否则保持原来的 `ConstantInt`。
- `tools/notdec-native-llvm.cpp:848` `buildConfirmedModule()` 构建
  `codeAddressTargets`，排除 `callTargets.External` 中的 PLT stub 和
  `function.IsPcThunk`，并在每个 lowering config 中传入。

## 2. SummarySSA 跟随 address-taken use

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:8069` 新增
  `redirectAddressTakenUses()`：遍历旧函数的 ConstantExpr user，只处理
  `ptrtoint`，生成指向新函数的 ConstantExpr 并 `replaceAllUsesWith()`，然后
  `destroyConstant()` 旧表达式，避免旧函数残留 user。
- `rewriteSignatureShapes()` 末尾在 erase 旧函数前调用该 helper。

## 3. relocation 目标的函数体保留

- `tools/notdec-native-llvm.cpp:1040` 新增
  `preserveAddressTakenFunctions()`：把指定名字的 `define` 收集到标准
  `@llvm.used = appending global [N x ptr]`，让最后一次 GlobalDCE 不删除。
- `main()` 在 all-confirmed 路径根据 `selectedState->relocatedPointers()`，
  把指向非 runtime `NativeFunction` 的目标 name 收集为 preserve set，传给
  `runFinalCleanupPass()`；IR 输入/单函数路径保持空集。

## 4. 测试

- `tests/pcode_to_llvm_test.cpp:1447`
  `testFunctionAddressConstantBecomesFunctionReference`：验证 pointer-size const
  命中 `CodeAddressTargets` 后生成 `ptrtoint @function` use。
- `tests/native_register_summary_ssa_test.cpp:5974`
  `testAddressTakenInternalFunctionUseFollowsRewrite`：验证 SummarySSA
  重写同名函数后，ptrtoint 常量指向新函数，module verifier 通过。

# 验证

## wrk

命令：

```bash
build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  --all-confirmed \
  --summary-json-out /tmp/notdec-addrptr3/wrk.summary.json \
  --register-ssa-warning-out /tmp/notdec-addrptr3/wrk.warn.tsv \
  -o /tmp/notdec-addrptr3/wrk.native.ll
```

结果：

- `define` 数：39 -> 78。
- `pthread_create` 第三参从裸 `i64 42448` 变为
  `ptrtoint (ptr @FUN_a5d0 to i64)`。
- `sock_connect` / `sock_close` / `sock_read` / `sock_write` /
  `sock_readable` / `ssl_*` / `FUN_ae30` 等 relocation address-taken 函数已有
  `define`，并通过 `@llvm.used` 保留。
- `RSP.entry` load 仍为 0；register warning TSV 与修改前逐行内容一致
  （354 行，差异只有行序）。
- 新暴露 1 条 `FS_OFFSET` load residue，位于新保留的 `FUN_a260`
  (`socket_writeable`)；这是下一个独立清理项，不属于本次 address-taken 实体化。
- LLVM 22 `llvm-as` + `opt -passes=verify` 通过。

## 回归

- `pcode_to_llvm_test`：通过。
- `native_register_summary_ssa_test`：通过。
- `ctest --test-dir build --output-on-failure`：12/12 通过。
- fortune x86_64：
  - define 11 -> 13；warning 35 行完全一致；residue audit 仍只有表头。
- fortune i386：
  - define 40 -> 40；warning 9 行完全一致；residue 记录一致。

## 性能

wrk 全量命令：

- 修改前当前基线：`1:22.37`、RSS `137064KB`；
- 修改后：`1:27.20`、RSS `137960KB`。

约 6% 的额外耗时来自保留并继续分析更多 address-taken 函数体，内存基本不变。
结论：可以接受；如果后续只需 warning/audit，可再考虑增加“不实化纯数据指针”
的开关，但所有 confirmed 函数输出更接近 decompiler 语义。

# 评分与剩余

- 实现效果：8/10。代码立即数路线解决了 `thread_main` 和若干 callback 的 body
  丢失；relocation 数据表目标只保留了 body，还没把间接调用目标接到真实
  `@function`。
- 复杂度：6/10。改动跨 PcodeToLLVM、SummarySSA 和 CLI 三处，但规则都保守。
- 维护成本：6/10。`@llvm.used` 是标准机制，但后续如果实现 relocation slot
  initializer，应删除这层“只保留不引用”的兜底。

下一阶段建议：

1. 把 `.data.rel.ro` / relocation 函数指针 slot 建模成真正的 LLVM 全局初始化，
   让 `sock_connect` 等间接调用拿到 `ptr @function`，再移除 `@llvm.used` 兜底。
2. 清理 `FUN_a260` 的 `FS_OFFSET.entry` 残留（socket_writeable canary/FS 模式）。
