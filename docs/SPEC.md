# FrameFlow Technical Specification

Detailed design reference for implementation. Product goals are in `docs/PRD.md`, testable requirements (IDs) in `docs/SRS.md`, architecture in `docs/ARCHITECTURE.md`, UI in `docs/UIUX.md`.

## 1. Overview

FrameFlow is a C++17 video-processing pipeline for Linux that keeps its output close to real time when processing cannot keep up with incoming frames. It runs a paced video source, a bounded frame queue and a processing worker on separate threads. When the queue is under pressure, a pluggable overload policy decides what happens: wait (blocking), discard stale frames (drop-oldest / drop-newest), or adapt based on measured latency (adaptive with hysteresis). Every frame is timestamped at capture, so the tool reports true end-to-end latency, throughput, queue depth and drop rate for each policy.

**What it does not do.** It does not make processing faster. Under sustained overload no policy can beat the throughput limit: if frames arrive at 2x the processing rate, about half must be dropped or delayed. FrameFlow manages *which* frames are lost and *how much latency* builds up.

**Core question.** When video processing cannot keep up with incoming frames, how can the pipeline keep output fresh without needlessly sacrificing useful frames?

## 2. Background and industry context

A camera at 30 FPS delivers a frame every 33.3 ms. A stage taking 50 ms per frame handles only 20 FPS, so 10 extra frames arrive every second.

- Unbounded queue: memory and latency grow forever.
- Bounded blocking queue: the queue stops growing but the producer stalls, so the backlog moves upstream and end-to-end latency still grows.
- Dropping queue: latency stays bounded but frames are lost.

Real systems face this: GStreamer's `queue` element has a `leaky` property (no leak / drop new / drop old); NVIDIA DeepStream builds GPU/Jetson video-analytics pipelines on GStreamer; ROS 2 uses "keep last N" queues so controllers act on fresh data.

| Scenario | What matters more | Likely best policy |
| --- | --- | --- |
| Factory safety camera | Freshness | Drop stale frames |
| Drone / robot vision | Freshness with stable control | Adaptive |
| Video recording | Completeness | Blocking |
| Video call | Freshness, smooth playback | Adaptive |
| Offline analysis | Completeness | Blocking |

FrameFlow does not assume this table is correct; its experiments measure each policy.

## 3. Goals, non-goals and success criteria

See PRD sections 3 and 7. Summary of success criteria:

- [ ] 60 s runs with no crashes, deadlocks or leaks (ASan, TSan, Valgrind clean).
- [ ] Under no overload, all policies deliver every frame.
- [ ] Under sustained overload, blocking shows growing latency; drop-oldest shows bounded latency with drops.
- [ ] Adaptive holds p95 at or below its target under bursty load (verified, not assumed).
- [ ] Full experiment matrix reruns from one script with the same conclusions.

## 4. Users and use cases

See PRD sections 2 and 6.

## 5. Key concepts and definitions

| Term | Definition |
| --- | --- |
| Capture time (t_cap) | Scheduled release time of a frame (`steady_clock`) |
| Output time (t_out) | When the sink finishes the frame |
| End-to-end latency | t_out - t_cap per delivered frame |
| Queue wait | t_pop - t_push |
| Processing time | t_proc_end - t_proc_start |
| Processing capacity | 1 / mean processing time (frames/s) |
| Overload factor | Input FPS / processing capacity |
| Drop rate | Dropped / produced (%) |
| p50 / p95 / p99 | Latency below which 50/95/99% of delivered frames fall |
| Sustained overload | Cost constant and above the frame interval |
| Bursty overload | Cost normally below the interval, with periodic spikes above it |
| Latency target (T) | Max frame age the adaptive policy aims for at p95 |
| Hysteresis | Two thresholds (enter above T, exit below T_low) so the policy does not flip every frame |

Minimum drop rate under sustained overload ≈ 1 - 1 / overload factor. At 2.0x, about 50% of frames must be dropped or delayed indefinitely, whatever the policy.

## 6. System architecture

```
 Paced source ──push──▶ Bounded queue ──pop──▶ Worker ──out──▶ Sink
 (thread 1,             (capacity N,           (thread 2,       (stamps t_out,
  stamps t_cap)          mutex + cond var)      blur/Canny/cost) preview or null)
                              │                     │               │
                       asks when full            stamps          stamps
                              ▼                     ▼               ▼
                     ┌─────────────────┐   ┌───────────────────────────────┐
                     │ Overload policy │──▶│ Metrics collector             │
                     │ blocking, drop- │   │ per-frame timestamps, drops,  │
                     │ oldest, drop-   │   │ queue-depth samples           │
                     │ newest, adaptive│   └───────────────┬───────────────┘
                     └─────────────────┘                   ▼
                                           Reports: frames.csv, depth.csv,
                                           summary.json → analyze.py graphs
```

### 6.1 Data flow for one frame

1. Source decodes/generates a frame, waits until its scheduled release, stamps `t_cap`.
2. Pushes the frame (`std::unique_ptr<Frame>`, moved, never copied) into the queue, stamps `t_push`.
3. If full, the policy decides: wait, drop oldest, drop incoming, or (adaptive) drop stale frames.
4. Worker pops (`t_pop`); in adaptive shedding mode it may discard a frame already older than the target.
5. Worker processes (`t_proc_start`, `t_proc_end`) and hands it to the sink, which stamps `t_out`.
6. Metadata goes into a preallocated metrics record; the pixel buffer is released.

### 6.2 Threading model

| Thread | Owns | Blocks on |
| --- | --- | --- |
| Main | Config, `Pipeline` lifetime, signals, final report | Joining other threads |
| Source (1) | Capture, pacing clock | `sleep_until`; full queue (blocking only) |
| Worker (2) | Processor, sink | Empty queue (timeout) |

The metrics collector is not a thread: each thread writes its own records, merged at the end, so measurement adds no lock to the hot path.

### 6.3 Shutdown

On duration end or Ctrl+C, main sets a stop flag and calls `queue.shutdown()`, waking every waiting thread. Source stops, worker drains or discards remaining frames, threads join via RAII, reporter writes results. No path may leave a thread waiting forever.

### 6.4 Extension points (post-MVP)

- More stages (decode → convert → filter → encode), each with its own queue and policy.
- Worker delegates frame strips to a thread pool.
- Frame-buffer pool supplies preallocated `cv::Mat` buffers.

## 7. Functional requirements

Full numbered requirements are in `docs/SRS.md` section 4. Phase tags used in this spec: v1 = MVP, v2 = adaptive differentiator, v3 = only if measurements justify it.

| Area | v1 | v2 | v3 |
| --- | --- | --- | --- |
| Source | File + synthetic, paced, timestamped, looping | | |
| Queue | Fixed capacity, thread-safe, policy-driven push, timed pop, clean shutdown | | |
| Worker | Grayscale/blur/Canny, fixed + bursty synthetic cost | | Thread-pool strips, AI stage |
| Policies | Blocking, drop-oldest, drop-newest | Adaptive with hysteresis | |
| Sink | Latency per frame, optional preview | File output | |
| Metrics | Per-frame CSV, depth series, summary JSON | | |
| Control | CLI + YAML, duration/frames, Ctrl+C, seed | | |

## 8. Overload policies

The adaptive policy is the main contribution; the three fixed policies are the baselines.

| Policy | When full | Keeps every frame? | Latency under sustained overload | Best for |
| --- | --- | --- | --- | --- |
| Blocking | Producer waits | Yes (within FrameFlow) | Grows; backlog moves upstream | Recording |
| Drop-oldest | Discard oldest, insert new | No | Bounded by capacity x processing time | Live monitoring |
| Drop-newest | Discard incoming | No | Bounded, but queued frames are older | Simple systems |
| Adaptive | Drop stale frames only when measured latency is over target | No, drops only when needed | Held near target | Bursty workloads |

### 8.1 Blocking

```
push(frame):
    lock
    wait on not_full until size < capacity or shutdown
    queue.push_back(move(frame)); notify not_empty
```

The paced source keeps its schedule, so when push blocks the source falls behind its own clock. Late frames keep their scheduled t_cap, so reported latency includes the upstream backlog (modelling a camera that keeps capturing).

### 8.2 Drop-oldest

```
push(frame):
    lock
    if size == capacity: dropped = queue.pop_front(); record_drop(dropped, "oldest")
    queue.push_back(move(frame)); notify not_empty
```

### 8.3 Drop-newest

```
push(frame):
    lock
    if size == capacity: record_drop(frame, "newest"); return
    queue.push_back(move(frame)); notify not_empty
```

### 8.4 Adaptive (latency-driven, with hysteresis)

Inputs: target `T` (e.g. 100 ms), exit threshold `T_low` (e.g. 0.7 x T), window `W` (e.g. last 30 delivered frames).

```
state = NORMAL
on each delivered frame: update L = p95 of last W delivered latencies

if state == NORMAL   and L > T:     state = SHEDDING
if state == SHEDDING and L < T_low: state = NORMAL

push(frame):
    NORMAL:   behave like blocking
    SHEDDING: drop queued frames with age > T; if still full, drop oldest

worker pop: if SHEDDING and popped frame age > T, drop it instead of processing (stale)
```

Why hysteresis: one threshold makes latency hovering around `T` flip the state every few frames. Two thresholds give a calm band.

Rules:
- Never claims to beat throughput limits; under sustained 2x overload it still drops about half.
- Value is measured in bursty load (drops only during spikes) and latency control (p95 near `T`).
- Every state change is logged with time and measured `L`.

### 8.5 Parameters

| Parameter | Default | Sweep |
| --- | --- | --- |
| Queue capacity | 8 | 2, 4, 8, 16, 32 |
| Target T | 100 ms | 50, 100, 200 ms |
| Exit threshold T_low | 0.7 x T | 0.5-0.9 x T |
| Window W | 30 frames | 10, 30, 60 |

## 9. Class design and C++ concepts

### 9.1 Core classes

| Class | Responsibility | Key C++ features |
| --- | --- | --- |
| `Frame` | `cv::Mat` + `FrameMeta` | Move-only (deleted copy, defaulted move), rule of 5 |
| `FramePtr` | `std::unique_ptr<Frame>`; ownership source → queue → worker → sink | Unique ownership |
| `ISource` / `VideoFileSource` / `SyntheticSource` | Produce paced frames | Abstract base, virtuals, `steady_clock`, `sleep_until` |
| `BoundedQueue<T>` | Thread-safe fixed-capacity queue | Class template, `std::deque`, `std::mutex`, `std::condition_variable`, `std::unique_lock` |
| `OverloadPolicy` | Push/pop behaviour when full/stale | Strategy pattern, pure virtuals |
| `BlockingPolicy`, `DropOldestPolicy`, `DropNewestPolicy`, `AdaptivePolicy` | Concrete policies | Inheritance, `override`, `final` |
| `PolicyFactory` | Create a policy by name | Factory, `std::unique_ptr<OverloadPolicy>`, `std::unordered_map<std::string, Creator>` |
| `IProcessor` / `GrayscaleProcessor` / `BlurProcessor` / `CannyProcessor` | Process one frame | Polymorphism, const correctness |
| `CostModel` | Fixed or bursty synthetic cost | `std::mt19937` with fixed seed |
| `Worker` | Pop, process, pass to sink | `std::thread` + `std::atomic<bool>` (or `std::jthread`/`stop_token`) |
| `ISink` / `NullSink` / `PreviewSink` | Consume frames | Interface |
| `MetricsCollector` | Per-frame records, depth samples | Preallocated `std::vector`, per-thread slots |
| `Stats` | Percentiles, FPS, drop rate | `std::sort`, `std::nth_element`, `std::accumulate` |
| `Reporter` / `CsvReporter` / `JsonReporter` | Write results | `std::ofstream`, RAII |
| `Config` | Parsed CLI + config file | `std::optional`, `std::string_view`, validation |
| `Clock` / `SteadyClock` / `FakeClock` | Time source, fakeable in tests | Interface, dependency injection |
| `Pipeline` | Own and wire all parts; start, run, stop | Composition, RAII (destructor joins threads), exception safety |

### 9.2 Key interfaces (sketch)

```cpp
struct FrameMeta {
    uint64_t id;
    Clock::time_point t_cap, t_push, t_pop, t_proc_start, t_proc_end, t_out;
    bool dropped = false;
    DropReason reason = DropReason::None;
};

class OverloadPolicy {
public:
    virtual ~OverloadPolicy() = default;
    // Called with the queue lock held when a push arrives.
    virtual PushDecision on_push(const QueueView& q, const FrameMeta& incoming) = 0;
    // Called by the worker after pop; may mark a stale frame to drop.
    virtual bool should_drop_on_pop(const FrameMeta& f, Clock::time_point now) { return false; }
    // Fed with each delivered frame's latency (adaptive uses it).
    virtual void on_delivered(Clock::duration latency) {}
    virtual std::string_view name() const = 0;
};

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity, OverloadPolicy& policy);
    PushResult push(T item);                                  // policy decides when full
    std::optional<T> pop(std::chrono::milliseconds timeout);
    std::size_t size() const;
    void shutdown();                                          // wakes all waiters
private:
    mutable std::mutex m_;
    std::condition_variable not_empty_, not_full_;
    std::deque<T> items_;
    std::size_t capacity_;
    OverloadPolicy& policy_;
    bool closed_ = false;
};
```

### 9.3 C++ concepts demonstrated

- **OOP:** interfaces (`OverloadPolicy`, `IProcessor`, `ISource`, `ISink`, `Reporter`), inheritance, runtime polymorphism, strategy and factory patterns, composition in `Pipeline`.
- **Memory and ownership:** `unique_ptr` transfer between threads, move semantics, move-only `Frame`, RAII for threads and files, no raw `new`/`delete`.
- **Templates:** `BoundedQueue<T>`, generic stats helpers.
- **Concurrency:** threads, mutex, condition variables with predicates (spurious wake-ups), atomics, deadlock-free shutdown.
- **Time:** `std::chrono` and `steady_clock` (never `system_clock`).
- **STL:** `deque`, `vector`, `unordered_map`, algorithms for percentiles.
- **Modern C++:** `optional`, `string_view`, `enum class`, `constexpr`, structured bindings.

## 10. Metrics and measurement methodology

| Metric | Computation | Unit |
| --- | --- | --- |
| End-to-end latency | t_out - t_cap per delivered frame | ms |
| Queue wait | t_pop - t_push | ms |
| Processing time | t_proc_end - t_proc_start | ms |
| p50/p95/p99/max | Sorted delivered latencies after warm-up | ms |
| Input / output FPS | Produced or delivered / measured duration | frames/s |
| Drop rate | Dropped / produced x 100, by reason | % |
| Queue depth | Sampled every push and pop; mean, max, series | frames |
| Recovery time (bursty) | End of spike until latency back below target | ms |
| Adaptive state changes | Count + timestamps | count |
| Peak memory | VmHWM from `/proc/self/status` | MB |

Rules:
1. `steady_clock` for every timestamp.
2. Exclude warm-up (default 3 s) from statistics; keep it in raw CSV.
3. 5 repeats per configuration; report median plus min and max.
4. Release build (`-O2`), same compiler and flags for every run.
5. Record environment in every result: CPU, cores, RAM, kernel, compiler, OpenCV, git commit.
6. Close heavy apps; optionally pin with `taskset` and note it.
7. No file I/O on pipeline threads during a run.
8. Measure metrics overhead once (with vs without recording).

## 11. Experiments

Result cells stay blank until measured. No number goes on a resume before it appears here.

### E1 Baseline (no overload)
30 FPS, cost 15 ms (0.5x), queue 8, 60 s, all policies. Expect zero drops; confirms the harness.

### E2 Sustained overload (main result)
30 FPS; overload 1.2x, 1.5x, 2.0x (cost 40/50/67 ms); queue 8; 60 s; 5 repeats. Graphs: latency over time per policy; drop rate vs overload.

| Overload | Policy | p50 | p95 | p99 | Out FPS | Drop % | Peak depth |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1.5x | Blocking | | | | | | |
| 1.5x | Drop-oldest | | | | | | |
| 1.5x | Drop-newest | | | | | | |
| 1.5x | Adaptive | | | | | | |

Expect: blocking latency grows; dropping policies stay bounded and lose roughly 1 - 1/overload.

### E3 Bursty overload (where adaptive should matter)
Base 20 ms; spikes to 80 ms for 1 s every 5 s; fixed seed; 60 s; 5 repeats.

| Policy | p95 | p99 | Drop % | Mean recovery (ms) | Drops outside spikes |
| --- | --- | --- | --- | --- | --- |
| Blocking | | | | | |
| Drop-oldest | | | | | |
| Adaptive | | | | | |

Hypothesis: adaptive drops only during and right after spikes. Report the result either way.

### E4 Queue capacity sweep
Capacities 2-32 at 1.5x, blocking and drop-oldest. Graph p95 and drop rate vs capacity.

### E5 Adaptive tuning
Bursty load; T ∈ {50, 100, 200}; T_low ∈ {0.5, 0.7, 0.9} x T. Graph drop rate vs p95, plus state switches.

### E6 Real processing load
Repeat E2 and E3 with Gaussian blur and Canny at 720p and 1080p.

### Optional (post-MVP)
Parallel strips (1/2/4/8 threads), buffer pool (allocations/s), CPU AI stage (detect every Nth frame).

Write `docs/RESULTS.md` from measured data: setup, environment, graphs, interpretation, limitations, negative results.

## 12. Non-functional requirements

See SRS sections 11-13. Key points: TSan/ASan/Valgrind clean; no deadlock on shutdown; frame conservation every run; metrics overhead < 1%; builds on Ubuntu 24.04 from apt without root; `-Wall -Wextra -Werror`; honest reporting.

## 13. CLI, configuration and output formats

```bash
# Single run
frameflow run --input=samples/traffic_720p.mp4 --fps=30 \
    --processor=blur --cost-mode=fixed --cost-ms=50 \
    --queue=8 --policy=adaptive --target-ms=100 --exit-ratio=0.7 \
    --duration=60 --warmup=3 --seed=42 --preview --out=results/run_001

# Synthetic source, bursty cost
frameflow run --source=synthetic --resolution=1280x720 --fps=30 \
    --cost-mode=bursty --cost-ms=20 --spike-ms=80 --spike-len=1s --spike-every=5s \
    --policy=drop-oldest --queue=8 --duration=60 --out=results/run_002

# Experiment matrix
frameflow bench --config=experiments/e2_sustained.yaml --repeats=5 --out=results/e2
```

```yaml
experiment: e2_sustained
source: { type: synthetic, resolution: 1280x720, fps: 30 }
processor: { type: synthetic }
duration_s: 60
warmup_s: 3
repeats: 5
seed: 42
matrix:
  cost_ms: [40, 50, 67]        # overload 1.2x, 1.5x, 2.0x at 30 FPS
  policy: [blocking, drop-oldest, drop-newest, adaptive]
  queue: [8]
adaptive: { target_ms: 100, exit_ratio: 0.7, window: 30 }
```

Output files and schemas: SRS section 6. Console layout: UIUX section 4.

## 14. Tech stack, environment and repository

### 14.1 Stack

C++17 (g++ 13), CMake 3.28+, OpenCV 4.6, GoogleTest, CLI11 (header-only), nlohmann/json, yaml-cpp, ASan/TSan/Valgrind, clang-format/clang-tidy, Python 3 + pandas + matplotlib, GitHub Actions.

### 14.2 Setup on Ubuntu 24.04

```bash
sudo apt update
sudo apt install -y build-essential cmake git libopencv-dev libgtest-dev \
    nlohmann-json3-dev libyaml-cpp-dev valgrind clang-format clang-tidy \
    python3-pandas python3-matplotlib
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

### 14.3 Repository structure

```
frameflow/
├── CMakeLists.txt
├── CMakePresets.json
├── CLAUDE.md
├── README.md
├── docs/
│   ├── PRD.md  SPEC.md  SRS.md  ARCHITECTURE.md  UIUX.md
│   ├── DESIGN.md            # decisions as built
│   ├── RESULTS.md           # measured results and graphs
│   └── images/
├── include/frameflow/
│   ├── clock.hpp
│   ├── frame.hpp
│   ├── bounded_queue.hpp
│   ├── policy/              # overload_policy, blocking, drop_oldest, drop_newest, adaptive, factory
│   ├── source/              # source, video_file_source, synthetic_source
│   ├── processor/           # processor, grayscale, blur, canny, cost_model
│   ├── sink/                # sink, null_sink, preview_sink
│   ├── metrics/             # metrics_collector, stats, reporter
│   ├── config.hpp
│   └── pipeline.hpp
├── src/                     # matching .cpp files + main.cpp
├── tests/
│   ├── unit/                # queue, policies, stats, cost model, config
│   ├── integration/         # full pipeline runs, invariants
│   └── stress/              # long runs under TSan
├── experiments/             # e1_baseline.yaml ... e6_real_load.yaml, template.yaml
├── scripts/
│   ├── run_all.sh
│   └── analyze.py
├── samples/                 # short, license-free test videos
└── .github/workflows/ci.yml
```

Create folders only when a phase needs them.

## 15. Testing strategy

Timing code is tested with invariants and a fake clock, never exact timings.

| Component | Unit tests |
| --- | --- |
| `BoundedQueue` | FIFO order; capacity never exceeded; pop timeout; `shutdown()` wakes blocked push and pop; move-only items |
| Blocking | Producer blocks when full, resumes after pop |
| Drop-oldest | Oldest id dropped, newest kept, reason recorded |
| Drop-newest | Incoming dropped, queue unchanged |
| Adaptive | Fake clock: enters SHEDDING above T, exits below T_low, no flapping inside the band; stale-on-pop only while shedding |
| `Stats` | Known arrays (1..100 → p95 = 95); empty input; warm-up exclusion |
| `CostModel` | Fixed constant; bursty spikes at configured period with fixed seed |
| `Config` | Invalid values rejected (queue 0, fps 0, T_low > T) |

Integration: short runs per policy; frame conservation; strictly increasing delivered ids; timestamp order; no drops at 0.5x; 40-60% drops at 2.0x for dropping policies.

Concurrency/memory: full suite + 60 s stress under TSan; ASan + UBSan; Valgrind short run; 200 create/destroy cycles.

CI: release build + tests; TSan; ASan; clang-tidy and clang-format.

## 16. Milestones and roadmap

Seven phases, each ending working and tested. Phases 0-5 are the complete project; 6 is optional.

### Phase 0: Setup (~3 days)
CMake + presets, folder skeleton, OpenCV smoke test, GoogleTest wired in, CI with one passing test.
- [ ] Builds from a clean clone on Ubuntu 24.04
- [ ] CI green

### Phase 1: Two-thread baseline (~1 week)
Clock + FakeClock, Frame/FrameMeta, `BoundedQueue<FramePtr>` with blocking push, SyntheticSource (paced), Worker with fixed busy-wait cost, NullSink, Pipeline with clean shutdown, per-frame timestamps, CSV output.
- [ ] 60 s at 30 FPS with no overload, zero drops
- [ ] Frame conservation and timestamp-order invariants pass (BR-01..BR-04)
- [ ] TSan clean

### Phase 2: Policies and metrics (~1 week)
`OverloadPolicy` interface, blocking, drop-oldest, drop-newest, factory, `Stats`, `summary.json`, console summary, CLI flags.
- [ ] Three fixed policies selectable from the CLI
- [ ] Unit tests for each policy and `Stats`
- [ ] Sustained overload shows growing latency (blocking) vs bounded (dropping)

### Phase 3: Benchmark harness (~1 week)
Bursty cost model, experiment YAML, `frameflow bench`, `run_all.sh`, `analyze.py` graphs.
- [ ] E1, E2, E4 run end to end with 5 repeats
- [ ] Graphs generated into `docs/images`

### Phase 4: Adaptive policy (~1-2 weeks)
Latency estimator, hysteresis state machine, stale-on-pop, state-change events, fake-clock tests.
- [ ] Unit tests prove enter/exit and no flapping
- [ ] E3 and E5 complete; tables filled

### Phase 5: Real load, polish, report (~1 week)
VideoFileSource, blur/Canny, E6, preview overlay, README, DESIGN.md, RESULTS.md.
- [ ] Every section 11 table filled from measured data
- [ ] A new user can reproduce one experiment from the README

### Phase 6 (optional): Extensions with a measured reason
Thread-pool strips, frame-buffer pool, AI stage, file output; each with its own before/after measurement.

Total phases 0-5: about 6-8 weeks part-time.

## 17. Risks and limitations

| Risk | Mitigation |
| --- | --- |
| Looks like a basic producer-consumer exercise | Adaptive policy + measured experiments are mandatory |
| Adaptive shows no benefit | Report honestly; E5 still shows the trade-off curve |
| Laptop timing noise | 5 repeats, medians, warm-up, Release, environment recorded |
| `sleep` cost unrealistic | Busy-wait cost by default; E6 with real filters |
| Shutdown deadlocks/races | Predicate waits, single shutdown path, TSan in CI, start/stop stress test |
| Metrics disturb timing | Preallocated records, no I/O on hot path, overhead measured |
| Scope creep | Phase gates |

Limitations (state in README): CPU only; file source emulates a camera; blocking backlog modelled by the source schedule; results are relative comparisons on one machine; under sustained overload no policy avoids frame loss.

## 18. Resume and interview mapping

Resume (fill only with measured numbers):

**FrameFlow: Latency-Aware Video Processing Pipeline** | C++17, Linux, OpenCV, Python
- Built a multithreaded C++ video pipeline with pluggable overload policies (blocking, drop-oldest, drop-newest, adaptive) using the strategy pattern and move-only frame ownership.
- Designed a latency-driven adaptive policy with hysteresis; under bursty overload it held p95 latency at **__ ms** with **__%** drops, vs **__ ms** for blocking and **__%** for drop-oldest.
- Built a reproducible benchmark harness (5 repeats, warm-up, p50/p95/p99, queue depth) across 6 experiments; race-free under ThreadSanitizer in CI.

Interview topics from your own code: producer-consumer; condition-variable predicates and spurious wake-ups; deadlock-free shutdown; mutex vs atomic; `unique_ptr` vs `shared_ptr`; move semantics; RAII; strategy/factory; vtables; rule of 0/3/5; `deque` vs `vector`; percentiles with `nth_element`; latency vs throughput; p95 vs mean; why bounded queues; why hysteresis; why not lock-free first.

Not covered by this project (prepare separately for the OA): paging/virtual memory, cache numericals, CPU architecture, bit manipulation, Linux kernel internals.

## 19. References

- GStreamer `queue` element (leaky modes): https://gstreamer.freedesktop.org/documentation/coreelements/queue.html
- NVIDIA DeepStream plugin overview: https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_Intro.html
- *C++ Concurrency in Action* (Anthony Williams): condition variables, thread-safe queues
- cppreference: `std::condition_variable`, `std::unique_ptr`, `std::chrono::steady_clock`
- OpenCV docs: `cv::VideoCapture`, `cv::GaussianBlur`, `cv::Canny`
- Gil Tene, "How NOT to Measure Latency"
- Related projects to study, not copy: VidPipe, VisionStream-CPP
