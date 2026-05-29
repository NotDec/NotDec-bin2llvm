# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前已经支持：

- 单 input + 单 return 的 direct callsite 重写。
- return-only 多返回的 struct result + `extractvalue` 重写。

缺口是 input + 多返回。真实 ABI 里这很常见，例如一个参数经 RDI 传入，返回值经 RAX/RDX 传出。当前统一 dispatch 会把这种 recovered prototype 判成 unsupported shape。

# Ghidra 实现参考

Ghidra 把 callsite 输入和输出都挂在同一个 `FuncCallSpecs` 上处理：

- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::funcLinkInput(...)`：为 sub-function call 初始化 input recovery，已知 prototype 时插入输入 varnode，未知时准备 `ParamActive`。
  - `ActionFuncLink::funcLinkOutput(...)`：为 sub-function call 初始化 output recovery，已知非 void 输出时给 CALL 设置 output varnode，未知时准备 output trials。
  - `ActionReturnRecovery::buildReturnOutput(...)`：按 `ParamActive` 的 trial 顺序把函数 return 的多个输出 piece 接成返回值。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildInputFromTrials(...)`：按 used input trial 顺序重排 CALL 输入。
  - `FuncCallSpecs::buildOutputFromTrials(...)`：按 used output trial 顺序把 call 后的 output trial 移到 CALL output；两个 output trial 时用 whole + subpiece 表达。
  - `FuncCallSpecs::findPreexistingWhole(...)`：已有 piece whole 时复用。

native 侧已经把 Ghidra 的 CALL output whole 映射成 LLVM struct return，把 subpiece 映射成 `extractvalue`。这一步只把已有 input 参数收集和多返回 output 拆分合到一个窄 helper。

# native 侧复刻策略

先支持当前测试需要的最小形状：

- 原始函数必须还是 `void()`。
- recovered prototype 必须是 1 个 i64 register input + 2 个以上 i64 register returns。
- callee 内部必须能绑定唯一 external input load 和每个 return register store。
- 每个 user 必须是 direct `void` call。
- callsite 参数沿用 `callsiteInputValueBeforeCall(...)`，只接受同 block 或唯一前驱中的 register store value。
- callsite 返回沿用 `findCallsiteReturnLoad(...)`，每个返回寄存器都必须能在线性安全路径上找到对应 load。
- 新 call 类型是 `{ i64, ... } (i64)`，用参数 value 建 call，再用 `extractvalue` 替换旧返回寄存器 load。

暂时不做：

- 多 input + 多 return。
- 缺少某个返回 load 的部分重写。
- 分支/PHI 返回 load。
- piece/join 类型恢复，只保留当前 i64 register 粒度。

# 判断标准

- RDI input + RAX/RDX return 的 callee 能重写成 `{ i64, i64 } (i64)`。
- direct caller 的新 call 带一个参数，返回 struct。
- caller 里旧 RAX/RDX load 被 `extractvalue` 替换并删除。
- 现有单 input、单 return、多 return 测试继续通过。

# 风险

- 这一步复用现有线性 CFG 判断，真实多分支 caller 会保守跳过。
- helper 会和现有单 input、单 return、多 return 逻辑有少量重复；等更多形状稳定后再统一收敛。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:321` 新增 `InputMultiReturnCallsiteRewrite`，把一个 old direct call、一个参数 value 和多个旧 return load 放在一起。
- `lib/passes/NativePrototypeRecovery.cpp:330` 新增 `InputMultiReturnCallsiteCollectionResult`，保留收集结果和失败原因。
- `lib/passes/NativePrototypeRecovery.cpp:525` 新增 `collectInputMultiReturnDirectCallsites(...)`，同时检查 callsite input value 和每个 return register load。
- `lib/passes/NativePrototypeRecovery.cpp:566` 新增 `rewriteInputMultiReturnDirectCallsites(...)`，创建 `{ i64, ... } (i64)` 新 call，并用 `extractvalue` 替换旧返回寄存器 load。
- `lib/passes/NativePrototypeRecovery.cpp:1413` 新增 `rewriteNativeRecoveredPrototypeInputMultiReturn(...)`，重写 callee 参数、聚合多个返回值，并接上 direct callsite 重写。
- `lib/passes/NativePrototypeRecovery.cpp:1560` 在统一 dispatch 中接入 `Inputs.size()==1 && Returns.size()>1`。
- `tests/native_prototype_recovery_test.cpp:187` 新增 `createInputStoreTwoReturnLoadCallerFunction(...)`，构造 caller 写 RDI、调用、读取 RAX/RDX。
- `tests/native_prototype_recovery_test.cpp:751` 新增 `createInputTwoOutputReturnStoreFunction(...)`，构造 callee 使用 RDI 并写 RAX/RDX。
- `tests/native_prototype_recovery_test.cpp:1015` 新增 input + multi-return 测试函数和 caller。
- `tests/native_prototype_recovery_test.cpp:2112` 验证 callee 重写为 struct return + one i64 input，caller 新 call 带参数，并生成两个 `extractvalue`。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
git diff --check
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
```

结果：通过，`1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`。

性能：只在显式签名重写遇到单 input + 多 return 函数时遍历 direct users，并复用已有 input value 查找和 return load 线性查找。默认 metadata recovery 不增加额外扫描。目标测试耗时约 `0.04 sec`。

评分：

- 实现效果：7/10。单 input + 多返回 direct callsite 已能完成参数和 struct 返回值重写。
- 复杂度：6/10。新增一个窄 helper，和现有 input-return、多返回 helper 有重复，但边界清楚。
- 维护成本：6/10。后续支持多 input 时需要把 input callsite 收集从单参数扩展成参数列表。
