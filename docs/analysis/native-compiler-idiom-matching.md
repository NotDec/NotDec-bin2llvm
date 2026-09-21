# native 底层二进制操作的定位与处理模式

这份文档回答一个问题：**想把 lift 出来的机器码形态还原成源级操作（除法、取模、乘法、
变参、canary 之类）时，怎么找到真实形态、怎么证明改写是对的、怎么落地和验证。**

设计原则见 `AGENTS.md` 的"模式匹配类 pass 的设计理念"（完整命中或整体放弃、先看编译器
源码、结果挂 metadata）。这里只讲手法：**定位 → 证明 → 落地 → 验证**，以及这一路上
踩过的坑。所有例子都来自 native 侧的寄存器消除链路（`passes/summary/`）。

## 0. 一句话版本

**别猜形态。** 先造 ground truth 和覆盖率 fixture 确认"有没有、是什么"，
再 dump **目标 pass 执行那一刻**的 IR（不是最终产物），最后用"闭式 + 误差项 +
两次 APInt 检查"把等价性证掉。跳过任何一步都会得到"看着该匹配却没匹配"或
"匹配上了但语义错了"。

## 1. 定位：先确认"有没有"和"长什么样"

### 1.1 覆盖率 fixture：分清"没做"和"样本里没有"

不要只看手头几个样本就列"待做清单"。写一个覆盖矩阵的 C 文件：

```c
// 目标操作 × 类型 × 参数组合，全部 noinline
__attribute__((noinline)) uint32_t u32_0(uint32_t x){return x/3;}
__attribute__((noinline)) int32_t  s32_1(int32_t x){return x/7;}
...
// main 里逐个调用（不做这一步，internal 函数会在 final cleanup 里被 DCE 掉，
// 产物里就看不到识别结果了）
int main(void){ return (int)(u32_0(3) + s32_1(3) + ...); }
```

```bash
CLANG=/sn640/NotDec/llvm-22.1.0.obj/bin/clang
$CLANG -m64 -O2 -o cov64 cov.c && $CLANG -m32 -O2 -o cov32 cov.c
build/bin/notdec-native-llvm cov64 --all-confirmed --skip-runtime \
  --register-ssa-summary -o covariç64.ll 2>&1 | grep post-rewrite
# 然后按函数名列出产物里有没有目标操作，和期望条数对拍
```

这一步能立刻分开三种情况：

- **真缺口**：期望有、产物里没有（继续往下看形态）；
- **假缺口**：源码里根本不是那个操作（例如 32 位除以 10^12 会被折成常量 0；
  i386 上的 64 位除法是 `__udivdi3` 库调用，没有魔数）；
- **已经做了**：不要重复实现。

### 1.2 ground truth：clang 汇编 + 真实二进制

- 形态以 `clang -O2 -S`（对应 `-m32`/`-m64`）为准：魔数常量、移位量、修正步骤
  一目了然。**记下来当单测表**（见 §4.1）。
- 当 lift 出来的东西"看着不对"时，回头用
  `objdump -d --start-address=... --stop-address=...` 看真实机器码，
  对照 p-code/lift 的语义，别在 IR 上瞎猜。

### 1.3 关键一步：dump **目标 pass 执行那一刻**的 IR

这是本项目最容易踩的坑：**最终产物里形态完全正确，不代表匹配器看到的就是它**。
链路里 helper 的降级（`notdec.reg.*` → 显式位运算）、InstCombine、final cleanup
都可能发生在目标 pass **之后**。

做法（临时的，看完就删）：

```cpp
if (const char *want = std::getenv("NOTDEC_DEBUG_PEEPHOLE")) {
  for (llvm::Function &fn : module) {
    if (!fn.isDeclaration() && fn.getName().contains(want)) {
      llvm::errs() << "===== PEEPHOLE INPUT " << fn.getName() << " =====\n";
      fn.print(llvm::errs());
      llvm::errs() << "===== END =====\n";
    }
  }
}
```

`make -C build notdec-native-llvm -j16` 后带环境变量跑一次即可。**一次 dump 胜过十次猜。**

### 1.4 这一阶段常见的噪声（按出现频率）

| 噪声 | 样子 | 处理 |
|---|---|---|
| 位段读取调用 | `call i32 @notdec.reg.extract.i64.i32(i64 %v, i64 0)` 而不是 `trunc` | 在目标 pass 开头把它降成 `lshr`+`trunc`（final cleanup 反正要做） |
| 宽度往返 | `trunc i64 (zext i32 %v)`、`trunc i64 (sext i32 %v)` | 匹配时看穿（`stripWidthRoundTrip`，见 §4）；同类噪声出现第二次就别再逐处打补丁 |
| 拼位调用 | `notdec.reg.insert.iFULL.iW(...)` | 看穿（它表达"保留高位"） |
| 寄存器 store/load 还没消 | `store i64 %v, ptr @RAX` 后面又 `load i64, ptr @RAX` | 说明目标 pass 位置太早；要么后移，要么在识别时按值而不是按 SSA 指针比较 |
| 值被 poison | `zext i64 poison to i128` + `!notdec.register.summary_ssa.zero_demand_operand` | 这是 partial-demand 有意的诊断信号，先查 demand 分析是不是漏了 observer（见 logs/20260920 的"同宽 mul"一节） |
| demand 折叠 | 最终 IR 把 `ashr` 写成 `lshr`、把 `-d*q` 写成 `+(2^N-d)*q`（只保留低 N 位时的等价改写） | 形态要在 peephole 阶段认；按最终 IR 写 matcher 会一处都折不了（见 §4 的 `sub` 商形态） |
| 未建模 userop 占位 | `call i64 @notdec.register.userop.crc32.i64.i64.i8(...)` | 不是噪声：Sleigh userop（crc32、cpuid、ud2…）没有独立语义实现，占位函数只为类型推理保留宽度/实参/访存信息（真值不展开）。lift 时每个 userop 打一条 warning；要补语义就去 `PcodeToLLVM.cpp` 的 `lowerCallOther` 加分支 |

**判据**：如果"最终 IR 该匹配却没匹配"，九成是这一层的问题，而不是数学。

### 1.5 谁把值丢了 / 谁拒绝了：逆向追踪

- `!notdec.unknown.source` 的 `reason=` 就是归因（`entry_range_rejected`、
  `return_binding_uses_clobber_value`、`range_return_rewrite_missing_value` …）。
- 在拒绝/兜底的代码点（例如 `attributedRangeUnknown()`、`entryRangeInput()`）
  临时加同样的 `getenv` 打印，把 block / register / range 打出来。
- 再用"改 IR 二分"快速定位：把一个 store 换成常量、把一条分支改成无条件、
  删掉一段链，看失败是否消失。注意 InstCombine/DCE 会跟着变，别把副作用当成因。

## 2. 处理模式：匹配 + 证明

### 2.1 形态匹配的纪律

- 一个 matcher 只认一种编译器输出形态；**要么整块命中、要么整体跳过**，绝不半改。
- 常量在哪一侧、符号位从哪个值取、哪一步可能被折掉，都要按 ground truth 写死；
  不做通用数据流推断、不猜类型。
- 落地位置按"值图干不干净"选：pre-summary 早但缠着寄存器读写；
  post-rewrite 晚但值图接近最终形态。**位置换过一次就要在 log 里写清为什么。**

### 2.2 等价性证明的固定套路（四步）

1. **写闭式**：把匹配到的 IR 写成输入 x 的函数（例：`floor(x*M/2^K)`、
   `floor((x+h)/2^extra)`、`floor_s(x*M/2^K)+[x<0]`）。
2. **代入 `x = d*q + r`**（0 ≤ r < d）：展开后 q 单独出来，剩一个只依赖 (q,r)
   的误差项 E。
3. **证 `E ≡ 0`**：E 对 q、r 单调，可行域 `q*d + r ≤ 2^L - 1` 是阶梯形，
   最大值只可能在两个角点 `(q_max, r_max)`、`(q_max-1, d-1)` 取到。
   **于是"证明"就是两次 APInt 算术检查，不是抽样。**
4. **反解参数**：由 `M == ceil(2^K/d)` 把 d 夹在宽度 < 1 的区间里，
   `d = ceil(2^K/M)` 直接算出来再回代验证（O(1)，不搜索）。

已实现的四类形态与它们的条件：

| 形态 | 闭式 | 条件（L = 被除数位宽上界） |
|---|---|---|
| 无符号简单 | `floor(x*M/2^K)` | `M == ceil(2^K/d)` 且 `e*2^L <= 2^K`，`e = M*d - 2^K` |
| 无符号 add 修正 | `floor((x+h)/2^extra)` | `E(q,r) = r + floor((q*e+r*M)/2^N) < 2^extra`，两个角点 |
| 有符号简单 | `floor_s(x*M/2^K) + [x<0]` | 同无符号，但 `L = W-1` |
| 有符号负魔数 `+x` | 同上（换 `M_u = M_neg + 2^W`） | 同有符号 |

L 要按被除数**自己的值上界**取：`lshr x, k` 的 L 可以收窄到 W-k；
`zext iK` 的 L = K。收窄是很多魔数唯一能验证通过的原因（典型：`x/10^9`）。

### 2.3 改写的落地

- `root->replaceAllUsesWith(newValue)`；链上的指令**只在 `use_empty()` 时**才收掉
  （乘法的低半段、CF/OF 的 icmp 常常还挂在上面，但都是死值）。
- 保留原名（`takeName`），别让产物里出现一堆 `%42`。
- 不引入新的 metadata，除非真有消费方（参考 vararg 的 `!notdec.vararg` 模式）。

## 3. 验证

1. **单测**：把 §1.2 的 ground truth 表抄进 `tests/native_register_summary_ssa_test.cpp`，
   每条断言两件事——折成了什么、链有没有收干净；再配**反例**：不在搜索范围的参数、
   不对应任何目标操作的常量必须**原样保留**（防止误折比漏折更危险）。
2. **覆盖率 fixture**：§1.1 的全量对拍，打印 missing 列表。这是发现缺口的唯一可靠手段。
3. **端到端**：`-f <addr>` 单函数提升看目标函数变成什么；
   全量 memcached / wrk / fortune 比 unknown 计数、warning 数和耗时。
4. **IR 验证**：只用 `/sn640/NotDec/llvm-22.1.0.obj` 下的 `llvm-as` + `opt -passes=verify`。
5. **全量测试**：`ctest --test-dir build`（含 fortune x86_64/i386 回归）。

### 3.1 性能与稳定性坑

- 每次改动都按相同条件跑一遍 memcached/wrk，确认耗时同档（并发跑会互相干扰，
  要单独跑一次）。
- `APInt` 比较会**按位宽断言**：`lshr i64 %v, 63` 的移位量是 i64，用在 i32 形态里时
  `APInt(32, 31)` 比较会 abort（wrk 被这个搞崩过一次）。**移位量一律按值比较。**
- 加了临时 dump/打印后要 `git grep getenv` 复核，确认没带进提交。

## 4. 现状清单（2026-09-21）

- **已实现**（`NativeRegisterPeephole.cpp` 的 `runNativeRegisterPostRewritePeephole`）：
  无符号/有符号 × 简单/add 修正 四类，覆盖 W=64（mulq）、W=32（i386 mull 与
  x86-64 的 32 位除法）、窄乘积（16 位提升到 32 位）、窄源加宽。
- **x86-64 的 32 位除法两种形态**（同一套闭式，只是**形态**不同）：i386 风格是
  `trunc(lshr i64 %m, 32)` 再在 i32 上 `ashr s`；x86-64 是
  `trunc(ashr i64 %m, 32+s)`（`sarq` 直接在 64 位积上做）。后者只是把
  `extraShift` 从"trunc 之后的 ashr 量"改成"2W 位 ashr 量 - W"。
  两类都要求被除数是 `sext` 进 2W 位的源。
- **有符号商的两种符号修正写法**：`add shifted, (x >>_logical W-1)`（加 0/1）和
  `sub shifted, (x >>_arith W-1)`（减 0/-1）。clang 在"只要余数"的取模路径上用
  后者（fortune x86-64 的 `(c - 52) % 26`）；折出 `sdiv` 之后 `x - d*(x/d)`
  由 InstCombine 认成 `srem`，不需要单独的取模 matcher。**注意**：peephole
  **之后**最后一次 InstCombine 会按 demanded bits 把它改写成 `lshr` +
  `+ (2^N - d)*q`（只保留低 N 位时的等价改写），所以这个形态只有 peephole
  阶段能认，盯着最终 IR 会看错。
- **乘法不用做**：`x*3 → lea` 这类强度削减在 lift 出来的 IR 里本来就是 `mul`
  （代数扫描器在三份产物上是 0 处），没有"移位加法汤"要还原。
- **取模**：先折出 `sdiv/udiv`，InstCombine 会把 `x - d*(x/d)` 认成 `srem/urem`，
  不用单独写 matcher。
- **未做**：16 位 add 修正（sub/lshr 在 i16、add/lshr 在 i32，混宽度）、
  有符号 2 的幂、8 位形态。x86-64 上 32 位除法的边角（`u32_1` 与 `s32_1..s32_8`）
  已在 2026-09-21 补齐：它们**不需要新的数学**，是 peephole 阶段的宽度往返噪声
  （`trunc(zext/sext)`）没看穿，见 `logs/20260917-03-…md` 最后一节。

## 5. 相关文档

- `AGENTS.md`：模式匹配类 pass 的设计理念（原则）。
- `DEBUG.md`：本机命令、Bench2 产物位置。
- `logs/20260917-03-native-callee-saved-and-df-plan.md`：常数除法识别从设计到实现的
  完整记录（含四类形态的推导、ground truth 表、覆盖率对拍结果和踩坑）。
