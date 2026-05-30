# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

CLI `.ll` smoke 已覆盖各种显式签名重写形状，但当前只检查签名和 transient prototype metadata。库层已经有“签名重写后删除 callee 内旧 return register store”的测试；CLI smoke 也应该固定这个结果，避免命令行路径生成了新返回值后仍保留旧 RAX/RDX store。

这一步只补 CLI 输出负向检查，不改 recovery 算法。

# Ghidra 实现参考

Ghidra 更新函数 prototype 后，返回值会通过函数 return storage 表达，旧的 output varnode 不再作为普通寄存器副作用留在反编译结果里：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：把 return output trial 归入 prototype output map。
  - `FuncProto::updateAllTypes(...)`：把恢复出的 output storage 应用到函数原型。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：应用 prototype 类型恢复结果。

native 侧对应关系是：显式签名重写把 ABI output register store 的值变成 LLVM `ret`，然后删除旧的 RAX/RDX register store，避免同一个返回语义同时出现在函数 return 和 register side effect 里。

# native 侧复刻策略

- 扩展现有 CLI smoke：
  - 继续复用 `cli-signature-rewrite.ll`。
  - 在 rewritten 输出上增加负向检查：
    - 不应再出现 `ptr @RAX`。
    - 不应再出现 `ptr @RDX`。
    - 不应再出现 `notdec.register.access`。
- 不改 fixture，不新增测试文件。

暂时不做：

- 不检查 caller 侧旧 return load 删除；CLI fixture 当前没有 direct caller。
- 不拆 CMake 长命令；后续整理可单独做。

# 判断标准

- CLI rewritten 输出没有旧 RAX/RDX register store。
- CLI rewritten 输出没有 `notdec.register.access` metadata。
- 现有签名和 summary 检查继续通过。
- 全量测试继续通过。

# 风险

- 这个检查只覆盖当前 fixture 里的 RAX/RDX 返回寄存器。
- 如果后续 CLI fixture 增加非返回寄存器的普通 register access，这个全局负向检查需要改得更细。

# 实现记录

## 改动

- `tools/CMakeLists.txt:149` 扩展 CLI `.ll` 显式签名重写 smoke。
  - 在 rewritten 输出上检查不再出现 `ptr @RAX`。
  - 在 rewritten 输出上检查不再出现 `ptr @RDX`。
  - 在 rewritten 输出上检查不再出现 `notdec.register.access`。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本小步完成。

## 验证

已通过：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：目标 smoke `1/1 ... Passed`，约 `0.12 sec`。

额外确认生成 IR 中查不到旧返回寄存器 store 和 register access metadata：

```sh
grep -n "ptr @RAX\|ptr @RDX\|notdec.register.access" build/notdec-native-llvm-cli-signature-rewrite.ll || true
```

结果为空。

## 性能影响

只改 CTest 输出检查，不改运行时代码。没有新增 pass 或算法开销。

## 评分

- 实现效果：5/10。固定 CLI 路径上的旧 return store 删除结果，但不是新恢复能力。
- 复杂度：1/10。只是在现有 smoke 中追加三个负向 grep。
- 后期维护成本：3/10。当前全局负向检查依赖 fixture 里没有其它普通 register access。
