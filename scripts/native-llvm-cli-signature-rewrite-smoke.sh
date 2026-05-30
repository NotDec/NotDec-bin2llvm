#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: native-llvm-cli-signature-rewrite-smoke.sh NOTDEC_NATIVE_LLVM FIXTURE_LL BUILD_DIR LLVM_BIN
EOF
}

if [[ $# -ne 4 ]]; then
  usage >&2
  exit 1
fi

NATIVE_LLVM="$1"
FIXTURE_LL="$2"
BUILD_DIR="$3"
LLVM_BIN="$4"

OUT_LL="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite.ll"
SUMMARY_TXT="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-summary.txt"
OUT_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite.bc"
VERIFY_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite.opt.bc"

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

require_contains() {
  local needle="$1"
  local file="$2"
  if ! grep -Fq "$needle" "$file"; then
    echo "missing expected text in $file: $needle" >&2
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

require_executable "$NATIVE_LLVM"
require_executable "$LLVM_BIN/llvm-as"
require_executable "$LLVM_BIN/opt"
require_file "$FIXTURE_LL"
mkdir -p "$BUILD_DIR"

"$NATIVE_LLVM" "$FIXTURE_LL" \
  --no-instcombine-pass \
  --no-register-ssa-pass \
  --prototype-recovery-summary \
  --rewrite-prototype-signatures \
  -o "$OUT_LL" \
  2> "$SUMMARY_TXT"

require_contains "define void @cli_input_rdi(i64 %" "$OUT_LL"
require_contains "define i64 @cli_return_rax()" "$OUT_LL"
require_contains "define i64 @cli_input_rdi_return_rax(i64 %" "$OUT_LL"
require_contains "define i64 @cli_input_rdi_rsi_return_rax(i64 %" "$OUT_LL"
require_contains ", i64 %" "$OUT_LL"
require_contains "define { i64, i64 } @cli_return_rax_rdx()" "$OUT_LL"
require_contains "define { i64, i64 } @cli_input_rdi_return_rax_rdx(i64 %" "$OUT_LL"
require_contains "define { i64, i64 } @cli_input_rdi_rsi_return_rax_rdx(i64 %" "$OUT_LL"

require_not_contains "ptr @RAX" "$OUT_LL"
require_not_contains "ptr @RDX" "$OUT_LL"
require_not_contains "notdec.register.access" "$OUT_LL"
require_not_contains "notdec.register.external_inputs" "$OUT_LL"
require_not_contains "notdec.prototype.input_candidates" "$OUT_LL"
require_not_contains "notdec.prototype.return_candidates" "$OUT_LL"

require_contains "signature rewrite seen functions: 7" "$SUMMARY_TXT"
require_contains "signature rewrite rewritten functions: 7" "$SUMMARY_TXT"
require_contains "signature rewrite skipped functions: 0" "$SUMMARY_TXT"

"$LLVM_BIN/llvm-as" "$OUT_LL" -o "$OUT_BC"
"$LLVM_BIN/opt" -passes=verify "$OUT_BC" -o "$VERIFY_BC"
