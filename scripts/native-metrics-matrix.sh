#!/usr/bin/env bash
# 配置矩阵跑 native 指标：同一个二进制、多份 pipeline 配置，一条命令出对比表。
#
# 用法：
#   native-metrics-matrix.sh TOOL BINARY MATRIX_TSV OUT_DIR [-- extra tool args...]
#
# MATRIX_TSV 每行 <label>\t<config.json 或 ->（- 表示默认配置），空行和 # 注释忽略。
#
# 例：
#   cat > /tmp/hexx64/matrix.tsv <<'EOF'
#   default	-
#   abi	/tmp/hexx64/exp/pipeline-abi-all.json
#   abi-pin	/tmp/hexx64/exp/pipeline-pin-qfree-abi.json
#   EOF
#   native-metrics-matrix.sh build/bin/notdec-native-llvm /sn640/hexx64-dec/hexx64.so \
#     /tmp/hexx64/matrix.tsv /tmp/hexx64/metrics -- --gtirb /tmp/hexx64/hexx64.gtirb
#
# 每个 label 产出 OUT_DIR/<label>.ll 和 OUT_DIR/<label>.json（--metrics-json），
# 最后打印一张对比表；任一配置失败会以非零退出，但已经跑完的配置保留。
set -euo pipefail

usage() {
  cat <<'EOF'
usage: native-metrics-matrix.sh TOOL BINARY MATRIX_TSV OUT_DIR [-- extra tool args...]

MATRIX_TSV lines: <label>TAB<config path or ->
EOF
}

if [[ $# -lt 4 ]]; then
  usage >&2
  exit 1
fi

TOOL="$1"
BINARY="$2"
MATRIX="$3"
OUT_DIR="$4"
shift 4

EXTRA_ARGS=()
if [[ $# -gt 0 ]]; then
  if [[ "$1" != "--" ]]; then
    usage >&2
    exit 1
  fi
  shift
  EXTRA_ARGS=("$@")
fi

[[ -x "$TOOL" ]] || { echo "missing executable: $TOOL" >&2; exit 1; }
[[ -f "$BINARY" ]] || { echo "missing binary: $BINARY" >&2; exit 1; }
[[ -f "$MATRIX" ]] || { echo "missing matrix: $MATRIX" >&2; exit 1; }
mkdir -p "$OUT_DIR"

labels=()
while IFS=$'\t' read -r label config rest || [[ -n "${label:-}" ]]; do
  [[ -z "${label:-}" ]] && continue
  [[ "$label" == \#* ]] && continue
  labels+=("$label")
  config="${config:--}"
  out_ll="$OUT_DIR/$label.ll"
  out_json="$OUT_DIR/$label.json"
  echo "==> $label (config: $config)"
  config_args=()
  if [[ "$config" != "-" ]]; then
    config_args=(--config "$config")
  fi
  set +e
  "$TOOL" "$BINARY" --all-confirmed --register-ssa-summary \
    "${config_args[@]}" "${EXTRA_ARGS[@]}" \
    -o "$out_ll" --metrics-json "$out_json" \
    > "$OUT_DIR/$label.stdout" 2> "$OUT_DIR/$label.stderr"
  rc=$?
  set -e
  if [[ $rc -ne 0 ]]; then
    echo "FAILED: $label (exit $rc), see $OUT_DIR/$label.stderr" >&2
  fi
done < "$MATRIX"

python3 - "$OUT_DIR" "${labels[@]}" <<'PY'
import json, os, sys

out_dir = sys.argv[1]
labels = sys.argv[2:]
rows = []
for label in labels:
    path = os.path.join(out_dir, label + ".json")
    if not os.path.exists(path):
        rows.append((label, None))
        continue
    with open(path) as fh:
        data = json.load(fh)
    ll = os.path.join(out_dir, label + ".ll")
    size = os.path.getsize(ll) if os.path.exists(ll) else 0
    unk = data.get("unknown", {})
    abi = data.get("abi_undefined", {})
    reg = data.get("register_reads", {})
    rows.append((label, {
        "unknown": unk.get("calls", 0),
        "sources": unk.get("sources", 0),
        "abi_undef": abi.get("calls", 0),
        "poison": data.get("poison_operand_instructions", 0),
        "local_reads": reg.get("local", 0),
        "boundary_reads": reg.get("boundary", 0),
        "ir_mb": size / (1024.0 * 1024.0),
        "verify": "ok" if data.get("verify", {}).get("ok") else "FAIL",
        "abi_all": "yes" if data.get("abi_for_all_functions") else "no",
    }))

header = ("label", "unknown", "sources", "abi_undef", "poison", "local_reads",
          "boundary_reads", "ir_mb", "verify", "abi_all")
print()
print("\t".join(header))
for label, data in rows:
    if data is None:
        print("\t".join([label] + ["<missing>"] * (len(header) - 1)))
        continue
    print("\t".join([label,
                     str(data["unknown"]),
                     str(data["sources"]),
                     str(data["abi_undef"]),
                     str(data["poison"]),
                     str(data["local_reads"]),
                     str(data["boundary_reads"]),
                     "%.1f" % data["ir_mb"],
                     data["verify"],
                     data["abi_all"]]))
PY
