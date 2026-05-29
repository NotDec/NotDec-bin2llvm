# Progress

## 当前状态

- [ ] 阶段 1: `01-cspec-abi-model`
  - [x] 编写第一小步规划：复用 Ghidra XML 基础解析 x86-64 `.cspec` ABI 子集。
    - 记录：`01-cspec-abi-model/20260529-01-default-proto-register-abi.md`
  - [x] 实现 `NativeAbiSpec` / `NativeAbiStorage` / `NativeAbiParamEntry` / `NativeAbiEffect`。
    - 文件：`include/notdec-bin2llvm/NativeAbi.h`、`lib/NativeAbi.cpp`
  - [x] 输出 module 级 `!notdec.abi` metadata。
    - 文件：`lib/NativeAbi.cpp`、`tools/notdec-native-llvm.cpp`
  - [x] 添加 cspec 子集解析单元测试。
    - 记录：`01-cspec-abi-model/20260529-02-cspec-abi-parser-test.md`
    - 文件：`tests/native_abi_cspec_test.cpp`

- [ ] 阶段 2: `02-prototype-storage-model`
  - [x] 编写第一小步规划：复刻 `ParamEntry` / `ParamList` 的 register storage 匹配。
    - 记录：`02-prototype-storage-model/20260529-01-register-param-storage-match.md`
  - [x] 实现 ABI input/output register storage 查询。
    - 文件：`include/notdec-bin2llvm/NativePrototypeModel.h`、`lib/NativePrototypeModel.cpp`
  - [x] 添加 register storage 匹配测试。
    - 文件：`tests/native_prototype_model_test.cpp`
  - [x] 添加 stack storage 匹配测试。
    - 记录：`02-prototype-storage-model/20260529-02-stack-param-storage-match.md`

- [ ] 阶段 3: `03-function-register-effects`
  - [x] 编写第一小步规划：基于 SSA 判断 ABI preserved register。
    - 记录：`03-function-register-effects/20260529-01-preserved-unaffected-registers.md`
  - [x] 标注函数级 `notdec.register.preserves`。
    - 文件：`lib/passes/NativeRegisterSSA.cpp`
  - [x] 标注函数级 `notdec.register.clobbers`。
    - 记录：`03-function-register-effects/20260529-02-clobbered-unaffected-registers.md`
  - [x] 添加 preserved IR 小样例。
    - 文件：`tests/native_register_effects_test.cpp`
  - [x] 添加 clobber IR 小样例。
    - 文件：`tests/native_register_effects_test.cpp`

- [ ] 阶段 4: `04-callsite-effects`
  - [ ] 编写第一小步规划：外部 call 按 ABI killedbycall 处理。
  - [ ] 本模块 direct call 读取 callee metadata。
  - [ ] 未解析 call 保守 fallback。
  - [ ] 添加 callsite clobber IR 小样例。

- [ ] 阶段 5: `05-input-output-candidates`
  - [ ] 编写第一小步规划：从 external input 推出寄存器参数候选。
  - [ ] 标注 `notdec.prototype.input_candidates`。
  - [ ] 标注 `notdec.prototype.return_candidates`。
  - [ ] 添加参数/返回候选 IR 小样例。

- [ ] 阶段 6: `06-pass-pipeline-and-verification`
  - [ ] 编写第一小步规划：CLI 和 pass pipeline 接入。
  - [ ] `notdec-native-llvm` 默认输出 ABI/prototype metadata。
  - [ ] `.ll` / `.bc` 输入支持只跑 prototype recovery。
  - [ ] Bench2 selected native 全量验证。

## 记录规则

1. 每做一小块，先在对应阶段目录创建规划 markdown。
2. 实现完成后，把源码改动、函数、验证命令和结果回写到该规划 markdown。
3. 这里只记录已经完成的真实进度。
4. 如果某个阶段拆出新源码文件，同步更新 `GOAL.md` 的“拟放源码”列表。
