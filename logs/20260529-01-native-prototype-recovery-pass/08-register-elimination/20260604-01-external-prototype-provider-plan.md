# 原始 prompt

将当前规划写到一个新的logs/下的文件，然后开始推进

# 背景

当前 native RSP/RBP 清理已经处理了 canary、caller-frame、内部 helper 死参数、`abort` no-return 这几类明确语义。

扩展到 `libuv:shared-library` 和 `wolfssl:shared-library` 后，剩余一批 `RSP call_frame_state` 来自普通外部声明调用，例如：

```llvm
store i64 %1, ptr @RSP
%2 = call i64 @pthread_mutex_lock()
```

真实 ABI 下 `pthread_mutex_lock` 有参数，参数一般在 `RDI`。但当前 IR 里的声明没有参数：

```llvm
declare i64 @pthread_mutex_lock()
```

所以现有 declaration rewrite 如果只靠 call 前 `store @RDI` 推断参数，会漏掉“沿用 caller 入口 `RDI`”的情况。

# 目标

参照 Ghidra 的路线：先有外部函数签名，再用 compiler spec / ABI model 映射到参数位置。

当前只做最小版本：

- 增加 external prototype provider。
- provider 有签名时优先使用它给出的参数数量和 ABI slot。
- 参数位置仍由 `NativePrototypeModel` 从当前 cspec 推出。
- store 推断只作为没有 provider 时的兜底。

# 路线

1. 在 native prototype recovery 内新增小型 provider。
   - 输入外部函数名。
   - 输出固定参数个数。
   - 当前参数统一按 ABI integer/pointer slot，用 `i64` 表示。

2. provider 优先规则。
   - 如果 provider 有签名，按 provider 参数数生成 `RDI/RSI/RDX/...` 输入。
   - 如果某个参数值取不到，则跳过该 declaration。
   - 如果 call 前 store 推断出比 provider 更多的 ABI 参数，先跳过，避免表错或 varargs 情况被误改。

3. 没有 provider 时，保留现有 store 推断。

4. 先覆盖当前 shared library residue 里反复出现的外部函数。
   - `pthread_mutex_*`
   - `pthread_rwlock_*`
   - `pthread_cond_*`
   - `pthread_barrier_*`
   - `pthread_once`
   - `pthread_join`
   - `sem_post`
   - `sched_getcpu`
   - `chdir`
   - `kill`
   - `unsetenv`
   - `strlen`
   - `gnu_get_libc_version`
   - `fopen`
   - `send`
   - `recv`
   - `__tls_get_addr`

# 判断标准

- 单测覆盖：
  - `pthread_mutex_lock` call 前没有 `RDI` store，但 caller 有入口 `RDI`，rewrite 后 declaration/callsite 带 1 个参数。
  - call 前 store 推断数量大于 provider 时跳过。
- `native_prototype_recovery_test` 通过。
- 相关 native CTest 通过。
- Bench2 shared library gate 至少跑：
  - `libuv:shared-library`
  - `wolfssl:shared-library`
- residue audit 对比 `RSP/RBP` 下降，且不通过扩大 no-stack 白名单删除普通外部调用。

# 风险

- 手工表可能不完整。
- 有些函数是 varargs 或平台相关签名，当前先不支持。
- provider 只能解决普通外部声明参数缺失；`stack_frame_state`、`frame_base_state`、`chunk_phi` 仍要继续按本地栈帧和函数 chunk 处理。

# 不做什么

- 不接 DWARF / Ghidra datatype archive。
- 不把普通外部函数加入 no-stack 白名单。
- 不改变内部函数 prototype recovery 的核心推断规则。

# 2026-06-04 实现记录

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:802)
  - 新增 `knownExternalPrototypeInputCount()` / `knownExternalPrototypeInputs()`。
  - 当前 provider 只记录固定 integer/pointer 参数个数，实际 ABI slot 仍从 `NativeAbiSpec` / `NativePrototypeModel` 推出。
  - 覆盖 `pthread_*`、`send`、`recv`、`fopen`、`strlen`、`__tls_get_addr`、`sched_getcpu` 等当前 shared library residue 里反复出现的外部声明。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1075)
  - 新增 provider 安全检查：call 前显式 store 推断出的参数不能超过 provider，也不能落在 provider 之外的 ABI slot。
  - 新增 `providerDeclarationInputSuffix()`，允许 declaration 已有前缀参数时只补 provider 缺失后缀。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1127)
  - 新增 `annotateKnownExternalDeclarationPrototypes()`，对已经匹配 provider 形状的 declaration 写回 `notdec.prototype.recovered`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1201)
  - `collectDeclarationCallInputRewrites()` 优先走 provider 路径；没有 provider 时保留原来的 call 前 store 推断。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:6001)
  - signature rewrite 流程改为传入 ABI，先重写 declaration call input，再标注已知 external declaration prototype，后续 stack-frame cleanup 可以据此删掉无 stack 参数的 call-frame `RSP` store。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:7333)
  - 新增 provider 正例：`pthread_mutex_lock()` 没有 call 前 `RDI` store，但 caller 有入口 `RDI`，rewrite 后 declaration/callsite 带 1 个 `i64` 参数。
  - 新增 provider 负例：call 前显式推断出 `RDI/RSI`，但 provider 只有 1 个参数时跳过，避免误改 varargs 或表不准形态。
  - 新增 RSP call-frame cleanup 正例：provider 成功补参数后，普通 declaration call 前无用 `RSP` store 可被删除。
  - 新增 0 参数 provider 正例：`sched_getcpu` 不改变 call arity，但允许删除无 stack 参数的 `RSP` call-frame store。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:9872)
  - 在已有 declaration `RSP` store cleanup 大测试里补 `sched_getcpu` provider case。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-external-prototype-provider-gate \
  --target libuv:shared-library \
  --target wolfssl:shared-library

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-bin2llvm-external-prototype-provider-gate/wolfssl-shared-library.signature-rewrite.ll \
  -o /tmp/notdec-bin2llvm-external-prototype-provider-gate/wolfssl-shared-library.signature-rewrite.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-bin2llvm-external-prototype-provider-gate/wolfssl-shared-library.signature-rewrite.bc \
  -o /tmp/notdec-bin2llvm-external-prototype-provider-gate/wolfssl-shared-library.signature-rewrite.opt.bc

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-external-prototype-provider-gate/libuv-shared-library.signature-rewrite.ll \
  /tmp/notdec-bin2llvm-external-prototype-provider-gate/wolfssl-shared-library.signature-rewrite.ll
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `libuv:shared-library` 完整 gate 通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `libuv:shared-library` | 105s | 114s |

- `wolfssl:shared-library` 这轮全量 audit 没完成：`notdec-native-llvm --all-confirmed` 跑到 18 分钟以上仍在 CPU 满载运行，已中断。
- 已生成的 `wolfssl-shared-library.signature-rewrite.ll` 单独用 LLVM 22 `llvm-as` 和 `opt -passes=verify` 通过。
- 当前两个已生成 signature-rewrite IR 的 residue 汇总：

```text
category	access_kind	metadata_kind	shape	value_shape	synthetic	count
flags	load	access	full	full	no	2
flags	load	external_input	full	full	no	1
gpr	load	access	full	full	no	98
gpr	load	access	partial	full	no	12
gpr	load	external_input	full	full	no	269
gpr	store	access	full	full	no	2704
gpr	store	access	partial	full	yes	71
other	load	access	full	full	no	1
other	load	external_input	full	full	no	145
vector	load	access	full	full	no	1
vector	load	external_input	full	full	no	27
vector	store	access	partial	full	yes	1
```

`RSP/RBP` 明细：

```text
libuv:   25 RSP, 4 RBP
wolfssl: 88 RSP, 43 RBP
```

判断：

- provider 路径已经能解决“普通外部声明没有显式参数，导致 call 前 `RSP` call-frame store 不能删”的最小问题。
- 这不是 no-stack 白名单扩张：`pthread_mutex_lock` 这类 declaration 会被补成真实 ABI register 参数，再由后续 cleanup 判断 `RSP` store 是否无用。
- `libuv` 全量结果通过，但 `RSP/RBP` 没有清零，说明剩余还有 `stack_frame_state`、`frame_base_state`、`chunk_phi` 或未覆盖原型。
- `wolfssl` all-confirmed 耗时没有收敛，不能把这轮记成完整 shared-library gate 通过。下一步如果继续用 `wolfssl`，应先做 seed-limited gate 或定位 all-confirmed 性能边界。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 能补当前反复出现的一批外部 declaration 参数，并带动一类 call-frame `RSP` cleanup；但 shared library residue 还没清零。 |
| 理解成本 | 3 | 增加了 provider 分支，但仍复用现有 declaration rewrite 和 ABI slot 模型。 |
| 维护成本 | 3 | 手工表需要维护；后续更好的来源应是 Ghidra datatype/debug info/符号原型库。 |

有没有更好的方案：

- 更完整方案是接外部符号原型来源，而不是继续扩手工表。
- 当前先用小表，是因为它能验证路线：原型来源和 ABI slot 分离，provider 只告诉参数个数，storage 仍由 cspec 决定。
