#!/usr/bin/env bash
set -euo pipefail

# Keep the Bench2 smoke entry explicit.  The native path is still changing, so
# this script checks only stable facts: discovery finds functions and LLVM 22 can
# assemble and verify the generated IR.
BUILD_DIR="${NOTDEC_BIN2LLVM_BUILD_DIR:-/tmp/notdec-bin2llvm-build}"
BENCH2_ROOT="${BENCH2_ROOT:-/sn640/NotDec-Exp/Bench2/rootfs}"
OUT_DIR="${OUT_DIR:-/tmp/notdec-bin2llvm-bench2-smoke}"
LLVM_BIN="${LLVM_BIN:-/sn640/NotDec/llvm-22.1.0.obj/bin}"

usage() {
  cat <<'EOF'
usage: bench2-native-smoke.sh [--build-dir DIR] [--bench2-root DIR]
                              [--out-dir DIR] [--llvm-bin DIR]
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

check_ir_features() {
  local name="$1"
  local ll="$2"

  case "$name" in
  vsftpd)
    require_ir_pattern "$ll" "call void @notdec_native_5ba0()" \
      "$name internal direct call"
    require_ir_pattern "$ll" "call void @notdec_native_8290()" \
      "$name internal direct call"
    ;;
  libuv)
    require_ir_pattern "$ll" "call void @notdec_native_9d80()" \
      "$name internal direct call"
    require_ir_pattern "$ll" "call void @__cxa_finalize()" \
      "$name PLT.GOT external direct call"
    require_ir_pattern "$ll" "call void @pthread_key_delete()" \
      "$name PLT external direct call"
    ;;
  memcached)
    require_ir_pattern "$ll" "call void @notdec_native_5b80()" \
      "$name internal direct call"
    require_ir_pattern "$ll" "call void @notdec_native_b950()" \
      "$name internal direct call"
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

for index in "${!TARGET_NAMES[@]}"; do
  name="${TARGET_NAMES[$index]}"
  target="${TARGET_PATHS[$index]}"
  require_file "$target"

  summary="$OUT_DIR/$name.summary.json"
  discover_stderr="$OUT_DIR/$name.discover.stderr"
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

  "$NATIVE_LLVM" "$target" --all-confirmed -o "$ll" \
    >"$native_stdout" 2>"$native_stderr"
  "$LLVM_AS" "$ll" -o "$bc" >"$llvm_as_stdout" 2>"$llvm_as_stderr"
  "$OPT" -passes=verify "$bc" -o "$opt_bc" >"$opt_stdout" 2>"$opt_stderr"
  check_ir_features "$name" "$ll"
  finished_at="$(date +%s)"

  echo "$name ok elapsed=$((finished_at - started_at))s summary=$summary ll=$ll"
done
