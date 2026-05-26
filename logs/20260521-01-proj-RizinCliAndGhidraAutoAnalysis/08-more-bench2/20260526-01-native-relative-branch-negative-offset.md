# Native Relative Branch Negative Offset

## 原始 prompt

尝试看看怎么修复

## 背景

正式 selected targets 全量导出后，`python` 日志里还剩一个被跳过的函数：

```text
skip native function 0x1bcfa0: BRANCH target must be direct ram or relative const
```

这个问题不影响 `notdec_pcode_` helper 残留，但说明 raw P-Code terminator lowering 有一个相对分支场景没覆盖。

## 定位

入口指令：

```bash
objdump -d --start-address=0x1bcf80 --stop-address=0x1bcfc0 \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libpython3.12.so.1.0
```

`0x1bcfa0` 是普通函数入口，开头 P-Code 也正常：

```text
CBRANCH (ram,0x1bd0b0,8) (register,0x206,1)
```

实际失败点来自函数内部 raw P-Code：

```text
BRANCH (const,0xfffffffc,4)
```

这是 Sleigh 的相对 P-Code 跳转，`0xfffffffc` 应按 4 字节有符号数解释为 `-4`。旧代码把 const offset 当无符号数处理，所以把它当成巨大正数，最后报 “BRANCH target must be direct ram or relative const”。

## 实现

修改 `lib/PcodeToLLVM.cpp:140-179`：

- `relativeTargetIndex(...)` 不再直接用无符号 `Offset`。
- 新增 `relativeBranchOffset(...)`，按 const varnode 的 byte size 做符号扩展。
- 支持负 offset，比如 `0xfffffffc:4` -> `-4`。
- 保留越界检查，负 offset 不能跳到 op 列表前面，正 offset 不能跳出 op 列表。

## 验证

单函数复现修复：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libpython3.12.so.1.0 \
  -f 0x1bcfa0 \
  -o /tmp/notdec-python-1bcfa0-check/function.ll

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-python-1bcfa0-check/function.ll \
  -o /tmp/notdec-python-1bcfa0-check/function.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-python-1bcfa0-check/function.bc \
  -o /tmp/notdec-python-1bcfa0-check/function.verified.bc
```

结果：通过，`native-llvm.log` 没有 `BRANCH target must be direct ram`。

重跑 `python` 全量正式产物：

```text
TIME python-native-llvm-full-branch-fix 1665.46
```

检查：

```bash
rg -n 'skip native function 0x1bcfa0|BRANCH target must be direct ram|notdec_pcode_' \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/python/shared-library/module-all.native-llvm.log \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/python/shared-library/module-all.ll
```

结果：无命中。

全 selected targets helper 检查：

```bash
rg -n 'notdec_pcode_' \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native -g '*.ll'
```

结果：`no_helpers`。

CTest：

```bash
ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec.native_discover.x86_64_smoke|notdec.native_llvm.x86_64_smoke' \
  --output-on-failure
```

结果：

```text
notdec.native_discover.x86_64_smoke Passed 0.23 sec
notdec.native_llvm.x86_64_smoke     Passed 0.60 sec
Total Test time                     0.84 sec
```

## 结论

`python` 的 `0x1bcfa0` 跳过问题来自相对 P-Code `BRANCH const` 的负 offset 解析错误。修复后该函数可以 lowered，`python` 全量正式产物通过 LLVM 22 verify，日志里不再出现这个 skip。

## 评分

- 实现效果：9/10。直接修掉已知 skip，并保持 selected targets 无 helper 残留。
- 复杂度：8/10。只改相对 branch offset 解析，范围很小。
- 维护成本：8/10。逻辑符合 Sleigh relative branch 语义，后续同类负 offset 不需要再特判。
