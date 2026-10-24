# FrameFlow PRD: Latency-Aware Video Processing

FrameFlow is a C++ video-processing pipeline for Linux that keeps output close to real time when processing cannot keep up with incoming frames. The MVP ships a two-thread pipeline, three fixed overload policies, one adaptive policy and a reproducible benchmark that measures the trade-offs. Full technical detail lives in `docs/SPEC.md`.

## 1. Problem statement

When video processing is slower than the incoming frame rate, frames pile up in buffers and the output shows the past instead of the present.

- A camera at 30 FPS delivers a frame every 33 ms. If processing takes 50 ms, 10 extra frames arrive every second.
- An unbounded buffer grows memory and latency forever. A bounded blocking buffer pushes the backlog upstream, so latency still grows. A dropping buffer keeps latency low but loses frames.
- In live systems (safety cameras, robots, drones) stale output can be worse than skipped frames, while in recording systems every frame matters.

Developers usually pick a buffering strategy by habit, without data on how it behaves under their workload. Frameworks such as GStreamer expose blocking and leaky queues, but leave the choice, and its measurement, to the developer.

**Opportunity:** a small, measurable pipeline that compares overload policies side by side, and an adaptive policy that drops frames only when measured latency exceeds a target.

## 2. Target users

The primary user is a developer building a real-time video application who needs evidence for how the pipeline should behave under load.

| User | Context | Primary need |
| --- | --- | --- |
| Video / computer-vision developer (primary) | Builds camera or streaming apps in C++ | Choose a buffering policy based on measured latency and frame loss |
| Robotics / embedded engineer | Runs perception on limited CPUs | Keep output within a latency budget |
| Performance engineer | Investigates lag in an existing pipeline | See where latency builds up and why |
| Student / learner | Studies concurrency and real-time systems | A readable reference implementation with experiments |

## 3. Goals and non-goals

The product goal is to make the latency-versus-frame-loss trade-off visible and controllable, backed by reproducible measurements.

### Goals

1. **Bound latency under overload.** Let the user cap end-to-end latency with a configurable policy.
2. **Compare policies fairly.** Run identical workloads under each policy and report the same metrics.
3. **Adapt to changing load.** Provide an adaptive policy that drops frames only when measured latency exceeds a target.
4. **Make results reproducible.** One command reruns an experiment and produces the same conclusions.
5. **Stay correct under concurrency.** No data races, deadlocks or leaked frames.

### Non-goals

- Making processing itself faster (FrameFlow manages overload; it does not optimise filters).
- Beating the throughput limit: under sustained 2x overload about half of frames are lost under any policy.
- Competing with production frameworks such as GStreamer or DeepStream.

## 4. Core features

Six features make up the product; each one exists to create, control or measure overload.

| # | Feature | What it does | Why it is needed |
| --- | --- | --- | --- |
| F1 | Paced frame source | Releases frames from a video file or a synthetic generator at a fixed FPS and timestamps each one at capture | Emulates a live camera; files otherwise decode faster than real time |
| F2 | Bounded frame queue | Thread-safe, fixed-capacity queue between source and worker | Caps memory and is where overload decisions happen |
| F3 | Processing worker with cost control | Applies a real filter (blur, edge detection) or a configurable synthetic cost, fixed or bursty | Creates controlled, repeatable overload |
| F4 | Overload policies | Blocking, drop-oldest, drop-newest and adaptive (latency target with hysteresis), selectable at runtime | The product's core: different answers to the same overload |
| F5 | Metrics and reports | Per-frame timestamps; p50/p95/p99 latency, output FPS, drop rate, queue depth; CSV and JSON output | Turns behaviour into comparable numbers |
| F6 | Benchmark runner | Runs an experiment matrix from a config file with repeats and a fixed seed; Python script draws graphs | Makes results reproducible and shareable |

## 5. MVP scope

The MVP is one source thread, one bounded queue, one worker thread, four policies and a benchmark that answers the core question; everything else waits for a measured reason.

### In the MVP

| Area | Included |
| --- | --- |
| Source | Video file (OpenCV) and synthetic generator; paced at a configured FPS; capture timestamp |
| Pipeline | Single stage: source → bounded queue → worker → sink (null sink, optional preview window) |
| Processing | Gaussian blur and Canny edges; synthetic fixed and bursty cost modes |
| Policies | Blocking, drop-oldest, drop-newest, adaptive with hysteresis |
| Metrics | End-to-end latency percentiles, output FPS, drop rate by reason, queue depth; CSV + JSON |
| Benchmark | YAML experiment matrix, 5 repeats, warm-up, fixed seed; Python graphs |
| Interface | Command-line tool (`frameflow run`, `frameflow bench`) |
| Quality | Unit and integration tests; ThreadSanitizer and AddressSanitizer clean; CI on Ubuntu 24.04 |

### After the MVP (only with a measured justification)

1. Multi-stage pipeline (decode → convert → filter → encode), each stage with its own queue and policy.
2. Strip-parallel processing on a thread pool.
3. Reusable frame-buffer pool to cut allocation overhead.
4. CPU-based AI inference stage as a realistic slow workload.
5. Writing processed video to a file.

## 6. User stories

Ten stories cover the MVP; each maps to a feature and to an acceptance criterion in section 11.

| ID | As a… | I want to… | So that… | Feature |
| --- | --- | --- | --- | --- |
| US-1 | Video developer | feed a video file at a fixed frame rate | my tests behave like a live camera | F1 |
| US-2 | Video developer | use a synthetic source with a fixed seed | I can repeat a test exactly | F1 |
| US-3 | Video developer | set the processing cost or pick a real filter | I can create a known level of overload | F3 |
| US-4 | Video developer | simulate bursty processing spikes | I can test behaviour under realistic, uneven load | F3 |
| US-5 | Video developer | choose an overload policy from the command line | I can compare policies without changing code | F4 |
| US-6 | Robotics engineer | set a latency target for the adaptive policy | output stays fresh enough for my control loop | F4 |
| US-7 | Performance engineer | see p50/p95/p99 latency, output FPS, drop rate and queue depth after each run | I understand how the pipeline behaved | F5 |
| US-8 | Performance engineer | export per-frame data as CSV and a summary as JSON | I can analyse results in my own tools | F5 |
| US-9 | Any user | run a full experiment matrix with one command | results are reproducible and comparable | F6 |
| US-10 | Any user | stop a run with Ctrl+C and still get results | I never lose a long run's data | F5 |

## 7. Success metrics

The MVP succeeds when the targets below are met on the test machine; measured values are recorded in the results report, never estimated.

### Product outcome metrics

| Metric | Target | How measured |
| --- | --- | --- |
| Latency control (adaptive, bursty load) | p95 end-to-end latency at or below the configured target (default 100 ms) | Experiment E3, median of 5 runs |
| Latency bound (drop-oldest, sustained 1.5x overload) | p95 latency stays below queue capacity x processing time | Experiment E2 |
| Blocking behaviour shown | Latency grows over the run under sustained overload | Latency-over-time graph, E2 |
| Unnecessary drops avoided (adaptive) | Fewer drops outside load spikes than drop-oldest | Experiment E3 (reported either way) |
| Reproducibility | Repeat runs agree on policy ranking; p95 spread reported | 5 repeats per configuration |

### Quality metrics

| Metric | Target |
| --- | --- |
| Data races / memory errors | 0 under ThreadSanitizer and AddressSanitizer |
| Frame accounting | produced = delivered + dropped + in-flight, every run |
| Measurement overhead | Under 1% of processing time |
| Shutdown | Exits within 1 s of Ctrl+C with results written |
| Test coverage | Every policy and the queue have unit tests; integration tests for all four policies |

## 8. Assumptions

These assumptions shape the MVP; if one proves false, the related scope is revisited.

1. **Platform:** Ubuntu 24.04 on a standard x86-64 laptop or desktop; all dependencies install from apt; no GPU required.
2. **Camera emulation:** a paced file or synthetic source is a fair stand-in for a live camera for comparing policies.
3. **Synthetic cost is representative enough:** fixed and bursty delays reproduce the overload patterns of real filters; experiment E6 checks this with real blur and Canny work.
4. **Mutex-based queue is fast enough:** at video frame rates (tens of frames per second) lock overhead is negligible compared with processing time.
5. **Relative, not absolute, results:** conclusions compare policies on one machine; absolute numbers differ across hardware.
6. **Single stage is enough to answer the core question:** multi-stage behaviour is a later extension.
7. **Users are comfortable with a command line** and with reading CSV/JSON output or the generated graphs.

## 9. Risks and mitigations

The highest risk is a result-free project; the benchmark and the honest reporting of outcomes are the mitigation.

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| Adaptive policy shows little or no benefit | Medium | High | Report the result honestly; the tuning sweep (target and hysteresis) still shows the trade-off curve |
| Timing noise from CPU scaling and background load | High | Medium | Release build, warm-up, 5 repeats, medians, recorded environment; optional performance CPU governor |
| Synthetic cost judged unrealistic | Medium | Medium | Repeat key experiments with real blur and Canny processing |
| Deadlock or race on shutdown | Medium | High | Predicate-based waits, single shutdown path, ThreadSanitizer in CI, start/stop stress test |
| Measurement disturbs what it measures | Low | Medium | Preallocated records, no file I/O during runs, overhead measured |
| Scope creep (thread pools, AI, lock-free) | High | High | Strict MVP list; extensions only after acceptance criteria pass |
| Project seen as a basic threading exercise | Medium | High | The adaptive policy and measured experiments are part of the MVP, not optional |

## 10. Out of scope

The following are explicitly excluded from the MVP to keep it focused on the core question.

| Excluded | Reason |
| --- | --- |
| GPU, CUDA or hardware-accelerated decode | Not needed to study overload policies; adds hardware dependence |
| Live camera input | File and synthetic sources give repeatable tests |
| Custom codecs or decoding from scratch | OpenCV handles decode; not the problem being solved |
| GUI or web dashboard | CLI plus generated graphs is enough; a preview window is optional |
| Lock-free queues | Only justified if profiling shows the mutex queue is a bottleneck |
| Multi-stage pipelines, thread pools, buffer pools, AI inference | Post-MVP extensions, each needing a measured reason |
| Network streaming (RTSP, WebRTC) | Adds network effects unrelated to the core question |
| Windows or macOS support | Linux (Ubuntu 24.04) only |

## 11. Acceptance criteria

The MVP is accepted when every box below is ticked on Ubuntu 24.04 from a clean clone.

### Source and processing (US-1 to US-4)

- [ ] Given a video file and `--fps=30`, frames are released every 33 ms (±2 ms median) regardless of decode speed.
- [ ] Given `--source=synthetic --seed=42`, two runs produce identical frame and spike schedules.
- [ ] Given `--cost-ms=50`, measured mean processing time is 50 ms (±5%).
- [ ] Given bursty settings, spikes occur at the configured period and duration.

### Policies (US-5, US-6)

- [ ] `--policy` accepts `blocking`, `drop-oldest`, `drop-newest` and `adaptive`; an unknown name fails with a clear error.
- [ ] Blocking: no frames dropped inside FrameFlow; under sustained 1.5x overload, latency rises across the run.
- [ ] Drop-oldest: queue never exceeds capacity; the oldest frame is the one dropped; p95 latency stays bounded.
- [ ] Drop-newest: when full, the incoming frame is dropped and queue contents are unchanged.
- [ ] Adaptive: enters shedding mode above the target, exits below the lower threshold, and does not switch state inside the band (verified by unit test with a controllable clock).
- [ ] Adaptive under bursty load: p95 latency at or below the target in at least 4 of 5 runs.

### Metrics and reporting (US-7, US-8, US-10)

- [ ] Every run writes `frames.csv`, `depth.csv` and `summary.json`, with config and environment recorded.
- [ ] Console summary shows p50/p95/p99 latency, output FPS, drop rate by reason and peak queue depth.
- [ ] For every run: produced = delivered + dropped + in-flight.
- [ ] Ctrl+C exits within 1 s and still writes all result files.

### Benchmark (US-9)

- [ ] `frameflow bench --config=<file>` runs the full matrix with the configured repeats.
- [ ] `scripts/analyze.py` produces latency-over-time and summary graphs from the results.
- [ ] Rerunning an experiment gives the same policy ranking.

### Quality

- [ ] All unit and integration tests pass in CI.
- [ ] ThreadSanitizer and AddressSanitizer runs report zero errors.
- [ ] README explains setup, usage, architecture and measured results, including limitations.
