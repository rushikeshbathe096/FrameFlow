# FrameFlow Software Requirements Specification

This SRS defines every testable requirement for the FrameFlow MVP, a command-line C++ video pipeline that controls latency under overload. Each requirement has an ID, uses "shall", and names how it is verified: Test (T), Inspection (I), Demonstration (D) or Analysis (A). Product goals and scope are in `docs/PRD.md`.

## 1. Introduction

### 1.1 Purpose

This document specifies the functional and non-functional requirements of FrameFlow v1.0 (MVP) so that it can be implemented, tested and accepted without ambiguity. It is written for the developer, reviewers and anyone reproducing the results.

### 1.2 Scope

FrameFlow is a single-user, local command-line tool for Linux. It reads or generates video frames at a fixed rate, passes them through a bounded queue to a processing worker, applies a selectable overload policy when the worker falls behind, and records per-frame measurements. It has no network service, no accounts and no persistent server state.

### 1.3 Conventions

- **"shall"** = mandatory; **"should"** = recommended; **"may"** = optional.
- Requirement IDs: `FR-<module>-<nn>` functional, `BR-nn` business rule, `DR-nn` data, `VR-nn` validation, `ER-nn` error handling, `EC-nn` edge case, `SR-nn` security, `PR-nn` performance, `RR-nn` reliability and other qualities.
- Verification: **T** = automated test, **I** = inspection (code or file review), **D** = demonstration (run and observe), **A** = analysis (measurement report).
- Times are measured with a monotonic clock (`std::chrono::steady_clock`). "ms" = milliseconds.

### 1.4 Definitions

| Term | Definition |
| --- | --- |
| Frame | One image plus metadata (id, timestamps, drop status) |
| Run | One execution of the pipeline with one configuration |
| Experiment | A set of runs defined by a matrix of configurations and a repeat count |
| Capture time (t_cap) | Scheduled release time of a frame by the source |
| End-to-end latency | t_out - t_cap for a delivered frame |
| Delivered frame | A frame that reaches the sink |
| Dropped frame | A frame discarded by a policy or by the source |
| In-flight frame | A frame inside the pipeline when the run stops |
| Overload factor | Input FPS x mean processing time (s) |
| Warm-up | Initial period excluded from statistics |
| Latency target (T) | Adaptive policy's p95 latency goal |
| Exit threshold (T_low) | Latency below which the adaptive policy stops shedding |

## 2. Overall description

### 2.1 Product perspective

FrameFlow is a standalone executable (`frameflow`) plus a Python analysis script (`scripts/analyze.py`). It depends on OpenCV for video I/O and image filters. It interacts only with the local file system, the terminal and, optionally, a preview window.

### 2.2 Product functions (summary)

1. Produce paced frames from a video file or a synthetic generator.
2. Pass frames through a bounded queue to a processing worker.
3. Apply one of four overload policies when the worker cannot keep up.
4. Record per-frame timing and drop data; compute summary statistics.
5. Run experiment matrices and write CSV/JSON results.
6. Generate graphs from results.

### 2.3 Operating environment

| Item | Requirement |
| --- | --- |
| OS | Ubuntu 24.04 LTS, x86-64 |
| Compiler | g++ 13 (C++17) or newer |
| Build | CMake 3.28 or newer |
| Libraries | OpenCV 4.6 (`libopencv-dev`), GoogleTest, nlohmann-json, yaml-cpp, CLI11 or cxxopts |
| Python | Python 3.12 with pandas and matplotlib (analysis only) |
| Hardware | Any x86-64 CPU with 2 or more cores, 4 GB RAM; no GPU |
| Display | Required only for the optional preview window |

### 2.4 Design and implementation constraints

- C1. Core shall be written in C++17; C++20 features may be used only where g++ 13 supports them.
- C2. No raw `new`/`delete` in application code; ownership via `std::unique_ptr` or containers.
- C3. Concurrency via standard library primitives only (`std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic`).
- C4. The program shall not require root privileges.
- C5. All third-party dependencies shall be installable from Ubuntu 24.04 apt repositories or vendored as header-only.

### 2.5 Dependencies on assumptions

The assumptions listed in the PRD (section 8) apply. In particular, results are relative comparisons between policies on one machine.

## 3. User roles and permissions

FrameFlow is a single-user local tool, so roles describe how it is used rather than accounts in the software; permissions come from the Linux user who runs it.

### 3.1 Roles

| Role | Description | Typical actions |
| --- | --- | --- |
| Operator | Runs single pipeline runs | `frameflow run` with flags; views console summary and preview |
| Experimenter | Defines and runs experiment matrices | Writes YAML configs; `frameflow bench`; runs `analyze.py` |
| Developer | Builds, tests and modifies FrameFlow | Builds from source; runs tests and sanitizers; adds policies or processors |

One person usually holds all three roles. The software makes no distinction between them at runtime.

### 3.2 Permission model

| Resource | Access needed | Granted by |
| --- | --- | --- |
| Input video file | Read | Linux file permissions of the invoking user |
| Config file | Read | Linux file permissions |
| Output directory | Create, write | Linux file permissions |
| Display (preview) | Open a window | User's desktop session |
| CPU affinity / priority (optional) | Standard user limits only | Linux; no elevated priority required |

| ID | Requirement | Verify |
| --- | --- | --- |
| FR-ROLE-01 | FrameFlow shall run all features as an unprivileged user. | D |
| FR-ROLE-02 | FrameFlow shall refuse to start when run as root (effective UID 0) unless `--allow-root` is given, and shall print a warning explaining why. | T |
| FR-ROLE-03 | FrameFlow shall access only the files and directories named in its arguments or config, plus its own output directory. | I |
| FR-ROLE-04 | When a required permission is missing, FrameFlow shall stop with the error codes in section 9 and name the path involved. | T |

## 4. Functional requirements

Every requirement below is mandatory for v1.0 and has a pass/fail verification.

### 4.1 Frame source (SRC)

| ID | Requirement | Verify |
| --- | --- | --- |
| FR-SRC-01 | The system shall read frames from a video file given by `--input` using OpenCV. | T |
| FR-SRC-02 | The system shall generate synthetic frames of a given `--resolution` when `--source=synthetic`, filled with a deterministic pattern derived from the frame id. | T |
| FR-SRC-03 | The source shall release frame k at time t0 + k / fps, where t0 is the run start, using `sleep_until`. | T |
| FR-SRC-04 | The source shall set each frame's t_cap to its scheduled release time, even if the actual release is late. | T |
| FR-SRC-05 | The source shall assign frame ids starting at 0, increasing by 1 with no gaps. | T |
| FR-SRC-06 | When a video file ends before the run ends, the source shall restart from the first frame (loop) unless `--no-loop` is set. | T |
| FR-SRC-07 | The source shall record lateness (actual release - scheduled release) for every frame. | T |

### 4.2 Bounded queue (Q)

| ID | Requirement | Verify |
| --- | --- | --- |
| FR-Q-01 | The queue shall hold at most `--queue` frames at any time. | T |
| FR-Q-02 | The queue shall deliver frames in first-in, first-out order. | T |
| FR-Q-03 | Push and pop shall be safe to call concurrently from the source and worker threads. | T (TSan) |
| FR-Q-04 | When the queue is full, the push outcome shall be decided by the active overload policy. | T |
| FR-Q-05 | Pop shall block while the queue is empty, returning when a frame arrives, the timeout (default 100 ms) expires, or shutdown is requested. | T |
| FR-Q-06 | `shutdown()` shall wake all threads blocked in push or pop within 50 ms. | T |
| FR-Q-07 | The queue shall record its depth after every push and pop. | T |

### 4.3 Processing worker (PROC)

| ID | Requirement | Verify |
| --- | --- | --- |
| FR-PROC-01 | The worker shall support processors `none`, `grayscale`, `blur` (Gaussian, kernel `--kernel`, default 15) and `canny`. | T |
| FR-PROC-02 | In `--cost-mode=fixed`, the worker shall spend `--cost-ms` per frame using a busy-wait (default) or sleep (`--cost-wait=sleep`). | T |
| FR-PROC-03 | In `--cost-mode=bursty`, the worker shall use `--cost-ms` normally and `--spike-ms` during spikes of length `--spike-len` repeating every `--spike-every`. | T |
| FR-PROC-04 | Spike timing shall be derived from run start time and `--seed` so that runs with the same settings have identical spike schedules. | T |
| FR-PROC-05 | The worker shall stamp t_pop, t_proc_start and t_proc_end on every frame it handles. | T |
| FR-PROC-06 | The worker shall pass every processed frame to the sink, which stamps t_out. | T |

### 4.4 Overload policies (POL)

| ID | Requirement | Verify |
| --- | --- | --- |
| FR-POL-01 | The system shall provide policies `blocking`, `drop-oldest`, `drop-newest` and `adaptive`, selected by `--policy`. | T |
| FR-POL-02 | **Blocking:** when the queue is full, push shall wait until space is free or shutdown is requested; no frame shall be dropped by the queue. | T |
| FR-POL-03 | **Drop-oldest:** when the queue is full, the frame at the head shall be removed and recorded as dropped (reason `oldest`), then the new frame inserted. | T |
| FR-POL-04 | **Drop-newest:** when the queue is full, the incoming frame shall be recorded as dropped (reason `newest`) and the queue left unchanged. | T |
| FR-POL-05 | **Adaptive:** the policy shall keep an estimate L of delivered-frame latency as the p95 of the last `--window` delivered frames (default 30). | T |
| FR-POL-06 | Adaptive shall switch from NORMAL to SHEDDING when L > T (`--target-ms`) and from SHEDDING to NORMAL when L < T_low (`--exit-ratio` x T). | T |
| FR-POL-07 | In NORMAL state adaptive shall behave as blocking. | T |
| FR-POL-08 | In SHEDDING state adaptive shall, on push, remove queued frames whose age exceeds T (reason `stale`) and then, if still full, drop the oldest; on pop, it shall drop a frame whose age exceeds T instead of processing it. | T |
| FR-POL-09 | Adaptive shall log every state change with time, old state, new state and L. | T |
| FR-POL-10 | Every dropped frame shall be recorded with id, time and reason. | T |

### 4.5 Metrics and reporting (MET)

| ID | Requirement | Verify |
| --- | --- | --- |
| FR-MET-01 | The system shall store one record per frame produced, with fields defined in DR-01. | T |
| FR-MET-02 | The system shall compute p50, p95, p99 and max end-to-end latency over delivered frames after warm-up. | T |
| FR-MET-03 | Percentiles shall use the nearest-rank method: value at index ceil(p/100 x n) - 1 of the sorted list. | T |
| FR-MET-04 | The system shall compute input FPS, output FPS, drop rate (overall and per reason), mean and max queue depth, and number of adaptive state changes. | T |
| FR-MET-05 | At run end the system shall write `frames.csv`, `depth.csv`, `events.csv` and `summary.json` to `--out`. | T |
| FR-MET-06 | At run end the system shall print a console summary with the metrics in FR-MET-02 and FR-MET-04. | D |
| FR-MET-07 | No file I/O shall occur on the source or worker thread during a run. | I |

### 4.6 Command-line interface and control (CLI)

| ID | Requirement | Verify |
| --- | --- | --- |
| FR-CLI-01 | `frameflow run` shall execute one run using flags and/or `--config`; flags shall override config values. | T |
| FR-CLI-02 | `frameflow bench --config=<file>` shall execute every configuration in the matrix `--repeats` times (default 5), each into its own numbered output folder. | T |
| FR-CLI-03 | `frameflow --help` and `frameflow <command> --help` shall list every option with its default. | D |
| FR-CLI-04 | `frameflow --version` shall print the version and git commit. | D |
| FR-CLI-05 | A run shall end after `--duration` seconds or `--frames` frames, whichever is set (exactly one is required). | T |
| FR-CLI-06 | On SIGINT or SIGTERM the system shall stop gracefully, write all output files, and exit with code 0 for the first signal; a second signal shall exit immediately with code 130. | T |
| FR-CLI-07 | `--preview` shall open a window showing the processed frame with an overlay of frame age, output FPS and dropped count. | D |

### 4.7 Analysis script (ANA)

| ID | Requirement | Verify |
| --- | --- | --- |
| FR-ANA-01 | `analyze.py <results-dir>` shall produce a latency-over-time PNG per run and a summary table (CSV and Markdown) per experiment. | T |
| FR-ANA-02 | For experiments with repeats, the script shall report the median, min and max of each summary metric. | T |
| FR-ANA-03 | The script shall exit with a clear error if a required result file is missing or malformed. | T |

## 5. Business rules

These rules govern how frames are counted and how results are produced; any run that breaks one is invalid.

| ID | Rule | Verify |
| --- | --- | --- |
| BR-01 | Frame conservation: for every run, produced = delivered + dropped + in-flight-at-stop. | T |
| BR-02 | A frame is counted in exactly one of delivered, dropped or in-flight. | T |
| BR-03 | Delivered frames shall reach the sink in strictly increasing id order (no reordering). | T |
| BR-04 | Timestamps of a delivered frame shall satisfy t_cap ≤ t_push ≤ t_pop ≤ t_proc_start ≤ t_proc_end ≤ t_out. | T |
| BR-05 | Latency statistics use delivered frames only; dropped frames are reported through the drop rate. | T |
| BR-06 | Frames with t_cap in the warm-up period are excluded from summary statistics but kept in `frames.csv` with `warmup=1`. | T |
| BR-07 | The adaptive policy shall never drop a frame while in NORMAL state. | T |
| BR-08 | Only one policy is active per run; it cannot change during a run. | I |
| BR-09 | A run's summary is valid only if it was not stopped before the warm-up ended; otherwise `summary.json` shall set `"valid": false`. | T |
| BR-10 | Each summary shall record the full effective configuration, seed, environment and git commit so the run can be reproduced. | I |
| BR-11 | Results of a bench experiment are reported as the median across repeats, with min and max. | T |
| BR-12 | An existing output directory shall never be overwritten unless `--overwrite` is given. | T |

## 6. Data requirements

All data lives in memory during a run and in plain files afterwards; there is no database.

### 6.1 In-memory entities

| Entity | Fields | Lifetime |
| --- | --- | --- |
| Frame | `cv::Mat` image; `FrameMeta` | Created by source; destroyed after the sink or when dropped |
| FrameMeta | id (uint64), t_cap, t_push, t_pop, t_proc_start, t_proc_end, t_out (steady_clock time points), source_lateness_us, dropped (bool), drop_reason (enum), warmup (bool) | Copied into the metrics store when the frame ends |
| DepthSample | time, depth | Whole run |
| PolicyEvent | time, old_state, new_state, L_ms | Whole run |
| RunConfig | All validated parameters (section 7) | Whole run |
| RunSummary | Metrics from FR-MET-02 and FR-MET-04, valid flag | Written at run end |

### 6.2 Output files

| ID | File | Format and content | Verify |
| --- | --- | --- | --- |
| DR-01 | `frames.csv` | Header `id,t_cap_us,t_push_us,t_pop_us,t_proc_start_us,t_proc_end_us,t_out_us,latency_ms,lateness_us,dropped,reason,warmup`; one row per produced frame; times in microseconds from run start; empty field where not reached | T |
| DR-02 | `depth.csv` | Header `t_us,depth`; one row per push and pop | T |
| DR-03 | `events.csv` | Header `t_us,type,frame_id,detail`; drops and adaptive state changes | T |
| DR-04 | `summary.json` | Object with `config`, `environment`, `metrics`, `valid`, `frameflow_version`, `git_commit`, `schema_version` keys | T |
| DR-05 | Bench layout | `<out>/<config-name>/rep_<n>/` containing DR-01 to DR-04, plus `<out>/experiment.json` with the matrix | T |

### 6.3 Data rules

| ID | Requirement | Verify |
| --- | --- | --- |
| DR-06 | CSV files shall be UTF-8, comma-separated, `\n` line endings, with a header row. | T |
| DR-07 | Numeric values shall use `.` as decimal separator regardless of locale. | T |
| DR-08 | `environment` shall contain CPU model, logical core count, total RAM (MB), kernel version, compiler version, OpenCV version and hostname hash (not the plain hostname). | T |
| DR-09 | The metrics store shall be preallocated for the expected frame count (fps x duration x 1.1) to avoid reallocation during a run. | I |
| DR-10 | Image pixel data shall never be written to result files. | I |
| DR-11 | FrameFlow shall not delete or modify any file outside its output directory. | I |

Retention is the user's choice; FrameFlow keeps no data between runs other than the files it writes.

## 7. Input validation

All inputs are validated before any thread starts; an invalid value stops the program with exit code 2 and a message naming the parameter, the value and the allowed range.

| ID | Parameter | Rule | Default |
| --- | --- | --- | --- |
| VR-01 | `--fps` | Number, 1 to 240 | 30 |
| VR-02 | `--queue` | Integer, 1 to 1024 | 8 |
| VR-03 | `--duration` / `--frames` | Exactly one set; duration 1-3600 s; frames 1-1,000,000 | 60 s |
| VR-04 | `--warmup` | Seconds, 0 to less than duration | 3 |
| VR-05 | `--cost-ms` | Number, 0 to 10,000 | 0 |
| VR-06 | `--spike-ms`, `--spike-len`, `--spike-every` | Required when `--cost-mode=bursty`; spike-ms ≥ cost-ms; spike-len < spike-every | none |
| VR-07 | `--policy` | One of `blocking`, `drop-oldest`, `drop-newest`, `adaptive` (case-sensitive) | blocking |
| VR-08 | `--target-ms` | Number, 1 to 60,000; only with `adaptive` | 100 |
| VR-09 | `--exit-ratio` | Number, greater than 0 and less than 1 | 0.7 |
| VR-10 | `--window` | Integer, 5 to 1000 | 30 |
| VR-11 | `--processor` | One of `none`, `grayscale`, `blur`, `canny` | none |
| VR-12 | `--kernel` | Odd integer, 3 to 99 | 15 |
| VR-13 | `--resolution` | `WxH`, each 16 to 7680 | 1280x720 |
| VR-14 | `--input` | Existing, readable file that OpenCV can open and decode at least one frame from | none |
| VR-15 | `--source` | `file` or `synthetic`; `file` requires `--input` | file |
| VR-16 | `--seed` | Unsigned 64-bit integer | 42 |
| VR-17 | `--out` | Path whose parent exists and is writable; must not exist unless `--overwrite` | `results/<timestamp>` |
| VR-18 | `--config` | Readable YAML; unknown keys rejected; same rules as matching flags | none |
| VR-19 | `--repeats` | Integer, 1 to 100 | 5 |
| VR-20 | Any numeric flag | Rejects NaN, infinity, trailing characters (e.g. `30fps`) and values that overflow | n/a |
| VR-21 | Unknown flags | Rejected with a suggestion of the closest valid flag | n/a |

| ID | Requirement | Verify |
| --- | --- | --- |
| VR-22 | Each rule VR-01 to VR-21 shall have at least one passing and one failing unit test. | T |
| VR-23 | Validation shall report all invalid parameters in one message, not only the first. | T |
| VR-24 | The effective configuration after defaults and overrides shall be printed at startup and stored in `summary.json`. | T |

## 8. Authentication and authorization

FrameFlow v1.0 has no built-in authentication or authorization because it is a local, single-user tool with no network interface and no shared data; it relies entirely on the Linux user account that runs it.

### 8.1 Rationale

- There are no accounts, sessions, secrets or remote clients to authenticate.
- Access control is already enforced by Linux: login authenticates the user, and file permissions authorize reads and writes.
- Adding a login layer to a local CLI would add attack surface without protecting anything.

### 8.2 Requirements

| ID | Requirement | Verify |
| --- | --- | --- |
| SR-AUTH-01 | FrameFlow shall not open any network socket (listening or outgoing). | T (run under `strace -e trace=network` shows no socket calls) |
| SR-AUTH-02 | FrameFlow shall not store, request or read passwords, tokens or keys. | I |
| SR-AUTH-03 | FrameFlow shall perform every file operation with the invoking user's permissions and shall not change its UID, GID or capabilities. | I |
| SR-AUTH-04 | Output files shall be created with permissions 0644 and directories 0755 (subject to the user's umask). | T |
| SR-AUTH-05 | FrameFlow shall honour FR-ROLE-02 (refuse to run as root without `--allow-root`). | T |

### 8.3 Future consideration

If a later version adds a network control API or remote dashboard, it shall require authentication (e.g. a local token) and a new SRS revision before release.

## 9. Error handling

Every failure produces one clear message on stderr, a documented exit code, and, if the run had started, whatever results could be saved.

### 9.1 Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success (including a graceful stop on the first SIGINT/SIGTERM) |
| 1 | Runtime error during a run (e.g. decode failure, worker exception) |
| 2 | Invalid arguments or configuration |
| 3 | Input file missing, unreadable or not decodable |
| 4 | Output directory not writable, already exists, or disk full |
| 5 | Refused to run as root without `--allow-root` |
| 6 | Preview requested but no display available |
| 130 | Forced exit on a second interrupt signal |

### 9.2 Requirements

| ID | Requirement | Verify |
| --- | --- | --- |
| ER-01 | Error messages shall have the form `frameflow: error: <what failed>: <detail> (<hint>)` and name the file or parameter involved. | T |
| ER-02 | Each error condition in 9.1 shall return its exact exit code. | T |
| ER-03 | If the input file fails to decode mid-run, the source shall retry the next frame up to 3 times, then stop the run, write partial results with `"valid": false` and exit 1. | T |
| ER-04 | An exception thrown in the source or worker thread shall be caught, logged with the thread name, trigger a graceful stop of the whole pipeline, and exit 1; it shall never terminate the process without writing results. | T |
| ER-05 | If an output file write fails (e.g. disk full), the system shall report which file failed, keep any files already written, and exit 4. | T |
| ER-06 | If `--preview` is set and no display is available (`DISPLAY` and `WAYLAND_DISPLAY` unset), the system shall exit 6 before starting threads, unless `--preview=auto`, which disables preview with a warning. | T |
| ER-07 | Warnings (e.g. source lateness above 5 ms on more than 1% of frames, warm-up longer than half the run) shall be printed to stderr with prefix `frameflow: warning:` and recorded in `summary.json`. | T |
| ER-08 | No error path shall leave a thread blocked; all paths shall call the single shutdown routine. | T (TSan + timeout test) |
| ER-09 | `analyze.py` shall exit non-zero with a message naming the missing or malformed file and column. | T |

## 10. Edge cases

Each edge case has a defined expected behaviour and a test.

| ID | Situation | Expected behaviour | Verify |
| --- | --- | --- | --- |
| EC-01 | Queue capacity 1 | All policies work; drop-oldest replaces the single queued frame | T |
| EC-02 | Processing cost 0 | No drops under any policy; latency near zero; output FPS equals input FPS | T |
| EC-03 | Extreme overload (cost 10x frame interval) | No crash; dropping policies drop about 90%; blocking latency grows linearly; frame conservation holds | T |
| EC-04 | Video shorter than one second with looping | Loops without a gap in frame ids | T |
| EC-05 | Video with `--no-loop` ending before duration | Run ends early, summary marks `ended_by: "input_eof"`, results valid if warm-up passed | T |
| EC-06 | Video file with zero decodable frames | Exit 3 before threads start | T |
| EC-07 | Corrupted frame mid-file | ER-03 retry logic applies | T |
| EC-08 | Ctrl+C during warm-up | Results written with `"valid": false` | T |
| EC-09 | Ctrl+C while the source is blocked on a full queue (blocking policy) | Source wakes within 50 ms; clean exit 0 | T |
| EC-10 | Ctrl+C while the worker waits on an empty queue | Worker wakes within 50 ms; clean exit 0 | T |
| EC-11 | Two Ctrl+C in quick succession | Immediate exit 130 | T |
| EC-12 | Source falls behind its own schedule (system overloaded) | Lateness recorded; t_cap stays the scheduled time; ER-07 warning if over threshold | T |
| EC-13 | Adaptive with latency hovering exactly at T | No state change while L stays between T_low and T | T |
| EC-14 | Adaptive with fewer delivered frames than `--window` | Uses all delivered frames so far; no division by zero | T |
| EC-15 | Every frame dropped (no delivered frames) | Latency fields `null` in JSON, drop rate 100%, no crash | T |
| EC-16 | Very high FPS (240) and small frames | Runs; lateness reported honestly | D |
| EC-17 | Output directory on a full disk | Exit 4 per ER-05 | T (small tmpfs) |
| EC-18 | Steady-clock values compared across threads | Only steady_clock used; wall-clock changes during a run do not affect results | I |
| EC-19 | Config file sets a value and a flag overrides it | Flag wins; effective config shows the flag value | T |
| EC-20 | Bench interrupted midway | Completed repetitions kept; `experiment.json` lists completed and missing runs | T |

## 11. Security

The main threats to a local media tool are malformed input files, unsafe file writes and memory-safety bugs; the requirements below address each.

| ID | Requirement | Verify |
| --- | --- | --- |
| SR-01 | Input files shall be treated as untrusted: decode errors shall be handled per ER-03 and never cause a crash. | T (fuzzed/corrupted sample files) |
| SR-02 | The build shall enable `-Wall -Wextra -Werror`, `-D_FORTIFY_SOURCE=2`, `-fstack-protector-strong` and position-independent executables in Release mode. | I |
| SR-03 | The full test suite shall pass under AddressSanitizer and UndefinedBehaviorSanitizer with zero reports. | T |
| SR-04 | The full test suite and a 60 s stress run shall pass under ThreadSanitizer with zero reports. | T |
| SR-05 | Output paths shall be resolved to absolute paths; FrameFlow shall refuse an `--out` that is a symlink to a location outside the user's home or the current directory unless `--allow-any-out` is given. | T |
| SR-06 | Output files shall be created with `O_EXCL` semantics (never following an existing symlink) to prevent overwriting unrelated files. | T |
| SR-07 | YAML config parsing shall reject unknown keys, anchors that expand beyond 1 MB, and files larger than 1 MB. | T |
| SR-08 | The environment record shall store a hash of the hostname, not the hostname itself, and shall not record usernames or file paths outside the output directory. | I |
| SR-09 | No shell commands shall be executed from C++ code (no `system()` or `popen()`). | I |
| SR-10 | Third-party dependencies shall be pinned to the Ubuntu 24.04 package versions or vendored at a fixed version listed in the README. | I |
| SR-11 | clang-tidy with `bugprone-*`, `cert-*` and `concurrency-*` checks shall report zero warnings in CI. | T |

## 12. Performance

Performance requirements cover FrameFlow's own overhead and timing accuracy, measured on the reference machine (any x86-64, 4+ cores, Ubuntu 24.04, Release build, no other heavy load).

| ID | Requirement | Verify |
| --- | --- | --- |
| PR-01 | Pacing accuracy: with `--cost-ms=0` at 30 FPS for 60 s, median source lateness shall be under 1 ms and p99 under 5 ms. | A |
| PR-02 | Pipeline overhead: with `--processor=none --cost-ms=0` at 30 FPS, median end-to-end latency shall be under 1 ms. | A |
| PR-03 | Queue operation cost: an uncontended push+pop pair shall take under 5 µs median (micro-benchmark). | A |
| PR-04 | Metrics overhead: recording per-frame metrics shall add under 1% to mean processing time compared with recording disabled. | A |
| PR-05 | Cost accuracy: in fixed mode, measured mean processing time shall be within ±5% of `--cost-ms` for values ≥ 5 ms. | T |
| PR-06 | Shutdown latency: from signal to process exit (excluding file writes) under 1 s. | T |
| PR-07 | Memory bound: peak resident memory shall not exceed base usage + (queue + 2) x frame size + metrics store, within 10%. | A |
| PR-08 | Memory stability: in a 10-minute run under any policy, resident memory after warm-up shall not grow by more than 5%. | A |
| PR-09 | Throughput ceiling: at 720p with `--processor=none`, the pipeline shall sustain 240 FPS without drops. | A |
| PR-10 | Result writing: writing results for a 60 s, 30 FPS run shall take under 2 s. | T |
| PR-11 | Each performance figure shall be reported as the median of 5 runs with min and max, plus the environment record. | I |

## 13. Reliability, maintainability and portability

### 13.1 Reliability

| ID | Requirement | Verify |
| --- | --- | --- |
| RR-01 | 100 consecutive 5-second runs (25 per policy) shall complete with exit 0 and frame conservation intact. | T |
| RR-02 | 200 create-start-stop-destroy cycles of the pipeline in one process shall complete without deadlock, crash or leak. | T |
| RR-03 | A 60-minute run at overload 1.5x under each policy shall complete without crash or memory growth beyond PR-08. | A |
| RR-04 | Reproducibility: two bench runs of the same experiment shall rank the four policies identically by p95 latency. | A |

### 13.2 Maintainability

| ID | Requirement | Verify |
| --- | --- | --- |
| RR-05 | Adding a new overload policy shall require only a new class implementing `OverloadPolicy` and one factory registration line. | I |
| RR-06 | Adding a new processor shall require only a new class implementing `IProcessor` and one factory registration line. | I |
| RR-07 | Code shall pass clang-format and clang-tidy checks in CI. | T |
| RR-08 | Each public class shall have a header comment stating its responsibility and thread-safety. | I |
| RR-09 | Unit test line coverage of `queue`, `policy` and `stats` modules shall be at least 85%. | A (gcov/lcov) |

### 13.3 Portability

| ID | Requirement | Verify |
| --- | --- | --- |
| RR-10 | The project shall build and pass all tests on a fresh Ubuntu 24.04 install following only the README steps. | D |
| RR-11 | CI shall build and test on GitHub Actions `ubuntu-24.04` runners. | T |
| RR-12 | Platform-specific code (signal handling, `/proc` reads) shall be isolated in one module. | I |

## 14. Acceptance criteria and verification

Release 1.0 is accepted when every requirement in this SRS is verified by its stated method and the gates below pass.

### 14.1 Release gates

- [ ] **G1 Build:** clean clone builds on Ubuntu 24.04 with README steps only (RR-10).
- [ ] **G2 Tests:** all unit and integration tests pass in CI, including one pass and one fail case per validation rule (VR-22).
- [ ] **G3 Sanitizers:** zero ASan, UBSan and TSan reports (SR-03, SR-04).
- [ ] **G4 Business rules:** frame conservation, ordering and timestamp-order checks pass on every test run (BR-01 to BR-04).
- [ ] **G5 Policies:** behaviour tests for all four policies pass, including the adaptive hysteresis tests with a controllable clock (FR-POL-01 to FR-POL-10, EC-13).
- [ ] **G6 Error handling:** each exit code in 9.1 is triggered by at least one test (ER-02).
- [ ] **G7 Edge cases:** EC-01 to EC-20 tests pass.
- [ ] **G8 Performance:** PR-01 to PR-10 measured and within limits; results stored in `docs/RESULTS.md` with environment.
- [ ] **G9 Reliability:** RR-01 to RR-04 pass.
- [ ] **G10 Documentation:** README (setup, usage, architecture, results, limitations) reviewed.

### 14.2 Verification matrix

| Requirement group | IDs | Method | Evidence |
| --- | --- | --- | --- |
| Roles and permissions | FR-ROLE-01 to 04 | T, D, I | Test log; code review note |
| Frame source | FR-SRC-01 to 07 | T | Unit + integration test results |
| Bounded queue | FR-Q-01 to 07 | T | Unit tests, TSan log |
| Processing | FR-PROC-01 to 06 | T | Unit tests, cost accuracy report |
| Policies | FR-POL-01 to 10 | T | Policy unit tests, integration runs |
| Metrics | FR-MET-01 to 07 | T, D, I | Golden-file tests on CSV/JSON output |
| CLI and control | FR-CLI-01 to 07 | T, D | CLI tests, signal tests, demo recording |
| Analysis script | FR-ANA-01 to 03 | T | pytest results |
| Business rules | BR-01 to 12 | T, I | Invariant checks in integration tests |
| Data | DR-01 to 11 | T, I | Schema tests on output files |
| Validation | VR-01 to 24 | T | Parameter test table |
| Auth and security | SR-AUTH-01 to 05, SR-01 to 11 | T, I | strace log, sanitizer logs, build flags review |
| Error handling | ER-01 to 09 | T | Exit-code tests |
| Edge cases | EC-01 to 20 | T, D | Edge-case test suite |
| Performance | PR-01 to 11 | A, T | `RESULTS.md` performance section |
| Reliability etc. | RR-01 to 12 | T, A, I, D | Soak test log, coverage report, CI history |

### 14.3 Traceability to PRD user stories

| User story | Requirements |
| --- | --- |
| US-1 Paced video file | FR-SRC-01, 03, 04, 06; PR-01 |
| US-2 Reproducible synthetic source | FR-SRC-02, 05; FR-PROC-04 |
| US-3 Set processing cost | FR-PROC-01, 02; PR-05 |
| US-4 Bursty spikes | FR-PROC-03, 04 |
| US-5 Choose policy | FR-POL-01 to 04; VR-07 |
| US-6 Latency target | FR-POL-05 to 09; VR-08 to 10 |
| US-7 See metrics | FR-MET-02, 04, 06 |
| US-8 Export data | FR-MET-05; DR-01 to 07 |
| US-9 Run experiments | FR-CLI-02; FR-ANA-01, 02; BR-11 |
| US-10 Safe stop | FR-CLI-06; EC-08 to 11 |
