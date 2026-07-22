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
OUT_STDERR="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite.stderr"
OUT_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite.bc"
VERIFY_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite.opt.bc"
RERUN_LL="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-rerun.ll"
RERUN_STDERR="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-rerun.stderr"
RERUN_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-rerun.bc"
RERUN_VERIFY_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-rerun.opt.bc"
FIXTURE_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-input.bc"
OUT_LL_FROM_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-from-bc.ll"
OUT_STDERR_FROM_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-from-bc.stderr"
OUT_BC_FROM_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-from-bc.bc"
VERIFY_BC_FROM_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-from-bc.opt.bc"
RERUN_LL_FROM_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-from-bc-rerun.ll"
RERUN_STDERR_FROM_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-from-bc-rerun.stderr"
RERUN_BC_FROM_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-from-bc-rerun.bc"
RERUN_VERIFY_BC_FROM_BC="$BUILD_DIR/notdec-native-llvm-cli-signature-rewrite-from-bc-rerun.opt.bc"

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

require_empty() {
  local file="$1"
  if [[ -s "$file" ]]; then
    echo "expected empty file: $file" >&2
    sed -n '1,80p' "$file" >&2
    exit 1
  fi
}

require_executable "$NATIVE_LLVM"
require_executable "$LLVM_BIN/llvm-as"
require_executable "$LLVM_BIN/opt"
require_file "$FIXTURE_LL"
mkdir -p "$BUILD_DIR"

run_default_summary_rewrite_check() {
  local input_ir="$1"
  local out_ll="$2"
  local stderr_txt="$3"
  local out_bc="$4"
  local verify_bc="$5"

  "$NATIVE_LLVM" "$input_ir" \
    --no-instcombine-pass \
    --rewrite-prototype-signatures \
    -o "$out_ll" \
    2> "$stderr_txt"

  require_contains "define void @cli_empty_recovered()" "$out_ll"
  require_contains "define void @cli_input_rdi(i64 %" "$out_ll"
  require_contains "define i64 @cli_return_rax()" "$out_ll"
  require_contains "define i64 @cli_input_rdi_return_rax(i64 %" "$out_ll"
  require_contains "define i64 @cli_input_rdi_rsi_return_rax(i64 %" "$out_ll"
  require_contains ", i64 %" "$out_ll"
  require_contains "define i64 @cli_return_rax_rdx()" "$out_ll"
  require_contains "define i64 @cli_input_rdi_return_rax_rdx(i64 %" "$out_ll"
  require_contains "define i64 @cli_input_rdi_rsi_return_rax_rdx(i64 %" "$out_ll"

  require_not_contains "@RDI =" "$out_ll"
  require_not_contains "@RSI =" "$out_ll"
  require_not_contains "@RAX =" "$out_ll"
  require_not_contains "@RDX =" "$out_ll"
  require_not_contains "ptr @RDI" "$out_ll"
  require_not_contains "ptr @RSI" "$out_ll"
  require_not_contains "ptr @RAX" "$out_ll"
  require_not_contains "ptr @RDX" "$out_ll"
  require_not_contains "notdec.register.access" "$out_ll"
  require_not_contains "notdec.register.summary_return" "$out_ll"
  require_not_contains "notdec.register.summary_clobber" "$out_ll"
  require_not_contains "notdec.unknown" "$out_ll"
  require_not_contains "notdec.prototype.recovered" "$out_ll"
  require_empty "$stderr_txt"

  "$LLVM_BIN/llvm-as" "$out_ll" -o "$out_bc"
  "$LLVM_BIN/opt" -passes=verify "$out_bc" -o "$verify_bc"
}

run_default_summary_rewrite_check "$FIXTURE_LL" "$OUT_LL" "$OUT_STDERR" \
  "$OUT_BC" "$VERIFY_BC"
run_default_summary_rewrite_check "$OUT_LL" "$RERUN_LL" "$RERUN_STDERR" \
  "$RERUN_BC" "$RERUN_VERIFY_BC"

"$LLVM_BIN/llvm-as" "$FIXTURE_LL" -o "$FIXTURE_BC"
run_default_summary_rewrite_check "$FIXTURE_BC" "$OUT_LL_FROM_BC" \
  "$OUT_STDERR_FROM_BC" "$OUT_BC_FROM_BC" "$VERIFY_BC_FROM_BC"
run_default_summary_rewrite_check "$OUT_LL_FROM_BC" "$RERUN_LL_FROM_BC" \
  "$RERUN_STDERR_FROM_BC" "$RERUN_BC_FROM_BC" \
  "$RERUN_VERIFY_BC_FROM_BC"
