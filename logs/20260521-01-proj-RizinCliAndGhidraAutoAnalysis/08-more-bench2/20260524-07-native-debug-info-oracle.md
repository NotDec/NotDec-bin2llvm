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
