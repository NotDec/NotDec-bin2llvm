# Native Bench2 IR Feature Checks

## 原始 prompt

```text
在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## 当前目标和已有 native 状态

Bench2 smoke 现在只检查两件事：native discovery 找到 confirmed function，生成的 IR 能被
LLVM 22 `llvm-as` 和 `opt -passes=verify` 接受。这是底线，但还不能防止关键语义退化。

最近已经补了两个可观察的 native lowering 行为：

1. confirmed function 之间的 direct `CALL` 会 lower 成 LLVM direct call。
2. direct `CALL` 命中已知 PLT stub 时，会 lower 成外部 LLVM function call。

这次只把这些行为写进 Bench2 smoke 的固定检查里。

## Ghidra 相关实现

Ghidra / heritage 路线里，这类行为来自模块级符号规划和 call lowering：

- `ghidra_scripts/ExportHeritageModule.java::directCallTargetName(...)`
  - 导出 direct call 可解析到的目标名字。
- `ghidra_scripts/ExportHeritageModule.java::writeExternals(...)`
  - 导出外部函数。
- `lib/HeritageToLLVM.cpp::planModuleSymbols(...)`
  - 先为内部函数和外部函数规划 LLVM symbol。
- `lib/HeritageToLLVM.cpp::lowerCall(...)`
  - direct call 命中符号表时生成普通 LLVM call。

Bench2 smoke 不复刻 Ghidra 逻辑，只用固定真实样本检查 native 是否已经产生同类 IR 形态。

## native 侧复刻策略

1. 在 `scripts/bench2-native-smoke.sh` 加最小 pattern 检查函数。
2. 保留原有 discovery / LLVM verify 流程。
3. 每个目标生成 IR 后，检查当前真实样本里稳定出现的 direct call：
   - `vsftpd`：内部 direct call 到 `notdec_native_5ba0` 和 `notdec_native_8290`。
   - `memcached`：内部 direct call 到 `notdec_native_5b80` 和 `notdec_native_b950`。
   - `libuv`：内部 direct call 到 `notdec_native_9d80`，外部 PLT call 到 `pthread_key_delete`。
4. 不检查完整 IR，不检查所有函数数量的精确值，避免把 smoke 变成脆弱快照。

暂时不做：

- 不要求所有 helper call 消失。
- 不检查参数和返回值签名。
- 不引入 Python 依赖。

## 判断标准

1. Bench2 smoke 在三个固定目标上继续通过。
2. 如果 direct call 回退成 helper，脚本能失败并指出缺失的 pattern。
3. 仍使用本地 LLVM 22，不使用系统 LLVM。

## 风险

1. 这些检查绑定了当前 Bench2 样本和当前 symbol 命名。后续如果 discovery 策略扩大、函数名去重变化，
   需要同步更新 smoke。
2. 检查过宽会漏掉退化，检查过窄会造成无意义失败。本次只检查最近已实现且在三个样本里稳定的行为。

## 实现记录

### 修改文件和函数

1. `scripts/bench2-native-smoke.sh:67`
   - 新增 `require_ir_pattern(...)`，用最小字符串匹配检查 IR 里的关键 direct call 形态。
2. `scripts/bench2-native-smoke.sh:78`
   - 新增 `check_ir_features(...)`。
   - `vsftpd` 检查两条内部 direct call。
   - `libuv` 检查一条内部 direct call 和一条 PLT 外部 call。
   - `memcached` 检查两条内部 direct call。
3. `scripts/bench2-native-smoke.sh:143`
   - 在 `llvm-as` 和 `opt -passes=verify` 之后追加 IR feature 检查，保持原有 smoke 流程不变。
4. `ARCHITECTURE.md:119`
   - 记录 Bench2 smoke 不只做 verify，还会检查当前已实现的关键 native IR 形态。
5. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:44`
   - 更新 Stage 7 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-36
rg -n "notdec_native_5ba0|notdec_native_8290|notdec_native_9d80|pthread_key_delete|notdec_native_5b80|notdec_native_b950" \
  /tmp/notdec-bin2llvm-bench2-smoke-20260521-36/*.all-confirmed.ll
```

### Bench2 结果

三个目标都通过。

```text
vsftpd ok elapsed=8s
libuv ok elapsed=7s
memcached ok elapsed=8s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=297, unresolved_indirect_flows=7
libuv: confirmed_functions=9, basic_blocks=29, instructions=85, xrefs.total=24, unresolved_indirect_flows=2
memcached: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=179, unresolved_indirect_flows=7
```

IR 里对应 pattern 都能找到：

```text
vsftpd: call void @notdec_native_5ba0(), call void @notdec_native_8290()
libuv: call void @notdec_native_9d80(), call void @pthread_key_delete()
memcached: call void @notdec_native_5b80(), call void @notdec_native_b950()
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只给 smoke 增加了固定 pattern 检查，没有改 lowering 主路径。三目标总耗时仍在 20 秒上下。

实现效果：3/5。能抓住最近补上的关键 IR 退化。
复杂度：1/5。只是脚本里多两个小函数和三组固定 pattern。
维护成本：2/5。后续如果 symbol 命名变化，需要同步更新 pattern。
