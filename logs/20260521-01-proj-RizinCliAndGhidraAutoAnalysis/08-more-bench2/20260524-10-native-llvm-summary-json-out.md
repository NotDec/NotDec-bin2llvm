# Native LLVM Summary JSON Output

## 原始 prompt

修复

## 背景

selected-targets-native 脚本原来每个目标先跑：

```text
notdec-native-discover --summary-json
```

再跑：

```text
notdec-native-llvm --all-confirmed
```

`--all-confirmed` 本来就必须跑 native discovery 才能拿 confirmed functions，所以这里多付了一次 discovery 成本。`libuv` 这种目标一次 discovery 就是一百多秒，重复跑不划算。

## 修改

- [tools/notdec-native-llvm.cpp:54](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:54)
  usage 增加 `--summary-json-out <path>`。
- [tools/notdec-native-llvm.cpp:117](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:117)
  参数解析支持 `--summary-json-out`。
- [tools/notdec-native-llvm.cpp:257](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:257)
  新增 `writeSummaryJson(...)`，输出和 `notdec-native-discover --summary-json` 同口径的关键字段：seed、confirmed function、basic block、instruction、source、confidence、xref、unresolved flow、notes 数量。
- [tools/notdec-native-llvm.cpp:624](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:624)
  `--all-confirmed` / `-f` / `-n` 路径只跑一次 discovery；如果传了 `--summary-json-out`，直接复用这份 state 写 summary。
- `/sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py:147`
  selected-targets-native 脚本改成：

```text
notdec-native-llvm <target> --all-confirmed --summary-json-out summary.json -o module-all.ll
```

不再前置调用 `notdec-native-discover --summary-json`。

## 验证

构建：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
```

小目标验证：

```text
/usr/bin/time -f 'TIME %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/php/20230831/calendar.so \
  --all-confirmed \
  --summary-json-out /tmp/notdec-native-php-calendar.summary.json \
  -o /tmp/notdec-native-php-calendar.ll

TIME 26.83
defines=52
summary confirmed_functions=52
llvm-as ok
opt -passes=verify ok
```

脚本检查：

```text
bash -n /sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py
/sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py --help
```

CTest：

```text
notdec.native_discover.x86_64_smoke passed 3.81s
notdec.native_llvm.x86_64_smoke passed 0.61s
```

## 影响判断

- 实现效果：7/10。selected-targets-native 的前置 discovery 被去掉，重跑每个目标少一次 native discovery。
- 复杂度：4/10。只给 `notdec-native-llvm` 增加 summary 输出，没有引入 discovery cache 格式。
- 维护成本：4/10。summary 输出字段和 discover summary 有少量重复代码，后续如果字段继续增加，可以再抽公共 JSON writer。

## 剩余问题

- `bench2-native-smoke.sh` 为了交叉检查 summary / seeds / blocks / xrefs，仍会多次调用 `notdec-native-discover`。这是 smoke 脚本自己的覆盖检查成本，不影响 selected-targets-native 主重跑路径。
