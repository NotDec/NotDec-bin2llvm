# Native Discovery Debug Info Oracle

## 原始 prompt

修复

## 背景

前面已经修了 native discovery 全量 seed decode、raw P-Code opcode 覆盖、relative P-Code branch 和 CTest smoke 限制。follow-up 清单里还缺一个能量化 native function discovery 的 oracle。

Bench2 manifest 已经有 debug 信息路径：

- 普通目标：`/usr/lib/debug/.build-id/...debug`
- embedded debug 目标：`EMBEDDED_DEBUG`

需要用这些 debug 信息区分两个问题：

- function seed 有没有找到。
- seed 找到了但 confirmed decode 有没有覆盖。

## 目标

新增一个脚本，用 Bench2 debug info 里的 concrete `DW_TAG_subprogram` low_pc 作为 oracle，对比 native seed 和 confirmed function 覆盖。

## 实现

新增：

- `scripts/bench2-native-discovery-debug-check.py`

脚本行为：

1. 读取 `Bench2/manifest/benchmark-targets.tsv`。
2. 支持 `--target project:role` 选择目标。
3. 解析目标 ELF 的 executable `LOAD` range。
4. 用 LLVM 22 `llvm-dwarfdump --debug-info` 读取 debug 文件。
5. 只统计 concrete subprogram：
   - 有 `DW_AT_low_pc`
   - 有 `DW_AT_high_pc`
   - 没有 `DW_AT_declaration(true)`
   - low_pc 落在 executable range 内
6. 跑 `notdec-native-discover --seeds-json` 和 `--functions-json`。
7. 输出 TSV：
   - debug concrete function 数
   - native seed 数
   - native confirmed function 数
   - seed 命中数 / 覆盖率
   - confirmed 命中数 / 覆盖率
   - confirmed 漏掉的 debug function 样本

脚本也支持：

```bash
--decode-seed-limit <count>
```

用于快速 sanity；默认不限制。

## 验证

语法检查：

```bash
python3 -m py_compile scripts/bench2-native-discovery-debug-check.py
```

帮助：

```bash
scripts/bench2-native-discovery-debug-check.py --help
```

限量 sanity：

```bash
scripts/bench2-native-discovery-debug-check.py \
  --target libuv:shared-library \
  --target vsftpd:executable \
  --target memcached:executable \
  --decode-seed-limit 20
```

结果：

```text
project	role	debug_functions	native_seeds	native_confirmed	seed_hits	confirmed_hits	seed_coverage	confirmed_coverage	missing_confirmed_sample
vsftpd	executable	175	186	20	174	12	0.9943	0.0686	0x6740:,0x9000:,0x9110:,0x91c0:,0x9290:,0x9340:,0x94a0:,0x9d10:
libuv	shared-library	394	484	20	376	12	0.9543	0.0305	0x9b40:,0x9c74:,0x9e40:,0x9e50:,0x9e70:,0x9ef0:,0x9f80:,0xa030:
memcached	executable	229	258	20	217	11	0.9476	0.0480	0x6978:,0x6a2d:,0x6ba0:,0xc150:,0xc3d0:,0xc4c0:,0xc4e0:,0xc6d0:
```

默认全量 sanity：

```bash
/usr/bin/time -f 'TIME %e' \
  scripts/bench2-native-discovery-debug-check.py \
  --target php:extension-calendar
```

结果：

```text
project	role	debug_functions	native_seeds	native_confirmed	seed_hits	confirmed_hits	seed_coverage	confirmed_coverage	missing_confirmed_sample
php	extension-calendar	36	52	52	28	28	0.7778	0.7778	0x3820:zif_cal_to_jd,0x3a90:zif_cal_info,0x3b68:zif_cal_days_in_month,0x3f65:zif_cal_from_jd,0x4790:zif_jdtojewish,0x5bc2:,0x5f20:zif_unixtojd,0x5fd8:zif_jdtounix
TIME 33.08
```

## 结论

现在有了基于 Bench2 debug info 的 discovery oracle。它已经能看出：

- `libuv` / `vsftpd` / `memcached` 的 native seed 覆盖 debug function 很高。
- 在 `--decode-seed-limit 20` 下，confirmed 覆盖很低，这是预期的 smoke 模式效果。
- `php calendar` 默认全量下 seed 和 confirmed 覆盖一致，说明脚本能区分 seed 覆盖和 confirmed 覆盖。

需要注意：脚本当前会分别跑 `--seeds-json` 和 `--functions-json`，所以默认全量模式会重复跑两次 discovery。后续应结合 discovery cache 或新增一次输出 seed+function 的 JSON，避免重复成本。

## 性能

`php:extension-calendar` 默认全量脚本耗时 33.08 秒，主要来自两次 native discovery。这和 follow-up 里的 “`--all-confirmed` / 脚本重复 discovery” 是同类问题。

## 评分

- 实现效果：8/10。已经能用 debug info 量化 seed / confirmed 覆盖。
- 复杂度：7/10。脚本独立，依赖 `llvm-dwarfdump`、`readelf` 和现有 `notdec-native-discover`。
- 维护成本：7/10。DWARF 文本解析足够做当前 oracle，但后续如果要更严谨，可以改成 llvm-dwarfdump JSON 或专门解析库。

## 2026-05-28 修正：不要把 inline 子 DIE 当函数

继续用 debug oracle 检查 selected Bench2 目标时，`vim` 一开始显示：

```text
vim	executable	4244	5309	5309	3577	3577	0.8428	0.8428	0x40747:,0x41bf8:,0x427aa:,0x46987:,0x470cf:,0x4f042:,0x4f825:,0x56f6d:
```

抽样查 `llvm-dwarfdump --debug-info` 后发现这些地址不是 `DW_TAG_subprogram` 入口，而是 `DW_TAG_inlined_subroutine` 或 lexical block 的 `DW_AT_low_pc`。脚本原来的 parser 在进入 subprogram 后，会继续接收子 DIE 的属性；如果子 DIE 也有 `DW_AT_low_pc`，就会污染父 subprogram，造成假漏报。

修改：

- [scripts/bench2-native-discovery-debug-check.py:90](/sn640/NotDec/external/NotDec-bin2llvm/scripts/bench2-native-discovery-debug-check.py:90) `parse_debug_functions`
  - 增加 `accepting_attrs`。
  - 只接收当前 `DW_TAG_subprogram` 自己的属性。
  - 遇到子 DIE 后停止接收属性，避免 inline range 覆盖父函数地址。
- [tests/bench2_native_discovery_debug_check_test.py:29](/sn640/NotDec/external/NotDec-bin2llvm/tests/bench2_native_discovery_debug_check_test.py:29)
  - 新增最小单测，确认 parser 会忽略 `DW_TAG_inlined_subroutine` 的 `DW_AT_low_pc`。
- [CMakeLists.txt:9](/sn640/NotDec/external/NotDec-bin2llvm/CMakeLists.txt:9)
  - 把这个单测接入 CTest：`notdec.bench2_native_discovery_debug_oracle_unit`。

验证：

```bash
python3 -m py_compile \
  scripts/bench2-native-discovery-debug-check.py \
  tests/bench2_native_discovery_debug_check_test.py

python3 tests/bench2_native_discovery_debug_check_test.py

ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec\.(bench2_native_discovery_debug_oracle_unit|native_discover\.x86_64_smoke|native_llvm\.x86_64_smoke)' \
  --output-on-failure
```

CTest 结果：

```text
100% tests passed, 0 tests failed out of 3
Total Test time (real) =   0.88 sec
```

修正后 `vim` 单项目标：

```bash
/usr/bin/time -f 'TIME vim-oracle-fixed %e' \
  scripts/bench2-native-discovery-debug-check.py --target vim:executable
```

```text
vim	executable	3569	5309	5309	3569	3569	1.0000	1.0000
TIME vim-oracle-fixed 21.43
```

selected6：

```bash
/usr/bin/time -f 'TIME debug-oracle-selected6-fixed %e' \
  scripts/bench2-native-discovery-debug-check.py \
  --target libuv:shared-library \
  --target vsftpd:executable \
  --target memcached:executable \
  --target python:shared-library \
  --target vim:executable \
  --target wolfssl:shared-library
```

```text
vsftpd	executable	174	187	187	174	174	1.0000	1.0000
libuv	shared-library	376	485	485	376	376	1.0000	1.0000
memcached	executable	216	259	259	216	216	1.0000	1.0000
wolfssl	shared-library	4127	4160	4160	4127	4127	1.0000	1.0000
vim	executable	3569	5309	5309	3569	3569	1.0000	1.0000
python	shared-library	6866	7343	7343	6866	6866	1.0000	1.0000
TIME debug-oracle-selected6-fixed 102.85
```

结论：

- selected6 里 native discovery 对 debug subprogram 入口的覆盖是 100%。
- 之前 `vim` 84.28% 是 oracle 误报，不是 native discovery 漏函数。
- full oracle 约 103 秒，主要是大 DWARF 文本解析和 discovery；只适合作为手动 coverage gate，不放进默认 CTest。

评分调整：

- 实现效果：9/10。oracle 对 inline 子范围的误报已修掉，并有单测覆盖。
- 复杂度：7/10。仍然是文本 parser，但逻辑边界更清楚。
- 维护成本：6/10。新增 CTest 能防止同类回归；后续如果还遇到 DWARF 表达形式问题，再考虑换成结构化 DWARF 读取。
