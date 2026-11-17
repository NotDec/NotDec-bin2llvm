#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: native-llvm-config-override-smoke.sh NOTDEC_NATIVE_LLVM LLVM_BIN BUILD_DIR
EOF
}

if [[ $# -ne 3 ]]; then
  usage >&2
  exit 1
fi

NATIVE_LLVM="$1"
LLVM_BIN="$2"
BUILD_DIR="$3"

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
  if ! grep -Fq "$1" "$2"; then
    echo "missing text in $2: $1" >&2
    exit 1
  fi
}

require_executable "$NATIVE_LLVM"
require_executable "$LLVM_BIN/llvm-as"
require_executable "$LLVM_BIN/opt"
mkdir -p "$BUILD_DIR"

WORK_DIR="$(mktemp -d "$BUILD_DIR/native-config-override.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

cat >"$WORK_DIR/input.ll" <<'IR'
define i64 @foo() {
entry:
  ret i64 1
}

define i64 @caller() {
entry:
  %v = call i64 @foo()
  ret i64 %v
}
IR

cat >"$WORK_DIR/foo.ll" <<'IR'
define i64 @foo() {
entry:
  ret i64 42
}
IR

cat >"$WORK_DIR/pipeline.json" <<JSON
{
  "schema_version": 1,
  "pipeline": [
    {"stage": "instcombine", "options": {"early_cse": false}},
    {
      "stage": "ir-override",
      "options": {
        "file": "$WORK_DIR/foo.ll",
        "select": [{"name": "foo"}]
      }
    },
    {"stage": "instcombine", "options": {"early_cse": true}}
  ]
}
JSON

OUT_LL="$WORK_DIR/out.ll"
MANIFEST="$WORK_DIR/manifest.json"
"$NATIVE_LLVM" "$WORK_DIR/input.ll" \
  --config "$WORK_DIR/pipeline.json" \
  --manifest-out "$MANIFEST" \
  --dump-stages "$WORK_DIR/stages" \
  -o "$OUT_LL"

require_file "$OUT_LL"
require_contains "ret i64 42" "$OUT_LL"
require_contains "!notdec.override" "$OUT_LL"
require_contains "!notdec.config" "$OUT_LL"
require_contains "\"role\": \"ir_override\"" "$MANIFEST"
require_file "$WORK_DIR/stages/001-ir-override-after.ll"

"$LLVM_BIN/llvm-as" "$OUT_LL" -o "$WORK_DIR/out.bc"
"$LLVM_BIN/opt" -passes=verify "$WORK_DIR/out.bc" -o "$WORK_DIR/verified.bc"

echo "native-llvm config override smoke passed"
