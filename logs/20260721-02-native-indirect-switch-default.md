对，搞成这种吧

# Native indirect switch default 改成 trap

## 背景

fortune 的 jump table 在 `0x2872` 已经由 GTIRB/DDISASM 给出完整 successor facts：
14 个 case successor，没有 default edge。原 lowering 把 resolved `BRANCHIND`
的 `switch` default 接到 `exitBlock()`，最终变成 `common.ret`，导致 IR 里出现
程序不存在的“默认返回”路径。

## 实现

- `lib/PcodeToLLVM.cpp:1110`：
  新增 `nativeIndirectSwitchDefaultBlock()`，生成
  `native_indirect_default_<addr>` block，里面调用 `llvm.trap()` 后 `unreachable`。
- `lib/PcodeToLLVM.cpp:1301`：
  native CFG 下 resolved multi-successor `BRANCHIND` 的 `switch` default 不再接
  `exitBlock()`，改接上面的 trap block。native successor facts 是权威输入；
  如果运行时落到未列出的 target，这是坏 fact 或暂不支持路径，不应伪造成函数返回。
- `tests/pcode_to_llvm_test.cpp:375`：
  修正一个 native conditional fixture，显式给 false successor，避免和“缺 false edge 报错”
  规则冲突。
- `tests/pcode_to_llvm_test.cpp:1210`：
  扩展 `testNativeIndirectBranchLowersMultipleSuccessorsAsSwitch()`，检查 switch default
  是 `llvm.trap()` + `unreachable`，并跑 verifier。

## 验证

```bash
cmake --build external/NotDec-bin2llvm/build --target pcode_to_llvm_test notdec-native-llvm -j4
external/NotDec-bin2llvm/build/bin/pcode_to_llvm_test

OUT=/tmp/notdec-bin2llvm-fortune-indirect-default-trap-20260721-161444
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" \
  --all-confirmed --skip-runtime \
  --external-prototypes /sn640/NotDec-Exp/Bench2/bin2llvm-external-prototypes/fortune-executable.external-prototypes.json \
  --register-ssa-warning-out "$OUT/register-ssa-warnings.tsv"
llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -disable-output
python3 external/NotDec-bin2llvm/scripts/native-register-residue-audit.py --details "$OUT/fortune.native.ll"
```

结果：

- fortune IR 里 `switch i64 %7` 的 default 已变成
  `%native_indirect_default_2872`。
- `%native_indirect_default_2872` 内是 `call void @llvm.trap()` 和 `unreachable`。
- 没有 `switch ... label %common.ret` 的伪默认返回。
- residue audit 只有表头，未发现寄存器残留。

## 评价

- 实现效果：8/10。修掉了 jump table default 的虚假返回路径。
- 复杂度：2/10。只在 native resolved indirect branch lowering 上加一个专用 default block。
- 维护成本：2/10。后续如果要区分“bad fact”和“动态未解析路径”，可以在 trap block 附加 diagnostic metadata。
