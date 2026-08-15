# 20260815-02-native-external-arity-entry-evidence.md

## 用户原始 prompt

> 详细分析一下第一点吧
> 修复的方法是什么意思，就是允许ForwardEntry吗？当前是怎么区分ForwardEntry和LocalDefine的？
> 实现一下试试吧，然后，过估的先不管，看下一个更严重的问题

## 背景

wrk 上 `inconsistent_unknown_external_arity` 10 条，都是 lua_* 外部函数。低估的一类是：
wrapper 函数把入口参数（通常是 lua_State* L）直接透传给外部调用，调用点前该寄存器从未被
显式写过，evidence 是 `ForwardedEntry` 而不是 `LocalDefinition`，`localDefinitionPrefix` 从
第 0 槽就断，arity 低估到 0（如 `luaL_checkudata` 12/13 个调用点 arity=0）。

## 实现

### `lib/passes/summary/NativeRegisterSummarySSA.cpp`

- `localDefinitionPrefix`（:2009）：入口透传参与前缀计数，但只算 **local 之前的开头 entry 段**
  （wrapper 透传 handle 参数），local 之后的 entry 是入口残留直接截断；没有任何 local 时返回 0
  （纯 entry 是"入口寄存器没被用过"，不是参数证据）。规则：
  `arity = 开头连续 entry 数 + 后续连续 local 数`。
- `addExitLiveRegisters`（:4881）：修复 APInt 断言崩溃——`ExitDemandMask & MayNonEntryMask`
  宽度可能不一致（hex 文本前导零丢失），统一按寄存器宽度 `zextOrTrunc` 后再求交集。
  该 bug 由 entry 修复改变部分函数签名后触发，属于 bit-range 改动（20260815-01）的潜伏问题。

## 验证

- wrk 全量：rc=0，`llvm-as` + verify 通过；warning 938 → **862**（净减 76）；inconsistent
  10 → 6（luaL_checkudata/lua_createtable/lua_newuserdata/lua_tolstring/lua_type 全部
  收敛到真实 arity：checkudata 12×0 → 全 3）。
- 剩余 6 条 inconsistent 全是**过估**（残留本地写污染前缀，如 lua_getfield 3×4、lua_pushinteger
  1×4），用户确认过估暂不处理。
- fortune i386/x86_64 回归脚本通过；native 测试全绿（ctest 16/17，唯一失败是既有 heritage）。

## 遗留

- 新增少量 call_arg warning（如 `FUN_88c0 stats_stdev RSI call_arg_binding_missing`）是 arity
  变化（含 `notdec.unknown.i8` 被推断成 4 参的过估连锁）的副作用，未深挖，待过估处理时一起看。
- `notdec.unknown.i8` 是 lifting 层 unknown 占位 helper，其调用点被当成外部调用点收集
  （20 个），不属于真实外部函数，应过滤（lifting 层 unknown 残留问题）。
