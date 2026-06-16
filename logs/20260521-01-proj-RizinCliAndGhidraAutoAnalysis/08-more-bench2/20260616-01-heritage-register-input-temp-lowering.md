# Java heritage lowering 将 register input 当临时值

用户原始需求摘要：

> 是的，尽量把仅仅是来源标记的寄存器值最好都弄成临时的value，可以把来源作为metadata标记，但是不要直接表示为寄存器。当前的修改都仅涉及Java链路，不要对native链路产生影响，即native链路禁止启用--register-inputs-as-temps

## 背景

Ghidra High P-Code 里的 register varnode 很多只是 SSA 值来源，不一定应该在 Java 链路 lowering 后变成 `@RSP`、`@FS_OFFSET` 这类 LLVM global。之前 Java heritage lowering 对没有本地 def 的 input register varnode 会 fallback 到 `RegisterStorage`，于是生成 register global load。

这次只给 heritage JSON lowering 工具加 opt-in flag。native 链路不加这个 flag，也不修改 native CLI。

## 实现

- `include/notdec-bin2llvm/HeritageToLLVM.h:16`
  - `HeritageLoweringConfig` 增加 `RegisterInputsAsTemps`，默认 `false`。
- `lib/HeritageToLLVM.cpp:337`
  - `HeritageLowerer` 接收 lowering config。
- `lib/HeritageToLLVM.cpp:607`
  - flag 打开后禁用 register input 到 `RegisterStorage` 的 fallback。
- `lib/HeritageToLLVM.cpp:617`
  - 新增 `registerInputTemp(...)`，把 register input 降成函数入口处的 `freeze poison` 临时值。
  - 临时值挂 `notdec.register.source` metadata，保留 `space/offset/size/register` 来源。
- `lib/HeritageToLLVM.cpp:668`
  - 普通 `read(...)` 遇到 register input 时走临时值。
- `lib/HeritageToLLVM.cpp:1635`
  - PHI incoming 补边也走同一个临时值，避免 PHI 路径重新读 RegisterStorage。
- `tools/notdec-heritage-llvm.cpp:16`
  - 单函数 heritage lowering 支持 `--register-inputs-as-temps`。
- `tools/notdec-heritage-module-llvm.cpp:16`
  - 模块 heritage lowering 支持 `--register-inputs-as-temps`，可和 `--declarations-only` 共存。
- `ghidra_scripts/README.md:63`
  - 记录 Java 链路使用方式，并说明 native 链路不使用。

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-heritage-llvm notdec-heritage-module-llvm -j2
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
```

`libuv` module JSON：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-java-skip-runtime/libuv/shared-library/module-all-skip-runtime.json \
  -o /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-java-skip-runtime/libuv/shared-library/module-all-skip-runtime.regtemps.ll \
  --register-inputs-as-temps
```

结果：

- register globals：`0`
- register global loads：`0`
- `notdec.register.source` metadata：`254`
- LLVM 22 `llvm-as` + `opt -passes=verify` 通过。

`vsftpd` module JSON：

- register globals：`0`
- register global loads：`0`
- `notdec.register.source` metadata：`144`
- 默认不加 flag 的对照输出仍有 register globals：`9`
- LLVM 22 `llvm-as` + `opt -passes=verify` 通过。

单函数 heritage JSON：

- `/sn640/NotDec-Exp/Bench2/bin2llvm-ir/vsftpd/main.json`
- register globals：`0`
- `notdec.register.source` metadata：`4`
- LLVM 22 `llvm-as` + `opt -passes=verify` 通过。

native CLI 禁止启用：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-java-skip-runtime/libuv/shared-library/module-all-skip-runtime.regtemps.ll \
  -o /tmp/native-should-not-enable.ll --register-inputs-as-temps
```

结果：`rc=1`，报 `flag has no value: --register-inputs-as-temps`，说明 native CLI 没有接受这个选项。

`git diff --check` 通过。

## 风险

这个 flag 会把无 def 的 register input 视为本函数未知临时值。它适合 Java 链路避免把 provenance 误建成全局寄存器，但也意味着 `FS_OFFSET`、`RSP` 这类特殊来源不会在 lowering 时保留为可跨函数追踪的 register storage。后续如果要恢复 TLS 或更细的栈语义，应基于 `notdec.register.source` metadata 单独处理。

效果：8/10。当前两个 Bench2 代表目标能清掉 register global。

复杂度：4/10。只加一个 opt-in flag 和一个本地 helper。

维护成本：4/10。默认行为不变，native 链路没有入口。
