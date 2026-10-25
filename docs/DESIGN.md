# FrameFlow Design Notes (as built)

## Decisions

Resolved during planning (see `docs/TASKS.md`). Where these contradict PRD, SPEC, SRS,
ARCHITECTURE or UIUX, these decisions win.

| ID | Topic | Decision | Supersedes / clarifies |
| --- | --- | --- | --- |
| D1 | Threads | `std::thread` + `std::atomic<bool>` stop flag + join in a destructor. No `std::jthread` (C++20). | SPEC 9.1, ARCHITECTURE 4.3 |
| D2 | Names | Policy hook is `should_drop_on_pop`; metrics class is `MetricsStore`. | ARCHITECTURE 6.2 (`drop_on_pop`), SPEC 9.1 (`MetricsCollector`) |
| D3 | Phase 1 run | The Phase 1 gate (60 s at 30 FPS) is an integration test labelled `long`; `main()` gains a CLI only in Phase 2. | SPEC 16 Phase 1 |
| D4 | Clock vs timeouts | `Clock` is used for every timestamp and for pacing. Queue timeouts use `condition_variable::wait_for` with real durations (steady clock inside the standard library) and are tested with lower bounds only. | CLAUDE.md "steady_clock via Clock" |
| D5 | Bursty seed | `--seed` seeds `std::mt19937`, which picks the first spike's offset in `[0, spike-every)`; spikes then repeat exactly every `spike-every`. | SRS FR-PROC-04 |
| D6 | UIUX extras | `--progress` is in scope (Phase 5). `--json`, `--no-color`/`NO_COLOR`, `--verbose`/`--trace`, short aliases, terminal-width handling and colours are out of scope. | UIUX 3, 10, 11, 12.2; ARCHITECTURE 10 |
| D7 | Python tests | `analyze.py` is tested with pytest (`python3-pytest` from apt); all C++ is tested with GoogleTest. | CLAUDE.md "GoogleTest tests" |
| D8 | Disk full | EC-17 is simulated by writing to `/dev/full`, not a tmpfs (mounting needs root). | SRS EC-17 |
| D9 | Timing tests | The PR-05 cost-accuracy test runs only in the release preset, under ctest label `timing`. | SPEC 15 "never exact timings" |
| D10 | Long tests | 60 s runs, stress and lifecycle tests use ctest labels `long`/`stress`, are excluded from the default run, and run in a separate CI job. | SPEC 15 |
| D11 | Experiments | The developer runs the multi-hour experiments on the reference machine; numbers in RESULTS.md come only from produced output files. | SPEC 11, CLAUDE.md |
| D12 | "Write it myself" tasks | Claude writes the GoogleTest tests first and explains them; the developer writes the code; Claude reviews it. | docs/TASKS.md |
| D13 | Stamping `t_push` | The source stamps `t_push` immediately before calling `push`, while it still owns the frame. `BoundedQueue<T>` stays generic and needs no hook for it. Consequence: `t_push` is the push *attempt* time, so under blocking `t_pop - t_push` includes time spent waiting for space; BR-04 still holds. | SPEC 6.1 step 2 |
| D14 | FakeClock sleeping | `FakeClock::sleep_until(t)` never blocks: it moves fake time to `max(now, t)` and returns, so a test can never hang waiting for someone to advance the clock. Its time is guarded by a `mutable std::mutex` so `now() const` is thread-safe. | Independent review assumption of a blocking fake |
| D15 | Frame move is `noexcept` | `Frame`'s move operations are declared `noexcept = default` even though OpenCV 4.6 does not mark `cv::Mat`'s move `noexcept`. GCC accepts this (P1286R2), and a `cv::Mat` move only swaps pointers and never allocates, so the promise holds; if it ever threw, `std::terminate` would be called. Frames move as `FramePtr` in the pipeline, so this affects only direct `Frame` moves. | Correction of the P1.2 explanation |
| D16 | Sink interface | `ISink::consume(Frame&)` instead of `consume(FramePtr)`: the sink stamps `t_out`, the worker keeps ownership and records the frame, then frees it. One place (the worker) records delivered frames, so frame accounting (BR-01) stays simple. | ARCHITECTURE 6.2 |
| D17 | How a run ends | Every end condition (duration, `--frames` count reached, input end, stop request, thread error) goes through the same shutdown routine. Frames still queued at that moment are recorded as in-flight; the worker does not drain the queue first. With `--frames=N`, N frames are released and the last few are usually in-flight. | SRS FR-CLI-05, BR-01 |
| D18 | Queue item type | `BoundedQueue<T>` stays a class template, but T must be pointer-like with a `meta` (FrameMeta) member (e.g. `FramePtr`), because the policy decides from the incoming frame's metadata. Queue unit tests use a minimal `std::unique_ptr<TestItem>` instead of `int`. | SPEC 9.2 (generic `T`) |
| D19 | What warm-up excludes | Frame counts (produced, delivered, dropped, in-flight) cover the whole run so conservation (BR-01) can be checked against them. Latency percentiles, input/output FPS, drop rates and queue-depth statistics use only frames with `t_cap` (and depth samples with `t`) at or after `t0 + warmup`, over the window `[t0 + warmup, t_end)` (BR-06). Mean depth is the plain mean of the samples taken at every push and pop. | SRS BR-01, BR-06, FR-MET-04; UIUX S4 |
| D20 | `--source=file` before Phase 5 | The default source stays `file` (VR-15), but until the video source exists (P5.1) a file source is rejected with "is not available yet (hint: use --source=synthetic)" instead of failing later. | SRS VR-15, SPEC 16 |
| D21 | Default warm-up vs short runs | VR-04 requires warm-up < run length. An explicit `--warmup` that breaks this is an error; the *default* 3 s warm-up is lowered to 0 for runs of 3 s or less, so `--duration=2` alone is valid. | SRS VR-04 |

## Testing and sanitizers

**Why the tsan preset runs tests under `setarch -R`.** GCC 13's TSan runtime crashes at
startup on recent kernels with high ASLR entropy (`vm.mmap_rnd_bits`); even an empty `main()`
segfaults. The tsan preset sets `CMAKE_CROSSCOMPILING_EMULATOR` to `setarch;-R`, so ctest and
`gtest_discover_tests` run test binaries with ASLR disabled for that process only. No system
setting is changed. Alternatives: `sudo sysctl vm.mmap_rnd_bits=28` (machine-wide), or a
newer toolchain whose TSan runtime handles the higher entropy.

**Why `tsan.supp` suppresses only libtbb.** OpenCV uses TBB as its parallel backend. TBB is
not built with TSan, so TSan cannot see its internal synchronization and reports false races
inside it (seen in the OpenCV `GaussianBlur` smoke test). `called_from_lib:libtbb.so` ignores
only calls made from inside TBB. Nothing in FrameFlow code is suppressed. Cost: a real race
whose accesses happen inside TBB (e.g. freeing a `cv::Mat` while OpenCV still reads it on a
pool thread) would also be hidden, so ASan runs and clear frame ownership still matter.

**Deliberate-race check.** A temporary test had two `std::thread`s increment a plain `int`
captured by reference. The tsan preset reported `ThreadSanitizer: data race` with both stacks
at the `++counter` line, and `halt_on_error=1` made the test fail. After the test was removed
the suite passed again. This proved that races in our own code are still detected with
`tsan.supp` loaded, and that a race fails CI instead of only printing a warning. The test was
not kept.

**Limitation: TSan only sees executed paths.** It checks the happens-before order of accesses
that actually run, so it does not depend on lucky timing, but a race on a path no test
executes (e.g. shutdown while the queue is full, pop timeout) is invisible. Concurrency tests
must therefore exercise shutdown, timeout and overload paths, not just the happy path.
