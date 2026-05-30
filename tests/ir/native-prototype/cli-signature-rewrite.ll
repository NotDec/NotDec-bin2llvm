@RDI = external global i64, !notdec.register !0
@SINK = external global i64

define void @cli_input_rdi() !notdec.register.external_inputs !1 {
entry:
  %RDI.external_input = load i64, ptr @RDI, align 8, !notdec.register.external_input !2
  %used = add i64 %RDI.external_input, 1
  store i64 %used, ptr @SINK, align 8
  ret void
}

!notdec.abi = !{!3}

!0 = !{!"space=register", !"offset=0", !"size=8", !"name=RDI"}
!1 = !{!2}
!2 = !{!"name=RDI", ptr @RDI}
!3 = !{!"prototype=__stdcall", !"stackpointer.register=RSP", !"stackpointer.space=register", !"extrapop=0", !"stackshift=0", !4, !7, !8}
!4 = !{!5}
!5 = !{!"minsize=1", !"maxsize=8", !"align=8", !"metatype=", !6}
!6 = !{!"kind=register", !"name=RDI", !"space=", !"offset=0"}
!7 = !{}
!8 = !{}
