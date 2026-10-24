# FrameFlow System Architecture

FrameFlow is a single local C++ executable with a small Python analysis script: no web frontend, no server and no database, because the problem (measuring overload behaviour on one machine) needs none of them. This document explains the components, interfaces and operational choices; requirements live in `docs/SRS.md`.

## 1. Architecture overview

FrameFlow is organised in five layers inside one process; only the engine layer runs on the time-critical threads, and the queue with its overload policy is the core of the design.

```
+-------------------------------------------------------------------+
| Interface    | CLI (run, bench) | Preview window | analyze.py      |
+-------------------------------------------------------------------+
                         | commands
+-------------------------------------------------------------------+
| Control      | Config + validation   | Pipeline + BenchRunner      |
+-------------------------------------------------------------------+
                         | RunConfig, start/stop
+-------------------------------------------------------------------+
| Engine       | Paced source -> [Queue + policy] -> Worker -> Sink  |
+-------------------------------------------------------------------+
                         | frame records
+-------------------------------------------------------------------+
| Measurement  | Metrics store | Stats (p50/p95/p99) | Reporters     |
+-------------------------------------------------------------------+
                         | built on
+-------------------------------------------------------------------+
| Platform     | OpenCV 4.6 | C++17 std library | Linux file system  |
+-------------------------------------------------------------------+
```

Each layer depends only on the one below it: the interface issues commands, the control layer builds and runs pipelines, the engine moves frames, the measurement layer turns timestamps into results, and the platform supplies OpenCV, the C++ standard library and the file system.

| Layer | Runs on | Time-critical? |
| --- | --- | --- |
| Interface | Main thread (CLI), sink side (preview), separate process (`analyze.py`) | No |
| Control | Main thread | No |
| Engine | Source thread + worker thread | Yes |
| Measurement | Slots written by engine threads; stats and reports on main thread after join | Writes only |
| Platform | Shared libraries and the OS | n/a |

## 2. Recommended tech stack

Every choice is the simplest tool that meets the requirements on Ubuntu 24.04; nothing needs a server, container runtime or GPU.

| Layer | Choice | Why | Rejected alternative |
| --- | --- | --- | --- |
| Language (core) | C++17 (g++ 13) | Precise control of threads, memory and timing; matches the target role | Python (GIL, timing noise), Rust (less relevant to the JD) |
| Concurrency | `std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic` | Standard, portable, easy to reason about and test with TSan | Lock-free libraries, TBB (premature for 30 FPS workloads) |
| Video and imaging | OpenCV 4.6 (`libopencv-dev`) | Decode, blur, Canny and preview window in one apt package | FFmpeg API directly (more code), GStreamer (hides the problem being studied) |
| CLI parsing | CLI11 (header-only) | Typed flags, validation, subcommands | Hand-written `getopt` parsing |
| Config | yaml-cpp (`libyaml-cpp-dev`) | Readable experiment matrices | JSON configs (no comments) |
| Result output | Plain CSV + nlohmann/json (`nlohmann-json3-dev`) | Readable by any tool, diff-able, no server | SQLite, time-series DB |
| Build | CMake 3.28 + CMake presets | Standard, supports sanitizer presets | Make, Bazel |
| Tests | GoogleTest (`libgtest-dev`), pytest for the script | De facto standards | Catch2 (fine, but one framework is enough) |
| Quality | ASan, UBSan, TSan, clang-tidy, clang-format | Catch memory and race bugs early | Manual review only |
| Analysis | Python 3.12, pandas, matplotlib | Fast plotting from CSV | Custom C++ plotting, web dashboards |
| CI | GitHub Actions `ubuntu-24.04` | Free, matches the target OS | Self-hosted CI |

## 3. Frontend (user interface)

The "frontend" is the terminal, an optional OpenCV preview window and the generated graphs; a web UI was considered and rejected because it adds a server and a browser without helping answer the core question.

| Interface | Technology | Purpose |
| --- | --- | --- |
| Command line | `frameflow run`, `frameflow bench` (CLI11) | Configure and start runs and experiments |
| Console output | stdout summary, stderr warnings/errors | Immediate feedback after each run |
| Preview window (optional) | OpenCV `imshow` on the sink thread, overlay with frame age, FPS, drops | Visual demo: see live vs lagging output |
| Graphs and tables | `scripts/analyze.py` → PNG and Markdown | Compare policies; feed the README and results report |

Design rules:

- The preview window runs on the sink side only and never blocks the worker; if the display is slow, preview frames are skipped, not queued.
- Preview is excluded from benchmark runs by default so drawing cost never affects measurements.
- All results are files, so any future UI (notebook, dashboard) can be built on top without changing the engine.

## 4. Backend (core engine)

The backend is an in-process C++ library, `libframeflow`, linked into the `frameflow` executable and the test binaries; it runs as three threads inside one process.

### 4.1 Components

| Component | Responsibility | Depends on |
| --- | --- | --- |
| `Config` | Parse flags and YAML, apply defaults, validate (SRS section 7) | CLI11, yaml-cpp |
| `Pipeline` | Build components from config, start/stop threads, own lifetimes (RAII) | All components |
| `ISource` (`VideoFileSource`, `SyntheticSource`) | Produce paced, timestamped frames | OpenCV, `Clock` |
| `BoundedQueue<FramePtr>` | Thread-safe fixed-capacity FIFO; delegates full/stale decisions to the policy | `OverloadPolicy` |
| `OverloadPolicy` (4 implementations) + `PolicyFactory` | Decide push/pop behaviour under pressure | `Clock`, latency feedback |
| `Worker` | Pop, apply processor and cost model, pass to sink | `IProcessor`, `CostModel` |
| `IProcessor` (`None`, `Grayscale`, `Blur`, `Canny`) | Image operation per frame | OpenCV |
| `CostModel` (fixed, bursty) | Deterministic synthetic processing cost | `Clock`, seed |
| `ISink` (`NullSink`, `PreviewSink`) | Final stage; stamps t_out; feeds latency back to the policy | OpenCV (preview) |
| `MetricsStore` | Preallocated per-frame records, depth samples, events | none |
| `Stats` | Percentiles, FPS, drop rates | STL |
| `Reporter` (`Csv`, `Json`, `Console`) | Write results after the run | nlohmann/json |
| `Clock` | Wrapper over `steady_clock`; injectable fake clock for tests | std::chrono |
| `BenchRunner` | Expand the experiment matrix and run pipelines sequentially | `Pipeline` |

### 4.2 Threading model

| Thread | Runs | Blocks on | Writes metrics to |
| --- | --- | --- | --- |
| Main | Config, pipeline control, signal wait, reporting | Stop condition (duration, frames, signal) | Summary only, after threads join |
| Source | `ISource` loop | `sleep_until`; full queue (blocking/adaptive-normal) | Its own record slots (per-frame id) |
| Worker | `Worker` loop + sink | Empty queue (100 ms timeout) | Its own record slots |

Metrics records are indexed by frame id and each field is written by exactly one thread, so no lock is needed on the hot path; the main thread reads them only after both threads have joined.

### 4.3 Shutdown sequence

1. Main thread sets the atomic stop flag (duration reached or SIGINT/SIGTERM received via `sigwait` on a dedicated signal set).
2. Main calls `queue.shutdown()`, waking any blocked push or pop.
3. Source and worker leave their loops; frames still queued are marked in-flight.
4. `std::jthread` destructors (or explicit joins) complete.
5. `Stats` computes the summary; reporters write files; process exits with the SRS exit code.

## 5. Database and storage

FrameFlow uses no database: each run writes a small folder of plain files, which is enough for thousands of runs and keeps results readable, diff-able and portable.

### 5.1 Storage tiers

| Tier | What | Where | Lifetime |
| --- | --- | --- | --- |
| In-process memory | Frames (`cv::Mat`), queue, preallocated metrics records | RAM | One run |
| Run output | `frames.csv`, `depth.csv`, `events.csv`, `summary.json` | `--out` directory | Until the user deletes it |
| Experiment output | One folder per configuration and repeat + `experiment.json` | `<out>/<config>/rep_<n>/` | Until deleted |
| Analysis output | PNG graphs, summary tables | `docs/images`, `docs/RESULTS.md` | Committed to git |
| Inputs | Sample videos, experiment YAML files | `samples/`, `experiments/` | In the repository (videos kept small) |

### 5.2 Sizing

A 60 s run at 30 FPS produces 1,800 frame rows of about 120 bytes each, roughly 0.2 MB of CSV. A full experiment matrix of about 100 runs is about 20-30 MB, which needs no database or compression.

### 5.3 When a database would be justified

Only if results from many machines needed to be collected and queried together. The upgrade path is to import the existing CSV/JSON files into SQLite or DuckDB; the engine would not change.

## 6. APIs and interfaces

FrameFlow exposes three interfaces: the CLI for users, a C++ library API for extension and tests, and file formats for analysis. There is no network API.

### 6.1 Command-line API

| Command | Purpose | Key options |
| --- | --- | --- |
| `frameflow run` | One run | `--source`, `--input`, `--fps`, `--processor`, `--cost-mode`, `--cost-ms`, `--queue`, `--policy`, `--target-ms`, `--duration`, `--out`, `--preview` |
| `frameflow bench` | Experiment matrix | `--config`, `--repeats`, `--out` |
| `frameflow --version` | Version and git commit | none |

The full option list, ranges and exit codes are defined in the SRS (sections 7 and 9).

### 6.2 C++ library API (`include/frameflow/`)

```cpp
// Extension point 1: overload policies
class OverloadPolicy {
public:
    virtual ~OverloadPolicy() = default;
    virtual PushDecision on_push(const QueueView& q, const FrameMeta& incoming) = 0;
    virtual bool drop_on_pop(const FrameMeta& f, Clock::time_point now) { return false; }
    virtual void on_delivered(Clock::duration latency) {}
    virtual std::string_view name() const = 0;
};

// Extension point 2: processors
class IProcessor {
public:
    virtual ~IProcessor() = default;
    virtual void process(cv::Mat& frame) = 0;
    virtual std::string_view name() const = 0;
};

// Extension point 3: sources and sinks
class ISource { public: virtual ~ISource() = default; virtual FramePtr next(Clock::time_point release) = 0; };
class ISink   { public: virtual ~ISink()   = default; virtual void consume(FramePtr frame) = 0; };

// Entry point used by main() and integration tests
RunResult run_pipeline(const RunConfig& cfg, Clock& clock, StopToken stop);
```

Registration is a single line in a factory map, e.g. `register_policy("drop-oldest", make<DropOldestPolicy>)`.

### 6.3 File interfaces

| File | Producer | Consumer | Schema owner |
| --- | --- | --- | --- |
| Experiment YAML | User | `Config`, `BenchRunner` | SRS VR-18 |
| `frames.csv`, `depth.csv`, `events.csv` | `CsvReporter` | `analyze.py`, spreadsheets | SRS DR-01 to DR-03 |
| `summary.json` | `JsonReporter` | `analyze.py`, CI checks | SRS DR-04 |

File schemas carry a `schema_version` field (in `summary.json`) so the analysis script can reject files from incompatible versions.

## 7. Data flow

Data moves through four stages: configuration in, frames through the pipeline, measurements into memory, results out to files.

1. **Configure.** CLI flags and YAML are merged and validated into an immutable `RunConfig`; invalid input stops here (exit 2).
2. **Produce.** The source creates a `Frame` (`std::unique_ptr`), stamps `t_cap` at its scheduled time and moves it into the queue (`t_push`).
3. **Decide.** If the queue is full or frames are stale, the policy keeps, drops or waits; each drop writes the frame's record with a reason.
4. **Process.** The worker pops (`t_pop`), runs the processor and cost model (`t_proc_start`, `t_proc_end`), and moves the frame to the sink.
5. **Deliver.** The sink stamps `t_out`, optionally shows the preview, reports latency back to the policy, and releases the pixel buffer.
6. **Record.** Each frame's metadata is already in its preallocated record slot; queue depth and policy events are appended to their own buffers.
7. **Report.** After threads join, `Stats` computes the summary and reporters write CSV/JSON; `analyze.py` turns those files into graphs and tables.

Only frame metadata leaves the process; pixel data never touches disk (SRS DR-10). The pipeline flow diagram is in `docs/SPEC.md`, section 6.

## 8. Authentication and security

Authentication is delegated entirely to the Linux login; the architecture's security work goes into safe handling of untrusted input files and memory-safe, race-free code.

| Concern | Architectural measure |
| --- | --- |
| Authentication | None in the app: no accounts, no network, no secrets. The invoking Linux user is the only identity (SRS section 8). |
| Authorization | Linux file permissions on inputs and output folder; app refuses to run as root by default. |
| Network exposure | No sockets at all; verified with `strace` in CI. |
| Untrusted input (video/YAML) | Decode errors handled and bounded retries; YAML size and key limits; validation before threads start. |
| Memory safety | RAII and `unique_ptr` ownership; no raw `new`/`delete`; ASan and UBSan in CI. |
| Concurrency safety | Single lock per queue, predicate waits, single shutdown path; TSan in CI. |
| File-system safety | Writes only inside `--out`; exclusive file creation; no overwrite without `--overwrite`. |
| Build hardening | `-D_FORTIFY_SOURCE=2`, `-fstack-protector-strong`, PIE in Release builds. |
| Privacy | Environment record stores a hostname hash, never usernames or unrelated paths. |
| Supply chain | Dependencies from Ubuntu 24.04 apt or vendored at pinned versions. |

## 9. Deployment and build pipeline

Deployment means "build from source on Ubuntu 24.04" plus a prebuilt binary attached to each GitHub release; there are no servers to deploy to.

### 9.1 Build targets

| Target | Output | Use |
| --- | --- | --- |
| `libframeflow` (static) | Core engine library | Linked by the app and tests |
| `frameflow` | CLI executable | Users |
| `frameflow_tests` | GoogleTest binary | CI and developers |
| `frameflow_bench_queue` | Micro-benchmark | Performance requirement PR-03 |

### 9.2 CMake presets

| Preset | Flags | Purpose |
| --- | --- | --- |
| `release` | `-O2`, hardening flags | Measurements and releases |
| `debug` | `-O0 -g` | Development |
| `asan` | `-fsanitize=address,undefined` | Memory checks |
| `tsan` | `-fsanitize=thread` | Race checks |
| `coverage` | `--coverage` | Coverage report |

### 9.3 CI pipeline (GitHub Actions, `ubuntu-24.04`)

1. Install apt dependencies.
2. Format and static-analysis check (clang-format, clang-tidy).
3. Release build and full tests.
4. ASan/UBSan build and tests.
5. TSan build, tests and a 60 s stress run.
6. Smoke run: `frameflow run --source=synthetic --duration=5` for each policy; validate output schemas.
7. On a version tag: build the release binary and attach it (plus checksums) to the GitHub release.

### 9.4 Installation options

- **From source** (primary): `cmake --preset release && cmake --build --preset release`.
- **Prebuilt binary**: download from the GitHub release; requires the same apt runtime libraries.
- **Optional later**: a Dockerfile for exact reproducibility of the environment. Not needed for the MVP, and benchmark numbers should be measured outside containers.

## 10. Monitoring and observability

Observability is built into the product itself: every run records what happened to every frame, so no external monitoring stack (Prometheus, Grafana) is needed.

| Signal | Mechanism | When |
| --- | --- | --- |
| Live status | Optional preview overlay (frame age, FPS, drops); `--progress` prints one status line per second to stderr | During a run |
| Per-frame trace | `frames.csv` with all timestamps and drop reasons | After a run |
| Queue behaviour | `depth.csv` time series | After a run |
| Policy decisions | `events.csv` (drops, adaptive state changes with measured latency) | After a run |
| Run health | `summary.json` warnings (source lateness, early stop, `valid` flag) | After a run |
| Logs | stderr with levels `error`, `warning`, `info`; `--verbose` adds `debug`; never logs per frame unless `--trace` | During a run |
| Resource use | Peak RSS from `/proc/self/status` recorded in the summary | After a run |
| Code health | CI status, sanitizer logs, coverage report | Every push |

Rule: logging and progress output never run on the source or worker thread; the main thread reads atomic counters once per second for `--progress`.

## 11. Scalability and evolution path

FrameFlow scales up within one machine (more stages, more cores), not out across machines; each step below is taken only when a measurement shows the current design is the limit.

| Dimension | MVP | Next step (when justified) | Trigger |
| --- | --- | --- | --- |
| Processing throughput | One worker thread | Strip-parallel processing on a thread pool | Worker CPU-bound at target resolution |
| Pipeline depth | One queue, one stage | Chain of stages (decode → convert → filter → encode), each with its own queue and policy | Need to study where latency builds in multi-stage pipelines |
| Memory allocation | `cv::Mat` allocated per frame | Reusable frame-buffer pool | Profiling shows allocation cost matters |
| Queue contention | Mutex + condition variable | Single-producer single-consumer ring buffer | Queue ops measurable vs processing time (unlikely at video rates) |
| Frame rate / resolution | Up to 240 FPS, up to 4K | Same design; validated by PR-09 | n/a |
| Workloads | Filters and synthetic cost | CPU AI inference stage | Need a realistic slow stage |
| Experiments | Sequential runs on one machine | Run multiple machines manually; merge results by `environment` | Comparing hardware |
| Results analysis | CSV/JSON + Python | Import into DuckDB/SQLite | Hundreds of experiments across machines |

The interfaces in section 6 are the seams for every step: a new stage, policy or processor plugs in without changing the existing components.

## 12. Key architecture decisions

Each decision favours the simplest design that answers the core question and can be measured honestly.

| # | Decision | Reason | Consequence |
| --- | --- | --- | --- |
| AD-1 | Single process, local CLI; no client-server split | Problem is local timing behaviour; a server adds latency noise and attack surface | No remote use; results shared as files |
| AD-2 | No database; CSV + JSON files | Small data, human-readable, easy to analyse and version | Cross-machine queries need an import step later |
| AD-3 | No web frontend; CLI + preview + graphs | UI does not help answer the question; keeps scope small | Less flashy demo; preview window covers visual needs |
| AD-4 | Mutex + condition variable queue, not lock-free | Correctness first; lock cost is tiny at video frame rates | Revisit only if PR-03 or profiling shows contention |
| AD-5 | Policy as a strategy interface owned by the queue | Policies are the core contribution and must be swappable | One virtual call per push/pop; negligible cost |
| AD-6 | Metrics in preallocated per-frame slots, no hot-path locks or I/O | Measurement must not distort what it measures | Memory sized up front from fps x duration |
| AD-7 | Injectable `Clock` | Deterministic unit tests for time-based policies | Slight indirection in code |
| AD-8 | OpenCV for decode and filters | One mature dependency covers decode, processing and preview | Not studying codec internals |
| AD-9 | Python only for offline analysis | Fastest way to plot; keeps C++ core focused | Two languages in the repo |
| AD-10 | Linux-only (Ubuntu 24.04) | Matches target environment; avoids portability work | No Windows/macOS support |

### Deliberately left out

Microservices, message brokers, container orchestration, cloud hosting, user accounts, databases, web dashboards, GPU acceleration and lock-free data structures. None of these helps measure overload policies on one machine, and each would add complexity to build, test and explain.
