# FrameFlow UI/UX Design

FrameFlow's interface is a command-line tool, an optional preview window and generated graphs; this document specifies each of them so they look consistent, read clearly at a glance and can be built directly. There is no web or mobile app (see `docs/ARCHITECTURE.md`, decision AD-3).

## 1. Design principles

1. **Answer first.** Every output leads with the result the user came for (latency, drops), then details.
2. **Quiet by default.** A run prints a short config block, one progress line and one summary; more only with `--verbose`.
3. **Never block the pipeline.** No UI element (progress, preview, logging) may slow the source or worker threads.
4. **Honest numbers.** Show units on every value, mark warm-up and invalid runs clearly, never round away a problem.
5. **Errors that fix themselves.** Every error names what failed, the value given, and the exact fix.
6. **Readable without colour.** Colour adds emphasis but never carries meaning alone; output works in plain text, pipes and screen readers.

## 2. Users and user journey

| Stage | User goal | What they do | What FrameFlow shows |
| --- | --- | --- | --- |
| 1. Install | Get it running | Clone, `apt install`, build | Build success; `frameflow --version` |
| 2. First run | See it work | `frameflow run --source=synthetic --duration=10` | Config block, progress line, summary |
| 3. Watch | See lag with their own eyes | Add `--preview` and a heavy `--cost-ms` | Live window with frame age and drops |
| 4. Compare | Try each policy | Rerun with `--policy=...` | Same summary layout each time |
| 5. Experiment | Get trustworthy numbers | Write YAML, `frameflow bench` | Per-run progress, experiment summary table |
| 6. Analyse | Explain the result | `python3 scripts/analyze.py results/e2` | Graphs + Markdown table |
| 7. Share | Put it in README / resume | Copy graphs and table | Consistent, labelled charts |

Pain points to avoid: unclear units, silent failures, walls of logs, results that differ for no visible reason.

## 3. Navigation

```
frameflow
├── run        one pipeline run
├── bench      experiment matrix from a YAML file
├── --help     overview + examples
└── --version  version + git commit

scripts/analyze.py <results-dir>   graphs and tables (separate step)
```

Flags are grouped in `--help` in pipeline order:

| Group | Flags |
| --- | --- |
| Source | `--source`, `--input`, `--fps`, `--resolution`, `--no-loop` |
| Processing | `--processor`, `--kernel`, `--cost-mode`, `--cost-ms`, `--spike-ms`, `--spike-len`, `--spike-every` |
| Queue and policy | `--queue`, `--policy`, `--target-ms`, `--exit-ratio`, `--window` |
| Run control | `--duration`, `--frames`, `--warmup`, `--seed` |
| Output | `--out`, `--overwrite`, `--preview`, `--progress`, `--verbose`, `--json`, `--no-color` |

Short aliases: `-p` policy, `-q` queue, `-f` fps, `-d` duration, `-o` out. Unknown flags suggest the closest match ("did you mean `--policy`?").

## 4. Screens

Values below are placeholders showing layout only, not measured results.

| ID | Screen | Where | When |
| --- | --- | --- | --- |
| S1 | Help | Terminal | `--help` |
| S2 | Run start | Terminal | Start of `run` |
| S3 | Progress line | Terminal (stderr) | During a run with `--progress` |
| S4 | Run summary | Terminal (stdout) | End of `run` |
| S5 | Bench progress and summary | Terminal | During and after `bench` |
| S6 | Preview window | Desktop window | `--preview` |
| S7 | Analysis report | PNG files + `RESULTS.md` | After `analyze.py` |

### S1 Help

```
FrameFlow 1.0 — latency-aware video pipeline

Usage:  frameflow run   [options]
        frameflow bench --config <file> [options]

Examples:
  frameflow run --source=synthetic --fps=30 --cost-ms=50 --policy=drop-oldest
  frameflow run --input=traffic.mp4 --processor=blur --policy=adaptive --target-ms=100 --preview
  frameflow bench --config=experiments/e2_sustained.yaml

Source:
  --source <file|synthetic>   frame source                       [file]
  --fps <1-240>               frames per second                  [30]
  ...
Run 'frameflow run --help' for all options.
```

### S2 Run start

```
FrameFlow 1.0  ·  run started 2026-10-24 19:05:12
  source     synthetic 1280x720 @ 30 fps
  processing synthetic cost 50 ms (fixed)          overload 1.50x
  queue      8 frames  ·  policy adaptive (target 100 ms, exit 70 ms)
  run        60 s (warm-up 3 s)  ·  seed 42
  output     results/run_001/
```

### S3 Progress line

Rewritten in place once per second (plain new lines when not a terminal):

```
[####------]  24/60 s  ·  out 19.8 fps  ·  queue 8/8  ·  drops 33%  ·  age 96 ms  ·  SHEDDING
```

### S4 Run summary

```
Result  ·  policy adaptive  ·  overload 1.50x  ·  valid

  Latency (ms)     p50  ___    p95  ___    p99  ___    max  ___
  Throughput       in  30.0 fps    out  ___ fps
  Frames           produced 1800   delivered ___   dropped ___ (__%)
                   stale ___   oldest ___   newest 0   in-flight _
  Queue depth      mean ___   peak 8 / 8
  Adaptive         ___ state changes   time shedding ___%

  Files  results/run_001/{frames.csv, depth.csv, events.csv, summary.json}
```

Warnings appear between the result block and the file line, each starting with `warning:`.

### S5 Bench

```
Experiment e2_sustained  ·  12 configurations x 5 repeats = 60 runs  ·  ~62 min
  [ 7/60]  cost=50ms policy=drop-oldest rep=2   p95 ___ ms   drops __%   ok
  [ 8/60]  cost=50ms policy=drop-oldest rep=3   running…
```

After the run: a median table (one row per configuration) and the path to `experiment.json`.

### S6 Preview window

Window titled `FrameFlow — <policy>` showing the processed frame (aspect ratio kept).

```
+-- FrameFlow — adaptive ---------------------------+
|#################################################|  <- 4 px amber border
|#  +------------------+                         #|     only while SHEDDING
|#  | Age     96 ms    |  <- info panel,         #|
|#  | FPS     19.8     |     top-left, 4 lines,  #|
|#  | Drops   33%      |     12 px padding       #|
|#  | Policy  adaptive |                         #|
|#  +------------------+                         #|
|#         processed frame                       #|
|#################################################|
+-------------------------------------------------+
Keys: q/Esc stop · Space freeze image · o toggle panel
```

### S7 Analysis report

- `latency_over_time.png`: one line per policy, x = time (s), y = frame age (ms), warm-up shaded.
- `latency_vs_drops.png`: one point per configuration, x = drop rate (%), y = p95 latency (ms).
- `depth_over_time.png`: queue depth per policy.
- `RESULTS.md`: summary table (median, min-max) with links to the graphs.

## 5. User flows

### F1 First run

1. `frameflow run --source=synthetic --duration=10`
2. S2 appears instantly; S3 updates once per second; S4 prints; files written.
   - Branch: invalid flag → validation error (section 8), nothing starts.

### F2 Watch and compare policies

1. `frameflow run --input=video.mp4 --cost-ms=50 --policy=blocking --preview`
2. Preview opens; frame age climbs; user presses `q`.
3. Summary shows `valid` (if past warm-up) and the latency numbers.
4. Rerun with `--policy=drop-oldest`, then `--policy=adaptive`; identical layout for direct comparison.
   - Branch: no display → error with hint to use `--preview=auto` or drop `--preview`.

### F3 Run an experiment

1. Copy `experiments/template.yaml`, edit values.
2. `frameflow bench --config=my_exp.yaml` shows total runs and estimated time first.
3. S5 progress per run; a failed run is marked `failed` and the bench continues.
4. Final median table and path.
   - Branch: Ctrl+C → completed runs kept; message lists missing runs (also in `experiment.json`).

### F4 Analyse and share

1. `python3 scripts/analyze.py results/my_exp`
2. Script lists files it wrote; user copies graphs into the README.
   - Branch: missing or old-format files → error naming the file and expected `schema_version`.

## 6. Components

### 6.1 Terminal components (one C++ function each in `ui/`)

| Component | Used in | Spec |
| --- | --- | --- |
| Header line | S2, S4, S5 | `FrameFlow 1.0  ·  <context>`; bold title, dim context |
| Key-value block | S2, S4 | Key column padded to 12 chars; one fact per line |
| Metric row | S4 | Label (16 chars) + labelled values (`p95  120`), units in the label `(ms)` |
| Progress bar | S3, S5 | 10-cell bar `[####------]`, elapsed/total, then `·`-separated metrics |
| Status badge | S3, S4, S5 | `NORMAL`, `SHEDDING`, `valid`, `invalid`, `ok`, `failed` |
| Message line | All | Prefix `error:`, `warning:` or `info:`, message, then `(hint: ...)` |
| Path line | S4, S5 | `Files  <dir>/{a, b, c}` |
| Table | S5, `RESULTS.md` | Right-aligned numbers, header row, median with `(min-max)` |

### 6.2 Preview overlay components

| Component | Spec |
| --- | --- |
| Info panel | Black box at 60% opacity, top-left, 12 px padding, 4 lines |
| Metric line | `Age  96 ms`, `FPS  19.8`, `Drops  33%`, `Policy  adaptive` |
| State border | 4 px: none = NORMAL, amber = SHEDDING |
| Paused tag | `PAUSED` centred at top while the image is frozen |

### 6.3 Chart components

| Component | Spec |
| --- | --- |
| Time-series chart | Line per policy, x = time (s), y = latency (ms), warm-up shaded grey, target as dashed line |
| Trade-off scatter | x = drop rate (%), y = p95 latency (ms), one labelled point per configuration |
| Legend | Fixed order: blocking, drop-oldest, drop-newest, adaptive |
| Footer note | e.g. `median of 5 runs · <CPU> · Ubuntu 24.04` |

## 7. Interactions

### 7.1 Terminal

| Input | Effect | Feedback |
| --- | --- | --- |
| Ctrl+C (first) | Graceful stop | `info: stopping… writing results` then the summary |
| Ctrl+C (second) | Immediate exit (code 130) | `error: forced exit, results may be incomplete` |

### 7.2 Preview window

| Key | Effect |
| --- | --- |
| `q` / `Esc` / close button | Graceful stop of the run (same as Ctrl+C) |
| `Space` | Freeze/unfreeze displayed image; pipeline keeps running and measuring |
| `o` | Toggle the info panel |

Rules: key handling uses a 1 ms `waitKey` on the sink side; the preview always shows the newest processed frame and skips (never queues) older ones; no mouse interaction in the MVP.

## 8. Forms: flags and config files

### 8.1 Rules

1. Every field has a default, shown in `--help` in brackets.
2. Required-together fields are validated as a group (bursty mode needs all three spike flags).
3. All errors are shown at once.
4. Effective values are echoed back in S2.
5. Units are part of flag names (`--cost-ms`, `--target-ms`).

### 8.2 Validation message format

```
frameflow: error: 2 invalid options
  --queue 0        must be between 1 and 1024            (hint: try --queue=8)
  --exit-ratio 1.2 must be greater than 0 and less than 1 (hint: 0.7 is a good start)
Run 'frameflow run --help' to see all options.
```

Exit code 2.

### 8.3 Experiment file template (`experiments/template.yaml`)

```yaml
# FrameFlow experiment — copy and edit
experiment: my_experiment      # folder name under --out
source: { type: synthetic, resolution: 1280x720, fps: 30 }
processor: { type: synthetic } # or: blur, canny, grayscale
duration_s: 60                 # 1-3600
warmup_s: 3                    # excluded from statistics
repeats: 5                     # runs per configuration
seed: 42
matrix:                        # every combination is run
  cost_ms: [40, 50, 67]
  policy: [blocking, drop-oldest, drop-newest, adaptive]
  queue: [8]
adaptive: { target_ms: 100, exit_ratio: 0.7, window: 30 }
```

YAML errors report file, line and column: `my_exp.yaml:9:11: unknown key 'polcy' (did you mean 'policy'?)`.

## 9. Loading, error and empty states

### 9.1 Loading

| Situation | What the user sees |
| --- | --- |
| Opening a video (> 0.5 s) | `info: opening traffic_720p.mp4…` then S2 |
| Warm-up | Progress line shows `warm-up` badge instead of metrics |
| Writing results (> 0.5 s) | `info: writing results…` |
| Bench starting | Total runs and estimated duration |
| `analyze.py` | One line per file: `wrote results/e2/latency_over_time.png` |

### 9.2 Errors (`frameflow: error: <what>: <detail> (hint: <fix>)`)

| Situation | Example | Exit |
| --- | --- | --- |
| Invalid options | Section 8.2 | 2 |
| File not found | `cannot open input: samples/trafic.mp4: no such file (hint: check the path)` | 3 |
| Not decodable | `cannot decode input: clip.mkv: no video stream (hint: try an .mp4 with H.264)` | 3 |
| Output exists | `output folder exists: results/run_001 (hint: use --overwrite or a new --out)` | 4 |
| No display | `--preview needs a display (hint: use --preview=auto or remove --preview)` | 6 |
| Run as root | `refusing to run as root (hint: run as a normal user, or add --allow-root)` | 5 |
| Runtime failure | `worker stopped: <reason>; partial results saved to results/run_001 (marked invalid)` | 1 |

### 9.3 Warnings

| Situation | Message |
| --- | --- |
| Source late | `warning: source was late by > 5 ms on 3.2% of frames; system may be overloaded` |
| Stopped during warm-up | `warning: stopped during warm-up; results marked invalid` |
| Very short run | `warning: run shorter than 10 s; percentiles may be unstable` |

### 9.4 Empty states

| Situation | What the user sees |
| --- | --- |
| No frames delivered | Latency row shows `—` with `no frames delivered`; drop rate 100% |
| Fixed policy | Adaptive row omitted |
| `analyze.py` on empty folder | `error: no runs found in results/x (hint: run frameflow bench first)` |
| No drops | `0 (0%)`, reason breakdown omitted |

## 10. Responsive behavior

| Context | Behaviour |
| --- | --- |
| Terminal ≥ 100 cols | Full layouts |
| 80-99 cols | Progress line drops the bar; summary unchanged (designed for 80) |
| < 80 cols | Progress shows time, out FPS, drops only; summary one value per line |
| Pipe/file output | No colour, no carriage returns; progress lines only with `--progress` |
| `--json` | Only summary JSON on stdout; messages on stderr |
| `CI=true` | Same as pipe; progress every 5 s |
| Preview window | Opens at source resolution, scaled to fit 80% of screen; resizable, letterboxed; panel text 2.5% of window height (min 14 px) |
| Charts | 1600x900 px, 150 DPI; `--format=svg` option; text ≥ 10 pt |

Terminal width is read with `ioctl TIOCGWINSZ` at start and on `SIGWINCH`.

## 11. Accessibility

| Area | Requirement |
| --- | --- |
| Colour independence | Status is always a word; colour only reinforces it |
| `NO_COLOR` | Honour `NO_COLOR` env var and `--no-color` |
| Screen readers | No spinners or box art by default; plain lines when `TERM=dumb` or not a terminal |
| Machine-readable | `--json` summary |
| Contrast | Terminal colours readable on dark and light themes; overlay white on 60% black (≥ 7:1) |
| Colour-blind safe charts | Okabe-Ito palette plus distinct line styles and markers |
| Chart alternative | `RESULTS.md` tables hold the numbers behind every chart; images have alt text |
| Motion | Updates at most once per second; no blinking |
| Keyboard only | No mouse required |

## 12. Visual design

### 12.1 Typography

| Surface | Font | Sizes |
| --- | --- | --- |
| Terminal | User's monospace font | Bold titles/labels, normal values, dim secondary; no italics |
| Preview overlay | OpenCV `FONT_HERSHEY_SIMPLEX` | 2.5% of window height (min 14 px), `LINE_AA` |
| Charts | DejaVu Sans | Title 14 pt, axis 11 pt, ticks/legend 10 pt, footer 9 pt grey |

Numbers: latency 1 decimal under 100 ms, 0 above; FPS 1 decimal; percentages 1 decimal; right-aligned in tables.

### 12.2 Colours

Terminal (ANSI): error red (31), warning/SHEDDING yellow (33), valid/ok green (32), header/policy cyan (36), secondary dim (2).

Charts and overlay (Okabe-Ito):

| Element | Colour | Style |
| --- | --- | --- |
| blocking | `#D55E00` | solid, circle |
| drop-oldest | `#0072B2` | dashed, square |
| drop-newest | `#009E73` | dotted, triangle |
| adaptive | `#E69F00` | solid thick 2.5 px, diamond |
| Latency target | `#555555` | dashed thin |
| Warm-up shading | `#DDDDDD` at 50% | area |
| Overlay panel | `#000000` at 60%, text `#FFFFFF` | — |
| Shedding border | `#E69F00` (BGR `0,159,230` in OpenCV) | 4 px |

### 12.3 Spacing

| Surface | Rule |
| --- | --- |
| Terminal | 2-space indent; blank line between blocks; key column 12 chars; metric labels 16 chars; values separated by 3 spaces |
| Overlay | 12 px panel padding; line height 1.5x text height; panel 16 px from edges |
| Charts | 16:9; legend top-right inside plot; y-grid only; no top/right spines |
