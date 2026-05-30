@RDI = external global i64, !notdec.register !0
@RAX = external global i64, !notdec.register !3
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

!notdec.abi = !{!5}

!0 = !{!"space=register", !"offset=0", !"size=8", !"name=RDI"}
!1 = !{!2}
!2 = !{!"name=RDI", ptr @RDI}
!3 = !{!"space=register", !"offset=0", !"size=8", !"name=RAX"}
!4 = !{!"base=RAX", !"space=register", !"offset=0", !"size=8", !"name=RAX"}
!5 = !{!"prototype=__stdcall", !"stackpointer.register=RSP", !"stackpointer.space=register", !"extrapop=0", !"stackshift=0", !6, !9, !12}
!6 = !{!7}
!7 = !{!"minsize=1", !"maxsize=8", !"align=8", !"metatype=", !8}
!8 = !{!"kind=register", !"name=RDI", !"space=", !"offset=0"}
!9 = !{!10}
!10 = !{!"minsize=1", !"maxsize=8", !"align=8", !"metatype=", !11}
!11 = !{!"kind=register", !"name=RAX", !"space=", !"offset=0"}
!12 = !{}
