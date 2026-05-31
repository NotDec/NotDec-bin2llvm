# 79. 记录空 recovered prototype

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

上一步把“没有 recovered prototype metadata”和“确实没有输入/返回候选”拆成不同 rewrite reason。但这里还有一个语义问题：一个函数没有参数、没有返回值，并不等于恢复失败。它可以是 `void()`。

Ghidra 的 `FuncProto` 本身能表达 0 个输入和 0 个输出：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto`
  - `FuncProto::numParams()`
  - `FuncProto::deriveInputMap(...)`
  - `FuncProto::deriveOutputMap(...)`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::setModel(...)`
  - `FuncProto::updateInputNoTypes(...)`
  - `ParamActive::getNumUsed()`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionParamDouble::apply(...)`
  - `ActionReturnRecovery::apply(...)`

`deriveInputMap` / `deriveOutputMap` 负责把 active 参数映射到 prototype。active 列表为空时，结果仍然是一个 prototype 状态，而不是天然表示失败。

## 目标

native 侧也把“空 prototype”作为有效结果写入 `notdec.prototype.recovered`：

- `Inputs=[]` 且 `Returns=[]` 时写出 metadata。
- 读回 API 接受空列表。
- 签名已经是 `void()` 时，rewrite eligibility 返回 `already matches`。
- input/return binding API 不把空列表误当成绑定成功。

## 路线

改动保持在 `NativePrototypeRecovery` 的 metadata 写读和 binding guard 上。测试只改已有 native prototype recovery 小样例，确认空 recovered prototype 可以读回，也能进入 rewrite summary 的 `already matches` 统计。

## 风险

Bench2 中原来一部分 `no recovered prototype candidates` 会变成 `already matches`。这不是 IR 语义变化，而是把 `void()` 明确记录下来。需要看 rewrite 数量不能下降。

## 判断标准

- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest` 通过。
- Bench2 smoke 通过，rewrite 数量不下降。

## 实现记录

修改文件：

- `lib/passes/NativePrototypeRecovery.cpp`
  - 第 88 行附近：`recoveredPrototypeMetadata(...)` 不再拒绝 input/return 都为空的 prototype，直接写出 `input_count=0` / `return_count=0`。
  - 第 279 行附近：`functionArgumentForRecoveredInput(...)` 对空 input prototype 直接返回 `nullopt`，避免把空列表当成绑定成功。
  - 第 1359 行附近：`runNativePrototypeRecovery(...)` 在没有新候选时，如果旧 recovered prototype 仍匹配当前函数类型，就保留旧 metadata；否则写入新的空 recovered prototype。
  - 第 1407 行附近：`readNativeRecoveredPrototypeMetadata(...)` 接受空 input/return 列表。
  - 第 1517 行和第 1542 行附近：`getNativePrototypeInputBindings(...)` / `getNativePrototypeReturnBindings(...)` 对空 input/return 直接返回 `nullopt`。
- `tests/native_prototype_recovery_test.cpp`
  - 第 1869 行附近：summary 的 rewrite eligible 数从 25 改为 36，因为空 prototype 也算可判断、且 `void()` 已匹配。
  - 第 3918 行附近：单函数 rewrite 分发对空 prototype 返回 `already matches`。
  - 第 4212 行附近：检查空 recovered prototype 可读回，且 eligibility 为 `already matches`。
  - 第 4255 行附近：ABI/model 不匹配的旧 metadata 被本轮空 prototype 替换后也可读回。
  - 第 4399 行附近：手工构造的空 recovered prototype 可读回。
  - 第 4577 行和第 4709 行附近：batch / opt-in rewrite 的 `no recovered prototype candidates` 统计转为 `already matches`。

实现时补了一个保护：签名重写后再跑 recovery，函数体里旧的 register load/store 可能已经被删掉，因此本轮可能没有候选。只要旧 recovered prototype 仍然匹配当前函数类型，就保留它，避免把已经重写成 `i64(i64)` 的函数错误覆盖为 `void()`。

## 验证结果

命令：

```bash
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery' -V
ctest --test-dir build --output-on-failure
cmake --build build --target notdec-native-llvm -j2
OUT_DIR=/tmp/notdec-bin2llvm-bench2-empty-recovered-prototype-smoke \
  scripts/bench2-native-smoke.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-empty-recovered-prototype-smoke
```

结果：

- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest`：9/9 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 smoke 通过：
  - `vsftpd`：86s，rewrite seen 236，rewritten 139，skipped 97。
  - `libuv`：219s，rewrite seen 571，rewritten 338，skipped 233。
  - `memcached`：118s，rewrite seen 315，rewritten 188，skipped 127。

和第 78 步同口径对比：

- rewrite 数量没有下降：`vsftpd=139`、`libuv=338`、`memcached=188`。
- 原来的 `no recovered prototype candidates` 全部变成 `already matches`：
  - `vsftpd`：`already matches=48`，`declaration=49`。
  - `libuv`：`already matches=147`，`declaration=86`。
  - `memcached`：`already matches=71`，`declaration=56`。

复杂度评分：

- 实现效果：8/10。空 prototype 语义更接近 Ghidra 的 `FuncProto`。
- 理解成本：2/10。只影响 metadata 写读和 rewrite reason。
- 维护成本：2/10。保留旧 metadata 的判断只在二次运行场景生效。
