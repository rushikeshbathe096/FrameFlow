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
