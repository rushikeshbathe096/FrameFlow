#!/usr/bin/env bash
# P5.9b reliability checks with the release build (D56). Every check is pass/fail; a failure
# stops the script with exit 1 and names the run. Logs and result folders go to --out.
#
#   default  RR-01      100 consecutive 5 s runs, 25 per policy, at 1.5x: exit 0 and frame
#                       conservation (produced = delivered + dropped + in_flight) in every run
#            SR-AUTH-01 a run under `strace -f -e trace=network` makes no socket calls
#            valgrind   a short run under memcheck reports no errors
#   --long   RR-03      a 60-minute run at 1.5x under each policy (about 4 h): exit 0 and RSS
#                       growth after warm-up within PR-08's 5%
#   RR-02 is the `stress` ctest label (ctest --preset debug-long / tsan-long); RR-04 compares the
#   p95 ranking of two E2 bench runs (analysis).
#
# Usage: scripts/soak.sh [--long] [--out DIR] [--binary PATH]
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="results/soak"
binary="$repo/build/release/frameflow"
long=0
policies=(blocking drop-oldest drop-newest adaptive)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --long) long=1; shift ;;
    --out) out="$2"; shift 2 ;;
    --binary) binary="$2"; shift 2 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "soak.sh: error: unknown option: $1 (hint: see --help)" >&2; exit 2 ;;
  esac
done
if [[ ! -x "$binary" ]]; then
  echo "soak.sh: error: $binary not found (hint: cmake --preset release && cmake --build --preset release)" >&2
  exit 2
fi
mkdir -p "$out/rr01" "$out/rr03"  # --out needs an existing parent (VR-17)

fail() {
  echo "soak.sh: FAIL: $*" >&2
  exit 1
}

# Frame conservation (BR-01) and, optionally, RSS growth from one summary.json.
check_summary() {
  python3 - "$1" "${2:-}" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
m = s["metrics"]
if m["produced"] != m["delivered"] + m["dropped"] + m["in_flight"]:
    sys.exit(f"{sys.argv[1]}: produced {m['produced']} != delivered + dropped + in_flight")
limit = sys.argv[2]
growth = (s.get("memory") or {}).get("growth_pct")
if limit and (growth is None or growth > float(limit)):
    sys.exit(f"{sys.argv[1]}: RSS growth {growth}% above {limit}% (PR-08)")
PY
}

one_run() {  # one_run <dir> <log> <args...>
  local dir="$1" log="$2"
  shift 2
  "$binary" run --source=synthetic --cost-ms=50 --out="$dir" "$@" >"$log" 2>&1 ||
    fail "exit $? in $dir (see $log)"
}

echo "RR-01: 100 consecutive 5 s runs"
for policy in "${policies[@]}"; do
  for n in $(seq -w 1 25); do
    dir="$out/rr01/${policy}_$n"
    rm -rf "$dir"
    one_run "$dir" "$out/rr01_${policy}_$n.log" --duration=5 --warmup=1 --policy="$policy"
    check_summary "$dir/summary.json" || fail "conservation in $dir"
  done
  echo "  $policy: 25/25 ok"
done

echo "SR-AUTH-01: no network calls"
rm -rf "$out/strace_run"
strace -f -e trace=network -o "$out/strace.log" \
  "$binary" run --source=synthetic --duration=2 --out="$out/strace_run" >/dev/null 2>&1 ||
  fail "strace run (see $out/strace.log)"
if grep -E '(socket|connect|bind|listen|accept)[0-9]*\(' "$out/strace.log"; then
  fail "network calls in $out/strace.log"
fi
echo "  no socket calls"

echo "valgrind: short run under memcheck"
rm -rf "$out/valgrind_run"
valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite \
  --log-file="$out/valgrind.log" \
  "$binary" run --source=synthetic --frames=30 --fps=60 --resolution=64x48 \
  --out="$out/valgrind_run" >/dev/null 2>&1 || fail "valgrind (see $out/valgrind.log)"
echo "  no errors"

if [[ $long -eq 1 ]]; then
  echo "RR-03: 60-minute runs at 1.5x"
  for policy in "${policies[@]}"; do
    dir="$out/rr03/$policy"
    rm -rf "$dir"
    one_run "$dir" "$out/rr03_$policy.log" --duration=3600 --policy="$policy"
    check_summary "$dir/summary.json" 5 || fail "RR-03 in $dir"
    echo "  $policy: ok"
  done
fi
echo "all checks passed"
