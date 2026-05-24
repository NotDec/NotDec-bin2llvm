# Native Discovery Combined JSON

## 原始 prompt

修复

## 背景

`scripts/bench2-native-discovery-debug-check.py` 初版为了同时拿 seed 和 confirmed function，分别调用：

```bash
notdec-native-discover --seeds-json
notdec-native-discover --functions-json
```

默认全量模式会重复跑两次 native discovery。`php:extension-calendar` 的 oracle 脚本耗时 33.08 秒，其中主要成本就是重复 discovery。

follow-up 清单里已经记录要处理 debug oracle 脚本重复 discovery。

## 目标

让 debug oracle 脚本只跑一次 discovery，同时保持原有 `--seeds-json` 和 `--functions-json` 兼容。

## 实现

修改 `tools/notdec-native-discover.cpp`：

- 新增输出模式：

```bash
--discovery-json <elf-file>
```

- 输出同一次 discovery 的 seeds 和 confirmed functions：

```json
{
  "seeds": [...],
  "seed_count": 0,
  "functions": [...],
  "function_count": 0
}
```

- 把原来的 seed/function JSON 输出拆成数组 helper：
  - `printSeedsArrayJson(...)`
  - `printFunctionsArrayJson(...)`
- 原有 `--seeds-json` / `--functions-json` 输出格式不变。

修改 `scripts/bench2-native-discovery-debug-check.py`：

- 改为调用一次：

```bash
notdec-native-discover --discovery-json
```

- 从同一份 JSON 里读取 `seeds` 和 `functions`。

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

JSON 格式检查：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --discovery-json /bin/ls \
  > /tmp/notdec-ls-discovery.json
python3 -m json.tool /tmp/notdec-ls-discovery.json
```

结果：

```text
seeds=176 seed_count=176 functions=20 function_count=20
```

debug oracle 限量 sanity：

```bash
scripts/bench2-native-discovery-debug-check.py \
  --target libuv:shared-library \
  --target vsftpd:executable \
  --target memcached:executable \
  --decode-seed-limit 20
```

结果和上一版一致：

```text
vsftpd	executable	175	186	20	174	12	0.9943	0.0686	...
libuv	shared-library	394	484	20	376	12	0.9543	0.0305	...
memcached	executable	229	258	20	217	11	0.9476	0.0480	...
```

debug oracle 默认全量：

```bash
/usr/bin/time -f 'TIME %e' \
  scripts/bench2-native-discovery-debug-check.py \
  --target php:extension-calendar
```

结果：

```text
php	extension-calendar	36	52	52	28	28	0.7778	0.7778	...
TIME 16.39
```

上一版同目标是 33.08 秒，这次降到 16.39 秒。

CTest：

```bash
ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec.native_discover.x86_64_smoke' \
  --output-on-failure
```

结果：

```text
notdec.native_discover.x86_64_smoke Passed 3.88 sec
```

## 结论

debug oracle 脚本的重复 discovery 已修复。现在一次 `notdec-native-discover --discovery-json` 就能拿到 seed 和 confirmed function。

剩余重复 discovery 主要在 `notdec-native-llvm --all-confirmed`：如果外部脚本先跑 discover 再跑 native-llvm，native-llvm 内部仍会重新 discovery。这个需要后续 cache/state 复用方案。

## 性能

`php:extension-calendar` debug oracle 从 33.08 秒降到 16.39 秒，基本符合减少一次 full discovery 的预期。

## 评分

- 实现效果：9/10。直接减少 debug oracle 一次 discovery，输出保持兼容。
- 复杂度：6/10。只新增一个组合 JSON 模式。
- 维护成本：6/10。后续其他脚本也可以复用 `--discovery-json`。
