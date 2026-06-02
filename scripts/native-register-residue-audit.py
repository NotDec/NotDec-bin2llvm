#!/usr/bin/env python3

"""Summarize remaining NotDec register accesses in LLVM IR."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


METADATA_RE = re.compile(r"^!(\d+)\s*=\s*!\{(.*)\}\s*$")
MD_REF_RE = re.compile(r"!notdec\.register\.(access|external_input)\s+!(\d+)")
GLOBAL_RE = re.compile(r"^(@[-a-zA-Z$._0-9]+)\s*=.*!notdec\.register\s+!(\d+)")
PTR_GLOBAL_RE = re.compile(r"ptr\s+(@[-a-zA-Z$._0-9]+)")
FIELD_RE = re.compile(r'!"([^"=]+)=([^"]*)"')
LOAD_RE = re.compile(r"(?:^|=\s*)load\s+([^,]+),")
STORE_RE = re.compile(r"^store\s+([^,]+),")


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
    function: str
    instruction: str
    metadata_kind: str
    access_kind: str
    category: str
    base: str
    name: str
    space: str
    offset: int
    size: int
    value_size: int | None
    unit_offset: int | None
    unit_size: int | None

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


def parse_accesses(path: Path) -> list[RegisterAccess]:
    text = path.read_text(encoding="utf-8")
    metadata = parse_metadata(text)
    globals_by_symbol = parse_globals(text, metadata)
    globals_by_name = {global_.name: global_ for global_ in globals_by_symbol.values()}

    accesses: list[RegisterAccess] = []
    function = "<module>"
    for line in text.splitlines():
        function = current_function(line, function)
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
        accesses.append(
            RegisterAccess(
                file=str(path),
                function=function,
                instruction=line.strip(),
                metadata_kind=metadata_kind,
                access_kind=instruction_kind(line),
                category=classify_register(access_name or base),
                base=base,
                name=access_name,
                space=space,
                offset=offset,
                size=size,
                value_size=value_size(line),
                unit_offset=unit.offset if unit is not None else None,
                unit_size=unit.size if unit is not None else None,
            )
        )
    return accesses


def summarize(accesses: Iterable[RegisterAccess]) -> dict[tuple[str, str, str, str, str], int]:
    counts: dict[tuple[str, str, str, str, str], int] = {}
    for access in accesses:
        shape = "full" if access.is_full else "partial"
        value_shape = "full" if access.value_is_full else "partial"
        if access.value_size is None:
            value_shape = "unknown"
        key = (access.category, access.access_kind, access.metadata_kind,
               shape, value_shape)
        counts[key] = counts.get(key, 0) + 1
    return counts


def write_summary(accesses: list[RegisterAccess], output) -> None:
    writer = csv.writer(output, delimiter="\t", lineterminator="\n")
    writer.writerow([
        "category", "access_kind", "metadata_kind", "shape", "value_shape",
        "count",
    ])
    for key, count in sorted(summarize(accesses).items()):
        writer.writerow([*key, count])


def write_details(accesses: list[RegisterAccess], output) -> None:
    writer = csv.writer(output, delimiter="\t", lineterminator="\n")
    writer.writerow([
        "file", "function", "category", "access_kind", "metadata_kind",
        "shape", "value_shape", "base", "name", "space", "offset", "size",
        "value_size", "instruction",
    ])
    for access in accesses:
        writer.writerow([
            access.file,
            access.function,
            access.category,
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
            access.instruction,
        ])


def main(argv: list[str] | None = None) -> int:
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
