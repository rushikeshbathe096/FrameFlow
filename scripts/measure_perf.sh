#!/usr/bin/env bash
# P5.9a: runs every performance measurement with the release build, 5 repeats each (PR-11), then
# analyze.py. Run it on the reference machine; copy numbers into docs/RESULTS.md only from the
# files it produces (D11, D56). Roughly 4 h with the memory runs, 20 min without.
#
#   PR-01, PR-02  experiments/perf/pr01_pr02_baseline.yaml  (lateness / latency columns)
#   PR-03, PR-04  frameflow_bench_queue                     (<out>/micro_benchmark.txt)
#   PR-07, PR-08  experiments/perf/pr07_pr08_memory.yaml    (peak RSS / RSS growth columns)
#   PR-09, EC-16  experiments/perf/pr09_throughput.yaml     (out FPS / drops / lateness)
#   PR-05, PR-06, PR-10 are tests: ctest --preset release -L timing; signal tests (PR-06)
#
# Usage: scripts/measure_perf.sh [--dry-run] [--out DIR] [--binary-dir DIR] [--skip-memory]
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dry_run=0
out="results/perf"
bin="$repo/build/release"
skip_memory=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) dry_run=1; shift ;;
    --out) out="$2"; shift 2 ;;
    --binary-dir) bin="$2"; shift 2 ;;
    --skip-memory) skip_memory=1; shift ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "measure_perf.sh: error: unknown option: $1 (hint: see --help)" >&2; exit 2 ;;
  esac
done

if [[ $dry_run -eq 0 && ! -x "$bin/frameflow" ]]; then
  echo "measure_perf.sh: error: $bin/frameflow not found (hint: cmake --preset release && cmake --build --preset release)" >&2
  exit 2
fi

run() {
  echo "+ $*"
  if [[ $dry_run -eq 0 ]]; then "$@"; fi
}

experiments=(pr01_pr02_baseline pr09_throughput)
if [[ $skip_memory -eq 0 ]]; then experiments+=(pr07_pr08_memory); fi

run mkdir -p "$out"
echo "+ $bin/frameflow_bench_queue > $out/micro_benchmark.txt"
if [[ $dry_run -eq 0 ]]; then "$bin/frameflow_bench_queue" | tee "$out/micro_benchmark.txt"; fi
for name in "${experiments[@]}"; do
  run "$bin/frameflow" bench --config="$repo/experiments/perf/$name.yaml" --out="$out"
done
run python3 "$repo/scripts/analyze.py" "$out" --no-charts
run ctest --test-dir "$bin" -L timing --output-on-failure
