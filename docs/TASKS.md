# FrameFlow Task Plan (Phases 1-5)

Each task is about 30-90 minutes of work, touches a few files and ends building and passing
tests. Requirement IDs are from `docs/SRS.md`; phases are from `docs/SPEC.md` section 16.
Verification: "debug+tsan" means `ctest` passes in both presets; "+asan" is added whenever memory
handling changes. Tasks marked **Write it myself: yes** follow the order: design + tests explained
first, you write the code, then I review it.

Tasks marked *(placement)* cover requirements that no SPEC phase names explicitly; they are
placed where their prerequisites first exist. See "Open questions" at the end.

Cross-cutting, checked by inspection in every task: RR-08 (header comment with responsibility and
thread-safety), SR-09 (no `system()`/`popen()`), SR-AUTH-02 (no secrets), SR-AUTH-03 (no UID/GID or
capability changes). SR-02 (warning and hardening flags) was done in Phase 0.

## Phase 1: Two-thread baseline

- [x] **P1.1** Injectable `Clock` interface, `SteadyClock` and thread-safe `FakeClock`
  - Files: `include/frameflow/clock.hpp`, `src/clock.cpp`, `tests/unit/clock_test.cpp`, `CMakeLists.txt` (link `Threads::Threads`), `tests/CMakeLists.txt`
  - Reqs: EC-18; base for FR-SRC-03 (`sleep_until`), AD-7
  - Design: `FakeClock` guards its time with a `mutable std::mutex` (so `now() const` can lock); `sleep_until` never blocks (D14)
  - Verify: unit tests (monotonic `now`, `advance`, `sleep_until` semantics, concurrent reads); debug+tsan
  - Write it myself: no
- [x] **P1.2** Move-only `Frame` (`cv::Mat` + `FrameMeta`), `FramePtr`, `DropReason` enum
  - Files: `include/frameflow/frame.hpp`, `tests/unit/frame_test.cpp`, `CMakeLists.txt` (core links OpenCV)
  - Reqs: SRS 6.1 Frame/FrameMeta entities; C2 (no raw new/delete)
  - Verify: `static_assert` not copyable / nothrow movable; meta defaults; debug+tsan+asan
  - Write it myself: no
- [x] **P1.3** `BoundedQueue<T>` single-thread semantics: capacity, FIFO, `try`-style push/pop, `size`
  - Files: `include/frameflow/bounded_queue.hpp`, `tests/unit/bounded_queue_test.cpp`
  - Reqs: FR-Q-01, FR-Q-02
  - Verify: FIFO order, capacity never exceeded, move-only items (`unique_ptr<int>`); debug+tsan+asan
  - Write it myself: **yes**
- [x] **P1.4** `BoundedQueue<T>` concurrency: blocking push, pop with timeout, `shutdown()`
  - Files: `include/frameflow/bounded_queue.hpp`, `tests/unit/bounded_queue_test.cpp`
  - Reqs: FR-Q-03, FR-Q-05, FR-Q-06, FR-POL-02 (blocking behaviour), EC-09/EC-10 (queue level)
  - Verify: producer blocks when full and resumes after pop; pop times out (>= timeout, no early
    return); shutdown wakes blocked push and pop within 50 ms; producer/consumer stress; debug+tsan
  - Write it myself: **yes**
- [x] **P1.5** `MetricsStore`: preallocated per-frame records indexed by id, depth-sample buffer
  - Files: `include/frameflow/metrics/metrics_store.hpp`, `src/metrics/metrics_store.cpp`, `tests/unit/metrics_store_test.cpp`
  - Reqs: FR-MET-01, DR-09, AD-6
  - Verify: capacity = fps x duration x 1.1 (or `--frames`); no reallocation; each slot written once; debug+tsan+asan
  - Write it myself: no
- [x] **P1.6** Queue depth recording after every push and pop (`t_push` is stamped by the source, D13)
  - Files: `include/frameflow/bounded_queue.hpp`, `tests/unit/bounded_queue_test.cpp`
  - Reqs: FR-Q-07
  - Verify: one depth sample per push and per pop, taken under the queue lock so the series is consistent; debug+tsan
  - Write it myself: **yes**
- [x] **P1.7** `CostModel` fixed mode: busy-wait (default) or sleep
  - Files: `include/frameflow/processor/cost_model.hpp`, `src/processor/cost_model.cpp`, `tests/unit/cost_model_test.cpp`
  - Reqs: FR-PROC-02, PR-05
  - Verify: cost 0 returns immediately; mean within +/-5% for >= 5 ms (release preset only, see D9); debug+tsan
  - Write it myself: no
- [x] **P1.8** `ISource` + `SyntheticSource`: deterministic pattern from frame id, given resolution
  - Files: `include/frameflow/source/source.hpp`, `include/frameflow/source/synthetic_source.hpp`, `src/source/synthetic_source.cpp`, `tests/unit/synthetic_source_test.cpp`
  - Reqs: FR-SRC-02, FR-SRC-05
  - Verify: same id gives identical pixels; ids 0,1,2... with no gaps; size/type match; debug+asan
  - Write it myself: no
- [x] **P1.9** Paced source loop: release frame k at t0 + k/fps via `Clock::sleep_until`, stamp `t_cap`, lateness, `t_push`
  - Files: `include/frameflow/source/source_runner.hpp`, `src/source/source_runner.cpp`, `tests/unit/source_runner_test.cpp`
  - Reqs: FR-SRC-03, FR-SRC-04, FR-SRC-07, EC-12, BR-04 (`t_cap` <= `t_push`, D13)
  - Verify: FakeClock schedule exact; late release keeps scheduled `t_cap` and records lateness; `t_push` stamped just before `push`; stop flag ends loop; debug+tsan
  - Write it myself: no
- [x] **P1.10** `ISink` + `NullSink` + `Worker` loop (pop, stamp, cost, sink stamps `t_out`)
  - Files: `include/frameflow/sink/sink.hpp`, `include/frameflow/sink/null_sink.hpp`, `include/frameflow/worker.hpp`, `src/worker.cpp`, `tests/unit/worker_test.cpp`
  - Reqs: FR-PROC-05, FR-PROC-06, FR-PROC-01 (`none` only), EC-10 (worker wakes on shutdown)
  - Verify: every popped frame stamped in order and recorded; worker exits on shutdown; debug+tsan+asan
  - Write it myself: no
- [x] **P1.11** `Pipeline`: wire parts, start threads, stop by duration or frame count, single shutdown routine, RAII join, in-flight marking
  - Files: `include/frameflow/pipeline.hpp`, `src/pipeline.cpp`, `tests/integration/pipeline_test.cpp`, `tests/CMakeLists.txt`
  - Reqs: FR-CLI-05 (engine side), ER-04, ER-08, BR-01, BR-02
  - Verify: run ends by frames and by duration; exception in worker stops pipeline without hang; destructor joins; debug+tsan+asan
  - Write it myself: no
- [x] **P1.12** `CsvReporter` for `frames.csv` and `depth.csv` (main thread, after join)
  - Files: `include/frameflow/metrics/csv_reporter.hpp`, `src/metrics/csv_reporter.cpp`, `tests/unit/csv_reporter_test.cpp`
  - Reqs: DR-01 (without `warmup` column semantics, see P2.5), DR-02, DR-06, DR-07, DR-10, FR-MET-07
  - Verify: golden-file test; empty fields for unreached stages; `.` decimal under a non-C locale; debug+asan
  - Write it myself: no
- [x] **P1.13** Phase 1 integration: invariants, cost-0 run, 200 create/stop cycles, 60 s gate run
  - Files: `tests/integration/invariants_test.cpp`, `tests/stress/lifecycle_test.cpp`, `tests/CMakeLists.txt` (ctest labels)
  - Reqs: BR-01, BR-02, BR-03, BR-04, EC-02, RR-02
  - Verify: invariants on every run; zero drops at 30 FPS cost 0; 60 s run under label `long` (D3, D10); debug+tsan+asan
  - Write it myself: no
- [x] **P1.14** CI: add ASan and TSan jobs next to debug *(placement)*
  - Files: `.github/workflows/ci.yml`
  - Reqs: SR-03, SR-04 (unit-suite part), RR-11
  - Verify: workflow runs green after you push (I cannot verify CI locally)
  - Write it myself: no

## Phase 2: Policies and metrics

- [x] **P2.1** `OverloadPolicy` interface, `PushDecision`, `QueueView`, `BlockingPolicy`; queue delegates "full" to the policy
  - Files: `include/frameflow/policy/overload_policy.hpp`, `include/frameflow/policy/blocking_policy.hpp`, `include/frameflow/bounded_queue.hpp`, `tests/unit/blocking_policy_test.cpp`, `tests/unit/bounded_queue_test.cpp`
  - Reqs: FR-Q-04, FR-POL-02, RR-05
  - Verify: all Phase 1 queue tests still pass unchanged in behaviour; debug+tsan
  - Write it myself: **yes** (queue refactor); policy class no
- [x] **P2.2** `DropOldestPolicy`, `DropNewestPolicy`; queue applies drop decisions and records drops (id, time, reason)
  - Files: `include/frameflow/policy/drop_oldest_policy.hpp`, `include/frameflow/policy/drop_newest_policy.hpp`, `include/frameflow/bounded_queue.hpp`, `include/frameflow/metrics/metrics_store.hpp`, `tests/unit/drop_policies_test.cpp`
  - Reqs: FR-POL-03, FR-POL-04, FR-POL-10, EC-01, BR-02
  - Verify: oldest id dropped and newest kept; incoming dropped and queue unchanged; capacity 1; debug+tsan+asan
  - Write it myself: **yes** (queue drop handling); policy classes no
- [x] **P2.3** `PolicyFactory`: create by name, one registration line per policy, unknown name error
  - Files: `include/frameflow/policy/policy_factory.hpp`, `src/policy/policy_factory.cpp`, `tests/unit/policy_factory_test.cpp`
  - Reqs: FR-POL-01 (three fixed policies), VR-07 (part), BR-08, RR-05
  - Verify: each name creates the right type; case-sensitive; unknown rejected; debug
  - Write it myself: no
- [x] **P2.4** `Stats`: nearest-rank percentiles, FPS, drop rate by reason, depth mean/max, warm-up exclusion
  - Files: `include/frameflow/metrics/stats.hpp`, `src/metrics/stats.cpp`, `tests/unit/stats_test.cpp`
  - Reqs: FR-MET-02, FR-MET-03, FR-MET-04 (except adaptive count), BR-05, BR-06, EC-15
  - Verify: 1..100 gives p95 = 95; empty input gives no values (not 0); warm-up excluded; debug+asan
  - Write it myself: no
- [x] **P2.5** `events.csv`, `warmup` column, run validity (`valid` flag)
  - Files: `src/metrics/csv_reporter.cpp`, `include/frameflow/metrics/run_summary.hpp`, `tests/unit/csv_reporter_test.cpp`
  - Reqs: DR-03, BR-06, BR-09, EC-08 (validity part)
  - Verify: golden files; stop before warm-up end gives `valid=false`; debug+asan
  - Write it myself: no
- [x] **P2.6** `summary.json`: config, environment, metrics, valid, version, git commit, schema version; platform module
  - Files: `include/frameflow/metrics/json_reporter.hpp`, `src/metrics/json_reporter.cpp`, `include/frameflow/platform/environment.hpp`, `src/platform/environment.cpp`, `CMakeLists.txt` (git commit, nlohmann-json), `tests/unit/json_reporter_test.cpp`, `.github/workflows/ci.yml` (apt: `nlohmann-json3-dev`)
  - Reqs: DR-04, DR-08, BR-10, SR-08, RR-12, FR-CLI-04 (commit embedding), FR-MET-05 (all four files)
  - Verify: schema test (all keys present, `null` latency when nothing delivered); hostname hashed; debug+asan
  - Write it myself: no
- [x] **P2.7** `RunConfig` + validation for Phase 2 parameters (no CLI parser yet)
  - Files: `include/frameflow/config.hpp`, `src/config.cpp`, `tests/unit/config_test.cpp`
  - Reqs: VR-01, VR-02, VR-03, VR-04, VR-05, VR-07, VR-13, VR-15 (synthetic only), VR-16, VR-20, VR-22, VR-23, ER-01
  - Verify: one pass + one fail test per rule; all errors in one message; debug
  - Write it myself: no
- [x] **P2.8** CLI11: `frameflow run`, `--help`, `--version`, effective-config block (S2)
  - Files: `src/main.cpp`, `include/frameflow/cli.hpp`, `src/cli.cpp`, `tests/unit/cli_test.cpp`, `.github/workflows/ci.yml` (apt: `libcli11-dev`)
  - Reqs: FR-CLI-01 (flags part), FR-CLI-03, FR-CLI-04, VR-21, VR-24, ER-02 (exit 2)
  - Verify: parse tests on argv arrays; unknown flag suggests closest; `--version` shows commit; debug
  - Write it myself: no
- [x] **P2.9a** Root refusal and permission errors *(placement)*
  - Files: `include/frameflow/platform/privileges.hpp`, `src/platform/privileges.cpp`, `src/cli.cpp`, `tests/unit/privileges_test.cpp`
  - Reqs: FR-ROLE-02, FR-ROLE-04, SR-AUTH-05, ER-02 (exits 4 and 5)
  - Verify: effective-UID check injected so tests need no root; `--allow-root` bypasses with warning; unwritable `--out` parent names the path and exits 4; debug
  - Write it myself: no
- [x] **P2.9b** Output-directory safety and file write errors *(placement)*
  - Files: `include/frameflow/platform/output_dir.hpp`, `src/platform/output_dir.cpp`, `src/cli.cpp`, `tests/unit/output_dir_test.cpp`
  - Reqs: BR-12, VR-17, SR-05, SR-06, SR-AUTH-04, FR-ROLE-03, DR-11, ER-05, EC-17
  - Verify: existing dir refused without `--overwrite`; symlink escape refused; `O_EXCL` never follows a symlink; 0644/0755; disk-full via `/dev/full` (D8); nothing written outside `--out`; debug+asan
  - Write it myself: no
- [x] **P2.10** Signals and exit codes: first SIGINT/SIGTERM graceful (0), second immediate (130)
  - Files: `include/frameflow/platform/signals.hpp`, `src/platform/signals.cpp`, `src/main.cpp`, `tests/integration/signal_test.cpp`
  - Reqs: FR-CLI-06, EC-08, EC-09, EC-10, EC-11, ER-02, PR-06, RR-12
  - Verify: test spawns the `frameflow` binary and sends signals (no `system()`); exit codes; results written; debug+tsan
  - Write it myself: no
- [x] **P2.11** Console summary (S4) and warnings
  - Files: `include/frameflow/ui/console_reporter.hpp`, `src/ui/console_reporter.cpp`, `tests/unit/console_reporter_test.cpp`
  - Reqs: FR-MET-06, ER-07, ER-01 (format)
  - Verify: golden text output incl. empty state "no frames delivered"; warnings in stderr and `summary.json`; debug
  - Write it myself: no
- [x] **P2.12** Phase 2 integration: overload behaviour per policy, CI smoke runs, 60 s TSan stress
  - Files: `tests/integration/overload_test.cpp`, `tests/stress/policy_stress_test.cpp`, `.github/workflows/ci.yml`
  - Reqs: EC-03, BR-01..BR-04 (all policies), PR-10, SR-04 (stress part)
  - Verify: blocking latency grows vs dropping bounded (invariant checks, not exact numbers); 40-60% drops at 2.0x; debug+tsan+asan
  - Write it myself: no

## Phase 3: Benchmark harness

- [x] **P3.1** `CostModel` bursty mode with seeded, reproducible spike schedule
  - Files: `include/frameflow/processor/cost_model.hpp`, `src/processor/cost_model.cpp`, `src/config.cpp`, `tests/unit/cost_model_test.cpp`
  - Reqs: FR-PROC-03, FR-PROC-04, VR-06
  - Verify: FakeClock: spikes at configured period and length; same seed gives same schedule (D5); debug
  - Write it myself: no
- [x] **P3.2** YAML config for `run --config`, flags override config
  - Files: `include/frameflow/config_file.hpp`, `src/config_file.cpp`, `tests/unit/config_file_test.cpp`, `CMakeLists.txt` (yaml-cpp), `.github/workflows/ci.yml` (apt: `libyaml-cpp-dev`)
  - Reqs: FR-CLI-01, VR-18, SR-07, EC-19
  - Verify: unknown key rejected with file:line:col; >1 MB rejected; flag beats config; debug+asan
  - Write it myself: no
- [x] **P3.3** Experiment matrix: parse and expand to configurations, write `experiment.json`
  - Files: `include/frameflow/bench/experiment.hpp`, `src/bench/experiment.cpp`, `tests/unit/experiment_test.cpp`
  - Reqs: DR-05 (matrix part)
  - Verify: cartesian product count and order; config names stable; debug
  - Write it myself: no
- [x] **P3.4** `BenchRunner` + `frameflow bench`: repeats, `rep_<n>` folders, progress (S5), interruption
  - Files: `include/frameflow/bench/bench_runner.hpp`, `src/bench/bench_runner.cpp`, `src/cli.cpp`, `tests/integration/bench_test.cpp`
  - Reqs: FR-CLI-02, VR-19, DR-05, BR-11, EC-20
  - Verify: tiny matrix (2 configs x 2 repeats x 1 s) layout; interrupted bench lists missing runs; debug+tsan
  - Write it myself: no
- [x] **P3.5** Experiment files E1, E2, E4, `template.yaml`, `scripts/run_all.sh`
  - Files: `experiments/e1_baseline.yaml`, `experiments/e2_sustained.yaml`, `experiments/e4_capacity.yaml`, `experiments/template.yaml`, `scripts/run_all.sh`
  - Reqs: SPEC 11 (E1, E2, E4)
  - Verify: each file loads and expands in a unit test; `run_all.sh --dry-run`; debug
  - Write it myself: no
- [x] **P3.6** `analyze.py` part 1: load results, summary tables (CSV + Markdown), median/min/max, errors
  - Files: `scripts/analyze.py`, `scripts/tests/test_analyze.py`, `.github/workflows/ci.yml` (apt: `python3-pytest python3-pandas python3-matplotlib`, pytest step)
  - Reqs: FR-ANA-01 (tables), FR-ANA-02, FR-ANA-03, ER-09, BR-11
  - Verify: pytest on fixture results (D7); missing/malformed file names file and column
  - Write it myself: no
- [x] **P3.7** `analyze.py` part 2: `latency_over_time.png`, `depth_over_time.png` per UIUX 6.3
  - Files: `scripts/analyze.py`, `scripts/tests/test_analyze.py`
  - Reqs: FR-ANA-01 (graphs)
  - Verify: pytest checks files created with expected size; manual look at one chart
  - Write it myself: no
- [x] **P3.8** `frameflow_bench_queue` micro-benchmark *(placement)*
  - Files: `bench/bench_queue.cpp`, `CMakeLists.txt`
  - Reqs: PR-03 (tool only; measured in P5.9)
  - Verify: builds in release; runs and prints median; no number recorded until measured
  - Write it myself: no
- [ ] **P3.9** Run E1, E2, E4 with 5 repeats; generate graphs into `docs/images`
  - Files: `docs/images/*`, `docs/RESULTS.md` (raw tables only)
  - Reqs: RR-04 (ranking check), Phase 3 gate
  - Verify: release preset; numbers copied only from produced files; long wall-clock run (D11)
  - Write it myself: no

## Phase 4: Adaptive policy

- [x] **P4.1** Latency estimator: p95 of the last W delivered latencies (ring buffer)
  - Files: `include/frameflow/policy/latency_window.hpp`, `tests/unit/latency_window_test.cpp`
  - Reqs: FR-POL-05, EC-14
  - Verify: fewer than W samples uses all; window slides; nearest-rank p95; debug+asan
  - Write it myself: **yes**
- [x] **P4.2** `AdaptivePolicy` hysteresis state machine NORMAL/SHEDDING with state-change events
  - Files: `include/frameflow/policy/adaptive_policy.hpp`, `src/policy/adaptive_policy.cpp`, `tests/unit/adaptive_policy_test.cpp`
  - Reqs: FR-POL-06, FR-POL-07, FR-POL-09, BR-07, EC-13
  - Verify: FakeClock: enters above T, exits below T_low, no flapping inside the band; NORMAL never drops; debug+tsan
  - Write it myself: **yes**
- [x] **P4.3** Shedding on push: drop queued frames older than T (reason `stale`), then oldest if still full
  - Files: `src/policy/adaptive_policy.cpp`, `include/frameflow/bounded_queue.hpp`, `tests/unit/adaptive_policy_test.cpp`
  - Reqs: FR-POL-08 (push part), FR-POL-10
  - Verify: stale frames removed in order, fresh kept; capacity respected; debug+tsan+asan
  - Write it myself: **yes**
- [x] **P4.4** Stale-on-pop in worker and latency feedback from sink to policy
  - Files: `src/worker.cpp`, `src/policy/adaptive_policy.cpp`, `tests/unit/adaptive_policy_test.cpp`, `tests/unit/worker_test.cpp`
  - Reqs: FR-POL-08 (pop part), FR-POL-10, BR-07
  - Verify: stale dropped on pop only while SHEDDING; policy state shared by source and worker threads is race-free; debug+tsan
  - Write it myself: **yes**
- [x] **P4.5** Register adaptive; `--target-ms`, `--exit-ratio`, `--window`; adaptive row in console and summary
  - Files: `src/policy/policy_factory.cpp`, `src/config.cpp`, `src/cli.cpp`, `src/ui/console_reporter.cpp`, `src/metrics/stats.cpp`, tests
  - Reqs: FR-POL-01 (all four), VR-08, VR-09, VR-10, FR-MET-04 (state changes)
  - Verify: pass/fail test per new rule; integration run per policy incl. adaptive; debug+tsan
  - Write it myself: no
- [x] **P4.6** E3, E5 experiment files; `analyze.py` trade-off scatter, recovery time, drops outside spikes, state switches
  - Files: `experiments/e3_bursty.yaml`, `experiments/e5_tuning.yaml`, `scripts/analyze.py`, `scripts/tests/test_analyze.py`
  - Reqs: SPEC 10 (recovery time), SPEC 11 E3/E5
  - Verify: pytest on fixtures; experiment files expand in unit test
  - Write it myself: no
- [ ] **P4.7** Run E3, E5 with 5 repeats; fill tables
  - Files: `docs/RESULTS.md`, `docs/images/*`
  - Reqs: Phase 4 gate, RR-04
  - Verify: release preset; numbers only from produced files (D11)
  - Write it myself: no

## Phase 5: Real load, polish, report

- [x] **P5.1** `VideoFileSource`: open, decode, loop / `--no-loop`, early end; generated test clip
  - Files: `include/frameflow/source/video_file_source.hpp`, `src/source/video_file_source.cpp`, `tests/unit/video_file_source_test.cpp`, `src/config.cpp`
  - Reqs: FR-SRC-01, FR-SRC-06, VR-14, VR-15, EC-04, EC-05, EC-06
  - Verify: tests write a short clip with `cv::VideoWriter` (no downloaded files); exit 3 on undecodable; debug+asan
  - Write it myself: no
- [x] **P5.2** Decode-error retry and corrupted input handling
  - Files: `src/source/video_file_source.cpp`, `tests/unit/video_file_source_test.cpp`
  - Reqs: ER-03, EC-07, SR-01
  - Verify: truncated/corrupted clips never crash; retry 3 then stop with `valid=false`, exit 1; debug+asan
  - Write it myself: no
- [x] **P5.3** `IProcessor` + `none`, `grayscale`, `blur`, `canny` + factory; `--processor`, `--kernel`
  - Files: `include/frameflow/processor/*.hpp`, `src/processor/*.cpp`, `tests/unit/processor_test.cpp`
  - Reqs: FR-PROC-01, VR-11, VR-12, RR-06
  - Verify: output size/type per processor; even kernel rejected; debug+tsan (TBB suppression applies)
  - Write it myself: no
- [x] **P5.4** `PreviewSink` with overlay, keys, display check
  - Files: `include/frameflow/sink/preview_sink.hpp`, `src/sink/preview_sink.cpp`, `tests/unit/preview_sink_test.cpp`
  - Reqs: FR-CLI-07, ER-06
  - Verify: overlay drawn on an off-screen Mat in tests; exit 6 with no display; `--preview=auto` warns; demo by you
  - Write it myself: no
- [x] **P5.5** `--progress`: one status line per second on stderr (UIUX S3)
  - Files: `include/frameflow/ui/progress.hpp`, `src/ui/progress.cpp`, `src/cli.cpp`, `src/pipeline.cpp`, `tests/unit/progress_test.cpp`
  - Reqs: none in SRS; UIUX S3, ARCHITECTURE 10 (decision D6)
  - Verify: line formatting from a counter snapshot; main thread reads atomic counters only (no I/O on source/worker); debug+tsan
  - Write it myself: no
- [ ] **P5.6** E6 experiment file; run E6
  - Files: `experiments/e6_real_load.yaml`, `docs/RESULTS.md`
  - Reqs: SPEC 11 E6
  - Verify: release preset; numbers only from produced files (D11)
  - Write it myself: no
- [x] **P5.7** clang-format and clang-tidy checks in CI
  - Files: `.clang-tidy`, `.github/workflows/ci.yml`, fixes in sources
  - Reqs: SR-11, RR-07
  - Verify: zero tidy warnings locally; CI after your push
  - Write it myself: no
- [x] **P5.8** Coverage preset and report *(placement)*
  - Files: `CMakePresets.json`, `CMakeLists.txt`
  - Reqs: RR-09
  - Verify: lcov report for queue, policy and stats modules; result recorded only as measured
  - Write it myself: no
- [ ] **P5.9a** Performance measurements
  - Files: `docs/RESULTS.md`
  - Reqs: PR-01..PR-04, PR-07..PR-09, PR-11, EC-16; PR-05, PR-06, PR-10 re-measured in release (their tests exist from P1.7, P2.10, P2.12)
  - Verify: release preset, 5 repeats, median with min/max, environment recorded; numbers only from runs (D11)
  - Write it myself: no
- [ ] **P5.9b** Reliability and soak runs
  - Files: `docs/RESULTS.md`, `scripts/soak.sh`
  - Reqs: RR-01 (100 runs), RR-03 (60 min at 1.5x per policy), RR-02 and RR-04 final confirmation, SR-AUTH-01 (strace shows no sockets)
  - Verify: exit 0 and frame conservation on every run; Valgrind short run; memory growth checked against PR-08; numbers only from runs (D11)
  - Write it myself: no
- [x] **P5.10** Release workflow on version tag *(placement)*
  - Files: `.github/workflows/release.yml`
  - Reqs: ARCHITECTURE 9.3 step 7, SR-10
  - Verify: workflow syntax check; real run only when you push a tag
  - Write it myself: no
- [ ] **P5.11** README, DESIGN.md and RESULTS.md write-up
  - Files: `README.md`, `docs/DESIGN.md`, `docs/RESULTS.md`
  - Reqs: RR-10, FR-ROLE-01 (demo), SR-10, release gate G10
  - Verify: clean-clone build following only the README (you, on a fresh checkout)
  - Write it myself: no

## Decisions

The 12 open questions raised during planning are resolved, plus D13-D14 from the independent review.
All decisions (D1-D14) are recorded in `docs/DESIGN.md` under "Decisions"; references like (D9)
above point there.
