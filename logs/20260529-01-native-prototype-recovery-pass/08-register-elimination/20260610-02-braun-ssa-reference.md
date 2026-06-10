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
