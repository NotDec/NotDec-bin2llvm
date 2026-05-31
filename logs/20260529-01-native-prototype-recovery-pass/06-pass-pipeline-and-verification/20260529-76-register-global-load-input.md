# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

第 75 步后，当前 Bench2 关注用例已经没有 `unsafe callsite input value` 和
`unsafe callsite return load`。剩下主要是 `missing recovered prototype`。

`libuv.uv_key_delete` 是一个清楚的小 case：

```llvm
define void @uv_key_delete() !notdec.register.external_inputs !101 {
entry:
  %RSP.external_input = load i64, ptr @RSP, ...
  %RBP.external_input = load i64, ptr @RBP, ...
  ...
  %RDI = load i64, ptr @RDI, align 4
  %notdec_ram_ptr2 = inttoptr i64 %RDI to ptr
  %unique_de00_4 = load i32, ptr %notdec_ram_ptr2, align 1
  store i64 ..., ptr @RDI, ...
  call void @pthread_key_delete()
```

这里 `RDI` 明显是入口输入，先被当作地址使用，然后才被覆盖。但当前
`NativeRegisterSSA::registerLoad(...)` 只认带 `notdec.register.access` 的 load。
这条 `load ptr @RDI` 本身没有 access metadata，只有 `@RDI` global 上有
`notdec.register` metadata，所以没有被标成 external input，后面 prototype recovery
也就没有 input candidate。

# Ghidra 对应实现

Ghidra 的 heritage 不依赖每条 p-code op 额外贴一份 register access metadata；
varnode 所在 address space 和 offset 就能说明它是哪个寄存器：

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`
  - `HeritageInfo::buildInfoList(...)`
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`
  - `FuncCallSpecs::buildInputFromTrials(...)`
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`

对应到 native 侧，`@RDI` 这种 register global 已经带了 `notdec.register` metadata，
即使 load 指令上漏了 `notdec.register.access`，也应该能作为完整寄存器 load 进入
RegisterSSA。

# native 侧复刻路线

本轮只补 load，不补 store：

1. `registerLoad(...)` 仍优先接受现有 `notdec.register.access`。
2. 如果 load 没有 access metadata，但 pointer 是带 `notdec.register` metadata 的 global，
   也识别成 register load。
3. 仍要求 load 类型等于 global value type，避免把部分寄存器访问当完整输入。
4. store 暂不放宽，避免误把普通写全局变量当寄存器定义；真实 case 只需要 load。

# 判断标准

- 新增单测：函数里无 access metadata 的 `load @RDI` 应被 RegisterSSA 标成 external input。
- Bench2 `libuv.uv_key_delete` 应出现 `RDI` input candidate 或 recovered prototype。
- Bench2 selected smoke 通过，运行时间不能明显变差。

# 风险

如果有普通全局变量错误带了 `notdec.register` metadata，会被当作寄存器。这个风险来自
metadata 源头，不是本轮新增；本轮仍要求 global 已明确标为 register，且 load 是完整
寄存器宽度。

# 实现记录

已完成。

## 改动

- `lib/passes/NativeRegisterSSA.cpp:160` 修改 `registerLoad(...)`：没有
  `notdec.register.access` 的 load，如果 pointer 是带 `notdec.register` metadata 的
  register global，也按 register load 处理。
- `tests/native_register_effects_test.cpp:231` 新增
  `createUnmarkedRegisterLoadFunction(...)`，构造无 access metadata 的 `load @RDI`。
- `tests/native_register_effects_test.cpp:300` 增加 `RDI` register global。
- `tests/native_register_effects_test.cpp:362` 增加断言：无 access metadata 的 `RDI` load
  应生成 `notdec.register.external_inputs`，并被重写成入口 external input load。

## 验证

```text
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects' -V
ctest --test-dir build --output-on-failure
cmake --build build --target notdec-native-llvm -j2
OUT_DIR=/tmp/notdec-bin2llvm-bench2-register-global-load-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-register-global-load-smoke
```

结果：

- `native_register_effects_test` 通过。
- 全量 `ctest` 9/9 通过。
- Bench2 selected smoke 通过。
- metrics：
  - `vsftpd`: 84s，rewritten 138，skipped 98。
  - `libuv`: 217s，rewritten 338，skipped 233。
  - `memcached`: 116s，rewritten 188，skipped 127。
- `libuv.uv_key_delete`：`external_inputs=3 input_candidates=1 return_candidates=0`，已 rewritten。
- `libuv.uv_key_set`：`external_inputs=3 input_candidates=1 return_candidates=0`，已 rewritten。
- `libuv` 的 `missing recovered prototype` 从 199 降到 147。

性能：

- 本次只放宽 register global load 识别，不增加额外 CFG 扫描。
- Bench2 同口径时间：`vsftpd` 84s 不变，`libuv` 219s -> 217s，`memcached` 118s -> 116s。没有明显性能下降。

## 评分

- 实现效果：8/10。补上真实 IR 中常见的无 access metadata register load，明显增加 input candidate 和 rewrite 数。
- 复杂度：3/10。只改一个识别条件。
- 维护成本：4/10。依赖 register global metadata，和现有全局寄存器模型一致。

更完整的方案是让 lowering 阶段保证所有 register load/store 都带 access metadata。本轮先在 RegisterSSA 侧容错，避免漏掉已有全局寄存器事实。
