# 20260529-09 Instcombine Prototype Metadata Safety

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 prototype recovery 先依赖 heritage SSA 和 call effect 得到稳定的 input/output storage，再由 trial 规则筛出参数和返回值。中间的表达式简化不能破坏 storage 身份。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveParam::apply(...)`：收集并筛选活跃参数 trial。
  - `ActionPrototypeTypes::apply(...)`：推动 prototype 类型恢复。
  - `ActionFuncLink::apply(...)`：把 callsite 和 callee prototype 关联。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamActive` / `ParamTrial`：保存候选 storage、活动状态和命中信息。
  - `FuncProto`：保存函数 prototype 和 storage effect。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::registerTrial(...)`、`ParamActive::whichTrial(...)`：维护参数 trial。
  - `FuncProto::hasEffect(...)`：查询 call 对 storage 的影响。

native 侧当前用 `notdec.register.external_inputs` 和 `notdec.prototype.*_candidates` metadata 承接这些信息。`instcombine` 放进链路前，需要确认局部规整后仍能产出参数和返回候选。

## native 复刻方式

这一步只扩展安全测试，不改正式 pipeline：

- 构造一个函数：load `RDI`，做简单 `add`，store 到 `RAX`，然后返回。
- module 上写入测试 ABI：`RDI` 是 input slot 0，`RAX` 是 output slot 0。
- 先跑 `InstCombinePass`，再跑 `NativeRegisterSSA`，最后跑 `NativePrototypeRecovery`。
- 检查函数上仍有 `notdec.prototype.input_candidates = RDI` 和 `notdec.prototype.return_candidates = RAX`。

## 判断标准

- `native_instcombine_metadata_test` 通过。
- instcombine 后 prototype recovery 仍能标出 RDI 输入候选。
- instcombine 后 prototype recovery 仍能标出 RAX 返回候选。

## 实现记录

### 改动

- `tests/native_instcombine_metadata_test.cpp:1` 到 `tests/native_instcombine_metadata_test.cpp:2`：引入 `NativeAbi` 和 `NativePrototypeRecovery`，让 instcombine 安全测试能直接覆盖 prototype recovery metadata。
- `tests/native_instcombine_metadata_test.cpp:52` 到 `tests/native_instcombine_metadata_test.cpp:69`：新增 `registerParamEntry(...)` 和 `attachPrototypeTestAbi(...)`，给测试 module 写入最小 ABI：`RDI` input、`RAX` output。
- `tests/native_instcombine_metadata_test.cpp:94` 到 `tests/native_instcombine_metadata_test.cpp:119`：新增 `createPrototypeCandidateModule(...)`，构造 `load RDI -> add -> store RAX -> ret` 样例。
- `tests/native_instcombine_metadata_test.cpp:545` 到 `tests/native_instcombine_metadata_test.cpp:563`：对样例函数先跑 `InstCombinePass`，再跑 `NativeRegisterSSA` 和 `NativePrototypeRecovery`，并验证 module。
- `tests/native_instcombine_metadata_test.cpp:565` 到 `tests/native_instcombine_metadata_test.cpp:576`：断言 input/return candidate 计数存在，并检查函数 metadata 中有 `RDI` 输入候选和 `RAX` 返回候选。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_instcombine_metadata_test native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved|instcombine.metadata)' -V
```

结果：通过。5 个测试全部通过，总用时 0.13 秒。

### 性能影响

只扩展测试，不改运行时 pipeline，没有运行时性能影响。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：6/10。覆盖 instcombine 后参数和返回候选 metadata 的最小链路，但还没覆盖多 return、callsite 或真实 Bench2 输入。
- 复杂度：3/10。新增测试 ABI helper 和一个小 module 构造函数。
- 维护成本：3/10。测试仍集中在 instcombine 安全文件中，后续能继续扩展。

### 后续

- 覆盖 instcombine 后多 return 返回候选稳定性。
- 再决定是否把 instcombine 放进 native prototype recovery pipeline。
