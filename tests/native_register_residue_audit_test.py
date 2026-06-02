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
@ZF = external global i8, !notdec.register !5

define void @sample() !notdec.register.clobbers !8 !notdec.register.external_inputs !9 {
entry:
  %in = load i64, ptr @RAX, align 8, !notdec.register.external_input !4
  %al = load i8, ptr @RAX, align 1, !notdec.register.access !2
  store i64 %in, ptr @RAX, align 8, !notdec.register.access !1
  call void @callee()
  store i64 %in, ptr @RAX, align 8, !notdec.register.access !2, !notdec.register.synthetic !7
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
"""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "sample.ll"
        path.write_text(ir, encoding="utf-8")
        accesses = module.parse_accesses(path)

    counts = module.summarize(accesses)
    assert counts[("gpr", "load", "external_input", "full", "full", "no")] == 1
    assert counts[("gpr", "load", "access", "partial", "partial", "no")] == 1
    assert counts[("gpr", "store", "access", "full", "full", "no")] == 2
    assert counts[("gpr", "store", "access", "partial", "full", "yes")] == 1
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


if __name__ == "__main__":
    test_register_access_summary_classifies_full_and_partial()
    test_register_access_details_include_residue_reason()
