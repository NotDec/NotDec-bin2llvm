# 07 stack parameter recovery

本阶段参考 Ghidra stack parameter recovery，给 native prototype recovery 加入第一版栈上传参候选。

范围先收紧：

- 研究 Ghidra `ParamEntry` / `ParamList` / `ParamTrial` 如何表示 stack storage。
- 只支持 x86-64 SysV 简单栈参数形状。
- 第一版先写 metadata，不急着做 signature rewrite。
- 暂不做复杂 alias、varargs、动态栈调整、栈对象类型恢复。

具体实现前，每个小步仍需要先在本目录写规划文件，说明 Ghidra 源码文件、关键函数、native 侧复刻策略和验证方式。
