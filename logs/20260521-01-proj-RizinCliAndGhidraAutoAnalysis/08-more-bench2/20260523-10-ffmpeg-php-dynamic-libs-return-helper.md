# Ffmpeg And PHP Dynamic Library Verification

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 当前目标和已有 native 状态

Bench2 计划要求有动态库时优先动态库。之前 `ffmpeg` 和 `php` 已有二进制入口验证，但还缺动态库优先项。

本次选择：

- `ffmpeg`: `/usr/lib/x86_64-linux-gnu/libswresample.so.4.12.100`
- `php`: `/usr/lib/php/20230831/calendar.so`

两者都按 `--limit=5` 做 module 级导出和 lowering。

## Ghidra 相关实现

这次仍使用：

- `ghidra_scripts/ExportHeritageModule.java`
  - 导出 `notdec.heritage-module.v0`
  - `--limit=5`
  - `--timeout=60`

Ghidra auto-analysis 都能完成，两个 module 都是 `5/5` 函数导出成功。

## native 侧复刻策略

这次发现 `ffmpeg/libswresample` 的 RETURN 有一个明显问题：部分函数原型是非 void，但 Ghidra 的 `RETURN` op 没有返回值输入。旧 lowering 会生成 `ret undef`。

`undef` 会让 LLVM 优化器随意选择值，不适合继续作为语义结果。本次改成外部 helper：

```text
notdec_heritage_RETURN_i64()
```

这表示返回值未知，但不再用 LLVM `undef` 表示。

## 判断标准

1. `ffmpeg/libswresample` 和 `php/calendar` 都能导出 `module-limit5` JSON。
2. `notdec-heritage-module-check` 通过。
3. `notdec-heritage-module-llvm` 能 lower `5/5` bodies。
4. IR 里不再出现 `poison / undef / freeze`。
5. LLVM 22 `llvm-as` 和 `opt -passes=verify` 通过。

## 风险

1. `--limit=5` 只覆盖每个动态库的前 5 个内部函数，不代表全库。
2. `notdec_heritage_RETURN_i64` 仍是 helper，后续要继续收紧语义时，需要恢复真实返回寄存器或函数原型。

## 实现记录

### 修改内容

- `lib/HeritageToLLVM.cpp:1703`
  - 函数：`returnValueFor`
  - 对非 void 函数的 `RETURN`，如果 `op.Inputs.size() < 2`，不再返回 `llvm::UndefValue`。
  - 新逻辑创建 `notdec_heritage_RETURN_i<bits>()` helper call，并用 `rememberOpInstruction` 记录 effect metadata。

### 验证

Ghidra 导出：

```bash
/usr/bin/time -f 'GHIDRA_TIME %e' timeout 300s \
  /sn640/ghidra/build/dist/ghidra_11.3.2_DEV/support/analyzeHeadless \
  /tmp/notdec-ffmpeg-swresample-proj NotDecBench2_ffmpeg_swresample_limit5 \
  -import /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libswresample.so.4.12.100 \
  -scriptPath /sn640/NotDec/external/NotDec-bin2llvm/ghidra_scripts \
  -postScript ExportHeritageModule.java /tmp/ffmpeg-swresample-limit5.json \
  --limit=5 --timeout=60

/usr/bin/time -f 'GHIDRA_TIME %e' timeout 300s \
  /sn640/ghidra/build/dist/ghidra_11.3.2_DEV/support/analyzeHeadless \
  /tmp/notdec-php-calendar-proj NotDecBench2_php_calendar_limit5 \
  -import /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/php/20230831/calendar.so \
  -scriptPath /sn640/NotDec/external/NotDec-bin2llvm/ghidra_scripts \
  -postScript ExportHeritageModule.java /tmp/php-calendar-limit5.json \
  --limit=5 --timeout=60
```

结果：

```text
ffmpeg/libswresample: attempted functions: 5, succeeded functions: 5, failed functions: 0, GHIDRA_TIME 12.73
php/calendar: attempted functions: 5, succeeded functions: 5, failed functions: 0, GHIDRA_TIME 8.95
```

module check：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-check /tmp/ffmpeg-swresample-limit5.json
/tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-check /tmp/php-calendar-limit5.json
```

结果：

```text
ffmpeg/libswresample: functions: 5, externals: 11, failures: 0, unknown calls: 0, status: ok
php/calendar: functions: 5, externals: 7, failures: 0, unknown calls: 0, status: ok
```

lower：

```bash
/usr/bin/time -f 'LOWER_TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /tmp/ffmpeg-swresample-limit5.json -o /tmp/ffmpeg-swresample-limit5.ll
/usr/bin/time -f 'LOWER_TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /tmp/php-calendar-limit5.json -o /tmp/php-calendar-limit5.ll
```

结果：

```text
ffmpeg/libswresample: lowered function bodies: 5, failed function bodies: 0, LOWER_TIME 0.10
php/calendar: lowered function bodies: 5, failed function bodies: 0, LOWER_TIME 0.02
```

IR 检查：

```bash
rg -n "poison|undef|freeze|notdec_heritage" /tmp/ffmpeg-swresample-limit5.ll
rg -n "poison|undef|freeze|notdec_heritage" /tmp/php-calendar-limit5.ll
```

结果：

```text
ffmpeg/libswresample:
15:  call void (...) @notdec_heritage_CALLIND_void(i64 0), !notdec.effect !3
409:  %191 = call i64 (...) @notdec_heritage_RETURN_i64(), !notdec.effect !22
450:  %9 = call i64 (...) @notdec_heritage_RETURN_i64(), !notdec.effect !28
527:  %21 = call i64 (...) @notdec_heritage_RETURN_i64(), !notdec.effect !41
553:declare void @notdec_heritage_CALLIND_void(...)
555:declare i64 @notdec_heritage_RETURN_i64(...)

php/calendar:
15:  call void (...) @notdec_heritage_CALLIND_void(i64 0), !notdec.effect !3
71:declare void @notdec_heritage_CALLIND_void(...)
```

没有 `poison / undef / freeze`。

LLVM 22 验证：

```bash
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/ffmpeg-swresample-limit5.ll -o /tmp/ffmpeg-swresample-limit5.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/ffmpeg-swresample-limit5.bc -o /tmp/ffmpeg-swresample-limit5.verified.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/php-calendar-limit5.ll -o /tmp/php-calendar-limit5.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/php-calendar-limit5.bc -o /tmp/php-calendar-limit5.verified.bc
```

结果：

```text
ffmpeg/libswresample: AS_TIME 0.04, VERIFY_TIME 0.04
php/calendar: AS_TIME 0.02, VERIFY_TIME 0.03
```

回归：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/libuv/module-limit5.json \
  -o /tmp/libuv-return-regress.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/libuv-return-regress.ll -o /tmp/libuv-return-regress.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/libuv-return-regress.bc -o /tmp/libuv-return-regress.verified.bc
```

结果：通过。`libuv` 仍是 `5/5` bodies lowered。

### 性能和判断

这次只改变缺失 RETURN 值的 fallback，不影响正常有返回值的 RETURN。

实现效果：4/5。ffmpeg 和 php 都补上了动态库入口；ffmpeg 的 `ret undef` 被 helper 返回值替代。

复杂度：2/5。新增逻辑很小，但 helper 仍是保守表示。

维护成本：2/5。后续如果恢复 ABI 返回寄存器，可替换这个 helper。
