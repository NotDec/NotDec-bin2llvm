# native：寄存器重复 lane 链与 pipeline 尾部 CSE

## 原始 prompt

```text
看看下一步吧
```

（汇报"重复链 + EarlyCSE"的发现与量测后）

```text
按这个推进吧
```

## 背景：下一步候选里最大的一块其实是 CSE

上一轮收口后 wrk 还剩 64 个 `notdec.unknown`：`http_parser_execute` 33
（其中 28 条是 indirect call 后的 RDX，属"故意保守"、有 test 钉住）、
`http_parser_parse_url` 21、其余 10。

追 `http_parser_parse_url` 的 21 条 RSI unknown 时发现根因不在寄存器数据流，而在
**重复链**：

- `bb_e3b0` 的 `call {i32,i16} @FUN_6490(...)` 之后 RSI 是 caller-saved 残留
  （FUN_6490 body 里确实写了 9 次 @RSI），模型给 `range_unknown` 是诚实的；
- 但同一条 32 位 lane insert 链被**造了两遍**（`notdec.reg.insert.lower359` 与
  `...372`，递归规范化后完全相同），随后
  `%unique_27c80_4 = sub i32 359, 372`；
- 两者在 SSA 上不是同一个 Value，InstCombine 折不掉 `X - X`，于是这条 `sub`、
  分支条件和 21 条 unknown 全部留在 IR 里。实际 `sub` 恒为 0，那个分支只依赖
  `%R14 == 0`。

用 LLVM 22 `opt` 对当时 wrk 的 final IR 做后处理量测：

| 变体 | 指令数 | `notdec.unknown` | 字节 |
| --- | --- | --- | --- |
| 现状 | 22,917 | 64 | 1,915,062 |
| `early-cse` | 16,431 | 64 | 1,393,945（-27%） |
| `early-cse,instcombine` | 11,882 | 64 | 1,032,972（-46%） |
| `gvn` | 16,552 | 64 | 1,408,043 |

## 路线（最小修改）

`tools/notdec-native-llvm.cpp`：

1. `runInstCombinePassIfEnabled(..., bool runEarlyCSEFirst = false)`：置位时在
   InstCombine 之前插一个 `llvm::EarlyCSEPass()`；
2. 两个 driver（IR 输入 / ELF 输入）各在**两处**使用它：
   - 寄存器重写之后（重复链的第一来源）；
   - `runFinalCleanupPass` 之后（final cleanup 会把 `notdec.reg.*` 值域辅助调用
     降成显式 insert/extract 链，是第二轮重复的来源，也是大头）。

实测：只在重写后跑一次是 -18%；两处都跑才是 -49%。

### 不做什么

- SSA 之前不加：会改变证据分析看到的 IR，口径变化（warning/arity）要单独实验；
- 不给 `notdec.unknown.*` 加 `memory(none)` 之类属性：那是 unknown 表示法的语义
  决定（会让 LLVM 合并两个独立 unknown），而且在 CSE+InstCombine 之后收益只剩
  几十条死调用。

## 实现记录

- `tools/notdec-native-llvm.cpp`：新增 `#include "llvm/Transforms/Scalar/EarlyCSE.h"`；
  `runInstCombinePassIfEnabled()` 增加 `runEarlyCSEFirst` 参数；两个 driver 的
  "重写后" 与 "final cleanup 后" 调用点传 `true`。沿用 `--no-instcombine-pass`
  开关，raw IR 对照（`--no-register-ssa-pass --no-instcombine-pass`）不受影响。

## 验证（wrk）

| 项 | 之前（da8ff50） | 只加重写后 | 重写后 + final cleanup 后 |
| --- | --- | --- | --- |
| 指令数 | 22,917 | 18,830（-18%） | **11,707（-49%）** |
| IR 字节 / 行 | 1,915,062 / 27,140 | 1,584,232 / 23,029 | **1,017,412 / 15,791（-47%）** |
| define 数 | 87 | 87 | 87 |
| `notdec.unknown` | 64 | 43 | 43 |
| `summary_clobber` | 25 | 25 | 25 |
| warning TSV | 299 行数据 | 299 | 299（与之前 diff 0） |
| residue | 1 行 | 1 | 1 |
| `lua_getfield` / `script_verify_request` 签名 | 3 参 / 1 参 | 同 | 同 |
| 时间 / 内存 | 46.1s / 135MB | 41.8s | 41.0s / 138MB |

21 条 `http_parser_parse_url` 的 RSI unknown 随重复链一起消失；剩下的 43 条与
之前认定的"故意保守/窄 lane"一致（`http_parser_execute` 33、x87 ST0/ST1 7、
其它 3）。

## memcached 与 ctest

memcached（同命令）：

| 项 | 之前（da8ff50） | 本轮 |
| --- | --- | --- |
| 结果 | exit=0 | exit=0（331s） |
| 指令数 | 124,726 | 63,049（**-49%**） |
| IR 字节 | 10,760,089 | 5,652,860（**-47%**） |
| define 数 | 230 | 230 |
| `notdec.unknown` | 1,259 | 1,181 |
| `summary_clobber` | 4,076 | 3,939 |
| warning TSV | 4,974 行数据 | 4,970（上一轮在 `FUN_22b30` 新增的 4 条 RCX clobber 也消失了） |
| residue | 5 | 5 |
| `sasl_listmech` 声明 | 8 参 | 8 参（不变） |

ctest：12/12（含 fortune x86_64/i386；fortune 的"residue 零行"和"不含特定文本"
检查都通过）。`native_register_summary_ssa_test` 也仍然全绿（这个改动只在 CLI
pipeline 里，库函数没动）。

收尾时按 `git clang-format` 调整了 include 顺序并重建，wrk 输出与调整前
warning diff 0、指令数相同（字节差 174 = 编号位数）。
