# Braun 2013 PDF 转文本索引

本文记录 `Simple and Efficient Construction of Static Single Assignment Form`
这篇论文的 PDF 转文本结果信息。

出于版权原因，这里不保存论文全文转文本，只保存来源、转换命令、hash 和定位索引。
需要全文时，用下面命令在本地重新生成。

## 来源

- 标题：`Simple and Efficient Construction of Static Single Assignment Form`
- 作者：Matthias Braun, Sebastian Buchwald, Sebastian Hack, Roland Leißa, Christoph Mallon, Andreas Zwinkau
- PDF: https://c9x.me/compile/bib/braun13cc.pdf
- 项目页: https://compilers.cs.uni-saarland.de/projects/ssaconstr/

## 转换命令

```bash
curl -L --fail --silent --show-error \
  https://c9x.me/compile/bib/braun13cc.pdf \
  -o /tmp/braun13cc.pdf

pdftotext /tmp/braun13cc.pdf /tmp/braun13cc.txt
```

本次转换结果：

```text
PDF  sha256: 01f0b6d558096c27c1f37f4b451b8c4932dc200cee4e86d07357f0dacf21aa6a
TEXT sha256: 1faca940985005fd56b798bd2a13e1d7626e9e56441b2a79e794c30d1f1a594a
TEXT lines: 1356
TEXT bytes: 53189
```

## 章节定位

行号来自 `/tmp/braun13cc.txt`。
不同 `pdftotext` 版本可能有轻微差异。

```text
1     标题
20    1 Introduction
73    2 Simple SSA Construction
92    2.1 Local Value Numbering
151   2.2 Global Value Numbering
276   2.3 Handling Incomplete CFGs
377   3 Optimizations
381   3.1 On-the-fly Optimizations
479   3.2 Minimal SSA Form for Arbitrary Control Flow
683   4 Properties of our Algorithm
789   4.1 Time Complexity
826   5 Other Applications of the Algorithm
949   6 Evaluation
1252  Related Work
1295  Conclusions
1318  References
```

## 算法定位

```text
123   writeVariable(variable, block, value)
125   readVariable(variable, block)
132   Algorithm 1: Implementation of local value numbering
215   readVariableRecursive(variable, block)
230   addPhiOperands(variable, phi)
236   Algorithm 2: Implementation of global value numbering
237   tryRemoveTrivialPhi(phi)
255   Algorithm 3: Detect and recursively remove a trivial phi function
283   sealBlock(block)
288   Algorithm 4: Handling incomplete CFGs
543   removeRedundantPhis(phiFunctions)
564   Algorithm 5: Remove superfluous phi functions in case of irreducible data flow
```

## 关键词定位

```text
88    论文开始说明会处理 incomplete CFG
121   filled block 的定义
200   sealedBlocks 在基础算法里先假设总是 false/可忽略
213   trivial phi 的定义
219   incompletePhis[block][variable]
249   trivial phi 删除后递归检查 phi users
278   sealed block 的定义
283   sealBlock(block)
292   incompletePhis 作为 unsealed block 的临时代理
383   trivial phi 优化机制
481   irreducible CFG 下基础算法可能不是 minimal
537   Algorithm 5 用 SCC 删除冗余 phi
690   pruned SSA form
773   reducible CFG 上没有 trivial phi 时就是 minimal SSA
786   construction algorithm 删除 trivial phi 后，在 reducible CFG 上得到 minimal SSA
812   arbitrary control flow 需要收缩 SCC 以得到 minimal SSA
```

## 和本仓库文档的关系

基础知识和总体流程整理在：

```text
docs/analysis/braun-ssa-construction.md
```

已有问题背景和实现参考在：

```text
logs/20260529-01-native-prototype-recovery-pass/08-register-elimination/20260610-02-braun-ssa-reference.md
```

如果后续要核对原文，建议先按本文命令重新生成 `/tmp/braun13cc.txt`，
再按上面的行号查 `readVariableRecursive`、`incompletePhis`、`sealBlock` 和
`tryRemoveTrivialPhi`。
