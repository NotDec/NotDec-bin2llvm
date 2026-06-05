# 原始 prompt

按这个继续推进

# 背景

external prototype provider 后，`libuv:shared-library` 的 `RSP/RBP` residue 已经降到 29 行。重新分类后，剩余主要是：

```text
12 call_frame_state
10 stack_frame_state
3  saved_register_restore,call_frame_state
2  frame_base_state
2  chunk_phi
```

其中不少 `call_frame_state` 不是普通外部声明参数缺失，而是 no-return declaration 前的 call-frame 模拟：

```llvm
%RSP.external_input = load i64, ptr @RSP
%0 = add i64 %RSP.external_input, -8
store i64 %0, ptr @RSP
...
%7 = add i64 %0, -8
%ptr = inttoptr i64 %7 to ptr
store i64 41301, ptr %ptr
call void @__assert_fail(...)
```

这类 `RSP` 不是函数真实参数，也不是本地栈变量。它只是为 no-return call 写返回地址和栈顶状态。之前 `abort` 只覆盖 0 参数 no-return declaration，没覆盖 `__assert_fail(...)` 这种有显式参数的 no-return declaration。

# 目标

做一个窄小步：

- 识别固定 no-return declaration，即使它有显式参数。
- 对 no-return declaration 前的 return-address raw stack store 做清理。
- 由此让旧 `RSP.external_input` 和 `store @RSP` 变死后被现有 DCE / dead stack-frame store 清理。

# Ghidra 对应

Ghidra 里 no-return / dead control-flow 不是靠删除寄存器状态碰运气：

- `Funcdata::overrideFlow()` / `FlowInfo` 会把 no-return call 后的 fallthrough 处理成不可达或截断。
- call 的栈效果来自 cspec `stackshift` / `extrapop`，不是把 `RSP` 当普通全局寄存器保存。
- 对 native 侧来说，这一步只复刻其中很小一部分：已知 no-return declaration 后没有返回边，因此 call 前手写 return-address 到 caller stack 的 raw store 没有后续语义。

# 技术路线

1. 放宽 `isKnownNoReturnDeclaration()`。
   - 继续要求 declaration、非 vararg。
   - 对 `abort` 保持 0 参数。
   - 对 `__assert_fail` 允许固定参数声明。

2. 在 prototype recovery cleanup 里新增 no-return call-frame raw store 清理。
   - 只处理 call 同 block、call 前连续可回看的形态。
   - 只删 `store i64 const, ptr inttoptr(RSP-derived integer)`。
   - 地址必须能追到 ABI stack pointer / frame pointer external input 或当前函数内已知 stack-frame register value。
   - 中间遇到其它 call、未知 register access、非纯地址计算就停止。

3. 不处理普通 returning call。
   - `notdec_native_9e70` 这类内部 helper call 前的 return-address store 先不碰。
   - `geteuid` 这种普通 declaration 先不碰。

# 判断标准

- 单测覆盖：
  - `__assert_fail(i64, i64, i64)` 前 `RSP` call-frame store 和 raw return-address store 被删，死 `RSP.external_input` 也被清掉。
  - 普通 returning declaration 前同形态 raw return-address store 不删除。
- `native_prototype_recovery_test` 通过。
- 相关 native CTest 通过。
- `libuv:shared-library` signature-rewrite 通过 LLVM 22 verify，`RSP/RBP` residue 下降。

# 风险

- raw stack store 可能不一定是返回地址。第一版只删 no-return declaration 前、值为常量、地址来自 stack-frame register 的 store。
- 如果 no-return 表不准，会删掉真实 caller-stack 写入。因此只先加 `__assert_fail`。
- 这不能解决 `stack_frame_state`、`frame_base_state`、`chunk_phi`，也不解决普通 internal call 的 call-frame store。

# 不做什么

- 不扩大普通 no-stack declaration 白名单。
- 不处理 returning call 的 return-address store。
- 不处理 dynamic stack / frame base / chunk 边界。

# 2026-06-05 实现记录

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2400)
  - `callMayReadRegisterName()` 对 `RSP` + 已知 no-return declaration 放开，不再认为这类 call 会读取 `RSP`。
  - 只影响 `__assert_fail` 这类已知 no-return declaration，不改变普通 returning declaration。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2826)
  - 新增 `isNoReturnDeclarationWithExplicitArguments()`，当前只识别 `__assert_fail` 或显式 `noreturn` attribute。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2838)
  - 新增 `isRawStackFrameReturnAddressStore()`，只匹配 `store i64 const, ptr inttoptr(stack-frame-derived integer)`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2855)
  - 新增 `eraseNoReturnDeclarationCallFrameStores()`，删除 no-return declaration 前一条 raw return-address store。
  - 删除 raw store 后递归清理变死的 `inttoptr`、地址计算和 stored value。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:6082)
  - 在 signature rewrite cleanup 流程里，`truncateKnownNoReturnDeclarationCalls()` 后运行 no-return call-frame store 清理。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2715)
  - 新增 `createDeclarationRawCallFrameStoreCallerFunction()`，构造 declaration call 前 `RSP` store + raw return-address store 的形态。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:9924)
  - 新增 `__assert_fail` 正例，验证 `RSP` store、`RSP.external_input` 和 raw `inttoptr` 都被清掉。
  - 新增普通 returning declaration 负例，验证 `RSP` store 保留。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-noreturn-call-frame-gate \
  --target libuv:shared-library

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-noreturn-call-frame-gate/libuv-shared-library.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-noreturn-call-frame-gate/libuv-shared-library.signature-rewrite.ll \
  > /tmp/notdec-libuv-noreturn-call-frame-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `libuv:shared-library` 完整 gate 通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `libuv:shared-library` | 105s | 108s |

`libuv` 上 `RSP/RBP` 明细从 29 行降到 15 行：

```text
reason:
8 entry_external_input
5 stack_pointer
2 frame_pointer

semantic:
6 call_frame_state
3 saved_register_restore,call_frame_state
2 stack_frame_state
2 frame_base_state
2 chunk_phi
```

判断：

- `__assert_fail` 相关 no-return call-frame raw store 已清掉，说明这条规则命中了预期残留。
- 剩余 `call_frame_state` 主要是 `notdec_native_9e70` 和 `geteuid` 这类 returning call，不在本轮范围内。
- 剩余 `stack_frame_state` / `frame_base_state` / `chunk_phi` 仍要按本地栈帧和函数 chunk 继续分类。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | `libuv` 上 RSP/RBP residue 29 -> 15，清掉一类明确 no-return call-frame 噪声。 |
| 理解成本 | 2 | 新增一条小 cleanup，条件窄，和现有 no-return 截断、stack-frame store 清理放在同一区域。 |
| 维护成本 | 2 | 当前只手工识别 `__assert_fail`；后续若接入函数属性或外部原型库，可替换这张小表。 |

有没有更好的方案：

- 更完整方案是正式建模 call stackshift / return-address effect。
- 当前先做 no-return declaration 前的 raw store 清理，是因为 no-return 没有返回边，语义边界清楚，比处理普通 returning call 更稳。
