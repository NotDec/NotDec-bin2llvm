#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: native-fortune-i386-regression.sh NOTDEC_NATIVE_LLVM LLVM_BIN SOURCE_DIR BUILD_DIR
EOF
}

if [[ $# -ne 4 ]]; then
  usage >&2
  exit 1
fi

NATIVE_LLVM="$1"
LLVM_BIN="$2"
SOURCE_DIR="$3"
BUILD_DIR="$4"

OUT_DIR="${NOTDEC_NATIVE_FORTUNE_I386_OUT_DIR:-$BUILD_DIR/native-fortune-i386-regression}"
PROTO_JSON="$SOURCE_DIR/tests/fixtures/native/fortune-i386.external-prototypes.json"
FETCH_FIXTURE="$SOURCE_DIR/scripts/fetch-native-fixture.py"
RESIDUE_AUDIT="$SOURCE_DIR/scripts/native-register-residue-audit.py"

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

require_not_contains() {
  local needle="$1"
  local file="$2"
  if grep -Fq "$needle" "$file"; then
    echo "unexpected text in $file: $needle" >&2
    exit 1
  fi
}

require_contains() {
  local needle="$1"
  local file="$2"
  if ! grep -Fq "$needle" "$file"; then
    echo "missing expected text in $file: $needle" >&2
    exit 1
  fi
}

require_line_prefix() {
  local prefix="$1"
  local file="$2"
  if ! grep -q "^$prefix" "$file"; then
    echo "missing expected line prefix in $file: $prefix" >&2
    exit 1
  fi
}

require_executable "$NATIVE_LLVM"
require_executable "$LLVM_BIN/llvm-as"
require_executable "$LLVM_BIN/opt"
require_executable "$FETCH_FIXTURE"
require_executable "$RESIDUE_AUDIT"
require_file "$PROTO_JSON"
mkdir -p "$OUT_DIR"

set +e
FORTUNE="$("$FETCH_FIXTURE" fortune-i386 --source-dir "$SOURCE_DIR")"
FETCH_RC=$?
set -e
if [[ "$FETCH_RC" -ne 0 ]]; then
  exit "$FETCH_RC"
fi
require_file "$FORTUNE"

OUT_LL="$OUT_DIR/fortune.native.ll"
OUT_BC="$OUT_DIR/fortune.native.bc"
VERIFY_BC="$OUT_DIR/fortune.verified.bc"
SUMMARY_JSON="$OUT_DIR/summary.json"
WARNING_TSV="$OUT_DIR/register-ssa-warnings.tsv"
RESIDUE_TSV="$OUT_DIR/register-residue-audit.tsv"
STDOUT_TXT="$OUT_DIR/run.stdout"
STDERR_TXT="$OUT_DIR/run.stderr"

set +e
"$NATIVE_LLVM" "$FORTUNE" \
  -o "$OUT_LL" \
  --all-confirmed \
  --skip-runtime \
  --external-prototypes "$PROTO_JSON" \
  --summary-json-out "$SUMMARY_JSON" \
  --register-ssa-warning-out "$WARNING_TSV" \
  --register-ssa-summary \
  > "$STDOUT_TXT" 2> "$STDERR_TXT"
NATIVE_RC=$?
set -e

if [[ "$NATIVE_RC" -ne 0 ]]; then
  sed -n '1,120p' "$STDERR_TXT" >&2
  exit "$NATIVE_RC"
fi

"$LLVM_BIN/llvm-as" "$OUT_LL" -o "$OUT_BC"
"$LLVM_BIN/opt" -passes=verify "$OUT_BC" -o "$VERIFY_BC"
"$RESIDUE_AUDIT" --details "$OUT_LL" > "$RESIDUE_TSV"

require_file "$WARNING_TSV"
require_line_prefix "define " "$OUT_LL"
require_contains "stackpointer.register=ESP" "$OUT_LL"
require_not_contains "stackpointer.register=RSP" "$OUT_LL"

# x87 instructions are folded into library-style notdec.x87.* calls: the FPU
# stack owns no register globals and register SSA must not derive clobber or
# entry values for ST0..ST7.
require_not_contains "@ST" "$OUT_LL"
require_contains "notdec.x87.fild.i32" "$OUT_LL"
require_contains "notdec.x87.fstp.f64" "$OUT_LL"
if grep -P '\tST[0-7]\t' "$WARNING_TSV"; then
  echo "unexpected x87 register SSA warning in $WARNING_TSV" >&2
  exit 1
fi

echo "fortune fixture: $FORTUNE"
echo "fortune native IR: $OUT_LL"
echo "fortune register SSA warnings: $WARNING_TSV"
echo "fortune register residue audit: $RESIDUE_TSV"
