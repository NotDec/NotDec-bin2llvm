# Native RAM Varnode Read

## 原始 prompt

```text
在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## 当前目标和已有 native 状态

Bench2 smoke 已经没有 `CALL` / `CALLIND` / `CALLOTHER` helper，也没有 unresolved indirect flow。
但 all-confirmed IR 里仍有很多 direct `ram` varnode 被读成 `freeze poison`，例如：

```text
%ram_25ff0_8_in = freeze i64 poison
%ram_33fd0_8_in = freeze i64 poison
%ram_3eff0_8_in = freeze i64 poison
```

这些通常来自 Sleigh P-Code 里的 direct `ram` varnode。当前 `PcodeLowerer::read(...)` 只处理
`const`、临时 SSA 值和寄存器，漏掉了 `ram` 输入。

## Ghidra 相关实现

Ghidra 的 P-Code 语义里，varnode 的 address space 是值所在的位置；`ram` varnode 表示内存空间里的值：

- `Ghidra/Features/Decompiler/src/main/doc/sleigh.xml`
  - `Varnodes` 章节说明 P-Code 值由 address space、offset、size 标识。
  - `LOAD` / `STORE` 章节说明显式内存访问；但 direct `ram` varnode 本身也代表内存空间中的 storage。
- `Ghidra/Features/Base/src/main/java/ghidra/program/util/SymbolicPropogator.java::applyPcode(...)`
  - 对 `COPY`、`LOAD`、`STORE` 等 op 都通过 `VarnodeContext` 读写 varnode 值，不把内存 varnode 当未知 poison。

native 侧不做完整内存映射和初始化；只复用现有 `notdec_ram` 外部 byte array，让 direct `ram` 输入生成 load。

## native 侧复刻策略

1. 在 `PcodeLowerer::read(...)` 里，在寄存器 fallback 之前处理 `varnode.Space == "ram"`。
2. 用 varnode offset 生成 i64 常量地址，通过现有 `memoryPointer(...)` 得到 `notdec_ram` 指针。
3. 按 varnode size 生成 `load`，alignment 保持 1。
4. 其他未知 varnode 仍走 `freeze poison`，不扩大行为。
5. Bench2 smoke 增加一条回归检查：当前三个 all-confirmed IR 不应再出现 `ram_` 前缀的 `freeze poison`。

暂时不做：

- 不把 ELF 初始内存内容写进 LLVM global initializer。
- 不处理地址超过当前 `notdec_ram` 外部数组大小的问题。
- 不改 `LOAD` / `STORE` 的现有外部内存模型。
- 不做 ABI / prototype 恢复。

## 判断标准

1. Bench2 三目标 all-confirmed IR 中 direct `ram_*_in = freeze ... poison` 消失。
2. 现有 helper / unresolved 检查继续通过。
3. LLVM 22 `llvm-as` 和 `opt -passes=verify` 继续通过。
4. 性能不明显变慢。

## 风险

1. `notdec_ram` 仍是外部数组，不代表已经装载真实初始内存；这次只避免无意义 poison。
2. 当前数组大小仍是 1MiB，后续如果覆盖更大地址空间，需要单独改内存模型。
3. 有些 direct `ram` varnode 在 Ghidra 高层语义里可能被更精细地归类；本次只按低层 P-Code storage 处理。

## 实现记录

### 改动

- `lib/PcodeToLLVM.cpp:286` 的 `PcodeLowerer::read(...)`
  - 在 `Values` 命中之后、寄存器 fallback 之前处理 `varnode.Space == "ram"`。
  - 用 `varnode.Offset` 生成 i64 地址，复用 `memoryPointer(...)`，按 varnode size 生成
    `load`，alignment 为 1。
  - 其他未知 varnode 仍走原来的 `freeze poison` fallback。
- `scripts/bench2-native-smoke.sh:114` 增加 `forbid_ir_regex(...)`。
- `scripts/bench2-native-smoke.sh:172` 在 `check_ir_features(...)` 中禁止
  `ram_[0-9a-f]+_[0-9]+_in = freeze`。
- `ARCHITECTURE.md:144`、`ARCHITECTURE.md:332`、`ARCHITECTURE.md:342`、
  `ARCHITECTURE.md:498` 记录 direct `ram` varnode 现在从 `@notdec_ram` load，并说明
  smoke 会挡住回退。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:58` 和
  `PROGRESS.md:72` 更新阶段 6 / 阶段 7 进度。

### 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

结果：通过。

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-49
```

结果：

```text
vsftpd ok elapsed=7s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

summary unresolved indirect flow：

```text
vsftpd.summary.json unresolved total 0
libuv.summary.json unresolved total 0
memcached.summary.json unresolved total 0
```

direct `ram` 输入已经变成 load，例如：

```text
vsftpd.all-confirmed.ll: %ram_25ff0_8_in = load i64, ptr getelementptr ([1048576 x i8], ptr @notdec_ram, i64 0, i64 155632), align 1
vsftpd.all-confirmed.ll: %ram_25fe8_8_in = load i64, ptr getelementptr ([1048576 x i8], ptr @notdec_ram, i64 0, i64 155624), align 1
libuv.all-confirmed.ll: %ram_33fd0_8_in = load i64, ptr getelementptr ([1048576 x i8], ptr @notdec_ram, i64 0, i64 212944), align 1
memcached.all-confirmed.ll: %ram_3eff0_8_in = load i64, ptr getelementptr ([1048576 x i8], ptr @notdec_ram, i64 0, i64 258032), align 1
```

性能：本次 smoke 合计约 33s，三个目标分别约 7s / 19s / 7s。新增逻辑只是 lowering
阶段多生成 load，没有看到 smoke 时间明显变慢。

### 评分

- 实现效果：8/10。去掉了当前 Bench2 明确出现的 direct `ram` poison read。
- 复杂度：2/10。只复用已有 `notdec_ram` 和 `memoryPointer(...)`，没有引入新模型。
- 维护成本：2/10。回归检查直接挡住同类退化；后续真正初始化 ELF 内存时仍可复用这条路径。

### 未做

- 没有初始化 `@notdec_ram` 的 ELF 内容。
- 没有扩大当前 1MiB 内存模型。
- 没有改变 `LOAD` / `STORE` 的现有 lowering。
