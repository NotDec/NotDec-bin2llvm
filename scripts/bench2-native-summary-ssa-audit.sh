#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${NOTDEC_BIN2LLVM_BUILD_DIR:-/tmp/notdec-bin2llvm-build}"
BENCH2_ROOT="${BENCH2_ROOT:-/sn640/NotDec-Exp/Bench2/rootfs}"
MANIFEST="${BENCH2_MANIFEST:-/sn640/NotDec-Exp/Bench2/manifest/benchmark-targets.tsv}"
OUT_DIR="${OUT_DIR:-/tmp/notdec-bin2llvm-bench2-summary-ssa-audit}"
LLVM_BIN="${LLVM_BIN:-/sn640/NotDec/llvm-22.1.0.obj/bin}"
TARGETS=()
MODES=()
DECODE_SEED_LIMIT=""

usage() {
  cat <<'EOF'
usage: bench2-native-summary-ssa-audit.sh --target PROJECT:ROLE [--target PROJECT:ROLE ...]
                                         [--mode old|summary-no-residue|summary-residue ...]
                                         [--decode-seed-limit COUNT]
                                         [--build-dir DIR] [--bench2-root DIR]
                                         [--manifest FILE] [--out-dir DIR]
                                         [--llvm-bin DIR]

Runs register-SSA-focused native LLVM generation for selected Bench2 manifest
targets.  Prototype recovery is disabled; every generated IR is assembled and
verified with the configured LLVM.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  --target)
    TARGETS+=("$2")
    shift 2
    ;;
  --mode)
    MODES+=("$2")
    shift 2
    ;;
  --decode-seed-limit)
    DECODE_SEED_LIMIT="$2"
    shift 2
    ;;
  --build-dir)
    BUILD_DIR="$2"
    shift 2
    ;;
  --bench2-root)
    BENCH2_ROOT="$2"
    shift 2
    ;;
  --manifest)
    MANIFEST="$2"
    shift 2
    ;;
  --out-dir)
    OUT_DIR="$2"
    shift 2
    ;;
  --llvm-bin)
    LLVM_BIN="$2"
    shift 2
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  *)
    usage >&2
    exit 1
    ;;
  esac
done

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  usage >&2
  exit 1
fi
if [[ ${#MODES[@]} -eq 0 ]]; then
  MODES=(summary-no-residue summary-residue)
fi

NATIVE_LLVM="$BUILD_DIR/bin/notdec-native-llvm"
LLVM_AS="$LLVM_BIN/llvm-as"
OPT="$LLVM_BIN/opt"

require_executable() {
  if [[ ! -x "$1" ]]; then
    echo "missing executable: $1" >&2
    exit 1
  fi
}

require_file() {
  if [[ ! -f "$1" ]]; then
    echo "missing file: $1" >&2
    exit 1
  fi
}

target_id() {
  printf '%s' "$1" | tr ':/' '--'
}

manifest_row() {
  local wanted="$1"
  awk -F '\t' -v wanted="$wanted" '
    NR == 1 { next }
    ($1 ":" $2) == wanted {
      print
      found = 1
      exit
    }
    END {
      if (!found) {
        exit 1
      }
    }
  ' "$MANIFEST"
}

mode_args() {
  local mode="$1"
  case "$mode" in
  old)
    printf '%s\n' "--no-prototype-recovery-pass"
    ;;
  summary-no-residue)
    printf '%s\n' "--summary-register-ssa-pass"
    printf '%s\n' "--no-summary-register-residue-removal"
    printf '%s\n' "--register-ssa-summary"
    printf '%s\n' "--no-prototype-recovery-pass"
    ;;
  summary-residue)
    printf '%s\n' "--summary-register-ssa-pass"
    printf '%s\n' "--register-ssa-summary"
    printf '%s\n' "--no-prototype-recovery-pass"
    ;;
  *)
    echo "unknown mode: $mode" >&2
    exit 1
    ;;
  esac
}

summary_metric() {
  local file="$1"
  local key="$2"
  python3 - "$file" "$key" <<'PY'
import re
import sys

path, key = sys.argv[1:]
pattern = re.compile(rf"(?:^| ){re.escape(key)}=([0-9]+)(?: |$)")
with open(path, "r", encoding="utf-8") as handle:
    for line in handle:
        match = pattern.search(line)
        if match:
            print(match.group(1))
            raise SystemExit(0)
print("")
PY
}

require_executable "$NATIVE_LLVM"
require_executable "$LLVM_AS"
require_executable "$OPT"
require_file "$MANIFEST"

mkdir -p "$OUT_DIR"
echo "out_dir=$OUT_DIR"

METRICS="$OUT_DIR/metrics.tsv"
printf 'target\tproject\trole\trootfs_path\tdecode_seed_limit\tmode\tseconds\tlines\tloads_replaced\tdead_loads_removed\tdead_stores_removed\tphis_created\tphis_simplified\n' \
  >"$METRICS"

for target in "${TARGETS[@]}"; do
  row="$(manifest_row "$target")" || {
    echo "missing manifest target: $target" >&2
    exit 1
  }
  IFS=$'\t' read -r project role rootfs_path _debug_build_id _debug_path _file_type <<<"$row"
  binary="$BENCH2_ROOT/${rootfs_path#/}"
  require_file "$binary"
  name="$(target_id "$target")"

  for mode in "${MODES[@]}"; do
    prefix="$OUT_DIR/$name.$mode"
    ll="$prefix.ll"
    bc="$prefix.bc"
    opt_bc="$prefix.opt.bc"
    stdout="$prefix.stdout"
    stderr="$prefix.stderr"
    llvm_as_stderr="$prefix.llvm-as.stderr"
    opt_stderr="$prefix.opt.stderr"

    native_llvm_args=(--all-confirmed)
    if [[ -n "$DECODE_SEED_LIMIT" ]]; then
      native_llvm_args+=(--decode-seed-limit "$DECODE_SEED_LIMIT")
    fi
    while IFS= read -r arg; do
      native_llvm_args+=("$arg")
    done < <(mode_args "$mode")

    started_at="$(date +%s)"
    "$NATIVE_LLVM" "$binary" "${native_llvm_args[@]}" -o "$ll" \
      >"$stdout" 2>"$stderr"
    seconds="$(( $(date +%s) - started_at ))"
    "$LLVM_AS" "$ll" -o "$bc" 2>"$llvm_as_stderr"
    "$OPT" -passes=verify "$bc" -o "$opt_bc" 2>"$opt_stderr"

    lines="$(wc -l <"$ll")"
    loads_replaced="$(summary_metric "$stderr" "loads_replaced")"
    dead_loads_removed="$(summary_metric "$stderr" "dead_loads_removed")"
    dead_stores_removed="$(summary_metric "$stderr" "dead_stores_removed")"
    phis_created="$(summary_metric "$stderr" "phis_created")"
    phis_simplified="$(summary_metric "$stderr" "phis_simplified")"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$target" "$project" "$role" "$rootfs_path" \
      "${DECODE_SEED_LIMIT:-all}" "$mode" "$seconds" "$lines" \
      "$loads_replaced" "$dead_loads_removed" "$dead_stores_removed" \
      "$phis_created" "$phis_simplified" >>"$METRICS"

    echo "$target mode=$mode ok seconds=${seconds}s lines=$lines"
  done
done
