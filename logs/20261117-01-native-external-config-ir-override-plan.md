# 外部配置（`--config`）与 IR override 设计

> 用户原始 prompt：
>
> 当前需要思考，怎么设计一个合适的接口，使得用户可以通过外部命令行，override结果IR的生成。比如支持具体函数的IR指令的override？以及各自Pass的参数的精细控制，甚至Pass的顺序。要不通过json文件的方式输入怎么样？允许json中引用外部文件的绝对路径，这样就可以输入额外的文件？

## 计划

- [ ] P0 结论与边界：配置只描述 "module 已存在之后" 的 pipeline 与 override，不覆盖 lift/discovery。
- [x] P1 pipeline 数据驱动：把 `main()` 里硬编码的 stage 序列下沉成 registry + runner，新增 `--config` / `--print-config` / `--list-stages`；默认 spec 必须复现当前输出（逐字节 diff）。**已完成**：fortune x86_64 与 `.ll` 输入产物均与重构前逐字节一致。
- [x] P2 `ir-override` stage：整函数 `replace`，外部 `.ll`/`.bc` 文件，来源元数据。**已完成**：单测覆盖 replace/select/helper 导入/缺失符号/类型不匹配。
- [x] P3 函数级声明：选择器（name/glob/address/symbol）、`attributes`、`pin`，以及 manifest/sha256。**已完成**：`--manifest-out` + `!notdec.config`/`!notdec.override`。
- [x] P4 调试配套：`--dump-stages`、尾部 sanity gate、CLI smoke 与 fortune/Bench2 回归。**已完成**：`--dump-stages` + config override smoke；sanity 复用 `--register-ssa-summary` 的 existing 报告，不额外出声。
- [ ] 指令级 patch（`!notdec.pc` 锚点、retarget-call、block outline）列为 v2，暂不实现。

## 1. 结论

用 JSON，但只作为 **一份声明式 spec**，不引入脚本能力。spec 分三层，各管一件事：

1. **`pipeline`**：有序 stage 列表，stage 名字 + 该 stage 的类型化 options。这是"Pass 顺序 + Pass 参数"的唯一入口。
2. **`functions`**：按选择器匹配的函数级声明（`pin`、`attributes`），不写 IR。
3. **`ir-override` stage**：引用外部 `.ll`/`.bc` 文件，对选中函数做整函数 `replace`。

三条关键判断：

- **IR override 必须是一个 pipeline stage，而不是独立后处理**。替换时机（register 消除前/后、final cleanup 前）直接决定产物是否自洽，时机只能由顺序表达。
- **不做"指令级文本 patch"**。`.ll` 里的 SSA 值没法从片段引用，正则替换又会在重命名/重排后静默失效。函数级需求用整函数 `replace` 覆盖，剩余用"dump 某一 stage 的 IR → 手改 → 从下一 stage 继续"的 worklist 流程，而不是造一个半吊子的 patch 语言。
- **JSON 里可以写绝对路径，也可以写相对路径**；相对路径以配置文件目录（或显式 `root`）为基准，**绝不相对 CWD**。所有引用文件算 sha256 并写进产物元数据/清单，保证"这份 IR 是怎么来的"可回答。

## 2. 现状与约束（代码事实）

### 2.1 当前 pipeline 是硬编码的

`tools/notdec-native-llvm.cpp: main()` 里两条几乎重复的序列（binary 输入 / `.ll` 输入）。当前完整序列：

| # | 调用 | 说明 |
|---|------|------|
| 1 | `runNativeInstCombineStage` (`early_cse=false`) | InstCombine + SimplifyCFG |
| 2 | `runNativeRegisterEliminationStage` | summary 寄存器消除 + 签名重写 + 残留清除 |
| 3 | `runNativeInstCombineStage` (`early_cse=true`) | 收掉重复的 lane 链 |
| 4 | `runNativeFunctionPointerPromotion` | 仅 binary 路径 |
| 5 | `runNativePostRewritePeepholeStage` | |
| 6 | `runNativeFinalCleanupStage(preservedFunctions)` | `llvm.used` + 寄存器收尾 + 条件 GlobalDCE |
| 7 | `runNativeInstCombineStage` (`early_cse=true`) | |
| 8 | `eraseDeadUnknownValueCalls` + `eraseUnusedRegisterHelperDeclarations` | |

能影响这段序列的 CLI 只有 5 个布尔开关（`--no-register-ssa-pass`、`--no-summary-register-residue-removal`、`--no-vararg-recovery`、`--abi-for-all-functions`、`--no-instcombine-pass`）加一个路径参数（`--external-prototypes`）；`NativeRegisterEliminationOptions::IgnoredRegisters` 这类参数根本没有 CLI 入口。pipeline 一旦要换顺序，只能改代码重编译。

### 2.2 影响设计的硬约束

- **函数名在 lift 时定型**：`notdec_native_<hex>`（无符号）或 sanitize 后的符号名，之后不再变。选择器必须按"最终 IR 名"工作，地址只作为 binary 输入的等价入口。
- **IR 里没有逐指令 PC 元数据**（只有 `notdec.memory_map`、`notdec.abi`）。所以"按地址锚定某条指令"目前做不到，要么补元数据，要么不做。
- **stage 之间靠 module 级 metadata 传递事实**（ABI、函数指针槽位），且已有"生成顺序确定化"的要求（`4010690`、`8db025d`）。override 导入时必须排序，不能依赖文件里的出现顺序产生不确定输出。
- **`register-elimination` 是整模块语义重写**（签名、调用点、入口形参），不是逐函数独立 pass。因此**不支持**"跳过某函数的 register 消除"；要保护函数，就把 override 放在它之后，并让后续 LLVM pass 跳过（`pin`）。
- **已有 JSON 先例**：`loadNativeExternalPrototypesJson` 用 `llvm::json` + 手写校验。新配置沿用同一套风格，不引入第三方库。

## 3. 设计

### 3.1 数据流

```
argv ─┬─ 输入选择（-f/-n/--all-confirmed/...）  ── 仍是 CLI
      └─ --config spec.json ──► NativeConfig ──► 校验 ──► StageRunner ──► Module
                                   │                        │
                                   ├─ functions: 选择器/attrs │
                                   └─ pipeline: 有序 stage ──┘
```

配置只覆盖 "module 已存在之后"。lift/discovery 参数（`--gtirb`、`--memory-model`、`--all-confirmed` 等）保持 CLI：它们属于输入选择，不属于 IR 生成管线；放进 JSON 会和"同一份 spec 跑不同输入"的用法冲突。

### 3.2 schema v1（完整示例）

```json
{
  "schema_version": 1,
  "root": ".",

  "pipeline": [
    {"stage": "instcombine", "options": {"early_cse": false}},
    {"stage": "register-elimination", "options": {
      "rewrite": true,
      "residue_removal": true,
      "vararg_recovery": true,
      "abi_for_all_functions": false,
      "external_prototypes": "protos/fortune-x86_64.json",
      "ignored_registers": []
    }},
    {"stage": "instcombine", "options": {"early_cse": true}},
    {"stage": "function-pointer-promotion"},
    {"stage": "post-rewrite-peephole"},
    {"stage": "ir-override", "options": {
      "file": "overrides/getopt.ll",
      "mode": "replace",
      "select": [{"name": "getopt"}]
    }},
    {"stage": "final-cleanup", "options": {"global_dce": "auto"}},
    {"stage": "instcombine", "options": {
      "early_cse": true,
      "exclude": [{"name": "getopt"}]
    }},
    {"stage": "erase-dead-helpers"}
  ],

  "functions": [
    {"select": {"glob": "notdec_native_40*"}, "attributes": {"noreturn": true}},
    {"select": {"name": "getopt"}, "pin": true}
  ],

  "diagnostics": {"print_summary": false}
}
```

字段规则：

- `schema_version` 必填，目前只接受 `1`。
- `root` 可选，默认 `"."`；相对路径解析基准见 3.5。
- `pipeline` 必填。**没有默认合并**：写了就是完整序列；用 `--print-config` 把当前默认序列打印出来作为起点。允许空数组 = 只 lift、不跑任何 pass（scale audit 用这个形态取原始产物）。
- 未知字段一律报错（防拼写错误静默生效）；错误信息带 JSON 指针，如 `pipeline[1].options.residue_removal: expected bool, got string`。

### 3.3 stage 表

| stage | options | 可重复 | 支持 `exclude` |
|-------|---------|--------|----------------|
| `instcombine` | `early_cse`(false), `exclude`([]) | 是 | 是 |
| `register-elimination` | `rewrite`(true), `residue_removal`(true), `post_rewrite_instcombine`(true), `vararg_recovery`(true), `abi_for_all_functions`(false), `external_prototypes`(null), `ignored_registers`([]), `attach_metadata`(true) | 否 | 否 |
| `function-pointer-promotion` | — | 否 | 否 |
| `post-rewrite-peephole` | — | 否 | 否 |
| `ir-override` | 见 3.4 | 是 | — |
| `final-cleanup` | `global_dce`("auto"/true/false), `preserve_address_taken`(true) | 否 | 否 |
| `erase-dead-helpers` | — | 是 | 否 |

`--list-stages` 直接从这个注册表打印 JSON（名字、options、默认值、可重复性），配置语法自解释，不再维护第二份文档。

默认 pipeline（`--print-config` 输出里的 `pipeline` 字段，binary 输入）：

```json
[
  {"stage": "instcombine", "options": {"early_cse": false}},
  {"stage": "register-elimination"},
  {"stage": "instcombine", "options": {"early_cse": true}},
  {"stage": "function-pointer-promotion"},
  {"stage": "post-rewrite-peephole"},
  {"stage": "final-cleanup"},
  {"stage": "instcombine", "options": {"early_cse": true}},
  {"stage": "erase-dead-helpers"}
]
```

`.ll` 输入模式没有 `function-pointer-promotion`（现状如此），其余相同。

### 3.4 `ir-override` 语义

options：

```json
{
  "file": "overrides/getopt.ll",
  "select": [{"name": "getopt"}],
  "entry": "getopt_impl",
  "allow_undefined": false,
  "strip_debug": true,
  "attributes_from": "target"
}
```

- `file`：`.ll` 或 `.bc`，按扩展名选 parser。
- `select`：目标选择器数组，见 3.6。省略时要求文件里的入口函数名与目标同名。
- `entry`：文件里有多个定义时指定入口；否则要求恰好一个定义。
- `allow_undefined`：默认 true，文件里引用的新符号（helper、libc 声明）按 override module 里的类型导入；置 false 时要求所有符号在目标 module 里已存在，否则报错并列出名字。
- `strip_debug`：默认剥掉克隆指令上的 `!dbg`，避免把 override 文件的编译单元混进产物。
- `attributes_from`：`target`（默认，沿用目标函数的 linkage/visibility/attrs）或 `override`。

#### 替换规则

1. 解析文件为独立 `Module`，取入口函数 `Fsrc`。
2. 检查 `Fsrc->getFunctionType() == Fdst->getFunctionType()`，不等直接报错（打印两边类型）。v1 不支持改签名：签名重写是 register 消除的职责，两处同时改必然打架。
3. 先按名字把文件里**其余的函数/全局**导入目标 module：先建声明/全局变量，再克隆定义，名字冲突报错。顺序按名字排序，保证确定性。
4. `Fdst->deleteBody()` 后 `CloneFunctionInto(Fdst, Fsrc, VMap, CloneFunctionChangeType::DifferentModule, Returns)`；VMap 里预填"文件里同名符号 → 目标 module 符号"，这样文件里的自递归调用落在目标函数上（`CloneFunctionInto` 内部会把 `Fsrc` 映射到 `Fdst`）。
5. 调用点不需要动：类型没变，`Fdst` 名字没变。
6. 附加 `!notdec.override` 元数据（文件路径、sha256）。

#### 为什么不做指令级 patch

- 文本 patch 无法绑定 SSA 操作数：片段里写 `%x` 指不到被替换指令的操作数，只能靠位置猜，重排/重命名后静默失效。
- 真要做，前提是给每条 lift 出来的指令加 `!notdec.pc`（`notdec.lift.pc`），然后只支持操作数安全的动作：`retarget-call`（把某个 pc 上的 `call @a` 改成 `call @b`，签名必须兼容）、块级 outline-replace。这是 v2 的事，等真有需求再补元数据。
- 在那之前，"dump stage IR → 手改 → 从后续 stage 继续跑"就是逃生通道：`--dump-stages out/` 会在每个 stage 前后写 `<NN>-<stage>-after.ll`，改完用 `--config` 里只保留后续 stage 再跑。`!notdec.pipeline` 元数据记录已完成的 stage，重复执行时给出警告。

#### 与后续 pass 的交互

- `pin: true`（`functions` 里）等价于：加入 `preservedFunctions`（不被 GlobalDCE 删），并让实现了 `exclude` 的 stage（v1 只有 `instcombine`）跳过该函数。其他 stage 靠 pipeline 顺序保护：把 `ir-override` 放在它们之后。
- `register-elimination` 没有 `exclude`：**要保护手写 IR，就把 `ir-override` 放在它之后**。文档里写死这条，并在 `ir-override` 位于 `register-elimination` 之前且目标函数 `pin` 时给 warning（手写 IR 里的寄存器访问会被 summary 当成输入重写）。
- `final-cleanup` 的 `preserve_address_taken` 保留现有 `llvm.used` 行为；override 引入的 helper 若只被 pinned 函数调用，仍会被保留（可达）。

### 3.5 路径、来源与确定性

解析规则（按优先级）：

1. 绝对路径直接用。
2. 相对路径相对 `root`（若给了），`root` 本身相对配置文件所在目录解析。
3. 没有 `root` 时相对配置文件所在目录。

**不相对 CWD**：同一份 spec 在不同目录跑必须得到同一结果。

来源记录：

- 产物 module 加 `!notdec.config`：`schema_version`、规范化配置的 sha256、每个外部文件的 `{role, path, sha256}`。
- 每个被 override 的函数加 `!notdec.override`：`{mode, file, sha256}`。
- 可选 `--manifest-out <path>` 输出同内容的 JSON，供批量脚本/回滚比对。
- `--print-config` 打印**合并后**的 spec（含默认值），可直接当新配置用：`run --print-config > exp.json`，改完 `run --config exp.json`。输出里的 `root` 是绝对路径，所以存到别的目录也能复现；手写配置不写 `root` 时默认取配置文件所在目录。

确定性：导入符号按名字排序；override 按 pipeline 顺序执行；不遍历目录、不依赖文件系统顺序。

### 3.6 选择器

```json
{"name": "getopt"}          // 最终 LLVM 函数名（精确）
{"glob": "memcached_*"}     // fnmatch 风格
{"symbol": "memcpy"}        // 二进制里的原始符号名
{"address": "0x401136"}     // 函数入口地址，仅 binary 输入可用
```

- 多个字段同时出现 = AND。
- 匹配不到任何函数：默认报错（`select matched no function`），`"optional": true` 可降级为 warning。批量配置里写错名字不该静默通过。
- `.ll` 输入模式下 `address` 直接报错，不做猜测。

### 3.7 错误处理与验证

- 解析/校验错误：带 JSON 指针 + 期望类型；不产生半成品输出。
- 每个 stage 结束 `verifyModule`（与现状一致）；失败时把该 stage 输入/输出写到 `<out>.<NN>-<stage>.bad.ll` 并中止，便于二分定位是哪个 pass 被参数搞坏。
- `ir-override` 自己额外检查：类型匹配、缺失符号、重名、克隆后 verify。
- 尾部 sanity gate（不阻断，只报告）：若 `register-elimination` 没跑过而产物还有 `notdec.reg.*` 全局/`notdec.unknown` 调用，打印计数；复用现有 `auditNativeRegisterValueSources`。
- 顺序软约束：`final-cleanup` 早于 `register-elimination` → warning（可能是从中间 IR 续跑，不一定是错）；`register-elimination` 出现两次 → 直接报错（非可重复 stage）。

### 3.8 CLI 边界与旧 flag 处理

规则：**`--config` 独占"会改变 module 的配置"；CLI 只管输入、输出、诊断。**

- 输入选择：`-f` / `-n` / `-a -l` / `--all-confirmed`、decode 选项、spec 路径、`--memory-model`、`--skip-runtime`
- 输出：`-o`、`--summary-json-out`、`--register-ssa-warning-out`、`--manifest-out`、`--dump-stages`
- 诊断：`--register-ssa-summary`

新增（`--print-config` / `--list-stages` 可脱离输入单独运行，其余 flag 不要求）：

```
--config <file>
--print-config [<file>]     # 缺省 stdout
--list-stages
--dump-stages <dir>
--manifest-out <file>
```

**删除全部旧 pipeline flag**，不做 sugar、不做"互斥判断"：

| 旧 flag | 迁移 |
|---------|------|
| `--no-register-ssa-pass` | 从 `pipeline` 去掉 `register-elimination`（scale audit 直接用 `"pipeline": []`） |
| `--no-summary-register-residue-removal` | `register-elimination.options.residue_removal=false` |
| `--no-vararg-recovery` | `register-elimination.options.vararg_recovery=false` |
| `--abi-for-all-functions` | `register-elimination.options.abi_for_all_functions=true` |
| `--no-instcombine-pass` | 去掉所有 `instcombine` stage，同时 `register-elimination.options.post_rewrite_instcombine=false` |
| `--external-prototypes <p>` | `register-elimination.options.external_prototypes=<p>` |
| `--summary-register-ssa-pass` | 删除（本来就是 no-op 兼容别名） |

理由：

1. **单一来源**。旧 flag 本来就覆盖不全（`ignored_registers` 没有入口），留着必然和 spec 漂移，还得为每个新参数再补一个 flag。
2. **歧义**。默认序列里 `instcombine` 出现三次，"这个 flag 改哪一次"说不清；规定"作用于全部"又和"配置里特意只保留一次"的意图冲突。
3. **迁移面小**。只有 4 个脚本用到；迁成提交到仓库的 config 文件之后，脚本反而变成可读的回归输入。
4. **临时实验成本可接受**：`--print-config > /tmp/exp.json`，改一行再 `--config /tmp/exp.json`。

脚本迁移：

| 脚本 | 迁移 |
|------|------|
| `scripts/bench2-native-scale-audit.sh` | `--config scripts/config/raw-lift.json`（`"pipeline": []`） |
| `scripts/bench2-native-summary-ssa-audit.sh` | 两个模式各一份 config（`summary-no-residue` / `summary-residue`），保留 `--register-ssa-summary` |
| `scripts/native-fortune-{x86_64,i386}-regression.sh` | `tests/fixtures/native/fortune-*.pipeline.json`，相对引用现有 external-prototypes |
| `scripts/native-llvm-cli-signature-rewrite-smoke.sh` | `tests/fixtures/native/cli-no-instcombine.json` |
| `AGENTS.md` | 删除 `--summary-register-ssa-pass` 兼容别名那一句 |

## 4. 实现

| 文件 | 内容 |
|------|------|
| `include/notdec-bin2llvm/core/NativePipelineSpec.h` | `NativeConfig`/`NativeStageSpec`/`Selector` 类型，`loadNativeConfig`、`printNativeConfig`、`nativeStageRegistry` |
| `lib/core/NativePipelineSpec.cpp` | JSON 解析、严格校验、默认 spec、`--list-stages` 数据 |
| `include/notdec-bin2llvm/core/NativeIROverride.h` / `lib/core/NativeIROverride.cpp` | `replace`、符号导入、`!notdec.override` |
| `lib/core/NativePipeline.cpp` | `NativeStageContext` + runner；现有 `runNativeXxxStage` 改为从各自 options 结构取值（不再共用 `NativePipelineOptions`） |
| `tools/notdec-native-llvm.cpp` | 两条 main 路径合并成一次 runner 调用；新增 flag；删除旧 pipeline flag |
| `tests/native_pipeline_spec_test.cpp` | 解析/校验/默认值/错误路径 |
| `tests/native_ir_override_test.cpp` | `replace` 语义 |
| `tests/native_config_cli_smoke.sh` | `.ll` 输入模式的端到端 smoke |

关键类型草图：

```cpp
struct NativeStageContext {
  llvm::Module &Module;
  const NativeProgramState *State = nullptr;   // binary 输入才有
  std::set<std::string> PreservedFunctions;    // pin + 地址被取
  std::set<std::string> PinnedFunctions;
  const NativeConfig *Config = nullptr;
};

struct NativeStageDescriptor {
  llvm::StringRef Name;
  bool Repeatable = false;
  bool Filterable = false;
  std::function<bool(NativeStageContext &, const llvm::json::Object &,
                     std::string &)> Run;
};
```

每个 stage 自带类型化 options 结构和 `parse(json, path, error)`，runner 只负责顺序、过滤、verify、dump。

## 5. 测试与验收

1. **P1 无行为变化**（硬门槛）：默认 spec 重构前后，`scripts/native-fortune-{x86_64,i386}-regression.sh` 与 Bench2 memcached 产物 `diff` 为空；`--print-config` 输出的序列与上表一致。
2. 单测：坏 JSON、未知 stage、未知 key、类型错、非可重复 stage 重复、文件缺失、`select` 不匹配。
3. 单测：`replace` 后 body/调用点/linkage/元数据正确，自递归落回目标，类型不匹配与缺失符号报错。
4. 单测：`pin` + `exclude` 让指定函数躲过 instcombine，且被 GlobalDCE 保留。
5. CLI smoke：`cli-signature-rewrite.ll` + 一份配置（override `main` 的返回值）→ 产物含新 body、`llvm-as`/`opt -passes=verify` 通过、`!notdec.config` 存在。
6. 确定性：同一配置跑两次产物逐字节一致（包括 override 导入顺序）。
7. 性能：配置解析/哈希只在启动；stage 后 verify 与现状持平；`--dump-stages` 只在显式开启时写文件。
8. 迁移：4 个脚本改用提交的 config 后，CTest 全绿；scale audit 的 `rc`/产物大小与迁移前同量级。

## 6. 不做的

- 不覆盖 discovery/lift 参数（v1）；不做 `include`/多层合并（先靠 `--print-config` 物化）。
- 不支持改函数签名的 override（签名归 register 消除；要改就改 `external_prototypes`）。
- 不支持 `register-elimination` 的单函数跳过（整模块重写，跳过会产生调用点不一致）。
- 不支持文本/正则 patch `.ll`。
- 不把 JSON 做成脚本（无条件、循环、表达式）。

## 7. 已定决策

1. 旧 pipeline flag 全部删除，`--config` 是改变 module 的唯一入口；脚本迁移到提交的 config 文件。
2. 函数级 `attributes`（`noreturn` / `memory` / `nounwind` / `noinline` / `cold`）进 P3：纯属性设置、默认不启用，不改变默认产物。
3. 选择器不加 `"all": true`，用 `{"glob": "*"}`。
4. `--print-config` / `--list-stages` 允许脱离输入单独运行。

## 8. 实现记录（2026-11-17）

### 8.1 落地形态

- 新文件：`core/NativePipelineSpec.{h,cpp}`（schema/解析/打印/选择器/来源）、
  `core/NativeIROverride.{h,cpp}`（replace）；`core/NativePipeline.{h,cpp}` 改成 stage runner。
- CLI：新增 `--config` / `--print-config` / `--list-stages` / `--dump-stages` / `--manifest-out`；
  删除全部旧 pipeline flags，包括 `--summary-register-ssa-pass`。
- 脚本迁移：fortune x86_64/i386 改用 `tests/fixtures/native/fortune-*.pipeline.json`；
  cli smoke 用 `tests/ir/native-prototype/cli-no-instcombine.json`；
  scale audit 用 `scripts/config/raw-lift.json`（空 pipeline）；summary-ssa audit 用
  `scripts/config/summary-no-residue.json`。

### 8.2 实现中调整的决定

1. `allow_undefined` 默认改为 **true**。override 文件里自带 helper 是常态；false 只作为严格模式。
2. `exclude` 在 v1 只有 `instcombine` 实现（它本来就逐函数跑）。`pin` = 保活 + 让实现了
   `exclude` 的 stage 跳过；其他 stage 靠 pipeline 顺序保护。
3. 入口函数自动识别：文件里只有一个定义就用它；多个定义时优先选"名字在目标 module 里也存在"
   的那个，仍有歧义才要求 `entry`。
4. `!notdec.config` 是**命名元数据**。LLVM 打印时先给命名元数据编号，所以带 `--config` 的产物
   里后续 metadata 编号会整体后移（语义不变）。默认配置（不带 `--config`）不挂该元数据，
   因此默认产物仍逐字节不变。
5. `CloneFunctionInto(DifferentModule)` 会补一个空的 `!llvm.dbg.cu`，导致 `llvm-as` 报
   "invalid version (0)"；clone 完如果为空就删掉。

### 8.3 验证

- `ctest`：19/19 通过（新增 `notdec.native_pipeline_config`、`notdec.native_ir_override`、
  `notdec.native_llvm.config_override_smoke`）。
- 默认 pipeline 产物：fortune x86_64、`.ll` 输入与重构前逐字节一致；fortune 0.58s -> 0.56s。
- Bench2 memcached（limit 20）**不能**作为逐字节基准：重构前的 binary 连续跑两次同样有
  3862 行差异（`%RCX.clobber*` 数量与后续编号漂移），是本仓库既有的不确定性，与本次改动无关。
  memcached 的运行时间 11.33s -> 11.51s，属于同一量级。
- 配置路径：`--print-config` / `--list-stages` / `--dump-stages` / `--manifest-out` 手动跑通；
  override 产物可被 LLVM 22 `llvm-as` + `opt -passes=verify` 验证。

## 9. 在 hexx64.so 上实测 override / 原型接口（2026-11-18）

### 9.1 IR override 的两个 bug（都已修）

在 hexx64 上跑 `ir-override` stage 时发现：

1. **带形参的函数克隆直接段错误**。`CloneFunctionInto` 不会自己把源函数的形参映射到
   目标形参，`VMap` 缺这些映射时会在 `CloneFunctionAttributesInto` 里解引用空句柄。
   单测里的函数全是无参的，所以一直没暴露。
   修复：clone 前显式 `Map[Entry->getArg(i)] = Target->getArg(i)`。
   回归测试：`testReplaceWithArguments`（带 1/2 个形参的 replace；已验证去掉修复时
   测试二进制直接 SIGSEGV）。
2. **override 文件里的外部全局变量写法**：`@V = external global i64` 必须按全局变量
   声明；写成 `declare i64 @V` 会在解析期报错（这条是使用姿势，不是 bug）。

修复后 hexx64 全量跑通（`native46.ll`）：`FUN_2e83c0` 换成手写 body，
`!notdec.override` 元数据正确，`llvm-as`/`opt verify` 通过。

实测用例：`FUN_2e83c0` 里 `callui@GOTPCREL` 的间接调用在汇编里准备了 4 个实参
（edi=24, esi=4, rdx=入口 RDI, rcx=rsp=va_list），但寄存器 SSA 的"局部定义前缀"
规则只认到 RDI/RSI，提升后的调用只剩 2 个实参。手写 override 补回 4 个：
`call void %fn(i64 24, i64 4, i64 %RDI.arg, ptr %va)`。

### 9.2 external_prototypes JSON 实验（qfree 声明为 void）

配置：`{"qfree": {"fixed_args": 1, "return": "void"}}`（hexx64 的构建产物
`native45.ll`）。结果：

| 指标 | 默认（native44） | qfree=void（native45） |
|------|------------------|------------------------|
| qfree 调用点改成 `void` | 0 / 8797 | 8797 / 8797 |
| `range_return_rewrite_missing_value` | 157 | 115 |
| `return_binding_uses_clobber_value` | 314 | 547 |
| unknown 总数 | 728 | 895 |
| 产物大小 | 88.1 MB | 87.9 MB |

解释：qfree 在 IDA SDK 里是 `void qfree(void*)`，之前的 `{double,i64}` 返回形状是
"调用点读后置寄存器"反推出来的**假返回值**。声明成 void 后假返回值消失，
`range_return` 那类 unknown 少了 42 个，但原来被假返回值"遮住"的 clobber 返回槽
暴露出来，`return_binding_uses_clobber_value` 增加 233。也就是说：**用户提供的
真语义可能让 unknown 变多**——它把不忠实但看似有值的模型换成了忠实的模型。

### 9.3 最后两类 unknown 的构成与可补齐性

**A. `return_binding_uses_clobber_value`（314 处 / 192 个函数）**

- 寄存器分布：R9 96、RAX 82、R8 37、R10 33、R11 30、RSI 13、RCX 11、RDX 9。
- 机制：函数签名被内部约定扩展成"任意 value-carrying GPR 都能当返回槽"，返回槽的
  值在图上是**纯 clobber**：最后一次内部调用（qfree 等）之后的寄存器值。
  典型：`FUN_29a670` = `jmp qfree@PLT`，它的 R8/R9/R10/R11/RCX/RDX/RSI/RDI 返回槽
  就是 qfree 的 scratch。
- 忠实性已核对：`FUN_110760` 的 R9/R10 返回槽确实被 `FUN_d6110`、`FUN_2921b0` 等
  调用点 `extractvalue` 使用，`FUN_d6110` 还把它继续当自己的返回值传出；调用点
  汇编在这条路径上确实没有先写 R9。
- 二进制里没有可补齐的"正确值"：caller-saved 寄存器在调用后的内容 ABI 未定义，
  真值取决于 callee 实现（qfree 的 scratch 具体是多少并不在二进制里表达）。
  原型 JSON 也表达不了：`max_return_registers` 只覆盖 ABI 输出寄存器（SysV 的
  XMM0/XMM1/RAX），没有"某个非 ABI 寄存器在调用后是什么"的字段。

**B. `range_return_rewrite_missing_value`（157 个元数据点 / 212 处使用 / 112 函数）**

- 寄存器：ZMM0 211、RAX 1。callee 前几名：qfree 66、getinf 15、get_tinfo_property 11、
  getinf_flag 9、clear_tinfo_t 9、qvector_reserve 7、_ZdlPv 6、log2ceil 6、
  lexcompare_tinfo 6、cleanup_argloc 6。
- 机制：函数返回槽是 128 位 ZMM0（调用者真的读了 16 字节），但值来源的调用只返回
  64 位（double）。高 64 位落成 `notdec.unknown`。
- 同样不可补齐：double 返回后 XMM0 高 64 位 ABI 未定义；原型 schema 的返回类型只有
  i32/i64/ptr/float/double/longdouble（longdouble 走 ST0），没有 128 位/向量返回，
  也没有"高位固定为 0"这种表达。

### 9.4 结论：这两类要不要靠用户信息补？

- **原型 JSON 补不了**这两类（缺的是"调用后某个寄存器的具体值"，schema 没有这个维度）。
- **整函数 IR override 可以表达**（用户手写 body，想要什么语义都行），代价是要写整个
  函数体；对 192/112 个函数不现实，只适合少量关键函数（例如本例的 `FUN_2e83c0`）。
- 这两类的根因不是"缺信息"，而是"二进制本身读的就是未定义值"：提升器如实建模成
  unknown/carrier 是正确的。要让它们"变好看"，只能改表示层（把 unknown 换成
  clobber 载体——但现有单测与 fortune 门槛明确禁止），或接受。
- 如果以后要扩展 JSON 接口，最小有价值的增量是**按调用点/按 callee 的 effect 覆盖**
  （例如 `{"callee": "X", "preserves": ["R9"], "returns": {"RAX": "opaque"}}`），
  以及**返回槽显式取值**（`{"function": "F", "return_slot": {"R9": "zero"}}`）。
  这两种都需要新的 schema 字段和对应 pass 支持，属于后续工作，不在本轮范围。

## 10. 128 位返回与"调用约定范围"（class A/B 的补法，2026-11-18）

用户的问题：最后两类 unknown 能不能靠 override 补齐？"当前真的没有定义 128 位的返回吗？"
本节是实测回答。

### 10.1 之前确实没有 128 位返回

`NativeExternalPrototype::ValueType` 原来只有 `i32/i64/ptr/float/double/long_double`；
`llvmTypeForKnownValue` 没有 128 位类型，`long_double` 走 ST0（x87），不是 XMM。
所以"某个外部 callee 真的返回完整 128 位（XMM0）"没法声明。

本轮新增：

- `ValueType::Vector128`（JSON 里 `xmm128`/`i128`/`f128`/`vector128`/`__m128`），
  LLVM 类型用 `i128`，按 float 槽走 XMM0；
- 修了一个顺带发现的 bug：只写类型化 `return`、没写 `params` 时，`fixed_args`
  被忽略，调用点会被改写成零实参（悄悄丢参数）。现在会按 `fixed_args` 补无类型
  整数形参；
- 修了 `castSlotValueToRegister`：浮点/向量槽原来只接受浮点值，`i128` 的向量槽
  会被判成"缺失"→ `range_return_rewrite_missing_value`。现在整数位模式也能铺进槽位。

### 10.2 实测：qfree 声明为 xmm128

`{"qfree": {"fixed_args": 1, "return": "xmm128"}}`（构建产物 `native50.ll`）：

| 指标 | 默认（native44） | qfree=xmm128（native50） |
|------|------------------|--------------------------|
| qfree 调用点 | `call {double,i64} (...)` | `call i128 @qfree(i64)` |
| `range_return_rewrite_missing_value` | 157 | **115**（qfree 的 42 处解决） |
| `return_binding_uses_clobber_value` | 314 | 547（+233） |
| unknown 总数 | 728 | 895 |

结论：128 位返回**可以声明并且现在真的能绑上**（FUN_29a670 的 ZMM0 用上了调用的
i128 返回值），但是**单值类型化返回会把原来 `{double,i64}` 里的 RAX 返回槽挤掉**，
RAX 变成 clobber → clobber-return unknown 增加。也就是说：光有"128 位返回类型"
不够，真实场景是**一个 callee 同时在多个寄存器里返回值**。

### 10.3 实测：调用约定范围

已有的全局开关 `abi_for_all_functions: true`（所有函数只按外部 ABI 传参/返回）：

| 指标 | 默认（native44） | abi_for_all（native47） |
|------|------------------|-------------------------|
| poison | 6 | **2** |
| unknown 总数 | 728 | **712** |
| `return_binding_uses_clobber_value` | 314 | **91** |
| `range_return_rewrite_missing_value` | 157 | 157 |
| `call_effect_mixed` | 2 | 59 |
| 产物大小 | 88.1 MB | **83.2 MB** |

这正是用户说的"设置一个默认的调用约定的应用范围"：函数不再假设 R8/R9/R10/R11 等
都能当返回通道，class A 从 314 降到 91。代价是调用点读这些寄存器时改成
`call_effect_mixed`（59 处），总量仍是下降的。

本轮还把范围做到**按函数**：配置 `functions[].abi`（`"external"`/`"internal"`），
经 `NativePipelineContext` 传进 register-elimination，`abiConventionFor` 优先看
强制集合。验证：小模块里 `@f` 挂了 `!notdec.abi = !{!"conventions=external"}`、
`@g` 挂 `conventions=internal`。hexx64 上的 scoped 实验还没跑（全量一轮 11 分钟）。

### 10.4 下一步（真正把 class A/B 钉死的 schema）

两类 unknown 缺的都是"这个 callee 到底在哪些寄存器、多少位里返回值"，所以需要
**多寄存器返回声明**，而不是单值类型：

```json
{
  "qfree": {
    "params": ["i64"],
    "returns": [
      {"register": "RAX",  "width": 64},
      {"register": "XMM0", "width": 128}
    ]
  }
}
```

以及按函数钉死输入/输出（用户提的"直接设置每个函数的输入和输出寄存器"）：

```json
{
  "functions": [
    {"select": {"name": "FUN_x"},
     "signature": {
       "params":  [{"register": "RDI", "width": 64}],
       "returns": [{"register": "RAX", "width": 64},
                   {"register": "XMM0", "width": 128}]
     }}
  ]
}
```

实现落点：`shapeForKnownExternal`（按 returns 列表直接建 `shape.Returns`，宽度来自
`iN`）＋ `buildInitialSignatureShapes` 里对 pin 的函数覆盖 shape。`extractReturnRange`
和 `castSlotValueToRegister` 已经支持"整数位模式 + 任意宽度槽位"，这轮的两个 bug
修复就是为它铺的路。本次没有实现多寄存器列表，留作下一步。
