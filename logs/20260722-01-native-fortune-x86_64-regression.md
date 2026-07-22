# x64 fortune native real-world 回归测试

## 原始需求

> bin2llvm规划一下怎么直接把x64 fortune集成进去，内置x64 fortune的binary也不是不行，但是如果能找到合适的下载链接，同时把binary放到gitignore的地方，文件不存在时下载，这样怎么样。
>
> 对，按这个弄吧，不直接 full-diff .ll

## 背景

之前 x64 fortune 主要靠手工 Bench2 跑法验证。仓库里的 native CTest 只有 `/bin/ls`
smoke 和若干单元测试，不能稳定覆盖 fortune 暴露过的 GTIRB CFG、range SSA、
unknown helper、summary metadata 清理等问题。

## 目标

把 x64 fortune 接入 bin2llvm CTest，但不提交二进制，也不做 `.ll` 全量 diff。
测试只检查稳定事实：native lifting 成功、LLVM 22 能 assemble/verify、没有寄存器残留、
没有 summary return/clobber 和 unknown helper。`register-ssa-warnings.tsv` 作为诊断产物保留，
不作为 pass/fail 条件。

## 实现

- `scripts/fetch-native-fixture.py:23` 到 `:172`：新增 fixture 下载器。固定下载 Ubuntu
  `fortune-mod_1.99.1-7.3build1_amd64.deb`，校验 deb sha256，解包 `usr/games/fortune`，
  再校验 binary sha256。缓存目录默认是
  `tests/fixtures/native/downloads/fortune-x86_64/`。
- `.gitignore:3`：忽略 `tests/fixtures/native/downloads/`，避免把下载产物提交进仓库。
- `tests/fixtures/native/fortune-x86_64.external-prototypes.json:1` 到 `:6`：保留 fortune 当前需要的
  `recode_new_request` 外部原型 overlay。
- `scripts/native-fortune-x86_64-regression.sh:20` 到 `:108`：新增 CTest 入口。它调用下载器，运行
  `notdec-native-llvm --all-confirmed --skip-runtime`，再跑 `llvm-as`、`opt -passes=verify`
  和 `native-register-residue-audit.py --details`。warning TSV 会写出路径，但不要求为空。
- `tools/CMakeLists.txt:155` 到 `:170`：注册
  `notdec.native_llvm.realworld_fortune_x86_64`。如果 fixture 下载失败且没有缓存，
  脚本返回 77，CTest 标记为 skip；设置 `NOTDEC_REQUIRE_NATIVE_FIXTURES=1` 时下载失败会变成 fail。

## 判断标准

- 测试能从空缓存下载 fortune，生成 native IR，并通过 LLVM verify。
- `register-residue-audit.tsv` 只有表头。
- 输出 IR 不含 `notdec.register.summary_return`、`notdec.register.summary_clobber`、
  `notdec.unknown`。

## 验证

- `python3 -m py_compile external/NotDec-bin2llvm/scripts/fetch-native-fixture.py`：通过。
- `bash -n external/NotDec-bin2llvm/scripts/native-fortune-x86_64-regression.sh`：通过。
- `external/NotDec-bin2llvm/scripts/native-fortune-x86_64-regression.sh ...`：通过；输出
  `external/NotDec-bin2llvm/build/native-fortune-x86_64-regression/fortune.native.ll`。
- `cmake -S external/NotDec-bin2llvm -B external/NotDec-bin2llvm/build`：通过；新测试注册为
  `notdec.native_llvm.realworld_fortune_x86_64`。
- `ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_llvm.realworld_fortune_x86_64 --output-on-failure`：通过。
- `ctest --test-dir external/NotDec-bin2llvm/build --output-on-failure`：15/16 通过；
  `notdec.native_llvm.cli_signature_rewrite_ll_smoke` 失败，原因是旧 smoke 期望
  `define void @cli_input_rdi(i64 %`，实际输出仍是未重写的 register-global 形状。
  这个失败项不涉及本次新增 fortune 回归。

## 评分

- 实现效果：8/10。x64 fortune 已变成可重复 CTest，不依赖 Bench2 目录，也不做脆弱 IR diff。
- 复杂度：3/10。只新增下载器和 shell 入口，CMake 注册很小；主要复杂度来自 hash 校验和 skip 语义。
- 维护成本：3/10。fixture URL 和 hash 固定；未来如果 Ubuntu 清理 archive，可改同一个脚本里的 URL。
