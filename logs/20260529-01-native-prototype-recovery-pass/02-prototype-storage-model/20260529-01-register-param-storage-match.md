# 20260529-01 Register Param Storage Match

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的参数 storage 匹配核心在 Decompiler C++：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
- 关键数据结构：`ParamEntry`、`ParamListStandard`、`ParamEntryResolver`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
- 关键函数：
  - `ParamListStandard::parsePentry(...)`：把 cspec `<pentry>` decode 成 `ParamEntry`。
  - `ParamListStandard::populateResolver(...)`：按 address space 和 range 把 `ParamEntry` 放进 interval map。
  - `ParamListStandard::findEntry(...)`：给定 storage 地址和 size，查找包含它的 `ParamEntry`。
  - `ParamEntry::justifiedContain(...)`：判断给定 range 是否被 entry 包含，并处理 endian/justify。
  - `ParamListRegister::fillinMap(...)`：register 模型里，active trial 只要能 `findEntry(...)` 就可关联 entry。

真实 x86-64 SysV 当前需要的 register 参数在：

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`
- 默认 prototype 的 `<input>`：`RDI`、`RSI`、`RDX`、`RCX`、`R8`、`R9`
- 默认 prototype 的 `<output>`：`RAX`、`RDX`

## native 复刻方式

完整 Ghidra 做法依赖 register address space、offset、size 和 interval resolver。当前 `NativeAbiSpec` 第一版只保存了 cspec register 名字，还没把寄存器名解析到 p-code space/offset/size。

这一步先复刻当前测试用例需要的 register storage 匹配：

- 新增 `NativePrototypeModel`，从 `NativeAbiSpec` 构造。
- 新增 `NativeStorageMatch`，返回匹配到的 slot、size 范围、metatype 和 storage。
- 支持 `findInputRegister(name)` / `findOutputRegister(name)`。
- slot 使用 Ghidra pentry 顺序。为了当前参数恢复先关注整数 ABI 寄存器，测试只断言 `RDI`、`RSI`、`RDX`、`RCX`、`R8`、`R9` 和 `RAX` / `RDX`。
- `RBX` 应不匹配 input/output，但保留在 ABI effect 的 `unaffected`，后续 function-register-effects 阶段使用。

暂不做：

- 不实现 stack 参数匹配。
- 不处理 register alias、offset/size overlap。
- 不实现 Ghidra `justifiedContain(...)` 的 endian/align 语义。
- 不处理 `group` 的互斥分组和 protorule。

这些不是丢弃，只是等 register metadata 能提供 space/offset/size 后再补。

## 判断标准

- 新增 `NativePrototypeModel` 编译进 core。
- 新增 CTest：
  - `RDI` 匹配 input slot。
  - `RAX` 匹配 output slot。
  - `RBX` 不匹配 input/output。
  - `RBX` 仍可通过 ABI effect 判断为 unaffected。

## 实现记录

新增文件：

- `include/notdec-bin2llvm/NativePrototypeModel.h:14`：新增 `NativeStorageMatch`，保存匹配到的 slot 和 ABI entry。
- `include/notdec-bin2llvm/NativePrototypeModel.h:22`：新增 `NativePrototypeModel`。
- `include/notdec-bin2llvm/NativePrototypeModel.h:26`：声明 `findInputRegister(...)`。
- `include/notdec-bin2llvm/NativePrototypeModel.h:28`：声明 `findOutputRegister(...)`。
- `include/notdec-bin2llvm/NativePrototypeModel.h:39`：声明 `nativeAbiHasEffectRegister(...)`。
- `lib/NativePrototypeModel.cpp:7`：实现 `findInputRegister(...)`。
- `lib/NativePrototypeModel.cpp:12`：实现 `findOutputRegister(...)`。
- `lib/NativePrototypeModel.cpp:17`：实现 register 名字匹配，返回 pentry 顺序 slot。
- `lib/NativePrototypeModel.cpp:33`：实现 ABI effect register 查询。
- `tests/native_prototype_model_test.cpp:21`：新增测试入口，读取 `x86-64-gcc.cspec` 后检查 `RDI`、`RAX`、`RBX`。

修改文件：

- `lib/CMakeLists.txt:6`：把 `NativePrototypeModel.cpp` 加入 core。
- `CMakeLists.txt:148`：新增 `native_prototype_model_test`。
- `CMakeLists.txt:157`：新增 CTest `notdec.native_prototype_model.register`。

验证命令：

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register)' -V
```

结果：通过。`notdec.native_abi.cspec` 和 `notdec.native_prototype_model.register` 都通过，总用时 0.05 秒。

## 风险

- 当前 slot 是 cspec `<pentry>` 全顺序，所以 `RDI` 是 slot 8，因为前面有 8 个 XMM float pentry。后续如果要区分整数参数序号，需要新增 storage class / resource section 逻辑。
- 当前 register 匹配只按名字，不处理 `EAX`/`RAX`、`XMM0_Qa`/`XMM0` 这类 alias。等 ABI storage 带上 p-code space/offset/size 后再补 Ghidra `justifiedContain(...)` 风格匹配。
- 栈参数还没有匹配。

## 评分

- 实现效果：6/10。能为当前 register 参数/返回候选提供最小 storage 查询，但还不是完整 Ghidra ParamList。
- 复杂度：2/10。只包了一层 ABI 查询，没有新解析逻辑。
- 维护成本：3/10。接口后续可以保留，内部匹配从名字升级到 range resolver 即可。
