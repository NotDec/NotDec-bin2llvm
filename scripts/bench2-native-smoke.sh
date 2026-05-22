#!/usr/bin/env bash
set -euo pipefail

# Keep the Bench2 smoke entry explicit.  The native path is still changing, so
# this script checks only stable facts: discovery finds functions and LLVM 22 can
# assemble and verify the generated IR.
BUILD_DIR="${NOTDEC_BIN2LLVM_BUILD_DIR:-/tmp/notdec-bin2llvm-build}"
BENCH2_ROOT="${BENCH2_ROOT:-/sn640/NotDec-Exp/Bench2/rootfs}"
BENCH2_IR_ROOT="${BENCH2_IR_ROOT:-/sn640/NotDec-Exp/Bench2/bin2llvm-ir}"
OUT_DIR="${OUT_DIR:-/tmp/notdec-bin2llvm-bench2-smoke}"
LLVM_BIN="${LLVM_BIN:-/sn640/NotDec/llvm-22.1.0.obj/bin}"

usage() {
  cat <<'EOF'
usage: bench2-native-smoke.sh [--build-dir DIR] [--bench2-root DIR]
                              [--bench2-ir-root DIR] [--out-dir DIR]
                              [--llvm-bin DIR]
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  --build-dir)
    BUILD_DIR="$2"
    shift 2
    ;;
  --bench2-root)
    BENCH2_ROOT="$2"
    shift 2
    ;;
  --bench2-ir-root)
    BENCH2_IR_ROOT="$2"
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

DISCOVER="$BUILD_DIR/bin/notdec-native-discover"
NATIVE_LLVM="$BUILD_DIR/bin/notdec-native-llvm"
HERITAGE_CHECK="$BUILD_DIR/bin/notdec-heritage-module-check"
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
    echo "missing Bench2 target: $1" >&2
    exit 1
  fi
}

require_no_unresolved_indirect_calls() {
  local file="$1"
  local name="$2"
  if grep -Eq '"indirect call":[[:space:]]*[1-9]' "$file"; then
    echo "$name: unresolved indirect calls remain in $file" >&2
    exit 1
  fi
}

require_unresolved_indirect_branches_at_most() {
  local file="$1"
  local name="$2"
  local max_count="$3"
  local count
  count="$(sed -n 's/.*"indirect branch":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$file")"
  if [[ -z "$count" ]]; then
    echo "$name: missing unresolved indirect branch count in $file" >&2
    exit 1
  fi
  if ((count > max_count)); then
    echo "$name: unresolved indirect branches $count > $max_count in $file" >&2
    exit 1
  fi
}

summary_source_count() {
  local file="$1"
  local source="$2"
  sed -n "s/.*\"$source\":[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p" "$file" |
    head -n 1
}

require_summary_source() {
  local file="$1"
  local name="$2"
  local source="$3"
  local count
  count="$(summary_source_count "$file" "$source")"
  if [[ -z "$count" || "$count" == "0" ]]; then
    echo "$name: missing function seed source $source in $file" >&2
    exit 1
  fi
}

forbid_summary_source() {
  local file="$1"
  local name="$2"
  local source="$3"
  local count
  count="$(summary_source_count "$file" "$source")"
  if [[ -n "$count" && "$count" != "0" ]]; then
    echo "$name: unexpected function seed source $source=$count in $file" >&2
    exit 1
  fi
}

require_ir_pattern() {
  local file="$1"
  local pattern="$2"
  local description="$3"
  if ! grep -Fq "$pattern" "$file"; then
    echo "missing IR pattern for $description: $pattern" >&2
    echo "  file: $file" >&2
    exit 1
  fi
}

forbid_ir_pattern() {
  local file="$1"
  local pattern="$2"
  local description="$3"
  if grep -Fq "$pattern" "$file"; then
    echo "unexpected IR pattern for $description: $pattern" >&2
    echo "  file: $file" >&2
    exit 1
  fi
}

forbid_ir_regex() {
  local file="$1"
  local pattern="$2"
  local description="$3"
  if grep -Eq "$pattern" "$file"; then
    echo "unexpected IR pattern for $description: $pattern" >&2
    echo "  file: $file" >&2
    exit 1
  fi
}

summary_number_first() {
  local file="$1"
  local key="$2"
  sed -n "s/.*\"$key\":[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p" "$file" |
    head -n 1
}

summary_number_last() {
  local file="$1"
  local key="$2"
  sed -n "s/.*\"$key\":[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p" "$file" |
    tail -n 1
}

require_summary_number() {
  local value="$1"
  local file="$2"
  local key="$3"
  if [[ -z "$value" ]]; then
    echo "missing summary metric $key in $file" >&2
    exit 1
  fi
}

check_summary_confidence() {
  local name="$1"
  local summary="$2"
  local seeds="$3"
  local high="$4"
  local medium="$5"
  local low="$6"

  require_summary_number "$seeds" "$summary" "function_seeds"
  require_summary_number "$high" "$summary" "confidence.high"
  require_summary_number "$medium" "$summary" "confidence.medium"
  require_summary_number "$low" "$summary" "confidence.low"

  # Confidence buckets are the smoke-visible partition of all function seeds.
  if ((high + medium + low != seeds)); then
    echo "$name: confidence counts do not sum to function_seeds in $summary" >&2
    echo "  function_seeds=$seeds high=$high medium=$medium low=$low" >&2
    exit 1
  fi
}

check_block_cfg() {
  local name="$1"
  local summary="$2"
  local blocks="$3"

  python3 - "$name" "$summary" "$blocks" <<'PY'
import json
import sys

name, summary_path, blocks_path = sys.argv[1:]
with open(summary_path, "r", encoding="utf-8") as handle:
    summary = json.load(handle)
with open(blocks_path, "r", encoding="utf-8") as handle:
    blocks_root = json.load(handle)

blocks = blocks_root.get("blocks", [])
if blocks_root.get("count") != len(blocks):
    raise SystemExit(
        f"{name}: blocks-json count {blocks_root.get('count')} != {len(blocks)}"
    )
if summary.get("basic_blocks") != len(blocks):
    raise SystemExit(
        f"{name}: summary basic_blocks {summary.get('basic_blocks')} != {len(blocks)}"
    )

by_function = {}
for block in blocks:
    by_function.setdefault(block["function_entry"], []).append(block)

for function_entry, function_blocks in by_function.items():
    intervals = [
        (int(block["start"], 16), int(block["end"], 16), block)
        for block in function_blocks
    ]
    for index, (start, end, block) in enumerate(intervals):
        if start >= end:
            raise SystemExit(f"{name}: invalid block range {block}")
        for other_start, other_end, other in intervals[index + 1 :]:
            if max(start, other_start) < min(end, other_end):
                raise SystemExit(
                    f"{name}: overlapping blocks in {function_entry}: {block} {other}"
                )

    for block in function_blocks:
        for successor_text in block.get("successors", []):
            successor = int(successor_text, 16)
            for start, end, owner in intervals:
                if start < successor < end:
                    raise SystemExit(
                        f"{name}: successor {successor_text} from {block['start']} "
                        f"points inside block {owner['start']}..{owner['end']}"
                    )
PY
}

check_seed_boundaries() {
  local name="$1"
  local seeds="$2"
  local blocks="$3"

  python3 - "$name" "$seeds" "$blocks" <<'PY'
import json
import sys

name, seeds_path, blocks_path = sys.argv[1:]
with open(seeds_path, "r", encoding="utf-8") as handle:
    seeds_root = json.load(handle)
with open(blocks_path, "r", encoding="utf-8") as handle:
    blocks_root = json.load(handle)

seeds = seeds_root.get("seeds", [])
if seeds_root.get("count") != len(seeds):
    raise SystemExit(
        f"{name}: seeds-json count {seeds_root.get('count')} != {len(seeds)}"
    )

seed_addresses = sorted(
    int(seed["address"], 16)
    for seed in seeds
    if seed.get("confidence") != "low"
)
for block in blocks_root.get("blocks", []):
    function_entry = int(block["function_entry"], 16)
    start = int(block["start"], 16)
    end = int(block["end"], 16)
    for seed_address in seed_addresses:
        if seed_address <= start:
            continue
        if seed_address >= end:
            break
        if seed_address != function_entry:
            raise SystemExit(
                f"{name}: block {block['start']}..{block['end']} in "
                f"{block['function_entry']} covers seed {seed_address:#x}"
            )
PY
}

check_xref_sources() {
  local name="$1"
  local xrefs="$2"

  python3 - "$name" "$xrefs" <<'PY'
import collections
import json
import sys

name, xrefs_path = sys.argv[1:]
with open(xrefs_path, "r", encoding="utf-8") as handle:
    root = json.load(handle)

xrefs = root.get("xrefs", [])
if root.get("count") != len(xrefs):
    raise SystemExit(
        f"{name}: xrefs-json count {root.get('count')} != {len(xrefs)}"
    )

counts = collections.Counter((xref.get("kind"), xref.get("source")) for xref in xrefs)
required = [
    ("flow", "elf-relocation-code"),
    ("data", "elf-relocation-pointer"),
]
if name in {"vsftpd", "memcached"}:
    required.append(("string", "elf-relocation-string"))

for key in required:
    if counts[key] == 0:
        kind, source = key
        raise SystemExit(f"{name}: missing xref source {kind}/{source}")
PY
}

parse_heritage_metric() {
  local file="$1"
  local label="$2"
  sed -n "s/[[:space:]]*$label:[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p" "$file" |
    head -n 1
}

require_heritage_metric() {
  local value="$1"
  local file="$2"
  local label="$3"
  if [[ -z "$value" ]]; then
    echo "missing heritage metric $label in $file" >&2
    exit 1
  fi
}

check_ir_features() {
  local name="$1"
  local ll="$2"

  case "$name" in
  vsftpd)
    require_ir_pattern "$ll" "call void @__cxa_finalize()" \
      "$name PLT.GOT external direct call"
    require_ir_pattern "$ll" "call void @notdec_native_8290()" \
      "$name internal direct call"
    require_ir_pattern "$ll" "call void @__gmon_start__()" \
      "$name GOT external indirect call"
    require_ir_pattern "$ll" "call void @_ITM_deregisterTMCloneTable()" \
      "$name GOT external indirect tail jump"
    require_ir_pattern "$ll" "call void @notdec_plt0_resolver()" \
      "$name PLT0 resolver tail jump"
    require_ir_pattern "$ll" "call void @getegid()" \
      "$name PLT GOT indirect branch"
    require_ir_pattern "$ll" "call void @SSL_get_error()" \
      "$name PLT GOT indirect branch"
    ;;
  libuv)
    require_ir_pattern "$ll" "call void @notdec_native_9d80()" \
      "$name internal direct call"
    require_ir_pattern "$ll" "call void @__gmon_start__()" \
      "$name GOT external indirect call"
    require_ir_pattern "$ll" "call void @__cxa_finalize()" \
      "$name PLT.GOT external direct call"
    require_ir_pattern "$ll" "call void @pthread_key_delete()" \
      "$name PLT external direct call"
    require_ir_pattern "$ll" "call void @_ITM_deregisterTMCloneTable()" \
      "$name GOT external indirect tail jump"
    ;;
  memcached)
    require_ir_pattern "$ll" "call void @__cxa_finalize()" \
      "$name PLT.GOT external direct call"
    require_ir_pattern "$ll" "call void @notdec_native_b950()" \
      "$name internal direct call"
    require_ir_pattern "$ll" "call void @__gmon_start__()" \
      "$name GOT external indirect call"
    require_ir_pattern "$ll" "call void @_ITM_deregisterTMCloneTable()" \
      "$name GOT external indirect tail jump"
    require_ir_pattern "$ll" "call void @notdec_plt0_resolver()" \
      "$name PLT0 resolver tail jump"
    require_ir_pattern "$ll" "call void @SSL_CTX_use_PrivateKey_file()" \
      "$name PLT GOT indirect branch"
    require_ir_pattern "$ll" "call void @pthread_cond_signal()" \
      "$name PLT GOT indirect branch"
    ;;
  esac

  forbid_ir_pattern "$ll" "__cxa_finalize_1" \
    "$name duplicate external symbol regression"
  forbid_ir_regex "$ll" 'ram_[0-9a-f]+_[0-9]+_in = freeze' \
    "$name direct RAM poison read regression"
  forbid_ir_pattern "$ll" "notdec_exit" \
    "$name anonymous branch exit regression"

  forbid_ir_pattern "$ll" "notdec_pcode_CALL_void" \
    "$name direct call helper regression"
  forbid_ir_pattern "$ll" "notdec_pcode_CALLIND_void" \
    "$name indirect call helper regression"
  forbid_ir_pattern "$ll" "notdec_pcode_CALLOTHER_void" \
    "$name CALLOTHER helper regression"
}

check_entry_sources() {
  local name="$1"
  local summary="$2"

  require_summary_source "$summary" "$name" "dt-init"
  require_summary_source "$summary" "$name" "dt-fini"
  require_summary_source "$summary" "$name" "dt-init-array"
  require_summary_source "$summary" "$name" "dt-fini-array"
  require_summary_source "$summary" "$name" "eh-frame"
  require_summary_source "$summary" "$name" "elf-relocation-code"

  case "$name" in
  libuv)
    forbid_summary_source "$summary" "$name" "elf-entry"
    require_summary_source "$summary" "$name" "elf-dynamic-symbol"
    require_summary_source "$summary" "$name" "elf-symbol"
    ;;
  vsftpd | memcached)
    require_summary_source "$summary" "$name" "elf-entry"
    ;;
  esac
}

require_executable "$DISCOVER"
require_executable "$NATIVE_LLVM"
require_executable "$LLVM_AS"
require_executable "$OPT"

TARGET_NAMES=(vsftpd libuv memcached)
TARGET_PATHS=(
  "$BENCH2_ROOT/usr/sbin/vsftpd"
  "$BENCH2_ROOT/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0"
  "$BENCH2_ROOT/usr/bin/memcached"
)

mkdir -p "$OUT_DIR"
echo "out_dir=$OUT_DIR"
METRICS="$OUT_DIR/metrics.tsv"
printf 'target\telapsed_seconds\tfunction_seeds\tseed_confidence_high\tseed_confidence_medium\tseed_confidence_low\tconfirmed_functions\tbasic_blocks\tinstructions\txrefs_total\txrefs_flow\txrefs_call\txrefs_data\txrefs_string\tunresolved_total\tunresolved_indirect_call\tunresolved_indirect_branch\n' \
  >"$METRICS"
HERITAGE_METRICS="$OUT_DIR/heritage-metrics.tsv"
printf 'target\theritage_available\tfunctions\texternals\tfailures\tdirect_calls\tresolved_internal_calls\tresolved_external_calls\tunknown_calls\n' \
  >"$HERITAGE_METRICS"
COMPARE_METRICS="$OUT_DIR/native-heritage-compare.tsv"
printf 'target\theritage_available\tnative_confirmed_functions\theritage_functions\tnative_call_xrefs\theritage_direct_calls\tnative_unresolved_total\theritage_unknown_calls\n' \
  >"$COMPARE_METRICS"

for index in "${!TARGET_NAMES[@]}"; do
  name="${TARGET_NAMES[$index]}"
  target="${TARGET_PATHS[$index]}"
  require_file "$target"

  summary="$OUT_DIR/$name.summary.json"
  seeds="$OUT_DIR/$name.seeds.json"
  blocks="$OUT_DIR/$name.blocks.json"
  xrefs="$OUT_DIR/$name.xrefs.json"
  discover_stderr="$OUT_DIR/$name.discover.stderr"
  seeds_stderr="$OUT_DIR/$name.seeds.stderr"
  blocks_stderr="$OUT_DIR/$name.blocks.stderr"
  xrefs_stderr="$OUT_DIR/$name.xrefs.stderr"
  ll="$OUT_DIR/$name.all-confirmed.ll"
  bc="$OUT_DIR/$name.all-confirmed.bc"
  opt_bc="$OUT_DIR/$name.all-confirmed.opt.bc"
  native_stdout="$OUT_DIR/$name.native-llvm.stdout"
  native_stderr="$OUT_DIR/$name.native-llvm.stderr"
  llvm_as_stdout="$OUT_DIR/$name.llvm-as.stdout"
  llvm_as_stderr="$OUT_DIR/$name.llvm-as.stderr"
  opt_stdout="$OUT_DIR/$name.opt.stdout"
  opt_stderr="$OUT_DIR/$name.opt.stderr"

  started_at="$(date +%s)"
  "$DISCOVER" --summary-json "$target" >"$summary" 2>"$discover_stderr"
  if ! grep -Eq '"confirmed_functions":[[:space:]]*[1-9]' "$summary"; then
    echo "$name: no confirmed functions in $summary" >&2
    exit 1
  fi
  require_no_unresolved_indirect_calls "$summary" "$name"
  require_unresolved_indirect_branches_at_most "$summary" "$name" 0
  check_entry_sources "$name" "$summary"
  "$DISCOVER" --seeds-json "$target" >"$seeds" 2>"$seeds_stderr"
  "$DISCOVER" --blocks-json "$target" >"$blocks" 2>"$blocks_stderr"
  check_block_cfg "$name" "$summary" "$blocks"
  check_seed_boundaries "$name" "$seeds" "$blocks"
  "$DISCOVER" --xrefs-json "$target" >"$xrefs" 2>"$xrefs_stderr"
  check_xref_sources "$name" "$xrefs"

  "$NATIVE_LLVM" "$target" --all-confirmed -o "$ll" \
    >"$native_stdout" 2>"$native_stderr"
  "$LLVM_AS" "$ll" -o "$bc" >"$llvm_as_stdout" 2>"$llvm_as_stderr"
  "$OPT" -passes=verify "$bc" -o "$opt_bc" >"$opt_stdout" 2>"$opt_stderr"
  check_ir_features "$name" "$ll"

  if [[ "$name" == "libuv" ]]; then
    single_ll="$OUT_DIR/$name.single-function.ll"
    single_bc="$OUT_DIR/$name.single-function.bc"
    single_opt_bc="$OUT_DIR/$name.single-function.opt.bc"
    single_stdout="$OUT_DIR/$name.single-function.native-llvm.stdout"
    single_stderr="$OUT_DIR/$name.single-function.native-llvm.stderr"
    single_llvm_as_stdout="$OUT_DIR/$name.single-function.llvm-as.stdout"
    single_llvm_as_stderr="$OUT_DIR/$name.single-function.llvm-as.stderr"
    single_opt_stdout="$OUT_DIR/$name.single-function.opt.stdout"
    single_opt_stderr="$OUT_DIR/$name.single-function.opt.stderr"

    "$NATIVE_LLVM" "$target" -f 0x9df0 -o "$single_ll" \
      >"$single_stdout" 2>"$single_stderr"
    "$LLVM_AS" "$single_ll" -o "$single_bc" \
      >"$single_llvm_as_stdout" 2>"$single_llvm_as_stderr"
    "$OPT" -passes=verify "$single_bc" -o "$single_opt_bc" \
      >"$single_opt_stdout" 2>"$single_opt_stderr"
    require_ir_pattern "$single_ll" "call void @__cxa_finalize()" \
      "$name single-function PLT.GOT external direct call"
    require_ir_pattern "$single_ll" "call void @notdec_native_9d80()" \
      "$name single-function internal direct call"
    forbid_ir_pattern "$single_ll" "notdec_pcode_CALL_void" \
      "$name single-function direct call helper regression"
    forbid_ir_pattern "$single_ll" "notdec_pcode_CALLIND_void" \
      "$name single-function indirect call helper regression"
    forbid_ir_pattern "$single_ll" "notdec_pcode_CALLOTHER_void" \
      "$name single-function CALLOTHER helper regression"

    named_ll="$OUT_DIR/$name.named-function.ll"
    named_bc="$OUT_DIR/$name.named-function.bc"
    named_opt_bc="$OUT_DIR/$name.named-function.opt.bc"
    named_stdout="$OUT_DIR/$name.named-function.native-llvm.stdout"
    named_stderr="$OUT_DIR/$name.named-function.native-llvm.stderr"
    named_llvm_as_stdout="$OUT_DIR/$name.named-function.llvm-as.stdout"
    named_llvm_as_stderr="$OUT_DIR/$name.named-function.llvm-as.stderr"
    named_opt_stdout="$OUT_DIR/$name.named-function.opt.stdout"
    named_opt_stderr="$OUT_DIR/$name.named-function.opt.stderr"

    "$NATIVE_LLVM" "$target" -n uv_key_delete -o "$named_ll" \
      >"$named_stdout" 2>"$named_stderr"
    "$LLVM_AS" "$named_ll" -o "$named_bc" \
      >"$named_llvm_as_stdout" 2>"$named_llvm_as_stderr"
    "$OPT" -passes=verify "$named_bc" -o "$named_opt_bc" \
      >"$named_opt_stdout" 2>"$named_opt_stderr"
    require_ir_pattern "$named_ll" "call void @pthread_key_delete()" \
      "$name named-function PLT external direct call"
    forbid_ir_pattern "$named_ll" "notdec_pcode_CALL_void" \
      "$name named-function direct call helper regression"
    forbid_ir_pattern "$named_ll" "notdec_pcode_CALLIND_void" \
      "$name named-function indirect call helper regression"
    forbid_ir_pattern "$named_ll" "notdec_pcode_CALLOTHER_void" \
      "$name named-function CALLOTHER helper regression"
  fi
  finished_at="$(date +%s)"
  elapsed_seconds=$((finished_at - started_at))

  function_seeds="$(summary_number_first "$summary" "function_seeds")"
  seed_confidence_high="$(summary_number_first "$summary" "high")"
  seed_confidence_medium="$(summary_number_first "$summary" "medium")"
  seed_confidence_low="$(summary_number_first "$summary" "low")"
  confirmed_functions="$(summary_number_first "$summary" "confirmed_functions")"
  basic_blocks="$(summary_number_first "$summary" "basic_blocks")"
  instructions="$(summary_number_first "$summary" "instructions")"
  xrefs_total="$(summary_number_first "$summary" "total")"
  xrefs_flow="$(summary_number_first "$summary" "flow")"
  xrefs_call="$(summary_number_first "$summary" "call")"
  xrefs_data="$(summary_number_first "$summary" "data")"
  xrefs_string="$(summary_number_first "$summary" "string")"
  unresolved_total="$(summary_number_last "$summary" "total")"
  unresolved_indirect_call="$(summary_number_first "$summary" "indirect call")"
  unresolved_indirect_branch="$(summary_number_first "$summary" "indirect branch")"

  check_summary_confidence "$name" "$summary" "$function_seeds" \
    "$seed_confidence_high" "$seed_confidence_medium" "$seed_confidence_low"
  require_summary_number "$confirmed_functions" "$summary" "confirmed_functions"
  require_summary_number "$basic_blocks" "$summary" "basic_blocks"
  require_summary_number "$instructions" "$summary" "instructions"
  require_summary_number "$xrefs_total" "$summary" "xrefs.total"
  require_summary_number "$xrefs_flow" "$summary" "xrefs.flow"
  require_summary_number "$xrefs_call" "$summary" "xrefs.call"
  require_summary_number "$xrefs_data" "$summary" "xrefs.data"
  require_summary_number "$xrefs_string" "$summary" "xrefs.string"
  require_summary_number "$unresolved_total" "$summary" "unresolved_indirect_flows.total"
  require_summary_number "$unresolved_indirect_call" "$summary" "unresolved_indirect_flows.indirect call"
  require_summary_number "$unresolved_indirect_branch" "$summary" "unresolved_indirect_flows.indirect branch"

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$name" "$elapsed_seconds" "$function_seeds" \
    "$seed_confidence_high" "$seed_confidence_medium" \
    "$seed_confidence_low" "$confirmed_functions" "$basic_blocks" \
    "$instructions" "$xrefs_total" "$xrefs_flow" "$xrefs_call" \
    "$xrefs_data" "$xrefs_string" "$unresolved_total" \
    "$unresolved_indirect_call" "$unresolved_indirect_branch" >>"$METRICS"

  heritage_module="$BENCH2_IR_ROOT/$name/module-limit5.json"
  if [[ -f "$heritage_module" ]]; then
    require_executable "$HERITAGE_CHECK"
    heritage_check_stdout="$OUT_DIR/$name.heritage-module-check.stdout"
    heritage_check_stderr="$OUT_DIR/$name.heritage-module-check.stderr"
    "$HERITAGE_CHECK" "$heritage_module" \
      >"$heritage_check_stdout" 2>"$heritage_check_stderr"

    heritage_functions="$(parse_heritage_metric "$heritage_check_stdout" "functions")"
    heritage_externals="$(parse_heritage_metric "$heritage_check_stdout" "externals")"
    heritage_failures="$(parse_heritage_metric "$heritage_check_stdout" "failures")"
    heritage_direct_calls="$(parse_heritage_metric "$heritage_check_stdout" "direct calls")"
    heritage_resolved_internal="$(parse_heritage_metric "$heritage_check_stdout" "resolved internal calls")"
    heritage_resolved_external="$(parse_heritage_metric "$heritage_check_stdout" "resolved external calls")"
    heritage_unknown_calls="$(parse_heritage_metric "$heritage_check_stdout" "unknown calls")"

    require_heritage_metric "$heritage_functions" "$heritage_check_stdout" "functions"
    require_heritage_metric "$heritage_externals" "$heritage_check_stdout" "externals"
    require_heritage_metric "$heritage_failures" "$heritage_check_stdout" "failures"
    require_heritage_metric "$heritage_direct_calls" "$heritage_check_stdout" "direct calls"
    require_heritage_metric "$heritage_resolved_internal" "$heritage_check_stdout" "resolved internal calls"
    require_heritage_metric "$heritage_resolved_external" "$heritage_check_stdout" "resolved external calls"
    require_heritage_metric "$heritage_unknown_calls" "$heritage_check_stdout" "unknown calls"

    printf '%s\t1\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$name" "$heritage_functions" "$heritage_externals" \
      "$heritage_failures" "$heritage_direct_calls" \
      "$heritage_resolved_internal" "$heritage_resolved_external" \
      "$heritage_unknown_calls" >>"$HERITAGE_METRICS"
    printf '%s\t1\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$name" "$confirmed_functions" "$heritage_functions" "$xrefs_call" \
      "$heritage_direct_calls" "$unresolved_total" \
      "$heritage_unknown_calls" >>"$COMPARE_METRICS"
  else
    printf '%s\t0\t\t\t\t\t\t\t\n' "$name" >>"$HERITAGE_METRICS"
    printf '%s\t0\t%s\t\t%s\t\t%s\t\n' \
      "$name" "$confirmed_functions" "$xrefs_call" "$unresolved_total" \
      >>"$COMPARE_METRICS"
  fi

  echo "$name ok elapsed=${elapsed_seconds}s summary=$summary ll=$ll"
done
