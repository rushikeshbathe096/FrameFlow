# FrameFlow Results

Status: **the experiments and performance measurements have not been run yet.** Every cell
below stays blank until it is copied from a file produced by the scripts (SPEC 11, D11). No
number goes on a resume before it appears here.

## How the numbers are produced

1. `cmake --preset release && cmake --build --preset release` on the reference machine.
2. `scripts/run_all.sh` (E1-E6), `scripts/measure_perf.sh` (PR-xx), `scripts/soak.sh --long`.
3. `python3 scripts/analyze.py results --out docs/images` writes `summary.md` / `summary.csv`
   per experiment (median, min-max over 5 repeats, BR-11) and the graphs.
4. Copy the tables below from those `summary.md` files; record the environment from any
   `summary.json` (`environment` block) and the git commit.

## Environment

| Item | Value |
| --- | --- |
| CPU (model, logical cores) | |
| RAM | |
| Kernel / Ubuntu | |
| Compiler, OpenCV | |
| FrameFlow version, git commit | |

## E1 Baseline (0.5x)

| Policy | p50 | p95 | p99 | Out FPS | Drop % |
| --- | --- | --- | --- | --- | --- |
| Blocking | | | | | |
| Drop-oldest | | | | | |
| Drop-newest | | | | | |
| Adaptive | | | | | |

## E2 Sustained overload

| Overload | Policy | p50 | p95 | p99 | Out FPS | Drop % | Peak depth |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1.5x | Blocking | | | | | | |
| 1.5x | Drop-oldest | | | | | | |
| 1.5x | Drop-newest | | | | | | |
| 1.5x | Adaptive | | | | | | |

Graphs: `docs/images/e2_sustained/latency_over_time_cost_ms-50.png`, `latency_vs_drops.png`.

## E3 Bursty overload

| Policy | p95 | p99 | Drop % | Mean recovery (ms) | Drops outside spikes |
| --- | --- | --- | --- | --- | --- |
| Blocking | | | | | |
| Drop-oldest | | | | | |
| Adaptive | | | | | |

## E4 Queue capacity sweep

| Capacity | Policy | p95 | Drop % |
| --- | --- | --- | --- |
| | | | |

## E5 Adaptive tuning

| T (ms) | T_low / T | p95 | Drop % | State changes |
| --- | --- | --- | --- | --- |
| | | | | |

## E6 Real processing load

| Processor | Resolution | Policy | p95 | Out FPS | Drop % |
| --- | --- | --- | --- | --- | --- |
| | | | | | |

## Performance requirements

| ID | Requirement | Median (min-max) | Pass |
| --- | --- | --- | --- |
| PR-01 | source lateness median < 1 ms, p99 < 5 ms | | |
| PR-02 | end-to-end latency median < 1 ms (no processing) | | |
| PR-03 | push+pop pair < 5 µs | | |
| PR-04 | metrics add < 1% to processing time | | |
| PR-05 | mean cost within ±5% (≥ 5 ms) | | |
| PR-06 | signal to exit < 1 s | | |
| PR-07 | peak RSS within 10% of the bound | | |
| PR-08 | RSS growth after warm-up ≤ 5% (10 min) | | |
| PR-09 | 240 FPS at 720p without drops | | |
| PR-10 | writing a 60 s run's results < 2 s | | |

## Reliability and quality checks

These are pass/fail checks, run on the development machine (not the reference measurements).

| Check | Result | Where |
| --- | --- | --- |
| Test suite: debug, ASan, TSan, release | all pass | docs/PROGRESS.md (counts per task) |
| RR-02: 200 lifecycle cycles; SR-04 stress under TSan | pass | `ctest --preset debug-long` / `tsan-long` |
| RR-09: line coverage of queue / policy / stats | 100.0% / 96.7% / 100.0% (debug coverage build) | `scripts/coverage.py` |
| SR-11: clang-tidy (bugprone, cert, concurrency) | 0 findings | `scripts/check_tidy.sh` |
| RR-01: 100 consecutive 5 s runs (25 per policy, 1.5x) | pass: 100/100 exit 0, conservation held in every summary.json | `scripts/soak.sh` (release build) |
| SR-AUTH-01: no network calls (strace) | pass: no socket/connect/bind/listen/accept in any of the 4 threads | `scripts/soak.sh` |
| Valgrind memcheck, short run | pass: 0 errors, 0 bytes definitely lost | `scripts/soak.sh` |
| RR-03: 60 min at 1.5x per policy | not run | `scripts/soak.sh --long` |
| RR-04: same p95 ranking in two E2 runs | not run | two `run_all.sh --only e2_sustained` |

## Interpretation, limitations, negative results

To be written from the measured data. Known limits to discuss: adaptive cannot beat the
throughput limit (sustained 1.5x still drops about a third, D45/D47); its cost estimate learns
spikes only through an EWMA (D47); the preview delays processing and is for demos only (D51);
processing is single-threaded by design (D50).
