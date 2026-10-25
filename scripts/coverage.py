#!/usr/bin/env python3
"""Line coverage of the RR-09 modules from a `coverage` build (P5.8, D55).

Usage: scripts/coverage.py [BUILD_DIR] [--min PERCENT]
  BUILD_DIR  default build/coverage; run `ctest --preset coverage` first
  --min      fail (exit 1) if a module is below this, default 85 (RR-09)

Runs gcov (GCC's own tool, no lcov needed) in JSON mode on every .gcda file, merges the line
counts of every translation unit (BoundedQueue is a header template, so its lines are counted
wherever it is instantiated), and reports covered / executable lines per module:
  queue   include/frameflow/bounded_queue.hpp
  policy  include/frameflow/policy/*, src/policy/*
  stats   include/frameflow/metrics/stats.hpp, src/metrics/stats.cpp
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MODULES = {
    "queue": ("include/frameflow/bounded_queue.hpp",),
    "policy": ("include/frameflow/policy/", "src/policy/"),
    "stats": ("include/frameflow/metrics/stats.hpp", "src/metrics/stats.cpp"),
}


def gcov_documents(build: Path) -> list[dict]:
    gcda = sorted(str(p) for p in build.rglob("*.gcda"))
    if not gcda:
        raise SystemExit(f"coverage.py: error: no .gcda files under {build} "
                         "(hint: cmake --preset coverage && cmake --build --preset coverage && "
                         "ctest --preset coverage)")
    result = subprocess.run(["gcov", "--json-format", "--stdout", *gcda], cwd=build,
                            capture_output=True, text=True, check=True)
    return [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]


def merged_lines(documents: list[dict]) -> dict[str, dict[int, int]]:
    """repo-relative file -> line -> highest execution count over all translation units."""
    lines: dict[str, dict[int, int]] = {}
    for document in documents:
        for entry in document.get("files", []):
            path = Path(entry["file"])
            if not path.is_absolute():
                path = (Path(document.get("current_working_directory", ".")) / path).resolve()
            try:
                relative = str(path.resolve().relative_to(REPO))
            except ValueError:
                continue  # system and third-party headers
            file_lines = lines.setdefault(relative, {})
            for line in entry.get("lines", []):
                number = line["line_number"]
                file_lines[number] = max(file_lines.get(number, 0), line["count"])
    return lines


def module_coverage(lines: dict[str, dict[int, int]]) -> dict[str, tuple[int, int, list[str]]]:
    report = {}
    for module, prefixes in MODULES.items():
        files = sorted(f for f in lines if f.startswith(prefixes))
        executable = sum(len(lines[f]) for f in files)
        covered = sum(1 for f in files for count in lines[f].values() if count > 0)
        report[module] = (covered, executable, files)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build", nargs="?", type=Path, default=REPO / "build" / "coverage")
    parser.add_argument("--min", type=float, default=85.0)
    args = parser.parse_args(argv)
    report = module_coverage(merged_lines(gcov_documents(args.build.resolve())))
    failed = False
    for module, (covered, executable, files) in report.items():
        percent = 100.0 * covered / executable if executable else 0.0
        ok = executable > 0 and percent >= args.min
        failed |= not ok
        print(f"{module:7} {percent:6.1f}%  ({covered}/{executable} lines, {len(files)} files)"
              f"  {'ok' if ok else 'BELOW ' + str(args.min) + '%'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
