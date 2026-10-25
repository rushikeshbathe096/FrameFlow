# FrameFlow Progress Log

One line per task (task, files, tests passed), plus a summary after each phase. Test counts are
real `ctest` results; "debug 29/29" means 29 of 29 tests passed in the debug preset.

## Phase 1

- P1.1 Clock/SteadyClock/FakeClock: `clock.hpp`, `clock.cpp`, `clock_test.cpp` | debug 15/15, tsan 15/15
- P1.2 Frame/FrameMeta/FramePtr/DropReason: `frame.hpp`, `frame_test.cpp` | debug 22/22, tsan 22/22, asan 22/22
- P1.3 BoundedQueue single-thread (capacity, FIFO, try_push/try_pop): `bounded_queue.hpp`, `bounded_queue_test.cpp` | debug 29/29, tsan 29/29, asan 29/29
  - STUDY: `try_push(T&& item)` moves from `item` only after the capacity check, so a rejected
    push leaves the caller's item intact; `mutable std::mutex` lets `size() const` lock;
    `std::move(items_.front())` then `pop_front()` (move out first, then erase the moved-from slot).
- P1.4 BoundedQueue blocking push, timed pop, shutdown (replaces try_push/try_pop; P1.3 tests ported): `bounded_queue.hpp`, `bounded_queue_test.cpp` | debug 35/35, tsan 35/35, asan 35/35; mutation check: `notify_one` in shutdown fails ShutdownWakesManyWaiters
  - STUDY: `not_full_.wait(lock, [this] { return closed_ || items_.size() < capacity_; })` (predicate
    re-checked after every wake-up, so spurious wake-ups are harmless); `wait_for(...)` returns the
    predicate's value, so `!ready` means timeout; `closed_` is checked after waking so shutdown wins
    over queued items; `shutdown()` uses `notify_all` on both condition variables; `push` takes
    `std::unique_lock` (the wait must unlock/relock) while `size()` uses `std::lock_guard`.
- P1.5 MetricsStore (per-id frame slots, preallocated depth series, `frame_capacity_for`): `metrics/metrics_store.hpp`, `metrics/metrics_store.cpp`, `metrics_store_test.cpp` | debug 44/44, tsan 44/44, asan 44/44; fixed a real bug found by the test: `ceil(30*60*1.1)` gave 1981 due to binary rounding
- P1.6 Queue depth callback after every push/pop, called under the queue lock: `bounded_queue.hpp`, `bounded_queue_test.cpp` | debug 48/48, tsan 48/48, asan 48/48
  - STUDY: `report_depth()` is called inside `push`/`pop` while `mutex_` is held, so two threads
    can never call the callback at the same time and samples follow the real order of changes;
    `if (on_depth_)` skips an empty `std::function`; the callback must never call the queue back
    (re-locking a non-recursive `std::mutex` on the same thread deadlocks).
- P1.7 CostModel fixed mode (busy/sleep) + ctest labels `timing`/`long`/`stress` and presets `debug-long`, `tsan-long` (D9, D10): `processor/cost_model.hpp`, `processor/cost_model.cpp`, `cost_model_test.cpp`, `tests/CMakeLists.txt`, `CMakePresets.json` | debug 52/52, tsan 52/52, asan 52/52, release 54/54 (incl. 2 `timing` PR-05 tests)
- P1.8 ISource + SyntheticSource (id-derived colour + moving bar): `source/source.hpp`, `source/synthetic_source.hpp`, `source/synthetic_source.cpp`, `synthetic_source_test.cpp` | debug 59/59, tsan 59/59, asan 59/59
- P1.9 SourceRunner (pacing, t_cap/lateness/t_push, stop in <= 50 ms steps, in-flight on failed push): `source/source_runner.hpp`, `source/source_runner.cpp`, `source_runner_test.cpp` | debug 68/68, tsan 68/68, asan 68/68
- P1.10 ISink + NullSink + Worker (stamps, cost, sink, record; throwing frame recorded in-flight): `sink/sink.hpp`, `sink/null_sink.hpp`, `worker.hpp`, `worker.cpp`, `worker_test.cpp` | debug 73/73, tsan 73/73, asan 73/73
- P1.11 Pipeline (threads, end by duration/frames/input end/stop/error, single shutdown, RAII join, in-flight drain) + `BoundedQueue::drain`, `SourceRunner::produced()` (count survives a source exception): `pipeline.hpp`, `pipeline.cpp`, `tests/integration/pipeline_test.cpp`, `tests/integration/invariants.hpp` | debug 86/86, tsan 86/86, asan 86/86; pipeline/worker/source tests repeated 30x under TSan: 30/30 clean
- P1.12 CSV reporter (`frames.csv`, `depth.csv`, locale-independent via `std::to_chars`) + `drop_reason_name`: `metrics/csv_reporter.hpp`, `metrics/csv_reporter.cpp`, `csv_reporter_test.cpp` | debug 93/93, tsan 93/93, asan 93/93; mutation check: `out << value` fails OutputIgnoresLocale (prints `1.234.567`)
- P1.13 Phase 1 integration (EC-02 cost-0, 0.5x, 2x blocking, frame count) + 60 s gate (`long`) + RR-02 200-cycle lifecycle (`stress`); fixed `tsan-long` preset (inherited exclude filter hid every test): `tests/integration/invariants_test.cpp`, `tests/stress/lifecycle_test.cpp`, `CMakePresets.json` | debug 97/97, tsan 97/97, asan 97/97; debug-long 4/4, tsan-long 4/4 (60 s gate run passed in both), asan long+stress 4/4 with leak detection
- P1.14 CI: matrix debug/asan/tsan + long-and-stress job (debug-long, tsan-long): `.github/workflows/ci.yml` | YAML parses (5 jobs); real CI run needs a push by the user
- Phase review fix: added `PipelineTest.EndsWhenInputEnds` (the `EndedBy::InputEnd` path had no test)

### Phase 1 summary

Built: injectable Clock/FakeClock; move-only Frame; BoundedQueue (blocking push, timed pop,
shutdown, depth callback, drain); MetricsStore; fixed CostModel; SyntheticSource; paced
SourceRunner; Worker + NullSink; Pipeline with one shutdown routine and RAII join; frames.csv and
depth.csv writer; integration, long and stress tests; CI matrix.

Clean build, all presets: debug 98/98, release 100/100 (incl. 2 `timing`), asan 98/98, tsan 98/98;
debug-long 4/4 and tsan-long 4/4 (60 s gate: 30 FPS, no drops, BR-01..BR-04 hold).

SPEC 16 Phase 1 gate: 60 s at 30 FPS with zero drops (passed in debug-long and tsan-long);
BR-01..BR-04 invariants on every integration run; TSan clean (full suite + long/stress).
Static checks: no raw new/delete in product code (one `new` in a test, required by the
`std::locale` facet API, which owns it); every condition-variable wait has a predicate; no I/O on
pipeline threads; steady_clock only inside `clock.*`; every header has a thread-safety comment.
Real run (scratch driver, 2 s, 1.5x overload, blocking): latency grows frame by frame and the
source falls behind its schedule while `t_cap` keeps the scheduled time; depth samples =
pushes + pops exactly. `frameflow` prints `frameflow 0.1.0`, exit 0 (CLI is Phase 2, D3).

Deviations from the docs and why:
- `try_push`/`try_pop` (P1.3) were replaced by blocking `push` / timed `pop` in P1.4; nothing
  needed the non-blocking forms, and `pop(0ms)` covers try-pop in tests.
- D13 (source stamps `t_push`), D15 (Frame move declared noexcept though cv::Mat's is not),
  D16 (`ISink::consume(Frame&)`), D17 (every end condition uses the same shutdown; queued frames
  are in-flight) are recorded in DESIGN.md.
- Frame 0 is always slightly late (about 1-2 ms: thread start plus generating the first frame),
  because t0 is taken before the source thread starts. Recorded honestly as lateness; ER-07 only
  warns above 5 ms on more than 1% of frames.
- CI was not run (needs a push); the workflow YAML parses.

## Phase 2
- P2.1 OverloadPolicy + PushDecision + QueueView + BlockingPolicy; queue asks the policy on every push (Insert into a full queue is treated as Wait); queue tests ported to a `meta`-carrying item (D18): `policy/overload_policy.hpp`, `policy/blocking_policy.hpp`, `bounded_queue.hpp`, `pipeline.*`, `blocking_policy_test.cpp`, `bounded_queue_test.cpp` | debug 104/104, tsan 104/104, asan 104/104
  - STUDY: the `while (!closed_)` loop in `push`: ask `policy_.on_push(view(), item->meta)`, insert
    only if the decision is Insert *and* there is space, otherwise `not_full_.wait(...)` and ask
    again, so the policy sees fresh state after every wake-up; `QueueView` is a copy taken under the
    lock, so the policy never touches the deque; the policy is held by reference and must outlive
    the queue (Pipeline declares `policy_` before `queue_`).
- P2.2 DropOldestPolicy, DropNewestPolicy; queue applies DropOldest/DropIncoming, sets `drop_reason`, hands dropped frames to a drop callback; `push` returns `PushResult`; `FrameMeta::t_drop` stamped by the pipeline's callback (FR-POL-10): `policy/drop_oldest_policy.hpp`, `policy/drop_newest_policy.hpp`, `bounded_queue.hpp`, `frame.hpp`, `pipeline.*`, `source_runner.cpp`, `drop_policies_test.cpp` | debug 113/113, tsan 113/113, asan 113/113
  - STUDY: in `push`, `DropIncoming && full` -> `drop(std::move(item), Newest)` and return Dropped
    (the caller's frame is gone); `DropOldest && full` -> move the head out, `pop_front()`, drop it,
    then fall through to the normal insert; `drop()` sets `item->meta.drop_reason` and calls the
    callback under the lock, so drop records are serialised; the source only touches its frame
    again when the result is `Shutdown`.
- P2.3 PolicyFactory (ordered one-line registry, case-sensitive, error lists valid names); Pipeline creates its policy from `PipelineConfig::policy`; pipeline runs under each fixed policy at 2x overload: `policy/policy_factory.hpp`, `policy/policy_factory.cpp`, `pipeline.*`, `policy_factory_test.cpp`, `pipeline_test.cpp` | debug 123/123, tsan 123/123, asan 123/123
- P2.4 Stats: nearest-rank `percentile`, `compute_run_stats` (whole-run counts; window rates, percentiles, depth after warm-up, D19), `RunResult::t_end`: `metrics/stats.hpp`, `metrics/stats.cpp`, `stats_test.cpp`, `pipeline.*` | debug 135/135, tsan 135/135, asan 135/135
- P2.5 RunReport (warm-up flags, drop events from `t_drop`, validity BR-09: invalid if stopped in warm-up or ended by error) + `events.csv`; `write_csv_files` now takes the report and writes all three CSVs: `metrics/run_report.hpp`, `metrics/run_report.cpp`, `metrics/csv_reporter.*`, `run_report_test.cpp`, `csv_reporter_test.cpp` | debug 142/142, tsan 142/142, asan 142/142
- P2.6 `summary.json` (DR-04 keys + `warnings`, `ended_by`, `error`; `null` for missing metrics; D22-D25) + platform `Environment` (CPU model, cores, RAM, kernel, compiler, OpenCV, FNV-1a hostname hash) + git commit embedded at configure time; nlohmann-json added to CMake and CI: `metrics/json_reporter.*`, `platform/environment.*`, `version.hpp.in`, `version.cpp`, `CMakeLists.txt`, `ci.yml`, `json_reporter_test.cpp`, `environment_test.cpp`, `version_test.cpp` | debug 178/178, tsan 178/178, asan 178/178
- P2.7 RawConfig -> RunConfig validation (VR-01..05, 07, 13, 15, 16, 20; one pass + one fail test per rule, all errors at once, UIUX 8.2 message format) with strict `parse_number`/`parse_unsigned` (`std::from_chars`): `config.hpp`, `config.cpp`, `config_test.cpp` | debug 160/160, tsan 160/160, asan 160/160; fixed a real bug found by the test (an invalid `--source` also produced a spurious "needs --input" error)
- P2.8 CLI11 `frameflow run`/`--help`/`--version` (table-driven flags in UIUX groups with defaults, unknown flag -> closest by edit distance, exit codes enum), S2 effective-config block, `run_command` (validate -> S2 -> create `--out` -> run -> four result files; exit 1/2/4), default `results/run_NNN` (D26-D28); `ended_by` uses SRS name `input_eof`: `cli.*`, `run_command.*`, `main.cpp`, `CMakeLists.txt`, `ci.yml`, `cli_test.cpp`, `run_command_test.cpp` | debug 197/197, tsan 197/197, asan 197/197; real 2 s run at 1.5x drop-oldest: produced 61 = delivered 40 + dropped 13 + in-flight 8
- P2.9a Root refusal (exit 5, reason printed; `--allow-root` warns), checked before anything else; unwritable `--out` parent names the path, exit 4: `platform/privileges.*`, `exit_codes.hpp`, `cli.*`, `run_command.*`, `privileges_test.cpp`, `run_command_test.cpp` | debug 225/225, tsan 225/225, asan 225/225
- P2.9b Output-folder safety (absolute path, existing refused without `--overwrite`, `--overwrite` removes only our 4 files, symlink anywhere in `--out` must resolve inside `$HOME`/cwd unless `--allow-any-out`, dirs 0755, files `O_EXCL|O_NOFOLLOW` 0644 written through the fd, disk full via `/dev/full` names file + reason) (D29-D32); CSV/JSON reporters now use `write_result_file`: `platform/output_dir.*`, `csv_reporter.*`, `json_reporter.*`, `run_command.cpp`, `output_dir_test.cpp`, `run_command_test.cpp` | debug 225/225, tsan 225/225, asan 225/225; mutation check: `O_TRUNC` instead of `O_EXCL|O_NOFOLLOW` fails ExistingResultFileIsNeverOverwritten and SymlinkResultFileIsNeverFollowed
- P2.10 Signals: `SignalWatcher` (signals blocked before any thread, `sigtimedwait` watcher thread; first SIGINT/SIGTERM -> `request_stop`, second -> `_exit(130)`), created first in `run_command` and kept alive through file writes (D33, D34): `platform/signals.*`, `run_command.cpp`, `tests/CMakeLists.txt` (binary path), `signal_watcher_test.cpp`, `integration/signal_test.cpp` (spawns `frameflow` via posix_spawn) | debug 236/236, tsan 236/236, asan 236/236; signal tests repeated 20x debug, 10x TSan: all pass; first TSan run failed the 1 s check for the forced exit (1006 ms, TSan's exit pause), fixed for the child only (D34)
- P2.11 Console summary S4 (whole-run counts + reason breakdown, after-warm-up latency/FPS/drop %/depth with a note, `—` empty states) and ER-07 warnings (late source, stopped in warm-up, warm-up > half, run < 10 s) on stderr and in `summary.json` (D35); fixed P2.4 `output_fps` to count deliveries by `t_out` in the window (D36; a real 4 s run at a 50 ms cost now shows out 20.0 fps instead of 18.3): `ui/console_reporter.*`, `metrics/warnings.*`, `metrics/stats.*`, `run_command.cpp`, `console_reporter_test.cpp`, `warnings_test.cpp`, `stats_test.cpp`, `run_command_test.cpp` | debug 249/249, tsan 249/249, asan 249/249
- P2.12 Phase 2 integration (D37): per-policy overload tests (2.0x: 40-60% drops; EC-03 10x: 80-97% drops; blocking latency >= (k+1) x cost - k x interval for every frame; dropping max latency < half of blocking's and not growing), SR-04 stress 3 x 20 s at 2.0x with queue 1, PR-10 timing test, CI smoke run per policy in each preset: `tests/integration/overload_test.cpp`, `tests/integration/result_writing_test.cpp`, `tests/stress/policy_stress_test.cpp`, `ci.yml` | debug 256/256, asan 256/256, tsan 256/256, release 259/259 (incl. 3 `timing`: PR-10 write of 1800 frames passed), debug-long 7/7, tsan-long 7/7 (60 s gate + 60 s policy stress, TSan clean); smoke commands run locally against debug/asan/tsan binaries: 9/9 exit 0 with valid summary.json, no sanitizer output

### Phase 2 summary

Built: OverloadPolicy + blocking/drop-oldest/drop-newest + factory; Stats (nearest-rank, warm-up
window); RunReport with validity and events.csv; summary.json (schema 1, environment with hashed
hostname, git commit); RunConfig validation; CLI11 `frameflow run`/`--help`/`--version` with S2
block; root refusal; output-folder safety (O_EXCL/O_NOFOLLOW, symlink rule, `--overwrite`);
SIGINT/SIGTERM handling (graceful, then exit 130); S4 console summary and ER-07 warnings.

Clean build, all presets: debug 256/256, asan 256/256, tsan 256/256, release 259/259 (incl. 3
`timing`); debug-long 7/7, tsan-long 7/7.

SPEC 16 Phase 2 gate: three fixed policies selectable from the CLI (`--policy`, smoke-run per
policy); unit tests for each policy and Stats; sustained overload shows growing latency for
blocking (hard per-frame lower bound) vs bounded latency for dropping (overload_test).
Static checks: no raw new/delete in product code; every condition-variable wait has a predicate;
no I/O on source/worker threads; steady_clock only inside `clock.*`; no system()/popen(); every
header has responsibility and thread-safety comments.

Deviations from the docs and why (all in DESIGN.md):
- D26/D27: default `--out` is `results/run_NNN` and S2 has no wall-clock time (no system_clock).
- D35: warnings go to stderr (ER-07) rather than inside the S4 block on stdout.
- D36: `output_fps` from P2.4 was counting a capture cohort; fixed to a delivery rate.
- `ended_by` uses `input_eof` (SRS EC-05) instead of the `input_end` first written in P2.6.
- CI was not run (needs a push); the workflow YAML parses and the smoke commands were run locally.

## Phase 2 fixes after review

- Lateness warning fired on ~91% of frames for blocking at 1.5x (user's manual run): the lateness
  came from backpressure (the source waiting in earlier pushes), not CPU. Added
  `FrameMeta::backpressure` + `BackpressureTracker` in the source loop; the ER-07 warning now uses
  `own_lateness()`; `frames.csv` unchanged (D38): `frame.hpp`, `source_runner.*`, `warnings.*`,
  `source_runner_test.cpp`, `warnings_test.cpp`, `integration/source_lateness_test.cpp` | debug
  266/266, tsan 266/266, asan 266/266; mutation check (`own_lateness()` returning the total) fails
  BlockingBackpressureAtOnePointFiveXDoesNotWarn and LatenessFromBackpressureDoesNotWarn; re-run of
  the reported case (12 s, blocking, 50 ms cost): no warning, while 89.2% of frames still have
  `lateness_us` > 5 ms in frames.csv

## Phase 3

- P3.1 Bursty CostModel (seeded offset, spikes of `len` every `every` from t0, `start(t0)` before the worker thread), `--spike-ms/--spike-len/--spike-every` with VR-06 and duration parsing, S2 + summary.json fields (D39): `processor/cost_model.*`, `pipeline.*`, `config.*`, `cli.cpp`, `json_reporter.cpp`, `cost_model_test.cpp`, `config_test.cpp` | debug 281/281
- P3.2 YAML `run --config` (template schema, values validated like flags, unknown key -> file:line:col + closest key, >1 MB file and >1 MB alias expansion rejected, flags override file) (D40): `config_file.*`, `cli.*`, `run_command.cpp`, `CMakeLists.txt` (yaml-cpp), `ci.yml`, `config_file_test.cpp`, `run_command_test.cpp` | debug 298/298, asan 298/298
- P3.3 Experiment files: `experiment`/`repeats`/`matrix` over the run schema, cartesian expansion (file order, last key fastest), stable config names, per-config validation, `experiment.json` builder (D41): `bench/experiment.*`, `config_file.*` (`set_run_value`), `experiment_test.cpp` | debug 311/311
- P3.4 `frameflow bench` + BenchRunner (`<out>/<experiment>/<config>/rep_<n>/`, progress line per run, median table with p95 min-max, `experiment.json` after every run, Ctrl+C keeps completed runs and lists missing) (D42); `run_into` extracted from `run_command` for reuse; spawn helper moved to `tests/integration/child_process.hpp`: `bench/bench_runner.*`, `cli.*`, `run_command.*`, `bench_test.cpp`, `signal_test.cpp` | debug 320/320, tsan 320/320; a real 2x2 bench run by hand: 4 runs completed, layout and experiment.json as specified
- P3.5 Experiment files `template.yaml` (no adaptive section until P4.5), `e1_baseline.yaml` (3 configs), `e2_sustained.yaml` (9), `e4_capacity.yaml` (10), `scripts/run_all.sh` (`--dry-run`, `--out`, `--binary`, `--only`; release binary; then analyze.py): `experiments/*`, `scripts/run_all.sh`, `experiment_test.cpp`, `tests/CMakeLists.txt` (`RunAllDryRun` ctest) | debug 326/326
- P3.6 `analyze.py` part 1: discovery, loading with file/column/line errors and schema check, `summary.csv`/`summary.md` per experiment (median/min/max over completed, valid repetitions) (D43): `scripts/analyze.py`, `scripts/tests/test_analyze.py`, `tests/CMakeLists.txt` (`AnalyzePytest`), `ci.yml` (python3 packages) | pytest 18/18 (incl. one on real bench output), debug 327/327
- P3.7 `analyze.py` part 2: `latency_over_time.png` and `depth_over_time.png` per run and per experiment/group (UIUX 6.3 styles, warm-up shading, footer), `--out` copy: same files | pytest checks PNG signature and size > 10 kB; one chart inspected by eye (tiny 2-policy bench: blocking climbs, drop-oldest flat after the queue fills, warm-up band, legend order)
- P3.8 `frameflow_bench_queue` (uncontended push+pop pair on `BoundedQueue<FramePtr>`, blocking policy, no callbacks; median/min/max over samples; args `[samples] [pairs]`): `bench/bench_queue.cpp`, `CMakeLists.txt` | builds in release and runs; no number recorded (PR-03 is measured in P5.9)
- P3.9 not run: E1 + E2 + E4 are 35 configurations x 5 repeats x ~61 s, about 1 h 50 min of wall-clock time, so the developer runs them (D11): `scripts/run_all.sh` (release build), then `python3 scripts/analyze.py results --out docs/images`

### Phase 3 summary

Built: bursty cost model with a seeded schedule; YAML config for `run --config` with flag
override; experiment files with matrix expansion; `frameflow bench` with per-run folders,
progress, median table, interruption handling and `experiment.json`; E1/E2/E4 + template;
`run_all.sh`; `analyze.py` tables and charts with pytest; the queue micro-benchmark tool.

Clean build, all presets: debug 327/327, asan 327/327, tsan 327/327, release 330/330 (incl. 3
`timing`); pytest 18/18 (part of ctest as `AnalyzePytest`).

SPEC 16 Phase 3 gate: not yet met. "E1, E2, E4 run end to end with 5 repeats" and "graphs in
docs/images" need the ~2 h run above (D11). The harness was checked end to end on a tiny
2x2 experiment (bench -> experiment.json -> analyze.py tables and charts).

Deviations from the docs and why (DESIGN.md D39-D43):
- `--spike-len` / `--spike-every` take `1s`, `500ms` or bare seconds (SPEC 13 example uses `1s`).
- One YAML schema for run and bench files; validation errors name the matching flag.
- `template.yaml` has no `adaptive:` section yet (the adaptive policy is Phase 4).
- `analyze.py` experiment charts use rep 1 of each configuration (the footer says so).

## Phase 4

Note: P4.1-P4.4 are "write it myself" tasks (D12). The user asked for Phase 4 in autonomous
mode, so Claude wrote the tests first, then the code, and left STUDY notes on the key lines, as in
P1.3/P1.4/P2.1/P2.2. Rewriting them by hand against the same tests is still possible.

- P4.1 LatencyWindow (ring buffer of W latencies, nearest-rank p95, all samples while fewer than W, none when empty): `policy/latency_window.hpp`, `latency_window_test.cpp` | debug 7/7 new tests
  - STUDY: `next_ = (next_ + 1) % samples_.size()` and `count_ = min(count_ + 1, W)` make the ring
    buffer; before it wraps the samples sit in slots `[0, count_)`, after that in all slots, so
    `begin() + count_` is right in both cases; `(95 * count_ + 99) / 100` is ceil(0.95 n) in
    integers; `nth_element` finds the rank-th smallest in O(W) without a full sort.
- P4.2 AdaptivePolicy state machine (enter L > T, exit L < T_low, nothing inside the band, change log with t/from/to/L): `policy/adaptive_policy.*`, `policy/overload_policy.hpp`, `adaptive_policy_test.cpp` (FakeClock) | debug, tsan, asan
  - STUDY: `on_delivered` computes `next` from the *current* state and only logs when
    `next != state_`, which is the whole hysteresis: one `if` per direction with different
    thresholds; one `std::mutex` guards state, window and log because `on_push` (source thread,
    inside the queue lock) and `on_delivered` (worker thread) run at the same time.
- P4.3 Shedding on push: `PushDecision::DropStale`, queue `drop_stale()` erases stale frames in order (reason `stale`), then drops the head if still full (reason `oldest`): `bounded_queue.hpp`, `adaptive_policy_test.cpp` | debug, tsan, asan
  - STUDY: `it = items_.erase(it)` is the safe way to remove while iterating a deque (erase
    invalidates `it`, so use the iterator it returns); the decision is applied under the same lock
    as the scan, so no frame can be popped half-way.
- P4.4 Worker: `t_pop`, then `should_drop_on_pop` (stale -> `drop_reason`, `t_drop = t_pop`, recorded, not processed), else process and `on_delivered(t_out - t_cap, t_out)`; policy passed to Worker; worker tests use a 2 s watchdog so a regression fails instead of hanging (D44): `worker.*`, `pipeline.cpp`, `worker_test.cpp` | debug 355/355, tsan 355/355, asan 355/355; mutation checks: dropping on pop in NORMAL fails NormalNeverDropsOnPop and 2 worker tests; `<=` instead of `<` for the exit fails ExactlyLowThresholdDoesNotExit
  - STUDY: the policy is called from the worker *outside* the queue lock (after `pop` returned),
    so there is only one lock order (queue -> policy) in the whole program; the latency fed back
    is `t_out - t_cap`, the same end-to-end latency the statistics use.
- P4.5 `adaptive` registered (factory now takes the clock and AdaptiveParams), `--target-ms/--exit-ratio/--window` with VR-08..VR-10 and YAML `adaptive:`, state changes in RunResult -> events.csv + `state_changes`/`shedding_pct` metrics, S2 parameters, S4 Adaptive row, summary.json fields; adaptive added to E1/E2/template, the stress test (now 4 x 20 s) and CI smoke runs; integration: adaptive at 2.0x sheds with BR-01..04 and max latency < half of blocking's, at 0.5x never drops (D45): `policy_factory.*`, `pipeline.*`, `run_report.*`, `stats.hpp`, `config.*`, `config_file.cpp`, `cli.cpp`, `console_reporter.cpp`, `json_reporter.cpp`, `bench/experiment.cpp`, `experiments/*`, `ci.yml`, tests | debug 373/373, tsan 373/373, asan 373/373; real 12 s run at 1.5x: 1 state change, 33.2% stale drops, p95 137 ms (T + cost, D45)
- P4.6 `e3_bursty.yaml` (3 configs), `e5_tuning.yaml` (9: T x T_low); `summary.json` `spike_offset_s`; `analyze.py`: spike windows, recovery time, drops outside spikes, adaptive state changes/shedding in tables, `latency_vs_drops.png`, `state_changes.png`, `--target-ms` (D46): `experiments/*`, `json_reporter.cpp`, `scripts/analyze.py`, `scripts/tests/test_analyze.py`, `experiment_test.cpp`, `json_reporter_test.cpp` | pytest 29/29 (incl. 2 on real bench output); debug 377/377, asan 377/377, tsan 377/377, release 380/380; mini bursty bench (2 reps x 8 s, debug build, scratch only) analysed end to end and the scatter inspected by eye
- P4.7 not run: E3 (3 x 5 x ~61 s) + E5 (9 x 5 x ~61 s) are about 1 h of wall-clock time, so the developer runs them (D11) with `scripts/run_all.sh --only e3_bursty --only e5_tuning`

### Phase 4 summary

Built: LatencyWindow (ring buffer p95); AdaptivePolicy with hysteresis, stale shedding on push
(`DropStale`) and on pop (worker), latency feedback, state-change log; `adaptive` in the factory,
CLI, YAML, S2/S4, summary.json and events.csv; adaptive in E1/E2/template; E3 and E5 experiment
files; analyze.py recovery time, drops outside spikes, state changes, trade-off scatter.

Clean build, all presets: debug 377/377, asan 377/377, tsan 377/377, release 380/380 (incl. 3
`timing`); debug-long 8/8, tsan-long 8/8 (60 s gate + 4 x 20 s policy stress incl. adaptive);
pytest 29/29.

SPEC 16 Phase 4 gate: "unit tests prove enter/exit and no flapping" met (FakeClock tests: enter
above T, exit below T_low, exact thresholds, band hovering, alternating 95/105 ms gives one
change; mutation checks). "E3 and E5 complete; tables filled" not met: needs the ~1 h run above
(D11); together with Phase 3's ~2 h, `scripts/run_all.sh` runs all five experiments.

Deviations from the docs and why (DESIGN.md D44-D46):
- `on_delivered(latency, now)` takes the time too; `PushDecision::DropStale` added.
- Adaptive options are rejected unless `--policy=adaptive` (VR-08's rule for all three), but a
  bench file's `adaptive:` section is applied only to its adaptive configurations.
- Adaptive delivered latency can reach about T + processing cost (staleness is judged before
  processing); a real 1.5x run showed p95 137 ms at T = 100 ms. To be reported, not hidden.
- P4.1-P4.4 ("write it myself") were written by Claude in autonomous mode, with STUDY notes.

## Before Phase 5: adaptive overshoot fix

- D47 predicted delivery age: stale = age + est_cost > T on pop and on push; est_cost = EWMA
  (alpha 0.2, from 0) of processing time, fed by `on_delivered(latency, processing, now)`:
  `overload_policy.hpp`, `adaptive_policy.*`, `worker.cpp`, `adaptive_policy_test.cpp` (+5 tests:
  EWMA values, pop and push rule, NORMAL still never drops, queue shedding by predicted age),
  `worker_test.cpp`, `policy_factory_test.cpp` | debug 382/382, tsan 382/382, asan 382/382;
  mutation (cost term removed) fails PopDropsFrameThatWouldBeLateOnDelivery,
  PushUsesTheSamePredictedAgeRule, SheddingUsesPredictedDeliveryAge; 1.5x smoke run re-run
  (same settings as D45, debug build, 2 runs): p95 137 ms -> 86.6 ms at T = 100 ms, drops 33%,
  out 20.0 fps

## Phase 5

- P5.1 VideoFileSource (open + first frame in the constructor, loop by reopening, `--no-loop`/`source.loop`, InputError -> exit 3 before threads, file name only in summary.json, S2 shows the video's size) (D48): `source/video_file_source.*`, `config.*`, `config_file.cpp`, `cli.*`, `run_command.*`, `bench_runner.cpp`, `json_reporter.cpp`, `CMakeLists.txt` (videoio), `video_file_source_test.cpp`, `run_command_test.cpp`, `config_test.cpp` | debug 394/394, asan 394/394
- P5.2 Decode-error handling: retry the next frame up to 3 times before the reported end, then InputError -> run error (exit 1, valid=false, partial results); end of file vs decode error by position vs frame count (D49); OpenCV behaviour measured first with a scratch program (truncated / corrupted clips): `source/video_file_source.*`, `video_file_source_test.cpp` (truncated, single corrupt frame, 60 fuzzed variants), `run_command_test.cpp` | debug 398/398, asan 398/398
- P5.3 IProcessor + none/grayscale/blur/canny + factory, `--processor`/`--kernel` (VR-11, VR-12), YAML `processor: {type, kernel}`, worker runs processor then cost, S2 and summary.json fields; OpenCV thread pool off and FFmpeg-only video backend (D50) after TSan reported false races inside TBB (Canny) and GLib (GStreamer fallback): `processor/*`, `worker.*`, `pipeline.*`, `config.*`, `config_file.cpp`, `cli.cpp`, `json_reporter.cpp`, `video_file_source.cpp`, `experiments/*`, `processor_test.cpp`, `pipeline_test.cpp`, `config_test.cpp`, `config_file_test.cpp`, `worker_test.cpp` | debug 410/410, tsan 410/410, asan 410/410; mutation: removing `setNumThreads(1)` fails TurnsOffOpenCvThreadPool
- P5.4 PreviewSink (HighGUI window behind IPreviewWindow, overlay per UIUX 6.2, 33 ms display throttle, q/Esc stop, Space freeze, o panel), `--preview[=auto]` with display check before threads (exit 6 / warning), `OverloadPolicy::shedding()` and `Pipeline::shedding()` (D51): `sink/preview_sink.*`, `platform/display.*`, `run_command.cpp`, `cli.*`, `pipeline.hpp`, `policy/*.hpp`, `preview_sink_test.cpp`, `run_command_test.cpp`, `cli_test.cpp` | debug 428/428, tsan 428/428, asan 428/428; tests found and fixed: border was 5 px (cv::rectangle centres thick lines), white PAUSED was invisible on white; overlay rendered to PNG and inspected; the real window still needs a demo by hand (no display here)
- P5.5 `--progress` (S3 line once per second on stderr from the main thread; atomics in MetricsStore; `Pipeline::set_tick`/`live_status`; warm-up badge; terminal vs pipe) (D52): `ui/progress.*`, `metrics_store.*`, `pipeline.*`, `platform/display.*`, `run_command.*`, `cli.*`, `progress_test.cpp`, `metrics_store_test.cpp`, `pipeline_test.cpp`, `run_command_test.cpp` | debug 438/438, tsan 438/438, asan 438/438; real 4 s adaptive run printed 3 lines (out 20.0 fps, queue 2/8, SHEDDING)
- P5.6 (file only) `experiments/e6_real_load.yaml` (bursty load + blur/canny at 720p/1080p, 4 policies, 16 configs); `overload_text` says "(cost only)" with a real processor (D53): `experiments/e6_real_load.yaml`, `console_reporter.*`, `cli.cpp`, `experiment_test.cpp`, `console_reporter_test.cpp` | debug 441/441; one configuration (1080p canny, adaptive, 6 s) smoke-run end to end. Not run: E6 is about 81 min, so the developer runs it (D11): `scripts/run_all.sh --only e6_real_load`
- P5.7 `.clang-tidy` (bugprone-*, cert-*, concurrency-* as errors; easily-swappable-parameters off), 97 findings fixed (incl. a C++20-only lambda capture, strerror -> generic_category, throwing statics, `Event` -> `RunEvent`), `scripts/check_tidy.sh`, CI job `lint` (D54) | clang-format clean, clang-tidy 0 findings on 34 product files; the script exits 1 when a NOLINT is removed (checked)
- P5.8 Coverage preset (`FRAMEFLOW_COVERAGE`), `scripts/coverage.py` (gcov JSON, merged over translation units, per-module report, `--min`), CI job `coverage` (D55): `CMakeLists.txt`, `CMakePresets.json`, `scripts/coverage.py`, `ci.yml` | coverage 441/441; measured on this machine: queue 100.0% (79/79 lines), policy 96.7% (119/123), stats 100.0% (71/71), all above RR-09's 85%; the 4 missed policy lines checked by hand (default virtuals and `name()`)
- P5.10 `.github/workflows/release.yml`: on a `v*` tag, check the tag equals the CMake project version, build + test the release preset, package `frameflow`, `frameflow_bench_queue`, README, experiments and scripts as `frameflow-<tag>-linux-x86_64-ubuntu24.04.tar.gz` with `SHA256SUMS`, create the GitHub release with `gh` | YAML parses; the version check and packaging commands run locally against `build/release` (tar lists the files, checksum written); a real run needs a tag push by the user
- P5.9a (tooling only) memory sampling into summary.json, lateness percentiles and memory in analyze.py tables, PR-04 metrics timing in `frameflow_bench_queue`, `experiments/perf/*.yaml`, `scripts/measure_perf.sh` (D56): `platform/environment.*`, `run_report.hpp`, `json_reporter.cpp`, `run_command.cpp`, `bench/bench_queue.cpp`, `scripts/analyze.py`, tests | debug 447/447, pytest 31/31, tidy clean, `MeasurePerfDryRun` ctest. Not measured: the runs take about 4 h on the reference machine (D11)
- P5.9b `scripts/soak.sh` (RR-01, SR-AUTH-01, valgrind; `--long` for RR-03); a first run failed at once because the script did not create the runs' parent folder (exit 4, VR-17), fixed | quick checks run on this machine with the release build (8 min 35 s): RR-01 100/100 exit 0 with conservation, strace: no network calls in 4 threads, valgrind: 0 errors, 0 bytes definitely lost. Not run: RR-03 (`--long`, about 4 h) and RR-04 (two E2 runs)
- P5.11 README (requirements with tested versions, build, quick start, outputs, exit codes, reproducing, layout), DESIGN.md overview (threads, ownership, synchronisation, invariants, modules, limitations), RESULTS.md skeleton with blank measurement cells and the pass/fail checks above: `README.md`, `docs/DESIGN.md`, `docs/RESULTS.md` | the clean-clone check following only the README is the developer's (TASKS verify)

### Phase 5 summary

Built: VideoFileSource with loop/no-loop and ER-03 retry; grayscale/blur/canny processors
(single-threaded OpenCV, FFmpeg-only decoding); preview window with overlay and keys;
`--progress`; E6; clang-tidy clean code base with a lint CI job; coverage preset and CI job;
release workflow; memory sampling, lateness percentiles and PR-04 timing for the measurements;
`measure_perf.sh` and `soak.sh`; README, DESIGN.md overview, RESULTS.md skeleton. Before Phase 5:
the adaptive overshoot fix (D47).

Clean build, all presets: debug 447/447, asan 447/447, tsan 447/447, release 450/450 (incl. 3
`timing`); debug-long 8/8, tsan-long 8/8; pytest 31/31 (in ctest); clang-format and clang-tidy
clean; coverage queue 100.0%, policy 96.7%, stats 100.0%; soak quick checks pass (RR-01 100/100,
no network calls, valgrind clean).

Not done (the developer's, D11 or by hand): P3.9, P4.7, P5.6 runs (`scripts/run_all.sh`, about
5 h); P5.9a measurements (`scripts/measure_perf.sh`, about 4 h); RR-03 / RR-04
(`scripts/soak.sh --long`, two E2 runs); filling docs/RESULTS.md from the produced files; the
preview window demo (P5.4); a clean-clone build from the README (P5.11); a real CI run and a
tag push for the release workflow.

Deviations from the docs and why (DESIGN.md D47-D56): staleness by predicted delivery age (D47);
the file is checked when opened, exit 3 (D48); decode error vs end of file by frame count (D49);
single-threaded OpenCV and FFmpeg only (D50); preview is a run-only option (D51); E6 covers the
bursty half of "repeat E2 and E3" (D53); one clang-tidy check disabled (D54); gcov instead of
lcov (D55); PR-04 judged by micro-benchmark analysis (D56).

## After Phase 5: preview threading fix

- D57 preview on the main thread (user's manual run: Qt thread warnings, 18.5 instead of 20.0 fps
  with `--preview`): `PreviewMailbox` (one slot, newest only, post never waits), `PreviewSink`
  posts a shallow image + overlay data and calls no HighGUI, `PreviewDisplay::pump()` on the main
  thread every 15 ms via `Pipeline::set_tick(tick, interval)`; window created/destroyed on the
  main thread; close button stops the run: `sink/preview_sink.*`, `pipeline.*`, `run_command.cpp`,
  `preview_sink_test.cpp` (mailbox newest-only, post never waits, concurrent post/take; sink
  posts every frame without copying pixels; display: newest only, q/Esc/close stop once, Space,
  o, grey frames) | debug 452/452, tsan 452/452, asan 452/452, clang-tidy clean. Reproduced
  before the fix (release, 10 s, 1.5x): 20.0 fps without / 18.5 with preview + 3 Qt warnings.
  After, 2 runs each: 20.0 / 20.0 fps; the Timers warnings are gone; one QSocketNotifier line
  remains, which a minimal single-threaded OpenCV program also prints on this Wayland session
  (silent with QT_QPA_PLATFORM=xcb, where a FrameFlow preview run printed nothing at 20.0 fps)

