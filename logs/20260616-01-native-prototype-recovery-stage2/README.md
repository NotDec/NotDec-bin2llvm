# Native prototype recovery stage2

## 项目目标

这个目录记录 native prototype recovery / register elimination 的第二阶段。

第二阶段不再继续模仿 Ghidra 的 trial/use 链路，而是以静态分析为主：

- `NativeRegisterSummary` 做函数级 register effect summary。
- `NativeRegisterSummarySSA` 基于 summary 构建新的寄存器 SSA 和 register residue 清理。
- call signature rewrite 消费 SummarySSA 的结果，把 ABI register passing 改成 LLVM call operands。
- 后续 internal function signature rewrite 也必须基于 summary 链路，不依赖旧 `NativePrototypeRecovery`。

代码边界：

- 新链路代码在 `include/notdec-bin2llvm/passes/summary/` 和 `lib/passes/summary/`。
- 旧 Ghidra-style 链路代码在 `include/notdec-bin2llvm/passes/heritage/` 和 `lib/passes/heritage/`。

## 原始 prompt

本目录下各计划文件仍在文件开头保留完整原始 prompt。这里汇总本阶段关键 prompt，方便先读目标。

### Ghidra 机制梳理

> 写一个文档，详细分析一下Ghidra的这些所有的机制，以及他们是顺序运行的几个阶段呢？还是说在heritage阶段按需调用的分析？

> logs/下面创建一个新的文件夹吧，写到里面，表示一个新的第二阶段的native-prototype-recovery和寄存器消除，和之前的区分开

### 新 summary/SCC 链路

> 认真思考一下，能否设计一个静态分析算法，基于call graph SCC的基于summary的自底向上分析，能否组合分析每个函数/SCC对register的影响，基于静态分析的方式，更加系统化地解决寄存器的消除的问题。

> 外部函数/间接调用暂时就看ABI，是没问题的。stack 参数可能后续再拓展覆盖吧，初版先不考虑，不过确实是一个值得考虑的点。partial register感觉也没必要。就直接当做存放了整个寄存器吧。条件路径没必要啊，直接就当做两边路径都被改了就行了。内存和栈一样，都先不考虑。后期的话，也只是考虑占空间上的内存，其他地方的内存完全不考虑。未知clobber应该没啥吧。按这个写一个plan文档吧。文档可以明确说一下，这个是一个新版的单独链路，和之前的模仿Ghidra的做法完全独立，也不用参考那边

> 每个 register 当前值用少量抽象值表示，这一块感觉不太系统。初始值就是entry，如果改动了就变成改动了的，为什么还要单独分什么Call Clobber，Call Return。对某个函数的调用，目标函数的所有基本块在transfer function的合并下得到的结果就是函数的结果，根据函数的结果去分析call 的效应就可以了，没必要分什么clobber return。栈指针相关可以单独由专门的pass匹配和处理，当前这个pass可以不考虑，搞一个不考虑的寄存器集合作为pass的参数。另外3-5节重写一下按照transfer function的思路去描述，定义抽象域，join函数，meet函数，然后证明join和meet操作满足交换律，然后函数的summary就直接定义为整个函数CFG的整体效应即可。

> summary 算完后，可以考虑再top-down进一步确认一些不确定的寄存器信息？比如，部分函数在结尾修改了多个寄存器，但是真正的返回值可能只有RAX，可以通过分析所有的caller看修改的寄存器真正被用的有多少。因为顶层的寄存器调用情况是已知的，即假设需要遵守调用约定，所以从顶层不断确定下来。

### SummarySSA 和 signature rewrite

> 旧链路还没有改名字吗，改成叫HeritageSSA吧，然后可以替换，用新的作为默认链路，也不太需要和旧链路做太多对比吧，专注好新链路本身的实现和修bug

> 感觉既然之前都做register消除了，那是不是应该同步也做函数签名的修改。详细规划一下，是合并到前面的registerSSA，还是说怎么做？

> 类型改写那一块，找一下LLVM源码里面 /sn640/NotDec/llvm-source 里面，我记得有clone系列函数，比如cloneModule还是什么，里面有复制函数该怎么做的参考，涵盖了完整的所有metadata，属性等的拷贝，确保参考了那边做复制，保证信息不会遗漏

> signature rewrite就应该尝试去知道store的完整用途，通过让那边标记好，尽量多的消除store，或者说让前面的pass尽量标注好store是可以在signature恢复的地方消除的，总之不要因为分pass影响了最终的目标

### fortune 和 internal rewrite

> Bench2那边好像下载的是指定的ubuntu版本的源的，尝试加进去吧，加好之后再跑一下当前链路看看效果，看看有没有未消除的寄存器

> notdec_native_* call为什么还没做函数签名改写？连external函数都处理了，难道notdec_native_*还没处理？

> internal function signature rewrite 另开计划不对吧，就沿用这个计划就行了吧。接下来实现internal signature rewrite吧，先看看按照计划实现怎么样，有没有什么技术难点

### 当前整理要求

> 做好这几点：
> 1. 更好地区分开之前模仿Ghidra的操作写出来的pass链路（HeritageSSA），以及基于最新的静态分析的思路写出来的寄存器消除链路，最好，比如通过将他们两个的代码放在不同的文件夹里，同时在agents.md里清楚地说明。防止两个链路的开发混淆。我们当前开发新链路，就应该完全基于新链路的结果去做。
> 2. 整理当前新链路专用的project log文件夹，现在logs/下面最新的几个log都是单独开一个文件夹的，这样不对，把最新的几个这种只有单文件的文件夹都合并到logs/20260616-01-native-prototype-recovery-stage2里面，而且，放一个README.md，里面简单说明项目目标，以及所有的我的原始prompt。
> 3. NativeExternalCallSignatureRewrite当前做的很奇怪，考虑完全重新写新的pass，同时处理internal函数和external函数的rewrite，logs/20260617-01-native-external-call-signature-rewrite/20260617-01-native-external-call-signature-rewrite-plan.md这个规划里面，提到了实现前要参考 LLVM 自己的函数复制和函数类型改写代码，避免漏掉属性和 metadata。

### SummarySSA 内建 signature rewrite

> 接下来继续规划一下SignatureRewrite。NativeExternalCallSignatureRewrite这个pass感觉写得太简单，可以直接删掉了。总的来说，应该单独写一个pass，同时负责所有的函数的签名的rewrite，完全基于之前静态分析的结果来rewrite，参考那个LLVM的cloneModule相关的代码。你觉得怎么样？有没有什么没考虑到的技术难点

> 我指的不是让你列举技术难点，而是当前你觉得计划有没有什么没考虑到的问题。另外，不要说什么第一阶段只负责一部分，Rewrite是一开始就要直接做到位的，包括参数和返回值。具体不同call site不统一的情况，或者其他的复杂情况这一块单独考虑一下。你清楚这一块怎么做吗？你先介绍一下当前是怎么判断internal函数的参数和返回值的，以及callsite不统一的情况，还有其他复杂情况怎么处理

> 参数就严格按照read_entry那边使用到的值来，如果出现了跳过某个寄存器的情况，就按数量更多最多的那个值对应的数量处理，就当做前面传过参数，但是没有被用上。当前的理解是对的。返回值的处理是看top down的分析，分析所有call site中caller使用到的callee修改过的寄存器的值，然后是取并集。只要有任何一个caller用了就当做它用过了。对于external的函数，就按ABI假设返回值寄存器都有值即可，然后看用了多少寄存器。当前的理解看着问题不太大。indirect call / address-taken internal function要改问题也不大，没必要太小心，可以按照参数和返回值的数量偏多的角度考虑，反正调用前都要给它强制类型转换为对应的函数指针类型。关于SummarySSA，确实当前可能导出的不太多，思路上就是按需导出额外的信息即可，之前插入的不需要的导出信息可以直接去掉。目前看下来，我严重怀疑反正summary ssa也是要重写IR，而且目前和函数签名重写的耦合非常严重，我严重怀疑这两个步骤应该一起进行，即让summary ssa也负责函数签名的修改

> 名字可能可以不用改。先更新plan吧，然后写一个goal按这个实现吧，之前实现的没用就都清理掉，比如NativeExternalCallSignatureRewrite

> 记得把本次的几个原始prompt也记录到那个logs/20260616-01-native-prototype-recovery-stage2/README.md。

### 最大 backing register

> partial register会带来哪些问题和困难？

> x86 的特殊清零规则，以及任何清零高位的底层语义都不用考虑，已经在lifting阶段反映在IR中了。partial 写等价于保留高位、替换低位的话，按照这个思路去直接做有什么问题吗

> 如果直接在lifting阶段，就不生成这种部分寄存器的名字，而是把所有这种部分寄存器的访问都按底层的语义去改成对完整寄存器的访问，怎么样，比如partial 写等价于保留高位、替换低位的话

> 对，按照这个方向改，让最开始lifting生成的寄存器全局变量就只有那种最大的

## 文件索引

- `20260616-01-ghidra-register-elimination-mechanisms.md`：Ghidra heritage、copy propagation、cover、trial/use 等机制梳理。
- `20260616-02-native-register-summary-scc-fixpoint-plan.md`：新 register summary / SCC fixpoint / top-down demand 计划。
- `20260617-01-native-external-call-signature-rewrite-plan.md`：signature rewrite 计划，后续应改成统一 internal/external call rewrite。
- `20260618-01-native-summary-ssa-followup-plan.md`：SummarySSA、Bench2、fortune 等实现记录。
