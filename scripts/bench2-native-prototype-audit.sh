#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${NOTDEC_BIN2LLVM_BUILD_DIR:-/tmp/notdec-bin2llvm-build}"
BENCH2_ROOT="${BENCH2_ROOT:-/sn640/NotDec-Exp/Bench2/rootfs}"
MANIFEST="${BENCH2_MANIFEST:-/sn640/NotDec-Exp/Bench2/manifest/benchmark-targets.tsv}"
OUT_DIR="${OUT_DIR:-/tmp/notdec-bin2llvm-bench2-prototype-audit}"
LLVM_BIN="${LLVM_BIN:-/sn640/NotDec/llvm-22.1.0.obj/bin}"
TARGETS=()
ALLOWED_SKIP_REASONS=("already matches" "declaration")
DECODE_SEED_LIMIT=""

usage() {
  cat <<'EOF'
usage: bench2-native-prototype-audit.sh --target PROJECT:ROLE [--target PROJECT:ROLE ...]
                                      [--build-dir DIR] [--bench2-root DIR]
                                      [--manifest FILE] [--out-dir DIR]
                                      [--llvm-bin DIR]
                                      [--decode-seed-limit COUNT]
                                      [--allow-skip-reason REASON]

Runs the native prototype recovery audit for selected Bench2 manifest targets.
The default skip reason allowlist is: already matches, declaration.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  --target)
    TARGETS+=("$2")
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
  --decode-seed-limit)
    DECODE_SEED_LIMIT="$2"
    shift 2
    ;;
  --allow-skip-reason)
    ALLOWED_SKIP_REASONS+=("$2")
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

DISCOVER="$BUILD_DIR/bin/notdec-native-discover"
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

summary_metric() {
  local file="$1"
  local key="$2"
  python3 - "$file" "$key" <<'PY'
import json
import sys

path, key = sys.argv[1:]
with open(path, "r", encoding="utf-8") as handle:
    value = json.load(handle)
for part in key.split("."):
    value = value[part]
print(value)
PY
}

parse_prototype_metric() {
  local file="$1"
  local label="$2"
  sed -n "s/[[:space:]]*$label:[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p" "$file" |
    head -n 1
}

require_metric() {
  local target="$1"
  local value="$2"
  local label="$3"
  local file="$4"
  if [[ -z "$value" ]]; then
    echo "$target: missing metric '$label' in $file" >&2
    exit 1
  fi
}

check_skip_reasons() {
  local target="$1"
  local file="$2"
  python3 - "$target" "$file" "${ALLOWED_SKIP_REASONS[@]}" <<'PY'
import re
import sys

target = sys.argv[1]
path = sys.argv[2]
allowed = set(sys.argv[3:])
pattern = re.compile(r"signature rewrite skipped reason (.*): ([0-9]+)")
bad = []
with open(path, "r", encoding="utf-8") as handle:
    for line in handle:
        match = pattern.search(line)
        if match and match.group(1) not in allowed:
            bad.append((match.group(1), int(match.group(2))))
if bad:
    rendered = ", ".join(f"{reason}={count}" for reason, count in bad)
    raise SystemExit(f"{target}: unexpected signature rewrite skip reasons: {rendered}")
PY
}

require_executable "$DISCOVER"
require_executable "$NATIVE_LLVM"
require_executable "$LLVM_AS"
require_executable "$OPT"
require_file "$MANIFEST"

mkdir -p "$OUT_DIR"
echo "out_dir=$OUT_DIR"

METRICS="$OUT_DIR/metrics.tsv"
printf 'target\tproject\trole\trootfs_path\tdecode_seed_limit\tdiscover_seconds\tall_confirmed_seconds\tsignature_rewrite_seconds\tconfirmed_functions\tbasic_blocks\tinstructions\tunresolved_indirect_call\tunresolved_indirect_branch\tprototype_functions\tprototype_external_inputs\tprototype_input_candidates\tprototype_return_candidates\tsignature_rewrite_needed\tsignature_rewrite_seen\tsignature_rewrite_rewritten\tsignature_rewrite_skipped\n' \
  >"$METRICS"

native_llvm_args=()
if [[ -n "$DECODE_SEED_LIMIT" ]]; then
  native_llvm_args+=(--decode-seed-limit "$DECODE_SEED_LIMIT")
fi

for target in "${TARGETS[@]}"; do
  row="$(manifest_row "$target")" || {
    echo "missing manifest target: $target" >&2
    exit 1
  }
  IFS=$'\t' read -r project role rootfs_path _debug_build_id _debug_path _file_type <<<"$row"
  name="$(target_id "$target")"
  binary="$BENCH2_ROOT/${rootfs_path#/}"
  require_file "$binary"

  summary="$OUT_DIR/$name.summary.json"
  ll="$OUT_DIR/$name.all-confirmed.ll"
  bc="$OUT_DIR/$name.all-confirmed.bc"
  opt_bc="$OUT_DIR/$name.all-confirmed.opt.bc"
  native_stderr="$OUT_DIR/$name.native-llvm.stderr"
  native_stdout="$OUT_DIR/$name.native-llvm.stdout"
  llvm_as_stderr="$OUT_DIR/$name.llvm-as.stderr"
  opt_stderr="$OUT_DIR/$name.opt.stderr"
  rewrite_ll="$OUT_DIR/$name.signature-rewrite.ll"
  rewrite_bc="$OUT_DIR/$name.signature-rewrite.bc"
  rewrite_opt_bc="$OUT_DIR/$name.signature-rewrite.opt.bc"
  rewrite_stderr="$OUT_DIR/$name.signature-rewrite.native-llvm.stderr"
  rewrite_stdout="$OUT_DIR/$name.signature-rewrite.native-llvm.stdout"
  rewrite_llvm_as_stderr="$OUT_DIR/$name.signature-rewrite.llvm-as.stderr"
  rewrite_opt_stderr="$OUT_DIR/$name.signature-rewrite.opt.stderr"

  started_at="$(date +%s)"
  "$DISCOVER" --summary-json "$binary" >"$summary"
  discover_seconds="$(( $(date +%s) - started_at ))"

  started_at="$(date +%s)"
  "$NATIVE_LLVM" "$binary" --all-confirmed "${native_llvm_args[@]}" \
    --prototype-recovery-summary \
    -o "$ll" >"$native_stdout" 2>"$native_stderr"
  all_confirmed_seconds="$(( $(date +%s) - started_at ))"
  "$LLVM_AS" "$ll" -o "$bc" 2>"$llvm_as_stderr"
  "$OPT" -passes=verify "$bc" -o "$opt_bc" 2>"$opt_stderr"

  started_at="$(date +%s)"
  "$NATIVE_LLVM" "$binary" --all-confirmed "${native_llvm_args[@]}" \
    --prototype-recovery-summary --rewrite-prototype-signatures -o "$rewrite_ll" \
    >"$rewrite_stdout" 2>"$rewrite_stderr"
  signature_rewrite_seconds="$(( $(date +%s) - started_at ))"
  "$LLVM_AS" "$rewrite_ll" -o "$rewrite_bc" 2>"$rewrite_llvm_as_stderr"
  "$OPT" -passes=verify "$rewrite_bc" -o "$rewrite_opt_bc" \
    2>"$rewrite_opt_stderr"
  check_skip_reasons "$target" "$rewrite_stderr"

  confirmed_functions="$(summary_metric "$summary" "confirmed_functions")"
  basic_blocks="$(summary_metric "$summary" "basic_blocks")"
  instructions="$(summary_metric "$summary" "instructions")"
  unresolved_indirect_call="$(summary_metric "$summary" "unresolved_indirect_flows.indirect call")"
  unresolved_indirect_branch="$(summary_metric "$summary" "unresolved_indirect_flows.indirect branch")"
  prototype_functions="$(parse_prototype_metric "$native_stderr" "functions")"
  prototype_external_inputs="$(parse_prototype_metric "$native_stderr" "external inputs")"
  prototype_input_candidates="$(parse_prototype_metric "$native_stderr" "input candidates")"
  prototype_return_candidates="$(parse_prototype_metric "$native_stderr" "return candidates")"
  signature_rewrite_needed="$(parse_prototype_metric "$rewrite_stderr" "signature rewrite needed functions")"
  signature_rewrite_seen="$(parse_prototype_metric "$rewrite_stderr" "signature rewrite seen functions")"
  signature_rewrite_rewritten="$(parse_prototype_metric "$rewrite_stderr" "signature rewrite rewritten functions")"
  signature_rewrite_skipped="$(parse_prototype_metric "$rewrite_stderr" "signature rewrite skipped functions")"

  require_metric "$target" "$prototype_functions" "functions" "$native_stderr"
  require_metric "$target" "$signature_rewrite_needed" "signature rewrite needed functions" "$rewrite_stderr"
  require_metric "$target" "$signature_rewrite_rewritten" "signature rewrite rewritten functions" "$rewrite_stderr"

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$target" "$project" "$role" "$rootfs_path" \
    "${DECODE_SEED_LIMIT:-all}" \
    "$discover_seconds" "$all_confirmed_seconds" "$signature_rewrite_seconds" \
    "$confirmed_functions" "$basic_blocks" "$instructions" \
    "$unresolved_indirect_call" "$unresolved_indirect_branch" \
    "$prototype_functions" "$prototype_external_inputs" \
    "$prototype_input_candidates" "$prototype_return_candidates" \
    "$signature_rewrite_needed" "$signature_rewrite_seen" \
    "$signature_rewrite_rewritten" "$signature_rewrite_skipped" >>"$METRICS"

  echo "$target ok all_confirmed=${all_confirmed_seconds}s signature_rewrite=${signature_rewrite_seconds}s"
done
