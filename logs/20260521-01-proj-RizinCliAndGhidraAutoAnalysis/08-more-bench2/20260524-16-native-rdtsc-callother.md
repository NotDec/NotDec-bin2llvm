# Native RDTSC CALLOTHER Lowering

## 原始 prompt

修复

## 背景

`--decode-seed-limit 200` 重跑 selected-targets-native 后，所有目标都能通过 `llvm-as` / `opt -passes=verify`，剩余 helper 只剩：

```text
notdec_pcode_CALLOTHER_i64(i32 77)
```

Ghidra x86 spec 里：

```text
define pcodeop rdtsc;
RDTSC { tmp:8 = rdtsc(); EDX = tmp(4); EAX = tmp(0); }
```

这里的 userop id `77` 对应 `rdtsc()`，输出是 64-bit timestamp。LLVM 有 `llvm.readcyclecounter()`，可以作为当前 native lowering 的保守表达。

## 修改

- [lib/PcodeToLLVM.cpp:1040](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:1040)
  `lowerCallOther(...)` 识别 `CALLOTHER` 输入常量 `77` 且有输出时，生成：

```llvm
call i64 @llvm.readcyclecounter()
```

无输出的 x86 `LOCK` / `UNLOCK` userop 仍保持原逻辑。

## 验证

构建：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
```

memcached limited：

```text
TIME memcached 81.81
llvm-as ok
opt -passes=verify ok
```

输出里不再有 `notdec_pcode_`，对应位置变为：

```text
call i64 @llvm.readcyclecounter()
```

tmux limited：

```text
TIME tmux 82.05
llvm-as ok
opt -passes=verify ok
```

输出里不再有 `notdec_pcode_`，多处 `CALLOTHER 77` 都变为 `llvm.readcyclecounter()`。

CTest：

```text
notdec.native_discover.x86_64_smoke passed 3.86s
notdec.native_llvm.x86_64_smoke passed 0.59s
```

## 影响判断

- 实现效果：8/10。limited selected-targets 暴露出的最后一类 helper 已有精确 LLVM 表达。
- 复杂度：2/10。只处理一个 x86 userop id，逻辑局部。
- 维护成本：3/10。userop id 依赖当前 Ghidra x86 spec；如果后续支持更多架构，需要按架构分派。

## 剩余问题

- wolfssl full discovery 仍慢，这是 discovery 性能问题。

## 补充验证

用 `--decode-seed-limit 200` 重跑 selected-targets-native 到临时目录：

```text
/usr/bin/time -f 'TIME selected-limited2 %e' \
  /sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py \
  --decode-seed-limit 200 \
  --output-root /tmp/notdec-selected-native-ir-limited2 \
  --native-project-root /tmp/notdec-selected-native-projects-limited2

TIME selected-limited2 1064.23
```

结果：

- 14 个 selected targets 全部 `ok`。
- 所有 `module-all.ll` 都没有 `notdec_pcode_`。
- 每个目标的 `llvm-as` / `opt -passes=verify` 都已由脚本通过。

confirmed function 数：

```text
ffmpeg-resample-library    148
libicu-common-library      200
libuv-shared-library       200
lighttpd-executable        200
memcached-executable       200
openssh-client             200
php-extension-calendar     52
python-shared-library      200
redis-server-symlink       200
tmux-executable            200
vim-executable             200
vsftpd-executable          186
wolfssl-shared-library     200
wrk-executable             142
```
