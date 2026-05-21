# Stage 7: Bench2 Regression

## 这一步做什么

固化 `vsftpd`、`libuv`、`memcached` 的 native analysis 回归方式。

## 先写什么

1. Bench2 目标路径和运行方式。
2. 统计哪些指标。
3. 如何和 GhidraScript heritage 路线同口径对比。

## 后续要求

只记录这一阶段的规划和实现。

## 当前入口

`scripts/bench2-native-smoke.sh` 固定跑 `vsftpd`、`libuv`、`memcached`：

1. `notdec-native-discover --summary-json`
2. `notdec-native-llvm --all-confirmed`
3. LLVM 22 `llvm-as`
4. LLVM 22 `opt -passes=verify`
