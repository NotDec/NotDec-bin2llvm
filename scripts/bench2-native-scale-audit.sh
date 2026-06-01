#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${NOTDEC_BIN2LLVM_BUILD_DIR:-/tmp/notdec-bin2llvm-build}"
BENCH2_ROOT="${BENCH2_ROOT:-/sn640/NotDec-Exp/Bench2/rootfs}"
MANIFEST="${BENCH2_MANIFEST:-/sn640/NotDec-Exp/Bench2/manifest/benchmark-targets.tsv}"
OUT_DIR="${OUT_DIR:-/tmp/notdec-bin2llvm-bench2-scale-audit}"
TIMEOUT_SECONDS=180
TARGETS=()
LIMITS=(50 100 200 400)
LIMITS_SET=0

usage() {
  cat <<'EOF'
usage: bench2-native-scale-audit.sh --target PROJECT:ROLE [--target PROJECT:ROLE ...]
                                   [--limit COUNT ...]
                                   [--timeout-seconds SECONDS]
                                   [--build-dir DIR] [--bench2-root DIR]
                                   [--manifest FILE] [--out-dir DIR]

Runs all-confirmed native LLVM generation with register/prototype passes disabled
for selected Bench2 targets and seed limits.  This is for scale diagnosis, not
semantic verification.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  --target)
    TARGETS+=("$2")
    shift 2
    ;;
  --limit)
    if [[ "$LIMITS_SET" -eq 0 ]]; then
      LIMITS=()
      LIMITS_SET=1
    fi
    LIMITS+=("$2")
    shift 2
    ;;
  --timeout-seconds)
    TIMEOUT_SECONDS="$2"
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

NATIVE_LLVM="$BUILD_DIR/bin/notdec-native-llvm"
if [[ ! -x "$NATIVE_LLVM" ]]; then
  echo "missing executable: $NATIVE_LLVM" >&2
  exit 1
fi
if [[ ! -f "$MANIFEST" ]]; then
  echo "missing manifest: $MANIFEST" >&2
  exit 1
fi

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

elapsed_since() {
  local started_at="$1"
  printf '%s' "$(( $(date +%s) - started_at ))"
}

mkdir -p "$OUT_DIR"
echo "out_dir=$OUT_DIR"

METRICS="$OUT_DIR/metrics.tsv"
printf 'target\tproject\trole\trootfs_path\tlimit\ttimeout_seconds\trc\telapsed_seconds\toutput_bytes\tstderr_bytes\n' \
  >"$METRICS"

for target in "${TARGETS[@]}"; do
  row="$(manifest_row "$target")" || {
    echo "missing manifest target: $target" >&2
    exit 1
  }
  IFS=$'\t' read -r project role rootfs_path _debug_build_id _debug_path _file_type <<<"$row"
  binary="$BENCH2_ROOT/${rootfs_path#/}"
  if [[ ! -f "$binary" ]]; then
    echo "missing binary: $binary" >&2
    exit 1
  fi
  name="$(target_id "$target")"

  for limit in "${LIMITS[@]}"; do
    prefix="$OUT_DIR/$name.limit-$limit"
    ll="$prefix.ll"
    stdout="$prefix.stdout"
    stderr="$prefix.stderr"
    rm -f "$ll" "$stdout" "$stderr"

    started_at="$(date +%s)"
    set +e
    timeout "$TIMEOUT_SECONDS"s "$NATIVE_LLVM" "$binary" --all-confirmed \
      --decode-seed-limit "$limit" --no-instcombine-pass \
      --no-register-ssa-pass --no-prototype-recovery-pass -o "$ll" \
      >"$stdout" 2>"$stderr"
    rc="$?"
    set -e
    elapsed_seconds="$(elapsed_since "$started_at")"
    output_bytes="$(wc -c <"$ll" 2>/dev/null || printf '0')"
    stderr_bytes="$(wc -c <"$stderr" 2>/dev/null || printf '0')"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$target" "$project" "$role" "$rootfs_path" "$limit" \
      "$TIMEOUT_SECONDS" "$rc" "$elapsed_seconds" "$output_bytes" \
      "$stderr_bytes" >>"$METRICS"

    echo "$target limit=$limit rc=$rc elapsed=${elapsed_seconds}s bytes=$output_bytes"
  done
done
