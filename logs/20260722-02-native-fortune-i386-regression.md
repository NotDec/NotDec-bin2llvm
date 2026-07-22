# i386 fortune native real-world 回归测试

## 原始需求

> 接下来尝试用类似的方式集成x86的fortune，基于bionic的apt包。想着先集成进去，后面再修bug

## 背景

x64 fortune 已经接入 CTest。i386 fortune 也需要先有固定 fixture 和可见测试入口，
这样后续补 32 位 x86 SLEIGH/ABI/栈参数支持时，可以直接把 skip 变成真跑。

当前 `notdec-native-llvm` 自动 SLEIGH 选择只支持 x86-64 ELF。i386 binary 会报：
`automatic sleigh spec selection only supports x86-64 ELF`。

## 实现

- `scripts/fetch-native-fixture.py:34` 到 `:46`：新增 `fortune-i386` fixture。
  固定下载 Ubuntu bionic `fortune-mod_1.99.1-7build1_i386.deb`，校验 deb sha256，
  解包并校验 `usr/games/fortune` sha256。
- `tests/fixtures/native/fortune-i386.external-prototypes.json:1` 到 `:6`：放入和
  x64 fortune 一致的 `recode_new_request` 外部原型 overlay。
- `scripts/native-fortune-i386-regression.sh:20` 到 `:122`：新增 i386 CTest 入口。
  当前遇到已知 x86-64-only 错误时返回 77，CTest 记为 skip；如果后续 native 工具
  能生成 IR，则继续跑 `llvm-as`、`opt -passes=verify` 和寄存器残留检查。
- `tools/CMakeLists.txt:172` 到 `:187`：注册
  `notdec.native_llvm.realworld_fortune_i386`，标签为 `native;realworld;i386`。

## 判断标准

- fixture 能从空缓存下载并校验 bionic i386 fortune。
- 当前链路下 CTest 能稳定显示 i386 fortune 为 skip，而不是 fail。
- 后续 i386 支持接上后，同一个测试会继续检查 LLVM verify、无寄存器残留、无
  `summary_return` / `summary_clobber` / `notdec.unknown`。

## 验证

- `python3 -m py_compile external/NotDec-bin2llvm/scripts/fetch-native-fixture.py`：通过。
- `bash -n external/NotDec-bin2llvm/scripts/native-fortune-i386-regression.sh`：通过。
- `external/NotDec-bin2llvm/scripts/fetch-native-fixture.py fortune-i386 --source-dir external/NotDec-bin2llvm`：
  通过；下载出的 binary 是 `ELF 32-bit LSB pie executable, Intel 80386`，
  BuildID 为 `790ca855eceba75bba0b99c0c63694fcc4f24b9c`。
- `external/NotDec-bin2llvm/scripts/native-fortune-i386-regression.sh ...`：返回 77；
  命中当前已知错误 `automatic sleigh spec selection only supports x86-64 ELF`。
- `cmake -S external/NotDec-bin2llvm -B external/NotDec-bin2llvm/build`：通过；
  新测试注册为 `notdec.native_llvm.realworld_fortune_i386`。
- `ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_llvm.realworld_fortune_i386 --output-on-failure`：
  通过，测试状态为 skipped。
- `ctest --test-dir external/NotDec-bin2llvm/build --output-on-failure`：17 个测试里
  16 个通过，`notdec.native_llvm.realworld_fortune_i386` skipped，0 failed。

## 评分

- 实现效果：7/10。i386 fortune fixture 和 CTest 入口已经固定下来；当前由于 native 工具
  不支持 i386 自动 SLEIGH 选择，只能先 skip。
- 复杂度：3/10。复用 x64 fixture 下载和回归结构，只多一个已知错误 skip 分支。
- 维护成本：3/10。后续修完 i386 支持后，删除或绕过 skip 分支即可让同一个测试变成真回归。
