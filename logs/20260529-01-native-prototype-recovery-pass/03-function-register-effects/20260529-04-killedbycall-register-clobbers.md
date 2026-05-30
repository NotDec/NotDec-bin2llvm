# 原始 prompt

```text
继续实现 logs/20260529-01-native-prototype-recovery-pass/GOAL.md；基于已有进度，选择下一小步先写计划，再实现、验证、更新 PROGRESS.md，并提交。
```

# 背景

阶段 3 的第一批验证里写着“写后未恢复的 caller-saved 标成 clobbered”。当前 `notdec.register.clobbers` 只覆盖 ABI `unaffected` 寄存器没有恢复的情况；对 ABI `killedbycall` / caller-saved 寄存器，即使函数显式写了返回寄存器，也不会写函数级 clobber metadata。

这会让 direct callee summary 不完整。先补最小语义：函数内有完整宽度 store 到 ABI `killedbycall` register，就标成 clobber。

# Ghidra 实现参考

Ghidra 的 call effect 来自 prototype model 的 effect list，并在 heritage 里用来决定 varnode 是否跨 call 传播：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`：查询具体函数或默认 prototype model 的 effect。
  - `ProtoModel::hasEffect(...)`：返回 `unaffected` / `killedbycall` / unknown 等 effect。
- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::guardCalls(...)`：根据 effect 给 call 周围插入 guard，caller-saved/killed storage 不能直接跨 call 使用旧值。

native 侧已经解析了 ABI `killedbycall`，并用它作为 call barrier。这里把同一事实写入 callee summary：如果函数写了 killedbycall register，它对 caller 来说就是 clobber。

# native 侧复刻策略

- `NativeRegisterSSA` 扫描 store 时记录完整寄存器 store 的 global。
- 写 `notdec.register.clobbers` 时：
  - 保留已有 ABI `unaffected` 未恢复的判断；
  - 额外把 ABI `killedbycall` 且本函数写过的 register 加入 clobbers。
- 增加测试：现有 `call_effects` 写过 `RAX`，ABI 里 `RAX` 是 killedbycall，跑 pass 后应有 `notdec.register.clobbers` / `RAX`。

暂时不做：

- 不推断没有显式 store 的 caller-saved register。
- 不做部分寄存器别名。
- 不区分“返回值写 RAX”和“普通 clobber RAX”，先统一记为 callee clobber summary。

# 判断标准

- 写过 `RAX` 的函数被标为 clobber `RAX`。
- ABI `unaffected` preserved/clobbered 原有测试不回退。
- 全量测试继续通过。

# 风险

- 对返回寄存器，写 clobber 和写返回值在当前 metadata 上没有区分。后续若需要更细的 `returns` / `clobbers` summary，应拆分；目前 call barrier 只需要知道旧值不能跨 call。

# 实现记录

改动：

- `lib/passes/NativeRegisterSSA.cpp:274` 在扫描 store 时记录完整宽度 register store 到 `StoredFullUnits`。
- `lib/passes/NativeRegisterSSA.cpp:600` 修改 `attachRegisterEffectMetadata()`，把 ABI `killedbycall` 且本函数写过的 register 加入 `notdec.register.clobbers`。
- `lib/passes/NativeRegisterSSA.cpp:610` 用 `clobberedSet` 避免同一个 register 因 killedbycall 和 unaffected 未恢复规则重复写入。
- `tests/native_register_effects_test.cpp:319` 验证写过 ABI killedbycall `RAX` 的函数带 `notdec.register.clobbers` / `RAX`。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #5: notdec.native_register_effects.preserved ... Passed`，耗时约 `0.03 sec`；全量测试 `9/9` 通过，总耗时约 `0.91 sec`。

性能：每个函数多记录一个完整 store 的 set，规模上限为寄存器 global 数；不增加 CFG 遍历次数。

评分：

- 实现效果：7/10。补上 caller-saved/killedbycall register 被写后的 clobber summary。
- 复杂度：3/10。复用现有 store 扫描和 metadata 写入。
- 维护成本：3/10。后续如拆 returns/clobbers 语义，需要调整这一处规则。
