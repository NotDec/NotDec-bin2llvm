#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "scripts"
    / "native-register-residue-audit.py"
)


def load_audit_module():
    spec = importlib.util.spec_from_file_location("native_register_residue_audit", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_register_access_summary_classifies_full_and_partial() -> None:
    module = load_audit_module()
    ir = """
@RAX = external global i64, !notdec.register !0
@RSI = external global i64, !notdec.register !12
@ZF = external global i8, !notdec.register !5

define void @sample() !notdec.register.clobbers !8 !notdec.register.external_inputs !9 {
entry:
  %in = load i64, ptr @RAX, align 8, !notdec.register.external_input !4
  %al = load i8, ptr @RAX, align 1, !notdec.register.access !2
  store i64 %in, ptr @RAX, align 8, !notdec.register.access !1
  call void @callee()
  store i64 %in, ptr @RAX, align 8, !notdec.register.access !2, !notdec.register.synthetic !7
  store i64 %in, ptr @RSI, align 8, !notdec.register.access !13, !notdec.register.synthetic !7
  store i64 %in, ptr @RAX, align 8, !notdec.register.access !1
  store i8 1, ptr @ZF, align 1, !notdec.register.access !6
  ret void
}

!0 = !{!"space=register", !"offset=0", !"size=8", !"name=RAX"}
!1 = !{!"base=RAX", !"space=register", !"offset=0", !"size=8", !"name=RAX"}
!2 = !{!"base=RAX", !"space=register", !"offset=0", !"size=1", !"name=AL"}
!4 = !{!"name=RAX", ptr @RAX}
!5 = !{!"space=register", !"offset=16", !"size=1", !"name=ZF"}
!6 = !{!"base=ZF", !"space=register", !"offset=16", !"size=1", !"name=ZF"}
!7 = !{!"partial_storage_ssa"}
!8 = !{!10}
!9 = !{!11}
!10 = !{!"name=RAX", ptr @RAX}
!11 = !{!"name=ZF", ptr @ZF}
!12 = !{!"space=register", !"offset=48", !"size=8", !"name=RSI"}
!13 = !{!"base=RSI", !"space=register", !"offset=48", !"size=1", !"name=SIL"}
"""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "sample.ll"
        path.write_text(ir, encoding="utf-8")
        accesses = module.parse_accesses(path)

    counts = module.summarize(accesses)
    assert counts[("gpr", "load", "external_input", "full", "full", "no")] == 1
    assert counts[("gpr", "load", "access", "partial", "partial", "no")] == 1
    assert counts[("gpr", "store", "access", "full", "full", "no")] == 2
    assert counts[("gpr", "store", "access", "partial", "full", "yes")] == 2
    assert counts[("flags", "store", "access", "full", "full", "no")] == 1

    assert accesses[0].line > 0
    assert accesses[0].block == "entry"
    assert accesses[0].storage_role == "caller_saved_gpr"
    assert accesses[0].local_context == "entry_external_input"
    assert accesses[0].residue_reason == "entry_external_input"
    assert accesses[0].function_effects == "clobbers"
    assert accesses[2].local_context == "before_call"
    assert accesses[2].residue_reason == "callsite_input_store"
    assert accesses[1].residue_reason == "partial_access"
    assert accesses[-1].function_effects == "external_inputs"
    assert accesses[-2].local_context == "return_path"
    assert accesses[-1].local_context == "before_ret"
    assert accesses[-1].residue_reason == "flags"


def test_register_access_details_include_residue_reason() -> None:
    module = load_audit_module()
    ir = """
@RBX = external global i64, !notdec.register !0

define i64 @sample_callee_saved() !notdec.register.preserves !2 {
entry:
  %saved = load i64, ptr @RBX, align 8, !notdec.register.access !1
  ret i64 %saved
}

!0 = !{!"space=register", !"offset=24", !"size=8", !"name=RBX"}
!1 = !{!"base=RBX", !"space=register", !"offset=24", !"size=8", !"name=RBX"}
!2 = !{!3}
!3 = !{!"name=RBX", ptr @RBX}
"""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "sample.ll"
        path.write_text(ir, encoding="utf-8")
        accesses = module.parse_accesses(path)

    assert len(accesses) == 1
    assert accesses[0].storage_role == "callee_saved_gpr"
    assert accesses[0].local_context == "before_ret"
    assert accesses[0].residue_reason == "callee_saved_return_path"


def test_callsite_input_store_wins_over_after_call_context() -> None:
    module = load_audit_module()
    ir = """
@RDI = external global i64, !notdec.register !0

define void @sample_consecutive_calls() {
entry:
  call void @first()
  store i64 1, ptr @RDI, align 8, !notdec.register.access !1
  call void @second()
  ret void
}

declare void @first()
declare void @second()

!0 = !{!"space=register", !"offset=56", !"size=8", !"name=RDI"}
!1 = !{!"base=RDI", !"space=register", !"offset=56", !"size=8", !"name=RDI"}
"""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "sample.ll"
        path.write_text(ir, encoding="utf-8")
        accesses = module.parse_accesses(path)

    assert len(accesses) == 1
    assert accesses[0].local_context == "before_call"
    assert accesses[0].residue_reason == "callsite_input_store"
    assert accesses[0].nearby_call == "second"
    assert accesses[0].nearby_call_kind == "declaration"


def test_callsite_input_store_can_have_stack_adjustment_before_call() -> None:
    module = load_audit_module()
    ir = """
@RDI = external global i64, !notdec.register !0
@RSP = external global i64, !notdec.register !2

define void @sample_callsite_setup() {
entry:
  store i64 1, ptr @RDI, align 8, !notdec.register.access !1
  store i64 1024, ptr @RSP, align 8, !notdec.register.access !3
  call void @callee()
  ret void
}

declare void @callee()

!0 = !{!"space=register", !"offset=56", !"size=8", !"name=RDI"}
!1 = !{!"base=RDI", !"space=register", !"offset=56", !"size=8", !"name=RDI"}
!2 = !{!"space=register", !"offset=32", !"size=8", !"name=RSP"}
!3 = !{!"base=RSP", !"space=register", !"offset=32", !"size=8", !"name=RSP"}
"""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "sample.ll"
        path.write_text(ir, encoding="utf-8")
        accesses = module.parse_accesses(path)

    assert accesses[0].name == "RDI"
    assert accesses[0].local_context == "before_call_path"
    assert accesses[0].residue_reason == "callsite_input_store"
    assert accesses[0].nearby_call == "callee"
    assert accesses[0].nearby_call_kind == "declaration"


def test_nearby_call_kind_marks_defined_callee_as_internal() -> None:
    module = load_audit_module()
    ir = """
@RDI = external global i64, !notdec.register !0

define void @sample_internal_call() {
entry:
  store i64 1, ptr @RDI, align 8, !notdec.register.access !1
  call void @callee()
  ret void
}

define void @callee() {
entry:
  ret void
}

!0 = !{!"space=register", !"offset=56", !"size=8", !"name=RDI"}
!1 = !{!"base=RDI", !"space=register", !"offset=56", !"size=8", !"name=RDI"}
"""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "sample.ll"
        path.write_text(ir, encoding="utf-8")
        accesses = module.parse_accesses(path)

    assert accesses[0].nearby_call == "callee"
    assert accesses[0].nearby_call_kind == "internal"


def test_stack_semantic_labels_frame_and_caller_stack_patterns() -> None:
    module = load_audit_module()
    ir = """
@RSP = external global i64, !notdec.register !0
@RBP = external global i64, !notdec.register !2
@RBX = external global i64, !notdec.register !4
@FS_OFFSET = external global i64, !notdec.register !6

define void @sample_stack_semantics() {
entry:
  %rsp = load i64, ptr @RSP, align 8, !notdec.register.external_input !1
  %rbp = load i64, ptr @RBP, align 8, !notdec.register.external_input !3
  %canary_addr = add i64 %rbp, -24
  %canary_ptr = inttoptr i64 %canary_addr to ptr
  %canary = load i64, ptr %canary_ptr, align 1
  %fs = load i64, ptr @FS_OFFSET, align 8, !notdec.register.access !7
  %fs_canary_addr = add i64 %fs, 40
  %fs_canary_ptr = inttoptr i64 %fs_canary_addr to ptr
  %fs_canary = load i64, ptr %fs_canary_ptr, align 1
  %ok = icmp eq i64 %canary, %fs_canary
  br i1 %ok, label %restore, label %fail

restore:
  %restore_addr = add i64 %rsp, 16
  %restore_ptr = inttoptr i64 %restore_addr to ptr
  %saved = load i64, ptr %restore_ptr, align 1
  store i64 %saved, ptr @RBX, align 8, !notdec.register.access !5
  ret void

fail:
  call void @__stack_chk_fail()
  ret void
}

declare void @__stack_chk_fail()

!0 = !{!"space=register", !"offset=32", !"size=8", !"name=RSP"}
!1 = !{!"name=RSP", ptr @RSP}
!2 = !{!"space=register", !"offset=40", !"size=8", !"name=RBP"}
!3 = !{!"name=RBP", ptr @RBP}
!4 = !{!"space=register", !"offset=24", !"size=8", !"name=RBX"}
!5 = !{!"base=RBX", !"space=register", !"offset=24", !"size=8", !"name=RBX"}
!6 = !{!"space=register", !"offset=256", !"size=8", !"name=FS_OFFSET"}
!7 = !{!"base=FS_OFFSET", !"space=register", !"offset=256", !"size=8", !"name=FS_OFFSET"}
"""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "sample.ll"
        path.write_text(ir, encoding="utf-8")
        accesses = module.parse_accesses(path)

    by_name = {access.name: access for access in accesses}
    assert "stack_canary" in by_name["RBP"].stack_semantic
    assert "saved_register_restore" in by_name["RBP"].stack_semantic
    assert "caller_stack" in by_name["RSP"].stack_semantic
    assert "saved_register_restore" in by_name["RSP"].stack_semantic


def test_stack_semantic_marks_chunk_phi_only_for_phi_derived_frame() -> None:
    module = load_audit_module()
    ir = """
@RBP = external global i64, !notdec.register !0
@FS_OFFSET = external global i64, !notdec.register !2

define void @sample_direct_frame() {
entry:
  %rbp = load i64, ptr @RBP, align 8, !notdec.register.external_input !1
  %canary_addr = add i64 %rbp, -24
  %canary_ptr = inttoptr i64 %canary_addr to ptr
  %canary = load i64, ptr %canary_ptr, align 1
  %fs = load i64, ptr @FS_OFFSET, align 8, !notdec.register.access !3
  %fs_canary_addr = add i64 %fs, 40
  %fs_canary_ptr = inttoptr i64 %fs_canary_addr to ptr
  %fs_canary = load i64, ptr %fs_canary_ptr, align 1
  %ok = icmp eq i64 %canary, %fs_canary
  br i1 %ok, label %done, label %fail

done:
  ret void

fail:
  call void @__stack_chk_fail()
  ret void
}

define void @sample_phi_frame() {
entry:
  %rbp = load i64, ptr @RBP, align 8, !notdec.register.external_input !1
  br label %body

body:
  %frame = phi i64 [ %rbp, %entry ], [ %frame, %loop ]
  %canary_addr = add i64 %frame, -24
  %canary_ptr = inttoptr i64 %canary_addr to ptr
  %canary = load i64, ptr %canary_ptr, align 1
  %fs = load i64, ptr @FS_OFFSET, align 8, !notdec.register.access !3
  %fs_canary_addr = add i64 %fs, 40
  %fs_canary_ptr = inttoptr i64 %fs_canary_addr to ptr
  %fs_canary = load i64, ptr %fs_canary_ptr, align 1
  %ok = icmp eq i64 %canary, %fs_canary
  br i1 %ok, label %done, label %loop

loop:
  br label %body

done:
  ret void
}

declare void @__stack_chk_fail()

!0 = !{!"space=register", !"offset=40", !"size=8", !"name=RBP"}
!1 = !{!"name=RBP", ptr @RBP}
!2 = !{!"space=register", !"offset=256", !"size=8", !"name=FS_OFFSET"}
!3 = !{!"base=FS_OFFSET", !"space=register", !"offset=256", !"size=8", !"name=FS_OFFSET"}
"""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "sample.ll"
        path.write_text(ir, encoding="utf-8")
        accesses = module.parse_accesses(path)

    by_function = {access.function: access for access in accesses if access.name == "RBP"}
    assert "chunk_phi" not in by_function["sample_direct_frame"].stack_semantic
    assert "chunk_phi" in by_function["sample_phi_frame"].stack_semantic


if __name__ == "__main__":
    test_register_access_summary_classifies_full_and_partial()
    test_register_access_details_include_residue_reason()
    test_callsite_input_store_wins_over_after_call_context()
    test_callsite_input_store_can_have_stack_adjustment_before_call()
    test_nearby_call_kind_marks_defined_callee_as_internal()
    test_stack_semantic_labels_frame_and_caller_stack_patterns()
    test_stack_semantic_marks_chunk_phi_only_for_phi_derived_frame()
