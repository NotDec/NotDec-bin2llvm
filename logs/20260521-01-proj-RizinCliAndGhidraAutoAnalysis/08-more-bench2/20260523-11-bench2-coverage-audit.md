# Bench2 Coverage Audit

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 范围

按 `/sn640/NotDec-Exp/Bench2/plan.md` 的 14 个项目核对。规则是：有动态库时优先选一个动态库；没有动态库时用二进制 fallback。

## 覆盖表

| Project | 选中目标 | 类型 | 证据日志 | 当前结果 |
| --- | --- | --- | --- | --- |
| vsftpd | `/usr/sbin/vsftpd` | binary fallback | `20260523-05-vsftpd-poison-fallback-clean.md` | `module-limit5` 无 `poison/undef/freeze`，LLVM verify 通过 |
| libuv | `/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0` | shared library | `20260523-08-batch-current-verification-libuv-memcached-tmux-openssh-redis-wrk.md` | `module-limit5` 5/5 bodies，LLVM verify 通过 |
| memcached | `/usr/bin/memcached` | binary fallback | `20260523-08-batch-current-verification-libuv-memcached-tmux-openssh-redis-wrk.md` | 单函数 LLVM verify 通过，无 `poison/undef/freeze` |
| lighttpd | `/usr/sbin/lighttpd` | binary fallback | `20260523-06-lighttpd-poison-clean.md` | 旧 `freeze poison` 已清掉，LLVM verify 通过 |
| tmux | `/usr/bin/tmux` | binary fallback | `20260523-08-batch-current-verification-libuv-memcached-tmux-openssh-redis-wrk.md` | 单函数 LLVM verify 通过，无 `poison/undef/freeze` |
| openssh | `/usr/bin/ssh` | binary fallback | `20260523-08-batch-current-verification-libuv-memcached-tmux-openssh-redis-wrk.md` | 单函数 LLVM verify 通过，无匹配残留 |
| wolfssl | `/usr/lib/x86_64-linux-gnu/libwolfssl.so.42.0.0` | shared library | `20260523-04-wolfssl-same-address-cbranch.md` | full module 4057/4057 bodies，LLVM verify 通过 |
| redis | `/usr/bin/redis-server` | binary fallback | `20260523-08-batch-current-verification-libuv-memcached-tmux-openssh-redis-wrk.md` | 单函数 LLVM verify 通过，无匹配残留 |
| libicu | `/usr/lib/x86_64-linux-gnu/libicuuc.so.74.2` | shared library | `20260523-01-libicu-duplicate-function-names.md` | common-library 4487/4517 bodies，LLVM verify 通过 |
| vim | `/usr/bin/vim.basic` | binary fallback | `20260523-09-vim-noanalysis-address-function.md` | `-noanalysis` 单函数导出、lower、LLVM verify 通过 |
| python | `/usr/lib/x86_64-linux-gnu/libpython3.12.so.1.0` | shared library | `20260523-02-python-shared-duplicate-function-names.md` | shared-library 6733/6966 bodies，LLVM verify 通过 |
| wrk | `/usr/bin/wrk` | binary fallback | `20260523-08-batch-current-verification-libuv-memcached-tmux-openssh-redis-wrk.md` | 单函数 LLVM verify 通过，无匹配残留 |
| ffmpeg | `/usr/lib/x86_64-linux-gnu/libswresample.so.4.12.100` | shared library | `20260523-10-ffmpeg-php-dynamic-libs-return-helper.md` | `module-limit5` 5/5 bodies，无 `poison/undef/freeze`，LLVM verify 通过 |
| php | `/usr/lib/php/20230831/calendar.so` | shared library | `20260523-10-ffmpeg-php-dynamic-libs-return-helper.md` | `module-limit5` 5/5 bodies，无 `poison/undef/freeze`，LLVM verify 通过 |

## 修复点汇总

1. module 级同名函数解析改成优先 entry address，只允许唯一短名 fallback。
2. same-address `CBRANCH` 改成用真实 CFG 出边处理。
3. `vim` 可用 `-noanalysis` 直接按地址建函数导出，避免整文件 auto-analysis 超时。
4. 单函数 direct `CALL` 没有名字但有地址时，生成 `sub_<addr>` 声明。
5. 非 void `RETURN` 缺少返回值输入时，不再生成 `undef`，改用 `notdec_heritage_RETURN_i<bits>()` helper。

## 判断

当前 14 个 Bench2 项目都已有一个选中目标和对应验证记录。动态库优先项已经补齐：`libuv / wolfssl / libicu / python / ffmpeg / php` 都使用 shared-library 或 extension `.so` 记录；没有动态库的项目使用二进制 fallback。

还没做的事情不计入本阶段完成条件：全项目全函数导出、消除所有 helper call、导入完整依赖库。
