# 追加：intrinsic 前缀定为 notdec.register.（call 方案的两个 blocker）

按用户要求，把"内置 intrinsic"的命名空间从裸 `notdec.` 收紧为 **`notdec.register.`**：

- `NativeRegisterSummary.cpp` `isNotDecRegisterHelperCall()`：
  `notdec.register.*` + 历史前缀（`notdec.unknown.*`、`notdec.x87.*`、value-range）；
- `NativeRegisterSummarySSA.cpp` `isUnknownExternalFunction()`：同样处理；
- `NativeRegisterSummarySSA.cpp` `buildInitialSignatureShapes()` 过滤：同样处理。

约定：**新增寄存器模型 intrinsic 统一用 `notdec.register.` 前缀**，不再需要往三个名单里补；
历史前缀继续显式列出来保持兼容。keep-alive 调用因此改名为
`notdec.register.native_frame.keep`。

## call 方案实测：还是不行，而且是两个独立原因

复现命令：入口插 `call void @notdec.register.native_frame.keep(ptr %frame)`
（每帧一次）+ "非 spill 帧 store 一律保留"。

1. **range/clobber 那条路已经通了**：加前缀后 warning TSV 回到 348 行
   （之前 813 行、217 条来自 keep-alive 的 clobber 都没了），说明
   `isAnalyzableCall`/`applyCallEffect`（`NativeRegisterSummarySSA.cpp` 5761 起）
   确实会把 `notdec.register.*` 调用当作"不改寄存器"。
2. **真正挡住的是 `NativeStackFrame` 自己的一个支配 bug**：
   ```
   Instruction does not dominate all uses!
     %notdec_stack.native.ptr222 = getelementptr ..., i64 32
     %notdec_stack.native.int47 = ptrtoint ptr %notdec_stack.native.ptr222 to i64
   module verification failed after summary register SSA pass
   ```
   帧一旦能活到清理遍，`rewriteFunctionStackAccesses()` 的"复用已有 GEP"
   逻辑（`stackPointers.emplace(nativeOffset, gep)`，注释说为了让 clean pass
   按指针 identity 比较）会复用一个**位于使用点之后**的 GEP，于是新生成的
   `ptrtoint` 用了不支配它的指针。

## 下一步（很具体）

1. 修 `NativeStackFrame.cpp` 的 GEP 复用：只复用**支配使用点**的 GEP，否则在
   `storage` 后（entry builder 位置）新建一个；顺带把 store/load 匹配改成
   "alloca + 字节区间"而不是指针相等。
2. 然后重跑 call keep-alive + 非 spill 帧 store 全保留，预期：
   `FUN_9990`/`FUN_99d0` 回来（define 85 -> 87）、warning 仍 348 行、
   residue 1 行、IR +<1%、时间 ~41s。

本轮已提交的只有前缀收紧（wrk 中性：348 行 diff 0、IR +26 B、ctest 12/12）。
