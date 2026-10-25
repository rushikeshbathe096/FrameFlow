# FrameFlow

FrameFlow is a C++17 video pipeline for Linux that measures what happens when processing cannot
keep up with the incoming frames. A paced source releases frames into a bounded queue; one worker
thread processes them and hands them to a sink. When the queue is full, a pluggable overload
policy decides what happens:

| Policy | When the queue is full | Trade-off |
| --- | --- | --- |
| `blocking` | the source waits | never drops; latency grows without bound under overload |
| `drop-oldest` | the oldest queued frame is dropped | bounded latency; drops about 1 - 1/overload |
| `drop-newest` | the incoming frame is dropped | bounded latency; the queue holds older frames |
| `adaptive` | blocking until the measured p95 latency passes a target, then drops frames that would arrive late, until latency falls below a lower threshold | drops only while latency is too high (hysteresis) |

Every frame is timestamped from capture to output, so each run reports end-to-end latency
(p50/p95/p99/max), input and output FPS, drop rate by reason, queue depth and memory use.
Measured results are in [docs/RESULTS.md](docs/RESULTS.md); nothing in this README is a
measurement.

## Requirements

Ubuntu 24.04 with these apt packages (the versions FrameFlow is built and tested with, SR-10):

```
sudo apt install build-essential cmake git pkg-config \
  libopencv-dev libgtest-dev nlohmann-json3-dev libcli11-dev libyaml-cpp-dev \
  python3-pytest python3-pandas python3-matplotlib
```

| Package | Version |
| --- | --- |
| g++ | 13.2.0 |
| cmake | 3.28.3 |
| libopencv-dev | 4.6.0 |
| libgtest-dev | 1.14.0 |
| nlohmann-json3-dev | 3.11.3 |
| libcli11-dev | 2.4.1 |
| libyaml-cpp-dev | 0.8.0 |
| python3-pandas / python3-matplotlib / python3-pytest | 2.1.4 / 3.6.3 / 7.4.4 |

Optional, for the checks: `clang-format clang-tidy` (18), `valgrind`, `strace`.

## Build and test

```
cmake --preset release && cmake --build --preset release     # for runs and measurements
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

| Preset | Purpose |
| --- | --- |
| `debug` | development, unit and integration tests |
| `release` | `-O2`, hardened; runs, measurements, `timing` tests |
| `asan`, `tsan` | AddressSanitizer + UBSan, ThreadSanitizer (`tsan` runs tests under `setarch -R`, see docs/DESIGN.md) |
| `debug-long`, `tsan-long` | the 60 s gate run and the stress tests (test presets) |
| `coverage` | gcov build; then `scripts/coverage.py` |

`scripts/check_tidy.sh` runs clang-format and clang-tidy as CI does.

## Quick start

```
# One run: 30 FPS synthetic video, 50 ms of work per frame (1.5x overload), drop-oldest policy.
./build/release/frameflow run --source=synthetic --fps=30 --cost-ms=50 --policy=drop-oldest

# The adaptive policy with a 100 ms target, a status line every second.
./build/release/frameflow run --source=synthetic --cost-ms=50 --policy=adaptive \
  --target-ms=100 --progress

# A video file with real processing and a preview window (q quits, Space freezes).
./build/release/frameflow run --input=clip.mp4 --processor=blur --policy=adaptive --preview

# An experiment: every configuration of a YAML matrix, 5 repeats each, then graphs and tables.
./build/release/frameflow bench --config=experiments/e2_sustained.yaml
python3 scripts/analyze.py results/e2_sustained
```

`frameflow --help` and `frameflow run --help` list every option with its default. Settings can
also come from a YAML file (`frameflow run --config=file.yaml`; flags override it); the format
is shown in [experiments/template.yaml](experiments/template.yaml).

Each run writes four files into its output folder (default `results/run_NNN`):
`frames.csv` (every timestamp of every frame), `depth.csv` (queue depth at every push and pop),
`events.csv` (drops and adaptive state changes) and `summary.json` (configuration, environment,
metrics, validity). A bench writes `results/<experiment>/<configuration>/rep_<n>/` per run plus
`experiment.json`.

Exit codes: 0 success (also after Ctrl+C), 1 runtime error, 2 invalid options, 3 bad input file,
4 output folder problem, 5 run as root, 6 no display for `--preview`, 130 second Ctrl+C.

## Reproducing the results

All measurements use the release build. They take hours, so they are separate scripts:

| Script | What | Time |
| --- | --- | --- |
| `scripts/run_all.sh` | experiments E1-E6 (`experiments/e*.yaml`), then `analyze.py` | about 5 h |
| `scripts/measure_perf.sh` | performance requirements PR-01..PR-04, PR-07..PR-09 | about 4 h (20 min with `--skip-memory`) |
| `scripts/soak.sh` | reliability: 100 runs, no network calls, valgrind; `--long` adds 60-minute runs | 10 min (4 h with `--long`) |

`python3 scripts/analyze.py results --out docs/images` writes the summary tables and graphs.

## Project layout

```
include/frameflow/   headers (one class per file, each with its responsibility and thread-safety)
src/                 implementation: pipeline, queue, policies, sources, processors, sinks,
                     metrics, CLI, bench, platform (signals, /proc, output folder safety)
tests/               GoogleTest: unit/, integration/, stress/
scripts/             analyze.py (+ pytest), run_all.sh, measure_perf.sh, soak.sh, coverage.py
experiments/         experiment YAML files (E1-E6, perf/)
bench/               queue micro-benchmark
docs/                PRD, SPEC, SRS, ARCHITECTURE, UIUX (requirements); DESIGN (decisions as
                     built); TASKS, PROGRESS (plan and log); RESULTS (measurements)
```

## Design in one paragraph

Three threads: the source (paces and timestamps frames), the worker (processes, delivers,
measures) and the main thread (CLI, waiting, files, progress). Frames are move-only and owned by
exactly one stage at a time (`std::unique_ptr`). The queue has one mutex and two condition
variables, always waited on with a predicate; policies decide under that lock and never call
back into it. Pipeline threads do no file I/O; every end condition (duration, frame count, end
of input, Ctrl+C, error) goes through one shutdown routine, and frames still queued are counted
as in flight, so produced = delivered + dropped + in flight in every run. The full list of
decisions and their reasons is in [docs/DESIGN.md](docs/DESIGN.md).
