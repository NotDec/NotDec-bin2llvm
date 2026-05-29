# 20260529-02 Stack Param Storage Match

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 stack 参数也是 `ParamEntry` / `ParamListStandard` 的一部分：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
- 关键数据结构：`ParamEntry`、`ParamListStandard`、`ParamEntryResolver`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
- 关键函数：
  - `ParamEntry::decode(...)`：读取 `<pentry minsize maxsize align>` 和 `<addr offset space>`，保存 `addressbase`、`size`、`alignment`。
  - `ParamEntry::getAddrBySlot(...)`：对有 `align` 的 stack entry 按 slot 计算实际地址。
  - `ParamEntry::justifiedContain(...)`：判断一个地址范围是否落在 entry 范围内，并处理 align/right justify。
  - `ParamListStandard::populateResolver(...)`：把 entry range 放进 resolver。
  - `ParamListStandard::findEntry(...)`：用地址和 size 找匹配的 entry。

x86-64 SysV 的 stack 参数 entry 在：

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`
- 默认 prototype `<input>` 里有 `<pentry minsize="1" maxsize="500" align="8"><addr offset="8" space="stack"/></pentry>`

这表示返回地址之后，从 stack offset 8 开始按 8 字节 alignment 分配栈参数。

## native 复刻方式

这一步只补当前 ABI 子集需要的 stack storage 查询：

- 在 `NativePrototypeModel` 新增 `findInputStack(space, offset, size)`。
- 只查 input stack pentry；x86-64 SysV 默认 output 没有 stack output。
- 判断规则先保守：
  - storage kind 必须是 `Stack`。
  - space 必须相同。
  - 查询 offset 必须在 `[entry.offset, entry.offset + maxsize)` 内。
  - size 必须满足 `minsize <= size <= maxsize`。
  - 如果 entry 有 align，查询 offset 相对 entry.offset 必须按 align 对齐。
- 返回同一个 `NativeStorageMatch`，slot 仍是 cspec pentry 顺序。

暂不做：

- 不实现 `getAddrBySlot(...)` 的 slot 消耗和反向栈分配。
- 不做 right-justify/endian 处理。
- 不把 stack 参数接入后续 prototype candidate。GOAL 已说明当前底座第一阶段主要覆盖 register 参数和 register 返回值。

## 判断标准

- `findInputStack("stack", 8, 8)` 能匹配 x86-64 SysV stack pentry。
- `findInputStack("stack", 16, 8)` 能匹配同一个 stack pentry。
- `findInputStack("stack", 9, 8)` 不能匹配，因为未按 8 字节对齐。
- `findInputStack("stack", 0, 8)` 不能匹配，因为 offset 0 是 return address。

## 实现记录

修改文件：

- `include/notdec-bin2llvm/NativePrototypeModel.h:30`：新增 `findInputStack(...)`。
- `include/notdec-bin2llvm/NativePrototypeModel.h:38`：新增私有 `findStack(...)`。
- `lib/NativePrototypeModel.cpp:17`：实现 `findInputStack(...)`。
- `lib/NativePrototypeModel.cpp:38`：实现 stack storage 查询，检查 space、offset 范围、size 和 align。
- `tests/native_prototype_model_test.cpp:40`：新增 `stack8` / `stack16` 匹配样例。
- `tests/native_prototype_model_test.cpp:60`：新增 stack offset 8、16、9、0 的断言。

验证命令：

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register)' -V
```

结果：通过。`notdec.native_abi.cspec` 和 `notdec.native_prototype_model.register` 都通过，总用时 0.05 秒。

## 风险

- 这里只实现 stack pentry 的保守 range/align 查询，不等价于 Ghidra 完整 `getAddrBySlot(...)`。
- 当前还没有区分 stack 参数序号，返回的 slot 是 cspec pentry 顺序。
- GOAL 里已说明第一阶段主要覆盖 register 参数和 register 返回值，所以这一步只是补齐 storage model 的基础能力，不会马上接 candidate recovery。

## 评分

- 实现效果：6/10。覆盖 x86-64 SysV 当前 stack pentry 的基本匹配。
- 复杂度：2/10。只加一个查询函数和测试断言。
- 维护成本：3/10。后续实现 slot 分配时可能需要调整内部逻辑，但外部接口可以保留。
