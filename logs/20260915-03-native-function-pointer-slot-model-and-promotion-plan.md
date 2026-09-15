# 原始 prompt

```text
感觉应该分两阶段。low-level阶段就按照这样说的做，但是后面还是需要搞个pass匹配这种取函数指针的方式，转换为直接的函数调用，这样暴露成直接的函数调用更方便后续的分析？虽然说全局的地址槽位有可能会被修改。但是大概率还是不会被修改的吧？可以做一些简单的检测，它是否违反了我们的期望
```

后续确认：按这个方向推进，先做 low-level relocation function pointer slot
建模，再做 function pointer promotion pass。

# 背景

上一轮已经做到：

- 代码 immediate 函数地址会实体化成 `ptrtoint @function`；
- relocation 目标函数用 `@llvm.used` 保留 body。

但数据段里的 relocation function pointer slot 还是裸内存语义：

```llvm
%fn = load i64, ptr inttoptr (i64 82624 to ptr) ; 0x142c0 -> sock_connect
%p  = inttoptr i64 %fn to ptr
%r  = call i64 %p(...)
```

函数体和 slot 没有 LLVM 语义联系，间接调用也无法被后续分析解析。

# 目标

1. **low-level 阶段**：函数指针 slot 变成真实 LLVM global，initializer 是
   `ptrtoint(@function)`。
2. 模块内直接访问该 slot 的 load/store 改到 global 上，写点可见。
3. **promotion 阶段**：对 `load slot -> inttoptr -> call` 的间接调用，在
   “唯一目标、无未知写入、地址未 escape” 时提升为 direct call。
4. 多目标 / 未知写入不强行提升，只保留 indirect call 并计数。

# 不做什么

- 不把整个 data image 提升成 LLVM global；`base + offset`、跨函数传递的
  `parser_settings` 指针暂未覆盖。
- 不做 guarded multi-target promotion；第一版只做单目标，多目标计入 counter。
- 不移除 `@llvm.used` 兜底。pointer table 中尚未被 direct slot 访问建模的函数
  仍需要它保 body。

# 实现

## 1. NativeRelocationData

文件：

- `include/notdec-bin2llvm/NativeRelocationData.h`
- `lib/NativeRelocationData.cpp`

API：

```cpp
materializeNativeFunctionPointerSlots(Module &, ArrayRef<NativeFunctionPointerSlot>)
```

对每个 `(address, width, functionName)`：

1. 查找 `module.getFunction(functionName)`；
2. 建 internal global：

   ```llvm
   @notdec.reloc.0x142c0 = internal global i64 ptrtoint (ptr @sock_connect to i64)
   ```

3. 扫描所有 load/store，如果 pointer operand 恰为
   `inttoptr(constant == slot.Address)`，且 access size 等于 pointer width，
   把 pointer operand 换成该 global。

工具层 `tools/notdec-native-llvm.cpp` 从
`selectedState->relocatedPointers()` 构建 slot 列表；target 地址命中
`planNativeCallTargets().Direct` 时填入对应的 LLVM function name。

## 2. NativeFunctionPointerPromotion

文件：

- `include/notdec-bin2llvm/NativeFunctionPointerPromotion.h`
- `lib/NativeFunctionPointerPromotion.cpp`

API：

```cpp
runNativeFunctionPointerPromotion(Module &)
```

流程：

1. 收集所有 `notdec.reloc.*` global 的候选目标：
   - initializer `ptrtoint(@function)` 加入候选；
   - 遍历 global users：
     - load 必须是直接指向该 global；
     - store 必须是直接指向该 global，且 store value 也必须是
       `ptrtoint(@function)`；
     - 其他 user / 非 constant store 标记 Unknown。
2. 扫描间接 `CallInst`，从 callee operand 反查
   `inttoptr/bitcast -> load notdec.reloc.*`。
3. 只有 `!Unknown && candidates.size() == 1` 且目标有 body 时，替换 callee
   为直接 function，保留原 call 的 FunctionType / 参数 / calling convention /
   attributes / tail kind。
4. Unknown / 多目标只累计 counter，不重写。

工具流水线位置：

```text
PcodeToLLVM
  -> NativeRelocationData
  -> InstCombine
  -> NativeRegisterSummarySSA
  -> InstCombine
  -> NativeFunctionPointerPromotion
  -> post-rewrite peephole
  -> final cleanup
```

## 3. 测试

`tests/pcode_to_llvm_test.cpp` 新增：

- `testRelocatedFunctionPointerSlotBecomesGlobal`
  - 验证 slot global 的 initializer 是 `ptrtoint(@slot_callee)`；
  - load/store 的 pointer operand 被替换成 global。
- `testFunctionPointerSlotPromotesToDirectCall`
  - 无写入的 slot + indirect call -> 重写成 direct call。
- `testModifiedFunctionPointerSlotIsNotPromoted`
  - slot 被未知 store 写过后 -> 不提升，summary 记 unknown writes。

# 验证

## wrk

命令：

```bash
build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  --all-confirmed \
  --register-ssa-summary \
  --register-ssa-warning-out /tmp/notdec-reloc3/wrk.warn.tsv \
  -o /tmp/notdec-reloc3/wrk.native.ll
```

summary：

```text
Native relocation slots: created=20 reused=0 accesses=24
Native function pointer promotion: seen=54 promoted=20 slots=20 unknown_writes=0 multiple_targets=0
```

结果：

- 20 个 relocation slot 建成 `ptrtoint(@function)` global；
- 24 个 direct `inttoptr(slot)` load/store 被改到 global；
- 20 个间接调用提升为 direct call；
- wrk 中出现：

  ```llvm
  call i64 @sock_connect(...)
  call i64 @sock_read(...)
  call i64 @sock_write(...)
  call i64 @sock_readable(...)
  call void @sock_close(...)
  ```

- warning TSV 与上一版逐行内容集合一致（353 条，0 added / 0 removed）；
- `RSP.entry` 仍然为 0；
- residue audit 仍只有 1 条旧的 `FUN_a260` `FS_OFFSET` load；
- LLVM 22 `llvm-as` + `opt -passes=verify` 通过。

## 回归

- `pcode_to_llvm_test`：通过。
- `native_register_summary_ssa_test`：通过。
- `ctest --test-dir build --output-on-failure`：12/12 通过。
- fortune x86_64：define 13，warning 35 行完全一致，residue audit 0 条。
- fortune i386：define 40，warning 9 行完全一致，residue 记录一致。

## 性能

wrk 全量：

- 上一版 address-taken 提交：`1:27.20`、RSS `137960KB`；
- 本轮：`1:27.23`、RSS `138344KB`。

耗时和内存基本不变。

# 评分与下一步

- 实现效果：8/10。wrk 的 `sock_*` 间接调用已经变成 direct call，low-level
  slot 也有真实函数 initializer。
- 复杂度：6/10。两个 pass 都保持小范围、规则明确；promotion 只做单目标。
- 维护成本：5/10。后续 data image/guarded promotion 需要扩展这两个 pass，
  当前接口已经把 slot 和目标分析分开。

下一步：

1. 把 data image / `.data` relocation 槽位提升成更大范围的 LLVM global，
   覆盖 `base + offset` 和 `parser_settings` 跨函数指针传递。
2. 在多目标场景加入 guarded promotion（icmp/switch + fallback indirect call）。
3. 等数据引用完整后移除 `@llvm.used` 兜底。
4. 清理 `FUN_a260` 的 `FS_OFFSET` residue。
