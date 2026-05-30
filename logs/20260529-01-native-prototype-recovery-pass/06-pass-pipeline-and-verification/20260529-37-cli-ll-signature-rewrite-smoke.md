# 原始 prompt

```text
继续实现 logs/20260529-01-native-prototype-recovery-pass/GOAL.md；基于已有进度，选择下一小步先写计划，再实现、验证、更新 PROGRESS.md，并提交。
```

# 背景

`notdec-native-llvm --rewrite-prototype-signatures` 已经接到 prototype recovery pass，库级测试也覆盖了 opt-in rewrite。现在缺一个 CLI 层面的 `.ll` 输入 smoke，确认文本 IR 经过工具入口后真的会走签名重写，并且输出仍能被 LLVM 22 验证。

这一步只补验证，不改核心 pass。

# Ghidra 实现参考

Ghidra 的原型恢复不是只在内部数据结构里保存结果，最后会把恢复出的参数和返回值应用到函数原型：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：按当前推断结果更新函数原型。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：从调用点和 storage 使用推出 input/output map。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::apply(...)`：把恢复出的原型信息接入函数/调用关系。

native 侧已有 `NativePrototypeRecovery` 复刻了最小路径：从 `!notdec.abi` 和 register metadata 得到 `notdec.prototype.recovered`，再在显式 opt-in 时改 LLVM 函数签名。

# native 侧复刻策略

- 增加一个最小 `.ll` fixture：
  - module 带 `!notdec.abi`；
  - 函数 `@cli_input_rdi` 从 `@RDI` 读取 external input；
  - 函数级 metadata 标出 `RDI` 是 external input。
- 在 `tools/CMakeLists.txt` 增加 CLI smoke：
  - 调用 `notdec-native-llvm --rewrite-prototype-signatures <fixture.ll> -o <out.ll>`；
  - 用项目指定 LLVM 22 的 `llvm-as` / `opt -passes=verify` 验证输出；
  - 用文本检查确认输出里有 `define void @cli_input_rdi(i64 %...)`。

暂时不做：

- 不改 CLI summary 文本。
- 不扩大到 ELF smoke；ELF 路径更慢，也更受环境影响。
- 不加新的脚本。

# 判断标准

- 新 CTest 能证明 `.ll` CLI 输入的 opt-in signature rewrite 生效。
- 输出 `.ll` 能通过 LLVM 22 汇编和 verify。
- 现有测试继续通过。

# 风险

- fixture 手写 metadata 容易和当前 metadata reader 不一致。控制方法是只写当前 pass 真正需要的最小字段。
- CMake shell 命令有转义风险。控制方法是只用一个短 bash 命令。

# 实现记录

改动：

- `tests/ir/native-prototype/cli-signature-rewrite.ll:1` 新增最小 CLI fixture，包含 `@RDI` register metadata、`@cli_input_rdi` external input load、函数级 `notdec.register.external_inputs` 和 module 级 `!notdec.abi`。
- `tools/CMakeLists.txt:144` 新增 `notdec.native_llvm.cli_signature_rewrite_ll_smoke`。测试调用 `notdec-native-llvm` 的 `.ll` 输入路径并打开 `--rewrite-prototype-signatures`，检查输出函数变成 `void (i64)`，再用 LLVM 22 `llvm-as` / `opt -passes=verify` 验证。

验证：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #9: notdec.native_llvm.cli_signature_rewrite_ll_smoke ... Passed`，耗时约 `0.09 sec`；全量测试 `9/9` 通过，总耗时约 `0.92 sec`。

性能：只新增一个 CTest smoke，不改 runtime 代码。对 pass 运行性能无影响。

评分：

- 实现效果：6/10。覆盖了 CLI `.ll` opt-in rewrite 的关键入口。
- 复杂度：2/10。只有一个 fixture 和一个 CMake test。
- 维护成本：2/10。后续 CLI 参数或 metadata 格式变动时，这个 smoke 会直接失败。
