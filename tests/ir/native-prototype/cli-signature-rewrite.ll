@RDI = external global i64, !notdec.register !0
@RSI = external global i64, !notdec.register !13
@RAX = external global i64, !notdec.register !3
@RDX = external global i64, !notdec.register !18
@SINK = external global i64

define void @cli_input_rdi() !notdec.register.external_inputs !1 {
entry:
  %RDI.external_input = load i64, ptr @RDI, align 8, !notdec.register.external_input !2
  %used = add i64 %RDI.external_input, 1
  store i64 %used, ptr @SINK, align 8
  ret void
}

define void @cli_return_rax() {
entry:
  store i64 4660, ptr @RAX, align 8, !notdec.register.access !4
  ret void
}

define void @cli_input_rdi_return_rax() !notdec.register.external_inputs !1 {
entry:
  %RDI.external_input = load i64, ptr @RDI, align 8, !notdec.register.external_input !2
  %ret = add i64 %RDI.external_input, 2
  store i64 %ret, ptr @RAX, align 8, !notdec.register.access !4
  ret void
}

define void @cli_input_rdi_rsi_return_rax() !notdec.register.external_inputs !15 {
entry:
  %RDI.external_input = load i64, ptr @RDI, align 8, !notdec.register.external_input !2
  %RSI.external_input = load i64, ptr @RSI, align 8, !notdec.register.external_input !14
  %sum = add i64 %RDI.external_input, %RSI.external_input
  store i64 %sum, ptr @RAX, align 8, !notdec.register.access !4
  ret void
}

define void @cli_return_rax_rdx() {
entry:
  store i64 22136, ptr @RDX, align 8, !notdec.register.access !19
  store i64 4660, ptr @RAX, align 8, !notdec.register.access !4
  ret void
}

define void @cli_input_rdi_return_rax_rdx() !notdec.register.external_inputs !1 {
entry:
  %RDI.external_input = load i64, ptr @RDI, align 8, !notdec.register.external_input !2
  %rdx = add i64 %RDI.external_input, 4
  %rax = add i64 %RDI.external_input, 3
  store i64 %rdx, ptr @RDX, align 8, !notdec.register.access !19
  store i64 %rax, ptr @RAX, align 8, !notdec.register.access !4
  ret void
}

define void @cli_input_rdi_rsi_return_rax_rdx() !notdec.register.external_inputs !15 {
entry:
  %RDI.external_input = load i64, ptr @RDI, align 8, !notdec.register.external_input !2
  %RSI.external_input = load i64, ptr @RSI, align 8, !notdec.register.external_input !14
  %sum = add i64 %RDI.external_input, %RSI.external_input
  %diff = sub i64 %RDI.external_input, %RSI.external_input
  store i64 %diff, ptr @RDX, align 8, !notdec.register.access !19
  store i64 %sum, ptr @RAX, align 8, !notdec.register.access !4
  ret void
}

!notdec.abi = !{!5}

!0 = !{!"space=register", !"offset=0", !"size=8", !"name=RDI"}
!1 = !{!2}
!2 = !{!"name=RDI", ptr @RDI}
!3 = !{!"space=register", !"offset=0", !"size=8", !"name=RAX"}
!4 = !{!"base=RAX", !"space=register", !"offset=0", !"size=8", !"name=RAX"}
!5 = !{!"prototype=__stdcall", !"stackpointer.register=RSP", !"stackpointer.space=register", !"extrapop=0", !"stackshift=0", !6, !9, !12}
!6 = !{!7, !16}
!7 = !{!"minsize=1", !"maxsize=8", !"align=8", !"metatype=", !8}
!8 = !{!"kind=register", !"name=RDI", !"space=", !"offset=0"}
!9 = !{!10, !20}
!10 = !{!"minsize=1", !"maxsize=8", !"align=8", !"metatype=", !11}
!11 = !{!"kind=register", !"name=RAX", !"space=", !"offset=0"}
!12 = !{}
!13 = !{!"space=register", !"offset=0", !"size=8", !"name=RSI"}
!14 = !{!"name=RSI", ptr @RSI}
!15 = !{!2, !14}
!16 = !{!"minsize=1", !"maxsize=8", !"align=8", !"metatype=", !17}
!17 = !{!"kind=register", !"name=RSI", !"space=", !"offset=0"}
!18 = !{!"space=register", !"offset=0", !"size=8", !"name=RDX"}
!19 = !{!"base=RDX", !"space=register", !"offset=0", !"size=8", !"name=RDX"}
!20 = !{!"minsize=1", !"maxsize=8", !"align=8", !"metatype=", !21}
!21 = !{!"kind=register", !"name=RDX", !"space=", !"offset=0"}
