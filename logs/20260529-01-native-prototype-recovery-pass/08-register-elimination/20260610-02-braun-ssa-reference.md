# Braun lazy SSA reference

用户原始要求：

> 基于Simple and Efficient Construction of Static Single Assignment Form，尝试从网上找到一个简洁的代码实现，作为参考，并放到一个文档里

## 结论

当前 `NativeRegisterSSA` 的思路接近 Braun et al. 的 lazy/on-demand SSA，但实现缺了完整的 `incomplete phi` / `sealed block` 收尾机制。

可作为代码参考的实现：

1. SPIRV-Tools `source/opt/ssa_rewrite_pass.cpp`
   - 链接：https://github.com/KhronosGroup/SPIRV-Tools/blob/main/source/opt/ssa_rewrite_pass.cpp
   - 旧版可读行号链接：https://android.googlesource.com/platform/external/deqp-deps/SPIRV-Tools/+/1f03ac10/source/opt/ssa_rewrite_pass.cpp
   - 优点：C++，明确写着实现 Braun 2013 SSA rewriting；函数名也接近论文。

2. Cranelift frontend `ssa.rs`
   - 链接：https://raw.githubusercontent.com/bytecodealliance/wasmtime/main/cranelift/frontend/src/ssa.rs
   - API 文档：https://docs.wasmtime.dev/api/src/cranelift_frontend/frontend.rs.html
   - 优点：工业实现，sealed block 使用方式很清楚；缺点是当前 raw source 被压成单行，不如 SPIRV-Tools 好读。

原论文：

- Braun, Buchwald, Hack, Leißa, Mallon, Zwinkau, "Simple and Efficient Construction of Static Single Assignment Form"
- PDF：https://c9x.me/compile/bib/braun13cc.pdf
- 项目页：https://compilers.cs.uni-saarland.de/projects/ssaconstr/

## 论文里和当前 bug 直接相关的点

论文 Algorithm 2 的主干：

```text
readVariableRecursive(variable, block):
  if block not in sealedBlocks:
    val <- new Phi(block)
    incompletePhis[block][variable] <- val
  else if |block.preds| = 1:
    val <- readVariable(variable, block.preds[0])
  else:
    val <- new Phi(block)
    writeVariable(variable, block, val)
    val <- addPhiOperands(variable, val)
  writeVariable(variable, block, val)
  return val

addPhiOperands(variable, phi):
  for pred in phi.block.preds:
    phi.appendOperand(readVariable(variable, pred))
  return tryRemoveTrivialPhi(phi)
```

Algorithm 4 处理 incomplete CFG：

```text
sealBlock(block):
  for variable in incompletePhis[block]:
    addPhiOperands(variable, incompletePhis[block][variable])
  sealedBlocks.add(block)
```

关键点：

- operandless phi 只能是临时代理。
- block sealed 之后，必须给 incomplete phi 补 operands。
- 补完 operands 后，再做 trivial phi 删除。
- 不能把 operandless phi 长期留在最终 IR 里。

这正好对应当前 `NativeRegisterSSA` 的问题：`ensurePhi` 会创建占位 PHI，但没有单独的 `incompletePhis` 和 `sealBlock/finalize` 阶段保证它最终补齐 incoming。

## SPIRV-Tools 参考实现

文件头明确说明：

- 它实现 Braun 2013 的 SSA rewriting algorithm。
- 它不是先算 dominance frontier，而是从 load 往前查 reaching definition。
- 找不到 reaching definition 时，沿 CFG 往前查，并在 join 点插入 Phi。
- 用 memoization 避免重复查。

对应函数：

- `GetReachingDef`
  - 对应论文 `readVariableRecursive`。
  - 如果当前 block 有定义，直接返回。
  - 单前驱时递归 predecessor。
  - 多前驱时创建 `PhiCandidate`，先 `WriteVariable` 作为当前定义，打断递归环，然后 `AddPhiOperands`。

- `AddPhiOperands`
  - 对每个 predecessor 查 reaching definition。
  - 如果 predecessor 还没 sealed，用特殊占位值标记这个 phi 后续还要完成。
  - 完成后调用 `TryRemoveTrivialPhi`。

- `FinalizePhiCandidates`
  - 专门处理 incomplete phi。
  - 这是当前 `NativeRegisterSSA` 缺的关键阶段。

- `TryRemoveTrivialPhi`
  - 删除只引用自己和同一个值的 phi。
  - 删除时要同步更新 phi users 和 load replacement 表。

值得注意的一段注释在 `AddPhiOperands` 附近：如果 predecessor 没 sealed，它不会继续递归 `GetReachingDef`，因为那会在 predecessor 里产生空 phi，后面真正定义可能被这个空 phi 遮住。这一点和我们当前 bug 很接近。

## Cranelift 参考点

Cranelift frontend 文档里明确要求：

- 每个 block 创建时先 `declare_block`。
- 填完 block 后，才能把它声明成其它 block 的 predecessor。
- 当一个 block 的所有 predecessor 都已经知道时，调用 `seal_block`。
- 如果前端不好及时 seal，也可以最后 `seal_all_blocks`。

这套 API 直接把 “predecessor 是否完整” 变成显式状态，避免空 PHI 没有补 operands。

Cranelift 当前实现用 block parameters 表达 phi，逻辑仍然是 Braun 风格：

- `use_var` 查询变量当前 SSA value。
- 找不到本地定义时，查 predecessor。
- 必要时先创建 block parameter 作为 sentinel。
- block sealed 后，统一查 predecessor values。
- 如果 predecessor values 都一样，删除 block parameter，改成 alias。
- 如果 predecessor values 不同，就在 predecessor branch 上追加参数。

## 简化算法和当前代码的对应关系

下面按 Braun 简化算法里的常见名字，对应到当前 `NativeRegisterSSA`。

### variable

算法里的 `variable` 是要改写成 SSA 的变量。

当前代码里对应 `RegisterUnit`：

- `RegisterUnit::Global` 是寄存器 backing global，比如 `@RAX` / `@RCX`。
- `RegisterUnit::Name` 是寄存器名。
- `BlockRegKey = (BasicBlock *, GlobalVariable *)` 就是 `(block, variable)`。

当前 register SSA 是按完整 backing register 做 coarse unit，不把 `RAX/EAX/AX/AL` 拆成不同 SSA 变量。partial access 先用 `readRegister` 拿完整寄存器值，再用 bit 操作抽取或替换。

### currentDef / writeVariable

算法里通常有：

```text
currentDef[variable][block] = value
writeVariable(variable, block, value)
```

当前代码拆成两个缓存：

- `EntryValue[(block, register)]`：block 入口处的寄存器值。
- `ExitValue[(block, register)]`：block 出口处的寄存器值。

主要写入点：

- `readBlockEntry` 在拿到入口值后写 `EntryValue`。
- `readBlockExit` 在拿到出口值后写 `ExitValue`。

这个拆分符合当前 pass 的需求，因为 LLVM IR 里一个 block 内可能有 register store/load/call barrier，所以“入口值”和“出口值”不是同一个问题。

### readVariable

算法里的 `readVariable(variable, block)` 通常先查 `currentDef`，没有再递归。

当前最接近的是：

- `readRegister(block, unit, before)`

它查的是“某条指令之前的寄存器值”：

1. `localValueBefore(block, unit, before)` 查同一 block 内最近的 store/load。
2. `hasCallBefore(block, unit, before)` 遇到会 clobber 该寄存器的 call，就返回 `nullptr`。
3. 否则转到 `readBlockEntry(block, unit)`。

这比论文里的 `readVariable` 多了 call clobber 语义。这个差异是必要的，因为 native register global 不是普通局部变量，call 可能杀死 ABI caller-saved register。

### readVariableRecursive

算法里的 `readVariableRecursive(variable, block)` 是核心递归。

当前对应：

- `readBlockEntry(block, unit)`

当前逻辑：

1. 查 `EntryValue` 缓存。
2. 如果 `(block, register)` 正在解析，调用 `ensurePhi` 打断递归环。
3. 如果 block 没有 predecessor，返回 `externalInput(unit)`。
4. 收集所有 predecessor。
5. 如果某个 predecessor 有 clobbering call 且找不到 call 前本地值，返回 `nullptr`。
6. 对每个 predecessor 调 `readBlockExit(pred, unit)`。
7. 单前驱且没有 pending phi，直接返回 predecessor 出口值。
8. 否则创建或取出 PHI，给每个 predecessor 加 incoming。
9. 调 `simplifyPhi`。

对应关系很直接：

```text
readVariableRecursive(variable, block)
  -> readBlockEntry(block, unit)

readVariable(variable, pred)
  -> readBlockExit(pred, unit)

writeVariable(variable, block, val)
  -> EntryValue[(block, unit)] = val
```

当前缺口也在这里：步骤 2 的 `ensurePhi` 创建了占位 PHI，但没有像论文的 incomplete phi 那样保证后面一定 `addPhiOperands`。

### block exit lookup

Braun 算法通常按 block 级变量值讲，不单独区分 exit。

当前增加：

- `readBlockExit(block, unit)`

它的作用是：

1. 查 `ExitValue` 缓存。
2. 查 terminator 前的本地寄存器值。
3. 如果 block 内有 clobbering call，返回 `nullptr`。
4. 否则 exit 值等于 entry 值，调用 `readBlockEntry(block, unit)`。

这层是当前 native register 语义需要的适配。它本身不是问题，问题是它在递归环里可能间接触发 `ensurePhi`，但 `ensurePhi` 没有完整 finalize。

### incompletePhis

算法里的 `incompletePhis[block][variable]` 保存还不能补 operands 的临时 PHI。

当前最接近的是：

- `PendingPhi[(block, register)]`

但语义不完整：

- `PendingPhi` 只记录“已经创建过 PHI”。
- 它没有记录这个 PHI 是 `Incomplete`、`Completing` 还是 `Complete`。
- 它没有记录哪些 predecessor incoming 已经补过。
- 它没有统一的 finalize 入口。

所以 `PendingPhi` 现在只是 PHI cache，不是论文里的 incomplete phi 机制。

### sealedBlocks / sealBlock

算法里的 `sealedBlocks` 表示这个 block 的 predecessor 列表已经完整。`sealBlock(block)` 会给该 block 的 incomplete phi 补 operands。

当前代码没有对应物。

这点在当前场景下更容易修：`NativeRegisterSSA` 运行时 LLVM CFG 已经存在，理论上所有 block 都是 sealed 的。也就是说，不一定要实现动态 `sealBlock` API，但需要一个等价的 finalize 阶段：

```text
for each PendingPhi(block, register):
  ensure phi has one incoming for every predecessor(block)
  then simplify/remove trivial phi
```

如果保留当前递归写法，可以把所有 block 当 sealed，但不能跳过 `addPhiOperands` 的完整性检查。

### addPhiOperands

算法里的 `addPhiOperands(variable, phi)`：

```text
for pred in phi.block.preds:
  phi.appendOperand(readVariable(variable, pred))
return tryRemoveTrivialPhi(phi)
```

当前代码对应 `readBlockEntry` 里的这段：

```cpp
for (llvm::BasicBlock *pred : preds) {
  llvm::Value *incoming = readBlockExit(*pred, unit);
  incoming = resolveValue(incoming);
  ...
  incomingValues.push_back({pred, incoming});
}
...
for (const auto &[pred, incoming] : incomingValues) {
  phi->addIncoming(incoming, pred);
}
llvm::Value *simplified = simplifyPhi(phi);
```

差异：

- 当前 `addIncoming` 只在这次递归成功走到底时执行。
- 如果 `ensurePhi` 是在 `ResolvingEntry` 分支里创建的，占位 PHI 可能没有回到这里。
- 如果某个 predecessor 返回 `nullptr`，当前直接返回 `nullptr`，已经创建的 pending phi 没有被补齐或删除。

这是 `hexx64.so -f 0x1156e0` 上空 PHI 的直接来源。

### tryRemoveTrivialPhi

算法里的 `tryRemoveTrivialPhi` 会删除只包含自己和同一个值的 PHI，并更新所有引用。

当前对应：

- `simplifyPhi(phi)`
- `eraseDeadPhis()`
- `Replacement`

当前实现：

1. 忽略 incoming 中的 self reference。
2. 如果剩下的 incoming 都是同一个 value，就 `replaceAllUsesWith(same)`。
3. 把 phi 放进 `DeadPhis`，最后如果 use empty 就删除。

这个实现能处理简单 trivial phi，但和 SPIRV-Tools 相比还不完整：

- 没有主动更新 `EntryValue` / `ExitValue` 里已经缓存的旧 PHI。
- 没有递归处理因为这个 PHI 删除而变 trivial 的其它 PHI。
- 没有针对 incomplete phi 的 users 列表做统一替换。

### external input

算法里 entry block 没有定义时通常返回 undef 或参数。

当前对应：

- `externalInput(unit)`

它在函数 entry 插入一次 register global load，并标 `notdec.register.external_input` metadata。这个值表示“函数入口时寄存器原始值”。

这点是当前 register SSA 的业务语义，不是 Braun 算法本身的问题。

### call clobber barrier

Braun 算法没有 call clobber 这一层。

当前对应：

- `hasCallBefore`
- `blockHasClobberingCall`
- `callClobbersRegister`

它们会让某些 register read 返回 `nullptr`，表示这个寄存器值被 call 杀掉，不能安全替换。

这和 PHI bug 的关系是：如果递归过程中已经创建了 pending phi，然后某条 predecessor 路径因为 clobber 返回 `nullptr`，当前代码没有清理或完成那个 pending phi。

## 和 NativeRegisterSSA 的差距

当前代码位置：

- `lib/passes/NativeRegisterSSA.cpp:955` `readBlockEntry`
- `lib/passes/NativeRegisterSSA.cpp:1015` `readBlockExit`
- `lib/passes/NativeRegisterSSA.cpp:1035` `ensurePhi`
- `lib/passes/NativeRegisterSSA.cpp:1050` `simplifyPhi`

当前已有的部分：

- 有 local lookup：`localValueBefore`
- 有 recursive predecessor lookup：`readBlockEntry/readBlockExit`
- 有 cycle breaker：`ResolvingEntry` 命中时 `ensurePhi`
- 有 trivial phi 简化：`simplifyPhi`

当前缺的部分：

- 没有 `sealedBlocks` 状态。
- 没有 `incompletePhis[block][register]`。
- 没有统一 finalize 阶段。
- `ensurePhi` 创建出来的 PHI 可能没有被 `addIncoming` 补齐。
- `simplifyPhi` 删除 PHI 后，只放进 `DeadPhis`，但 replacement/cache/user 关系没有像 SPIRV-Tools 那样完整更新所有间接引用。

## 对后续修复的建议

不要直接照搬完整 Cranelift。当前 pass 是在已有完整 LLVM CFG 上做 register SSA，不是在构造 CFG，所以更适合参考 SPIRV-Tools 的 pass 形态。

最小修复方向：

1. 把 `PendingPhi` 改成明确的 `PhiState`，至少记录 `Incomplete/Completing/Complete`。
2. 所有通过 `ensurePhi` 创建的 PHI，最终必须进入一个 finalize 阶段。
3. finalize 时按当前 LLVM CFG 的 `predecessors(block)` 补齐 incoming。
4. 如果某个 predecessor 的寄存器值因为 clobbering call 不可恢复，不要留下空 PHI；要么该寄存器本轮不替换，要么用明确的 external/undef 策略，并在文档里说明语义。
5. trivial phi 删除时，必须同步更新 `EntryValue`、`ExitValue`、`Replacement`、其它 pending phi 的 incoming，不能只 `replaceAllUsesWith`。

判断标准：

- `verifyModule` 不能再出现 PHI incoming 数量不匹配。
- 对每个仍在 IR 里的 PHI：`phi.getNumIncomingValues()` 必须等于 `pred_size(phi.getParent())`。
- `hexx64.so -f 0x1156e0` 默认 register SSA 至少能通过 verifier。
- 不退回 slot fallback / mem2reg。

## 2026-06-10 实现记录：补 pending PHI finalize

从映射回 Braun 算法看，直接问题在这里：

- Braun 的 `incompletePhis` 只是临时状态，`sealBlock` 必须调用 `addPhiOperands` 补齐 predecessor operands。
- 当前 `NativeRegisterSSA` 的 `PendingPhi` 只记录“已经创建过 PHI”，没有 finalize。
- `readBlockEntry` 递归遇到环时会通过 `ensurePhi` 创建空 PHI；如果后续某条 predecessor 因 call clobber 返回 `nullptr`，这个空 PHI 可能留在 IR 里。

本次先做最小修复，不重写整套 SSA 构建：

- `lib/passes/NativeRegisterSSA.cpp:363`
  - `FunctionPromoter::run` 在 register SSA 读写和 effect metadata 都完成后调用 `finalizePendingPhis()`。
  - 然后删除 `DeadPhis` 和无 use 的 pending PHI。

- `lib/passes/NativeRegisterSSA.cpp:1086`
  - 新增 `finalizePendingPhis()`。
  - 遍历所有 `PendingPhi`，把 pass 结束当成 sealed point。

- `lib/passes/NativeRegisterSSA.cpp:1096`
  - 新增 `completePhiIncoming()`。
  - 按当前 LLVM CFG 的 `predecessors(block)` 检查每个 PHI 的 incoming。
  - 已有 incoming 就消费一个；缺失的 edge 补 `undef`。
  - 这里不用 function-entry external input，因为 call clobber 后的寄存器值不是函数入口值。`undef` 表示这条边的寄存器值未知。

- `lib/passes/NativeRegisterSSA.cpp:1128`
  - 新增 `eraseUnusedPendingPhis()`，删除 finalize 后仍无 use 的临时 PHI。

- `lib/passes/NativeRegisterSSA.cpp:1140`
  - 新增 `forgetPendingPhi()`，避免已删除 trivial PHI 还留在 `PendingPhi` 里。

验证：

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target notdec-native-llvm native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-regssa-fix %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-regssa-fix.ll \
  > /tmp/hexx64-1156e0-regssa-fix.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-regssa-fix.ll \
  -o /tmp/hexx64-1156e0-regssa-fix.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-regssa-fix.bc \
  -o /tmp/hexx64-1156e0-regssa-fix.verified.bc

/usr/bin/time -f 'TIME limited-summary-after-ssa %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls \
  > /tmp/binls-native-summary-after-ssa.json
```

结果：

- `native_register_effects_test` 通过。
- `hexx64.so -f 0x1156e0` 默认 register SSA 成功输出 `.ll`，时间 `53.62s`。
- `llvm-as` 通过，时间 `1.59s`。
- `opt -passes=verify` 通过，时间 `0.88s`。
- `/bin/ls --decode-seed-limit 20` 仍为 `0.22s`。
- 输出里原先报错的 `%RCX.regssa80782` 变成合法 PHI：两条 predecessor 都补为 `undef`。

风险：

- 这是结构修复，不是完整重写 Braun 算法。
- `undef` 的语义是“这条边寄存器值未知”。它比错误地使用 external input 更保守，但后续如果要表达 call-clobbered unknown value，最好引入明确的 clobber unknown value 或跳过相关 load rewrite。

评分：

- 实现效果：7/10。修掉 PHI incoming verifier 错误，`hexx64.so -f 0x1156e0` 默认路径可过验证。
- 复杂度：3/10。只加 pass 结束 finalize，没有重写递归算法。
- 维护成本：4/10。`undef` 是明确但偏粗的未知值表达，后续语义精化时需要回看。
