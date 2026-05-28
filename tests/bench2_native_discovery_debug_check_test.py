#!/usr/bin/env python3

"""Unit tests for the Bench2 native discovery debug oracle."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "scripts"
    / "bench2-native-discovery-debug-check.py"
)


def load_oracle_module():
    spec = importlib.util.spec_from_file_location("bench2_debug_oracle", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_subprogram_parser_ignores_child_low_pc() -> None:
    module = load_oracle_module()
    text = """
0x00000010: DW_TAG_compile_unit

0x00000020:   DW_TAG_subprogram
                DW_AT_name ("outer")
                DW_AT_low_pc (0x0000000000001000)
                DW_AT_high_pc (0x0000000000001040)

0x00000030:     DW_TAG_inlined_subroutine
                  DW_AT_name ("inline_child")
                  DW_AT_low_pc (0x0000000000002000)
                  DW_AT_high_pc (0x0000000000002010)

0x00000040:   DW_TAG_subprogram
                DW_AT_name ("next")
                DW_AT_low_pc (0x0000000000003000)
                DW_AT_high_pc (0x0000000000003020)
"""

    functions = module.parse_debug_functions(text, [(0x1000, 0x4000)])

    assert [(function.address, function.end, function.name)
            for function in functions] == [
                (0x1000, 0x1040, "outer"),
                (0x3000, 0x3020, "next"),
            ]


if __name__ == "__main__":
    test_subprogram_parser_ignores_child_low_pc()
