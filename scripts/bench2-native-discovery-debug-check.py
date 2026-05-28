#!/usr/bin/env python3
"""Compare native discovery against Bench2 debug-info function entries."""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


DEFAULT_BUILD_DIR = Path("/tmp/notdec-bin2llvm-build")
DEFAULT_BENCH2_ROOT = Path("/sn640/NotDec-Exp/Bench2/rootfs")
DEFAULT_MANIFEST = Path("/sn640/NotDec-Exp/Bench2/manifest/benchmark-targets.tsv")
DEFAULT_LLVM_BIN = Path("/sn640/NotDec/llvm-22.1.0.obj/bin")


@dataclass(frozen=True)
class ManifestRow:
    project: str
    role: str
    rootfs_path: str
    debug_path: str


@dataclass(frozen=True)
class DebugFunction:
    address: int
    end: int
    name: str


def run_text(command: list[str]) -> str:
    return subprocess.check_output(command, text=True, stderr=subprocess.PIPE)


def run_json(command: list[str]) -> dict:
    return json.loads(run_text(command))


def parse_int(text: str) -> int:
    return int(text, 0)


def load_manifest(path: Path) -> list[ManifestRow]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        return [
            ManifestRow(
                project=row["project"],
                role=row["role"],
                rootfs_path=row["rootfs_path"],
                debug_path=row["debug_path"],
            )
            for row in reader
        ]


def executable_ranges(elf_path: Path) -> list[tuple[int, int]]:
    output = run_text(["readelf", "-W", "-l", str(elf_path)])
    ranges: list[tuple[int, int]] = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 7 or fields[0] != "LOAD":
            continue
        flags = "".join(fields[6:-1])
        if "E" not in flags:
            continue
        start = parse_int(fields[2])
        size = parse_int(fields[5])
        ranges.append((start, start + size))
    return ranges


def in_ranges(address: int, ranges: list[tuple[int, int]]) -> bool:
    return any(start <= address < end for start, end in ranges)


def die_depth(line: str) -> int | None:
    match = re.match(r"^0x[0-9a-f]+:(\s*)DW_TAG_", line)
    if not match:
        return None
    return len(match.group(1))


def parse_debug_functions(dwarfdump_text: str,
                          exec_ranges: list[tuple[int, int]]) -> list[DebugFunction]:
    die_re = re.compile(r"^0x[0-9a-f]+:(\s*)DW_TAG_([A-Za-z0-9_]+)")
    attr_re = re.compile(r"^\s+(DW_AT_[A-Za-z0-9_]+)\s+\((.*)\)")
    functions: list[DebugFunction] = []
    current_depth: int | None = None
    accepting_attrs = False
    attrs: dict[str, str] = {}

    def flush() -> None:
        if current_depth is None:
            return
        if attrs.get("DW_AT_declaration") == "true":
            return
        low_text = attrs.get("DW_AT_low_pc")
        high_text = attrs.get("DW_AT_high_pc")
        if not low_text or not high_text:
            return
        low = parse_int(low_text.split()[0])
        high = parse_int(high_text.split()[0])
        if high <= low:
            high = low + high
        if low == 0 or high <= low or not in_ranges(low, exec_ranges):
            return
        name = attrs.get("DW_AT_linkage_name") or attrs.get("DW_AT_name") or ""
        functions.append(DebugFunction(low, high, name.strip('"')))

    for line in dwarfdump_text.splitlines():
        die_match = die_re.match(line)
        if die_match:
            depth = len(die_match.group(1))
            tag = die_match.group(2)
            if current_depth is not None and depth <= current_depth:
                flush()
                current_depth = None
                accepting_attrs = False
                attrs = {}
            elif current_depth is not None:
                # Child DIEs can also carry DW_AT_low_pc, for example
                # DW_TAG_inlined_subroutine.  Those ranges are not function
                # entries and must not overwrite the parent subprogram attrs.
                accepting_attrs = False
                continue
            if tag == "subprogram":
                current_depth = depth
                accepting_attrs = True
                attrs = {}
            continue

        if current_depth is None or not accepting_attrs:
            continue
        attr_match = attr_re.match(line)
        if not attr_match:
            continue
        name, value = attr_match.groups()
        if name in attrs:
            continue
        quoted = re.search(r'"([^"]*)"', value)
        if quoted:
            attrs[name] = quoted.group(1)
        else:
            attrs[name] = value.strip()

    flush()

    by_address: dict[int, DebugFunction] = {}
    for function in functions:
        by_address.setdefault(function.address, function)
    return [by_address[address] for address in sorted(by_address)]


def debug_path_for(row: ManifestRow, bench2_root: Path) -> Path:
    if row.debug_path == "EMBEDDED_DEBUG":
        return bench2_root / row.rootfs_path.lstrip("/")
    return bench2_root / row.debug_path.lstrip("/")


def select_rows(rows: list[ManifestRow], targets: list[str]) -> list[ManifestRow]:
    if not targets:
        return rows
    wanted = set(targets)
    selected = [
        row for row in rows if f"{row.project}:{row.role}" in wanted
    ]
    missing = wanted - {f"{row.project}:{row.role}" for row in selected}
    if missing:
        raise SystemExit("missing manifest targets: " + ", ".join(sorted(missing)))
    return selected


def sample_missing(addresses: set[int], known: set[int],
                   debug_by_addr: dict[int, DebugFunction]) -> str:
    missing = sorted(addresses - known)[:8]
    return ",".join(
        f"0x{address:x}:{debug_by_addr[address].name}" for address in missing
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--bench2-root", type=Path, default=DEFAULT_BENCH2_ROOT)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--llvm-bin", type=Path, default=DEFAULT_LLVM_BIN)
    parser.add_argument("--target", action="append", default=[],
                        help="manifest target in project:role form")
    parser.add_argument("--decode-seed-limit", type=int)
    args = parser.parse_args()

    discover = args.build_dir / "bin/notdec-native-discover"
    dwarfdump = args.llvm_bin / "llvm-dwarfdump"

    rows = select_rows(load_manifest(args.manifest), args.target)
    print(
        "project\trole\tdebug_functions\tnative_seeds\tnative_confirmed\t"
        "seed_hits\tconfirmed_hits\tseed_coverage\tconfirmed_coverage\t"
        "missing_confirmed_sample"
    )
    for row in rows:
        elf_path = args.bench2_root / row.rootfs_path.lstrip("/")
        debug_path = debug_path_for(row, args.bench2_root)
        if not elf_path.exists() or not debug_path.exists():
            continue

        exec_ranges = executable_ranges(elf_path)
        debug_text = run_text([str(dwarfdump), "--debug-info", str(debug_path)])
        debug_functions = parse_debug_functions(debug_text, exec_ranges)
        debug_by_addr = {function.address: function for function in debug_functions}
        debug_addrs = set(debug_by_addr)

        discover_prefix = [str(discover)]
        if args.decode_seed_limit is not None:
            discover_prefix += ["--decode-seed-limit", str(args.decode_seed_limit)]
        discovery_json = run_json(discover_prefix + ["--discovery-json", str(elf_path)])

        seed_addrs = {parse_int(seed["address"]) for seed in discovery_json["seeds"]}
        confirmed_addrs = {
            parse_int(function["entry"]) for function in discovery_json["functions"]
        }
        seed_hits = len(debug_addrs & seed_addrs)
        confirmed_hits = len(debug_addrs & confirmed_addrs)
        total = len(debug_addrs)
        seed_cov = seed_hits / total if total else 0.0
        confirmed_cov = confirmed_hits / total if total else 0.0
        print(
            f"{row.project}\t{row.role}\t{total}\t{len(seed_addrs)}\t"
            f"{len(confirmed_addrs)}\t{seed_hits}\t{confirmed_hits}\t"
            f"{seed_cov:.4f}\t{confirmed_cov:.4f}\t"
            f"{sample_missing(debug_addrs, confirmed_addrs, debug_by_addr)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
