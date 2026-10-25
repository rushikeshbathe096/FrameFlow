#!/usr/bin/env bash
# Runs every experiment in experiments/e*.yaml with the release build, then analyze.py.
# Multi-hour on the reference machine (D11): the developer runs it; numbers in docs/RESULTS.md
# come only from the files it produces.
#
# Usage: scripts/run_all.sh [--dry-run] [--out DIR] [--binary PATH] [--only NAME]...
#   --dry-run      print the commands, run nothing (each bench prints its run count)
#   --out DIR      parent folder for the experiment folders (default: results)
#   --binary PATH  frameflow binary (default: build/release/frameflow)
#   --only NAME    run only experiments/NAME.yaml (repeatable), e.g. --only e2_sustained
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dry_run=0
out="results"
binary="$repo/build/release/frameflow"
only=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) dry_run=1; shift ;;
    --out) out="$2"; shift 2 ;;
    --binary) binary="$2"; shift 2 ;;
    --only) only+=("$2"); shift 2 ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) echo "run_all.sh: error: unknown option: $1 (hint: see --help)" >&2; exit 2 ;;
  esac
done

files=()
if [[ ${#only[@]} -gt 0 ]]; then
  for name in "${only[@]}"; do files+=("$repo/experiments/$name.yaml"); done
else
  for file in "$repo"/experiments/e*.yaml; do files+=("$file"); done
fi

for file in "${files[@]}"; do
  if [[ ! -f "$file" ]]; then
    echo "run_all.sh: error: no such experiment file: $file" >&2
    exit 2
  fi
done

if [[ $dry_run -eq 0 && ! -x "$binary" ]]; then
  echo "run_all.sh: error: $binary not found (hint: cmake --preset release && cmake --build --preset release)" >&2
  exit 2
fi

run() {
  echo "+ $*"
  if [[ $dry_run -eq 0 ]]; then "$@"; fi
}

run mkdir -p "$out"
for file in "${files[@]}"; do
  run "$binary" bench --config="$file" --out="$out"
done
run python3 "$repo/scripts/analyze.py" "$out"
