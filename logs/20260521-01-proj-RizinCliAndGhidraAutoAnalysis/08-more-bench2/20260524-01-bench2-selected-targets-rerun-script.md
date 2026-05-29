# Bench2 Selected Targets Native Rerun Script

## 记录

这次主要动 Bench2 侧脚本。后续 native 侧修复完成后，又用同一个脚本做了一轮受限验证。

新增脚本：

- `Bench2/scripts/export-bin2llvm-selected-targets.py:4-185`

新增输出目录默认值：

- `Bench2/bin2llvm-ir/selected-targets-native/`
- `Bench2/bin2llvm-native-projects/selected-targets-native/`

脚本行为：

1. 从 `Bench2/manifest/benchmark-targets.tsv` 里按固定映射挑每个 project 的一个目标。
2. 走 native 链路，用 `notdec-native-llvm --all-confirmed --summary-json-out` 同一次分析结果导出 summary 和 LLVM IR。
3. 再依次跑 `llvm-as` 和 `opt -passes=verify`。
4. 每个项目的结果单独落在新的输出目录下，并写 `summary.tsv`。

后续补充：

- `Bench2/scripts/export-bin2llvm-selected-targets.py:38-108` 增加 `--decode-seed-limit COUNT`，用于先做受限验证，避免 `wolfssl` 这种目标在完整 discovery 优化前拖住整轮。
- `Bench2/scripts/export-bin2llvm-selected-targets.py:156` 改为调用 `notdec-native-llvm --summary-json-out`，避免 discover 和 LLVM export 各跑一遍 discovery。

固定选择的 target 仍然和当前 coverage audit 对齐，具体是：

- `vsftpd` -> `executable`
- `libuv` -> `shared-library`
- `memcached` -> `executable`
- `lighttpd` -> `executable`
- `tmux` -> `executable`
- `openssh` -> `client`
- `wolfssl` -> `shared-library`
- `redis` -> `server-symlink`
- `libicu` -> `common-library`
- `vim` -> `executable`
- `python` -> `shared-library`
- `wrk` -> `executable`
- `ffmpeg` -> `resample-library`
- `php` -> `extension-calendar`

## 验证

做了语法检查和 help 检查：

```bash
bash -n /sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py
/sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py --help
```

native 侧修复后，用 LLVM 22 做了一轮受限 selected targets 验证：

```bash
rm -rf /tmp/notdec-selected-native-ir-limited2 /tmp/notdec-selected-native-projects-limited2
/usr/bin/time -f 'TIME selected-limited2 %e' \
  /sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py \
  --decode-seed-limit 200 \
  --output-root /tmp/notdec-selected-native-ir-limited2 \
  --native-project-root /tmp/notdec-selected-native-projects-limited2
```

结果：

- 14 个 selected targets 全部 `ok`。
- 脚本内部使用 `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as` 和 `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify`，所有 `module-all.ll` 通过验证。
- 总耗时：`TIME selected-limited2 1064.23`。
- `rg -n 'notdec_pcode_' /tmp/notdec-selected-native-ir-limited2 -g '*.ll'` 没有命中，说明这轮没有残留未 lowering 的 helper 调用。

受限 discovery 的 confirmed 数量：

| target | confirmed functions |
| --- | ---: |
| `ffmpeg-resample-library` | 148 |
| `libicu-common-library` | 200 |
| `libuv-shared-library` | 200 |
| `lighttpd-executable` | 200 |
| `memcached-executable` | 200 |
| `openssh-client` | 200 |
| `php-extension-calendar` | 52 |
| `python-shared-library` | 200 |
| `redis-server-symlink` | 200 |
| `tmux-executable` | 200 |
| `vim-executable` | 200 |
| `vsftpd-executable` | 186 |
| `wolfssl-shared-library` | 200 |
| `wrk-executable` | 142 |

## 结论

现在 Bench2 侧有了独立的 native selected targets 重跑入口。native 修复后的受限验证已经能稳定导出 14 个目标，并通过 LLVM 22 verifier。

还没解决的是完整 discovery 性能，主要是 `wolfssl` 这种 `.eh_frame` seed 很多的目标。下一步应该先优化 native discovery 的性能，再去掉 `--decode-seed-limit 200` 跑正式输出目录。

## Debug Oracle 手动测试方法

debug oracle 不接入主项目默认测试。原因是它依赖 `/sn640/NotDec-Exp/Bench2` 里的二进制和 debug 文件，而且 selected6 全量约 103 秒，更适合作为手动 coverage gate。

脚本位置：

```bash
/sn640/NotDec/external/NotDec-bin2llvm/scripts/bench2-native-discovery-debug-check.py
```

先确认 native 工具已构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

快速单目标检查，例如 `vim`：

```bash
cd /sn640/NotDec/external/NotDec-bin2llvm
/usr/bin/time -f 'TIME vim-oracle %e' \
  scripts/bench2-native-discovery-debug-check.py \
  --target vim:executable
```

当前期望结果：

```text
vim	executable	3569	5309	5309	3569	3569	1.0000	1.0000
```

selected6 检查：

```bash
cd /sn640/NotDec/external/NotDec-bin2llvm
/usr/bin/time -f 'TIME debug-oracle-selected6 %e' \
  scripts/bench2-native-discovery-debug-check.py \
  --target libuv:shared-library \
  --target vsftpd:executable \
  --target memcached:executable \
  --target python:shared-library \
  --target vim:executable \
  --target wolfssl:shared-library
```

当前期望结果：

```text
vsftpd	executable	174	187	187	174	174	1.0000	1.0000
libuv	shared-library	376	485	485	376	376	1.0000	1.0000
memcached	executable	216	259	259	216	216	1.0000	1.0000
wolfssl	shared-library	4127	4160	4160	4127	4127	1.0000	1.0000
vim	executable	3569	5309	5309	3569	3569	1.0000	1.0000
python	shared-library	6866	7343	7343	6866	6866	1.0000	1.0000
TIME debug-oracle-selected6 102.85
```

看结果时重点看：

- `seed_coverage`：native function seed 是否覆盖 debug subprogram 入口。
- `confirmed_coverage`：recursive discovery 是否确认了这些入口。
- `missing_confirmed_sample`：非空时再用 `llvm-dwarfdump` 或 `objdump` 抽样确认是真漏函数，还是 debug inline / range 误报。
