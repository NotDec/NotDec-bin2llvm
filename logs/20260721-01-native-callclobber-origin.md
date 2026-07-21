把参数推断用的事实改成“寄存器 range 的最后定义方式”：的思路是对的，但是概念分类上尽量和静态分析的结果贴近。

struct RegisterOriginBits {
  APInt Entry;
  APInt Local;
  APInt CallProduced;
};

这里，不是Entry的话就是Local？即local是否一定等于not Entry？另外，从拓展静态分析的角度，把这个 call produced 换成CallClobber是否就够了？

按这个改一下试试

# Native callsite origin 改成 CallClobber

## 背景

`CallProduced` 同时表达了两件事：call effect 隐式弄脏寄存器，以及显式写入的值来自 call。这样会把
`malloc` 返回值保存后再写入 `RSI` 的场景误判成 `call_clobber`，导致 unknown external 参数推断漏参。

这里把 callsite 证据贴近现有静态分析事实：`Entry` 是入口值，`Local` 是本函数显式写入，`CallClobber`
只表示 call effect 隐式改写。`Local` 不是 `not Entry`；路径合并后无法保持单一来源时仍是 `Mixed`。

## 实现

- `include/notdec-bin2llvm/passes/summary/NativeRegisterSummary.h:47`：
  `NativeRegisterCallsiteValueOrigin::CallProduced` 改名为 `CallClobber`。
- `lib/passes/summary/NativeRegisterSummary.cpp:94`：
  `RegisterOriginBits` 的第三类改成 `CallClobber`，并删除 `State::CallProducedValues`。
- `lib/passes/summary/NativeRegisterSummary.cpp:860`、`lib/passes/summary/NativeRegisterSummary.cpp:918`：
  range origin 设置和 CFG join 继续按 bit 处理，但第三类改为 `CallClobber`。
- `lib/passes/summary/NativeRegisterSummary.cpp:1364`：
  callsite evidence 映射 `Local`、`Entry`、`CallClobber`，其他组合仍是 `Mixed`。
- `lib/passes/summary/NativeRegisterSummary.cpp:1680`：
  `registerOriginKindForStoredValue()` 只把 entry-derived value 识别为 `Entry`；其他显式 store / partial write 都是 `Local`。
- `lib/passes/summary/NativeRegisterSummary.cpp:1857`、`lib/passes/summary/NativeRegisterSummary.cpp:1890`：
  internal callee effect 和 ABI call clobber 继续写成 `CallClobber`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1744`：
  warning 输出仍显示为 `call_clobber`，但只对应新的 `CallClobber`。
- `tests/native_register_summary_test.cpp:554`：
  增加 `RAX` call value 显式写入 `RSI` 的 evidence 断言，确认它是 `LocalDefinition`。
- `tests/native_register_summary_ssa_test.cpp:4079`：
  binary expression 经显式 store 后按新语义计为参数，直接 clobber / PHI mixed 测试仍保留旧边界。
- `docs/analysis/abstract-interpretation-register-summary.md:348`、`ARCHITECTURE.md:281`：
  更新文档里的来源分类。

## 验证

```bash
cmake --build build --target native_register_summary_test native_register_summary_ssa_test notdec-native-llvm -j4
./build/bin/native_register_summary_test
./build/bin/native_register_summary_ssa_test

OUT=/tmp/notdec-bin2llvm-fortune-callclobber-origin-20260721-124818
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" \
  --all-confirmed --skip-runtime \
  --external-prototypes /sn640/NotDec-Exp/Bench2/bin2llvm-external-prototypes/fortune-executable.external-prototypes.json \
  --register-ssa-warning-out "$OUT/register-ssa-warnings.tsv"
llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -disable-output
python3 external/NotDec-bin2llvm/scripts/native-register-residue-audit.py --details "$OUT/fortune.native.ll"
```

结果：

- `recode_scan_request` 从 `declare i64 @recode_scan_request(i64)` 变成
  `declare i64 @recode_scan_request(i64, i64)`。
- fortune residue audit 只有表头，未发现寄存器残留。
- `notdec.unknown`、`range_unknown`、`freeze`、`summary_return`、`summary_clobber` 都是 0。
- IR 里出现的 `poison/undef` 只来自 LLVM attribute `nocreateundeforpoison` 文本，不是实际值。

### 已知暂不处理

- `FUN_27a0` 仍有一个未使用的 `RAX.arg`。汇编入口是 `push %rax; pop %rax; push %rax; call perror; call exit`，
  这里的 `RAX` 更像栈对齐/占位，不是源码参数。当前先跳过，不为 fortune 这个 case 专门加 cleanup。

## 评价

- 实现效果：8/10。修正了 `recode_scan_request` 漏参，同时保留直接 clobber / mixed 不作为参数证据的边界。
- 复杂度：3/10。删除了 value-level `CallProducedValues`，代码更少。
- 维护成本：3/10。后续如果要更细地区分“显式写但值来自脏寄存器”，应作为诊断信息加，不要再影响参数数量主判断。
