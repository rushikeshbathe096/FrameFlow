#!/usr/bin/env bash
# SR-11 / RR-07: clang-format and clang-tidy checks, as run by CI (P5.7, D54).
#   clang-format: every C++ file in src/, include/, tests/ and bench/ must already be formatted.
#   clang-tidy:   bugprone-*, cert-* and concurrency-* (see .clang-tidy) on the product code
#                 (src/ and bench/; headers in include/ are checked through them).
# Needs a configured build with compile_commands.json (default: build/debug).
# Usage: scripts/check_tidy.sh [BUILD_DIR]
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$repo/build/debug}"
cd "$repo"

if [[ ! -f "$build/compile_commands.json" ]]; then
  echo "check_tidy.sh: error: $build/compile_commands.json not found (hint: cmake --preset debug)" >&2
  exit 2
fi

echo "clang-format..."
find src include tests bench \( -name '*.cpp' -o -name '*.hpp' \) -print0 |
  xargs -0 clang-format --dry-run --Werror

echo "clang-tidy..."
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
status=0
find src bench -name '*.cpp' -print0 |
  xargs -0 -P "$(nproc)" -n 1 clang-tidy -p "$build" --quiet >"$log" 2>&1 || status=$?
if grep -E '(warning|error):' "$log"; then
  echo "check_tidy.sh: clang-tidy reported problems (see above)" >&2
  exit 1
fi
if [[ $status -ne 0 ]]; then
  cat "$log" >&2
  exit "$status"
fi
echo "clean"
