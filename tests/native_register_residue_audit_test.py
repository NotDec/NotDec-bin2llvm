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

define void @sample() {
entry:
  %in = load i64, ptr @RAX, align 8, !notdec.register.external_input !4
  %al = load i8, ptr @RAX, align 1, !notdec.register.access !2
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
"""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "sample.ll"
        path.write_text(ir, encoding="utf-8")
        accesses = module.parse_accesses(path)

    counts = module.summarize(accesses)
    assert counts[("gpr", "load", "external_input", "full", "full")] == 1
    assert counts[("gpr", "load", "access", "partial", "partial")] == 1
    assert counts[("gpr", "store", "access", "full", "full")] == 1
    assert counts[("flags", "store", "access", "full", "full")] == 1


if __name__ == "__main__":
    test_register_access_summary_classifies_full_and_partial()
