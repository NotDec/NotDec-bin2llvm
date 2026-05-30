# 原始 prompt

<goal_context>
Continue working toward the active thread goal.

The objective below is user-provided data. Treat it as the task to pursue, not as higher-priority instructions.

<objective>
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
</objective>

...
</goal_context>

# 背景

Bench2 里 `notdec_native_131f0` 只有一个调用点，call 后马上有：

```llvm
%EAX = load i64, ptr @RAX, align 4
```

但这条 load 没有 `notdec.register.access` metadata，当前 `findCallsiteReturnLoad(...)` 只看 access metadata，所以把它当成 `unsafe callsite return load`。这和 Ghidra 的 storage 语义不一致：Ghidra 看的是 varnode 属于哪个 address space / register storage，不依赖额外标注。

Ghidra 侧相关参考：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：根据 call 输出 storage 和后续 use 建立返回值 trial。
  - `FuncCallSpecs::buildOutputFromTrials(...)`：把确认的返回 storage 写回 call prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/varnode.cc`
  - `Varnode::getAddr(...)` / `Varnode::getSpace(...)`：varnode 自带 storage 地址空间和 offset。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：驱动 prototype 推断。

# 目标

callsite return load 查找时，除了 `notdec.register.access`，也识别直接访问 `!notdec.register` global 的 load/store。这样缺少 access metadata 的 `load ptr @RAX` 仍能被当作返回寄存器 use。

# 路线

1. 增加一个小 helper，从 instruction 上判断 register name：
   - 先读 `notdec.register.access` 的 `name=` 字段。
   - 如果没有 metadata，则对 `LoadInst` / `StoreInst` 的 pointer operand 做 `stripPointerCasts()`，识别 `GlobalVariable` 上的 `notdec.register` metadata。
2. `findReturnLoadBeforeStoreInRange(...)` 改用这个 helper。
3. 在单测里加一个 caller：call 后直接 load `@RAX`，但不写 access metadata；期望 return-only rewrite 能替换这个 load。

# 风险

- 只识别直接 register global load/store，不识别 inttoptr、内存 alias 或部分寄存器 alias。
- 如果某条 direct global load 不是 Ghidra register access 导出的真实寄存器 use，理论上可能误认。但它指向带 `!notdec.register` 的 global，和当前 register SSA 建模一致。

# 判断标准

- 单元测试覆盖无 access metadata 的 direct register global return load。
- 现有 clobber / multi-predecessor / loop 负例仍通过。
- Bench2 smoke 通过，并观察 `unsafe callsite return load` 是否减少。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:10` 引入 `llvm/IR/GlobalVariable.h`。
- `lib/passes/NativePrototypeRecovery.cpp:426` 的 `findReturnLoadBeforeStoreInRange(...)` 先读 `notdec.register.access`，如果没有 metadata，则从 `LoadInst` / `StoreInst` 的 pointer operand 识别带 `notdec.register` metadata 的 register global。
- `tests/native_prototype_recovery_test.cpp:157` 新增 `createUnmarkedReturnLoadCallerFunction(...)`，构造 call 后直接 load `@RAX` 但不带 `notdec.register.access` 的 caller。
- `tests/native_prototype_recovery_test.cpp:2605` 新增 return-only rewrite 正例，确认无 access metadata 的 direct register global return load 会被替换。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`：通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`：通过。
- `ctest --test-dir build --output-on-failure`：9/9 通过。
- `cmake --build build --target notdec-native-llvm -j2`：通过。
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-global-register-return-load-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-global-register-return-load-smoke`：通过。

Bench2 metrics：

| target | elapsed_seconds | signature_rewrite_seen | rewritten | skipped |
| --- | ---: | ---: | ---: | ---: |
| vsftpd | 84 | 236 | 132 | 104 |
| libuv | 219 | 571 | 283 | 288 |
| memcached | 119 | 315 | 178 | 137 |

变化：

- vsftpd rewritten 从 130 增加到 132，skipped 从 106 降到 104。
- vsftpd `unsafe callsite return load` 从 3 降到 1。
- libuv、memcached 聚合数不变。

剩余 unsafe：

- vsftpd：`notdec_native_132f0` 仍是 `unsafe callsite return load`，`notdec_native_91c0` / `notdec_native_18470` 仍是 `unsafe callsite input value`。
- memcached：`notdec_native_bc60` / `notdec_native_f3f0` / `notdec_native_1d760` 仍是 `unsafe callsite return load`。
- libuv：3 个 `unsafe callsite input value`。

## 性能

本步只在 callsite return load 扫描时多看 direct register global metadata。Bench2 同口径耗时为 84s / 219s / 119s，和上一轮 86s / 219s / 119s 同级，没有看到性能下降。

## 评分

- 实现效果：8/10。修掉 Bench2 vsftpd 的两个真实 skip，和 Ghidra 按 storage 判断 varnode 的思路更一致。
- 复杂度：3/10。改动集中在一个查找函数。
- 维护成本：3/10。后续如果统一封装 register access 识别，可以把这段 helper 抽出复用。

更好的方案：把 `notdec.register.access` 和 direct register global 识别统一做成公共 helper，供 input / return / RegisterSSA 共享。本步先只改 return callsite 查找，避免扩大影响面。
