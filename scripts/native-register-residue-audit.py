#!/usr/bin/env python3

"""Summarize remaining NotDec register accesses in LLVM IR."""

from __future__ import annotations

import argparse
import csv
import re
import signal
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


METADATA_RE = re.compile(r"^!(\d+)\s*=\s*!\{(.*)\}\s*$")
MD_REF_RE = re.compile(r"!notdec\.register\.(access|external_input)\s+!(\d+)")
FUNCTION_EFFECT_RE = re.compile(
    r"!notdec\.register\.(clobbers|preserves|external_inputs)\s+!(\d+)"
)
SYNTHETIC_RE = re.compile(r"!notdec\.register\.synthetic\s+!\d+")
GLOBAL_RE = re.compile(r"^(@[-a-zA-Z$._0-9]+)\s*=.*!notdec\.register\s+!(\d+)")
PTR_GLOBAL_RE = re.compile(r"ptr\s+(@[-a-zA-Z$._0-9]+)")
FIELD_RE = re.compile(r'!"([^"=]+)=([^"]*)"')
LOAD_RE = re.compile(r"(?:^|=\s*)load\s+([^,]+),")
STORE_RE = re.compile(r"^store\s+([^,]+),")
LABEL_RE = re.compile(r"^[-a-zA-Z$._0-9]+:\s*(?:;.*)?$")


@dataclass(frozen=True)
class RegisterGlobal:
    symbol: str
    name: str
    space: str
    offset: int
    size: int


@dataclass(frozen=True)
class RegisterAccess:
    file: str
    line: int
    function: str
    block: str
    instruction: str
    metadata_kind: str
    access_kind: str
    category: str
    storage_role: str
    local_context: str
    function_effects: str
    residue_reason: str
    base: str
    name: str
    space: str
    offset: int
    size: int
    value_size: int | None
    unit_offset: int | None
    unit_size: int | None
    synthetic: bool

    @property
    def is_full(self) -> bool:
        return (
            self.unit_offset is not None
            and self.unit_size is not None
            and self.offset == self.unit_offset
            and self.size == self.unit_size
        )

    @property
    def value_is_full(self) -> bool:
        return (
            self.value_size is not None
            and self.unit_size is not None
            and self.value_size == self.unit_size
        )


def parse_int(text: str | None, default: int = 0) -> int:
    if text is None or text == "":
        return default
    return int(text, 0)


def parse_metadata(text: str) -> dict[str, dict[str, str]]:
    metadata: dict[str, dict[str, str]] = {}
    for line in text.splitlines():
        match = METADATA_RE.match(line.strip())
        if not match:
            continue
        node_id, body = match.groups()
        fields = {key: value for key, value in FIELD_RE.findall(body)}
        metadata[node_id] = fields
    return metadata


def parse_globals(text: str, metadata: dict[str, dict[str, str]]) -> dict[str, RegisterGlobal]:
    globals_by_symbol: dict[str, RegisterGlobal] = {}
    for line in text.splitlines():
        match = GLOBAL_RE.match(line.strip())
        if not match:
            continue
        symbol, node_id = match.groups()
        fields = metadata.get(node_id, {})
        name = fields.get("name", symbol.removeprefix("@"))
        globals_by_symbol[symbol] = RegisterGlobal(
            symbol=symbol,
            name=name,
            space=fields.get("space", ""),
            offset=parse_int(fields.get("offset")),
            size=parse_int(fields.get("size")),
        )
    return globals_by_symbol


def classify_register(name: str) -> str:
    upper = name.upper()
    flags = {"CF", "PF", "AF", "ZF", "SF", "TF", "IF", "DF", "OF"}
    gpr = {
        "RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RSP", "RBP",
        "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
        "EAX", "EBX", "ECX", "EDX", "ESI", "EDI", "ESP", "EBP",
        "AX", "BX", "CX", "DX", "SI", "DI", "SP", "BP",
        "AL", "AH", "BL", "BH", "CL", "CH", "DL", "DH",
        "SIL", "DIL", "SPL", "BPL",
    }
    if upper in flags:
        return "flags"
    if upper in gpr or re.fullmatch(r"R(8|9|1[0-5])[DWB]?", upper):
        return "gpr"
    if re.fullmatch(r"[XYZ]?MM[0-9]+.*", upper):
        return "vector"
    return "other"


def current_function(line: str, previous: str) -> str:
    stripped = line.strip()
    if not stripped.startswith("define "):
        return previous
    at = stripped.find("@")
    if at < 0:
        return previous
    end = stripped.find("(", at)
    return stripped[at + 1:end] if end > at else previous


def parse_function_effects(
    lines: list[str], metadata: dict[str, dict[str, str]]
) -> dict[str, dict[str, set[str]]]:
    raw_metadata_lines: dict[str, str] = {}
    for line in lines:
        match = METADATA_RE.match(line.strip())
        if match:
            raw_metadata_lines[match.group(1)] = line.strip()

    def names_from_node(node_id: str) -> set[str]:
        names: set[str] = set()
        seen: set[str] = set()

        def visit(current: str) -> None:
            if current in seen:
                return
            seen.add(current)
            fields = metadata.get(current, {})
            name = fields.get("name")
            if name:
                names.add(name)
            line = raw_metadata_lines.get(current, "")
            match = METADATA_RE.match(line)
            if match is None:
                return
            for child in re.findall(r"!([0-9]+)", match.group(2)):
                visit(child)

        visit(node_id)
        return names

    effects_by_function: dict[str, dict[str, set[str]]] = {}
    function = "<module>"
    for line in lines:
        previous = function
        function = current_function(line, function)
        if function == previous or not line.strip().startswith("define "):
            continue
        effects = {"clobbers": set(), "preserves": set(), "external_inputs": set()}
        for kind, node_id in FUNCTION_EFFECT_RE.findall(line):
            effects[kind].update(names_from_node(node_id))
        effects_by_function[function] = effects
    return effects_by_function


def current_block(line: str, previous: str) -> str:
    stripped = line.strip()
    if stripped.startswith("define "):
        return "entry"
    if LABEL_RE.match(stripped):
        return stripped.split(":", 1)[0]
    return previous


def instruction_kind(line: str) -> str:
    stripped = line.strip()
    if re.search(r"(^|=\s*)load\b", stripped):
        return "load"
    if stripped.startswith("store "):
        return "store"
    return "other"


def value_size(line: str) -> int | None:
    stripped = line.strip()
    match = LOAD_RE.search(stripped)
    if match is None:
        match = STORE_RE.search(stripped)
    if match is None:
        return None
    value_type = match.group(1).strip().split()[0]
    int_match = re.fullmatch(r"i([0-9]+)", value_type)
    if int_match is None:
        return None
    bits = int(int_match.group(1))
    if bits % 8 != 0:
        return None
    return bits // 8


def is_call_instruction(line: str) -> bool:
    stripped = line.strip()
    return bool(re.search(r"(^|=\s*)call\b", stripped))


def is_ret_instruction(line: str) -> bool:
    return line.strip().startswith("ret ")


def is_block_boundary(line: str) -> bool:
    stripped = line.strip()
    return stripped.startswith("define ") or stripped == "}" or bool(LABEL_RE.match(stripped))


def previous_instruction(lines: list[str], index: int) -> str:
    for cursor in range(index - 1, -1, -1):
        stripped = lines[cursor].strip()
        if stripped == "" or stripped.startswith(";"):
            continue
        if is_block_boundary(stripped):
            return ""
        return stripped
    return ""


def next_instruction(lines: list[str], index: int) -> str:
    for cursor in range(index + 1, len(lines)):
        stripped = lines[cursor].strip()
        if stripped == "" or stripped.startswith(";"):
            continue
        if is_block_boundary(stripped):
            return ""
        return stripped
    return ""


def reaches_return_in_block_without_call(lines: list[str], index: int) -> bool:
    for cursor in range(index + 1, len(lines)):
        stripped = lines[cursor].strip()
        if stripped == "" or stripped.startswith(";"):
            continue
        if is_block_boundary(stripped):
            return False
        if is_call_instruction(stripped):
            return False
        if is_ret_instruction(stripped):
            return True
    return False


def reaches_call_in_block(lines: list[str], index: int) -> bool:
    for cursor in range(index + 1, len(lines)):
        stripped = lines[cursor].strip()
        if stripped == "" or stripped.startswith(";"):
            continue
        if is_block_boundary(stripped):
            return False
        if is_call_instruction(stripped):
            return True
        if is_ret_instruction(stripped):
            return False
    return False


def storage_role(name: str) -> str:
    upper = name.upper()
    if upper in {"RSP", "ESP", "SP"}:
        return "stack_pointer"
    if upper in {"RBP", "EBP", "BP"}:
        return "frame_pointer"
    if upper in {"RBX", "EBX", "BX", "BL", "BH", "R12", "R12D", "R12W", "R12B",
                 "R13", "R13D", "R13W", "R13B", "R14", "R14D", "R14W", "R14B",
                 "R15", "R15D", "R15W", "R15B"}:
        return "callee_saved_gpr"
    if classify_register(name) == "gpr":
        return "caller_saved_gpr"
    return classify_register(name)


def local_context(
    lines: list[str],
    index: int,
    block: str,
    metadata_kind: str,
) -> str:
    if block == "entry" and metadata_kind == "external_input":
        return "entry_external_input"
    previous = previous_instruction(lines, index)
    following = next_instruction(lines, index)
    if is_call_instruction(following):
        return "before_call"
    if reaches_call_in_block(lines, index):
        return "before_call_path"
    if is_call_instruction(previous):
        return "after_call"
    if is_ret_instruction(following):
        return "before_ret"
    if reaches_return_in_block_without_call(lines, index):
        return "return_path"
    return "ordinary"


def name_matches_effect(access_name: str, effect_name: str) -> bool:
    return access_name == effect_name or access_name.startswith(effect_name + "_")


def function_effects_for_access(
    effects: dict[str, set[str]],
    access_name: str,
    access_base: str,
) -> str:
    matched: list[str] = []
    for kind in ("clobbers", "preserves", "external_inputs"):
        names = effects.get(kind, set())
        if any(
            name_matches_effect(access_name, name) or
            name_matches_effect(access_base, name)
            for name in names
        ):
            matched.append(kind)
    return ",".join(matched) if matched else "none"


def residue_reason_for_access(
    access_kind: str,
    metadata_kind: str,
    category: str,
    storage_role: str,
    local_context: str,
    function_effects: str,
    is_full: bool,
) -> str:
    # Keep these labels factual.  They are used to pick the next pass change,
    # not to prove an access can be removed.
    if metadata_kind == "external_input":
        if local_context == "entry_external_input":
            return "entry_external_input"
        return "fallback_external_input"
    if not is_full:
        return "partial_access"
    if storage_role in {"stack_pointer", "frame_pointer"}:
        return storage_role
    if storage_role == "callee_saved_gpr":
        if local_context in {"before_ret", "return_path"}:
            return "callee_saved_return_path"
        return "callee_saved_gpr"
    if access_kind == "load" and local_context == "after_call":
        if "clobbers" in function_effects.split(","):
            return "after_call_clobbered"
        if "preserves" in function_effects.split(","):
            return "after_call_preserved"
        return "after_call_unknown_effect"
    if access_kind == "store" and local_context in {
        "before_call",
        "before_call_path",
    }:
        return "callsite_input_store"
    if access_kind == "load" and local_context == "before_ret":
        return "return_value_load"
    if category == "flags":
        return "flags"
    if category == "vector":
        return "vector"
    return local_context


def parse_accesses(path: Path) -> list[RegisterAccess]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    metadata = parse_metadata(text)
    globals_by_symbol = parse_globals(text, metadata)
    globals_by_name = {global_.name: global_ for global_ in globals_by_symbol.values()}
    effects_by_function = parse_function_effects(lines, metadata)

    accesses: list[RegisterAccess] = []
    function = "<module>"
    block = "<module>"
    for index, line in enumerate(lines):
        function = current_function(line, function)
        block = current_block(line, block)
        if "!notdec.register" not in line:
            continue
        md_match = MD_REF_RE.search(line)
        if not md_match:
            continue
        fields = metadata.get(md_match.group(2), {})
        if not fields:
            continue

        metadata_kind = md_match.group(1)

        base = fields.get("base", "")
        name = fields.get("name", base)
        space = fields.get("space", "")
        offset = parse_int(fields.get("offset"))
        size = parse_int(fields.get("size"))
        unit: RegisterGlobal | None = globals_by_name.get(base)

        if metadata_kind == "external_input":
            symbol_match = PTR_GLOBAL_RE.search(line)
            if symbol_match:
                unit = globals_by_symbol.get(symbol_match.group(1), unit)
            if unit is not None:
                base = unit.name
                name = fields.get("name", unit.name)
                space = unit.space
                offset = unit.offset
                size = unit.size

        access_name = name or base
        is_full = (
            unit is not None
            and offset == unit.offset
            and size == unit.size
        )
        effects = function_effects_for_access(
            effects_by_function.get(function, {}),
            access_name or base,
            base,
        )
        accesses.append(
            RegisterAccess(
                file=str(path),
                line=index + 1,
                function=function,
                block=block,
                instruction=line.strip(),
                metadata_kind=metadata_kind,
                access_kind=instruction_kind(line),
                category=classify_register(access_name or base),
                storage_role=storage_role(access_name or base),
                local_context=local_context(lines, index, block, metadata_kind),
                function_effects=effects,
                residue_reason=residue_reason_for_access(
                    instruction_kind(line),
                    metadata_kind,
                    classify_register(access_name or base),
                    storage_role(access_name or base),
                    local_context(lines, index, block, metadata_kind),
                    effects,
                    is_full,
                ),
                base=base,
                name=access_name,
                space=space,
                offset=offset,
                size=size,
                value_size=value_size(line),
                unit_offset=unit.offset if unit is not None else None,
                unit_size=unit.size if unit is not None else None,
                synthetic=SYNTHETIC_RE.search(line) is not None,
            )
        )
    return accesses


def summarize(accesses: Iterable[RegisterAccess]) -> dict[tuple[str, str, str, str, str, str], int]:
    counts: dict[tuple[str, str, str, str, str, str], int] = {}
    for access in accesses:
        shape = "full" if access.is_full else "partial"
        value_shape = "full" if access.value_is_full else "partial"
        if access.value_size is None:
            value_shape = "unknown"
        synthetic = "yes" if access.synthetic else "no"
        key = (access.category, access.access_kind, access.metadata_kind,
               shape, value_shape, synthetic)
        counts[key] = counts.get(key, 0) + 1
    return counts


def write_summary(accesses: list[RegisterAccess], output) -> None:
    writer = csv.writer(output, delimiter="\t", lineterminator="\n")
    writer.writerow([
        "category", "access_kind", "metadata_kind", "shape", "value_shape",
        "synthetic", "count",
    ])
    for key, count in sorted(summarize(accesses).items()):
        writer.writerow([*key, count])


def write_details(accesses: list[RegisterAccess], output) -> None:
    writer = csv.writer(output, delimiter="\t", lineterminator="\n")
    writer.writerow([
        "file", "line", "function", "block", "category", "storage_role",
        "local_context", "function_effects", "residue_reason", "access_kind",
        "metadata_kind", "shape", "value_shape", "base", "name", "space",
        "offset", "size", "value_size", "synthetic", "instruction",
    ])
    for access in accesses:
        writer.writerow([
            access.file,
            access.line,
            access.function,
            access.block,
            access.category,
            access.storage_role,
            access.local_context,
            access.function_effects,
            access.residue_reason,
            access.access_kind,
            access.metadata_kind,
            "full" if access.is_full else "partial",
            "full" if access.value_is_full else (
                "unknown" if access.value_size is None else "partial"
            ),
            access.base,
            access.name,
            access.space,
            access.offset,
            access.size,
            "" if access.value_size is None else access.value_size,
            "yes" if access.synthetic else "no",
            access.instruction,
        ])


def main(argv: list[str] | None = None) -> int:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    parser = argparse.ArgumentParser()
    parser.add_argument("ll", nargs="+", type=Path, help="LLVM .ll files to audit")
    parser.add_argument("--details", action="store_true", help="write per-access rows")
    args = parser.parse_args(argv)

    accesses: list[RegisterAccess] = []
    for path in args.ll:
        accesses.extend(parse_accesses(path))

    if args.details:
        write_details(accesses, sys.stdout)
    else:
        write_summary(accesses, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
