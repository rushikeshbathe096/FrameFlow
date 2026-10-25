#!/usr/bin/env python3
"""Turn FrameFlow result folders into summary tables and graphs (SRS FR-ANA-01..03, UIUX S7).

Usage: analyze.py <results-dir> [--out DIR] [--target-ms T] [--no-charts]

<results-dir> may be an experiment folder (has experiment.json), a folder of experiment folders
(e.g. results/), a single run folder (has summary.json) or a folder of run folders.

For every experiment it writes, inside the experiment folder:
  summary.csv, summary.md          one row per configuration: median, min and max over the
                                   completed, valid repetitions (BR-11, FR-ANA-02)
  latency_over_time[_<group>].png  frame age per policy over time, rep 1 of each configuration
  depth_over_time[_<group>].png    queue depth per policy over time, rep 1 of each configuration
  latency_vs_drops.png             trade-off scatter: median drop % vs median p95, one labelled
                                   point per configuration (UIUX 6.3)
  state_changes.png                median adaptive state changes per configuration (adaptive only)
and for every run folder: latency_over_time.png and depth_over_time.png.
Bursty runs also get recovery time and drops outside spikes (SPEC 10, E3; D46).
With --out DIR the experiment-level files are also copied to DIR/<experiment>/ (docs/images).

Every problem with an input file exits 1 with a message naming the file (and the column), e.g.
`analyze.py: error: results/e2/x/rep_1/frames.csv: missing column 'latency_ms'` (ER-09).
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import pandas as pd

SCHEMA_VERSION = 1
RESULT_FILES = ("frames.csv", "depth.csv", "events.csv", "summary.json")
FRAME_COLUMNS = ("id", "t_cap_us", "t_push_us", "t_pop_us", "t_proc_start_us", "t_proc_end_us",
                 "t_out_us", "latency_ms", "lateness_us", "dropped", "reason", "warmup")
DEPTH_COLUMNS = ("t_us", "depth")
EVENT_COLUMNS = ("t_us", "type", "frame_id", "detail")
SUMMARY_KEYS = ("config", "environment", "metrics", "valid", "frameflow_version", "git_commit",
                "schema_version")
# Metrics in the summary table, in column order (summary.json `metrics` keys, D24).
TABLE_METRICS = ("p50_ms", "p95_ms", "p99_ms", "max_ms", "output_fps", "drop_rate_pct",
                 "depth_mean", "depth_max")
# Adaptive only (null for fixed policies, D45) and computed here for bursty runs (D46).
OPTIONAL_METRICS = ("state_changes", "shedding_pct")
DERIVED_METRICS = ("recovery_ms", "drops_outside_spikes", "lateness_p50_ms", "lateness_p99_ms")
MEMORY_METRICS = ("peak_rss_mb", "growth_pct")  # summary.json "memory" (PR-07, PR-08, D56)
DEFAULT_TARGET_MS = 100.0
# UIUX 6.3 / 12.2: fixed legend order and Okabe-Ito styles per policy.
POLICY_ORDER = ("blocking", "drop-oldest", "drop-newest", "adaptive")
POLICY_STYLE = {
    "blocking": {"color": "#D55E00", "linestyle": "-", "marker": "o", "linewidth": 1.5},
    "drop-oldest": {"color": "#0072B2", "linestyle": "--", "marker": "s", "linewidth": 1.5},
    "drop-newest": {"color": "#009E73", "linestyle": ":", "marker": "^", "linewidth": 1.5},
    "adaptive": {"color": "#E69F00", "linestyle": "-", "marker": "D", "linewidth": 2.5},
}
WARMUP_COLOR = "#DDDDDD"


class AnalysisError(Exception):
    """A missing or malformed input file; the message names the file (and column)."""


@dataclass
class Run:
    path: Path
    summary: dict
    derived: dict = field(default_factory=dict)  # DERIVED_METRICS, filled for bursty runs

    @property
    def policy(self) -> str:
        return str(self.summary["config"].get("policy", "unknown"))

    @property
    def warmup_s(self) -> float:
        return float(self.summary["config"].get("warmup_s", 0.0))


@dataclass
class Configuration:
    name: str
    params: dict
    runs: list[Run] = field(default_factory=list)  # completed runs in rep order


@dataclass
class Experiment:
    path: Path
    name: str
    repeats: int
    configs: list[Configuration]


# ---------------------------------------------------------------------------- loading


def read_json(path: Path) -> dict:
    if not path.is_file():
        raise AnalysisError(f"{path}: file is missing")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as e:
        raise AnalysisError(f"{path}: not valid JSON ({e})") from e
    if not isinstance(data, dict):
        raise AnalysisError(f"{path}: expected a JSON object")
    return data


def check_schema(path: Path, data: dict) -> None:
    version = data.get("schema_version")
    if version != SCHEMA_VERSION:
        raise AnalysisError(f"{path}: schema_version is {version!r}, expected {SCHEMA_VERSION} "
                            "(hint: re-run with this FrameFlow version)")


def read_csv(path: Path, columns: Iterable[str], numeric: Iterable[str] = ()) -> pd.DataFrame:
    """Reads a result CSV, checking the header and that `numeric` columns hold numbers."""
    if not path.is_file():
        raise AnalysisError(f"{path}: file is missing")
    try:
        frame = pd.read_csv(path, dtype=str, keep_default_na=False)
    except (OSError, pd.errors.ParserError, pd.errors.EmptyDataError, UnicodeDecodeError) as e:
        raise AnalysisError(f"{path}: cannot parse CSV ({e})") from e
    for column in columns:
        if column not in frame.columns:
            raise AnalysisError(f"{path}: missing column '{column}'")
    for column in numeric:
        values = frame[column].mask(frame[column] == "")  # empty = stage not reached -> NaN
        converted = pd.to_numeric(values, errors="coerce")
        bad = converted.isna() & values.notna()
        if bad.any():
            row = int(bad.idxmax()) + 2  # 1-based, after the header line
            raise AnalysisError(f"{path}: column '{column}' line {row}: "
                                f"'{frame[column][bad.idxmax()]}' is not a number")
        frame[column] = converted
    return frame


def load_run(path: Path) -> Run:
    """Checks all four result files of a run (FR-ANA-03) and returns its summary."""
    for name in RESULT_FILES:
        if not (path / name).is_file():
            raise AnalysisError(f"{path / name}: file is missing")
    summary = read_json(path / "summary.json")
    check_schema(path / "summary.json", summary)
    for key in SUMMARY_KEYS:
        if key not in summary:
            raise AnalysisError(f"{path / 'summary.json'}: missing key '{key}'")
    for metric in TABLE_METRICS:
        if metric not in summary["metrics"]:
            raise AnalysisError(f"{path / 'summary.json'}: missing key 'metrics.{metric}'")
    for name, columns in (("frames.csv", FRAME_COLUMNS), ("depth.csv", DEPTH_COLUMNS),
                          ("events.csv", EVENT_COLUMNS)):
        header = (path / name).read_text(encoding="utf-8").split("\n", 1)[0].split(",")
        for column in columns:
            if column not in header:
                raise AnalysisError(f"{path / name}: missing column '{column}'")
    return Run(path, summary)


def load_experiment(path: Path) -> Experiment:
    meta = read_json(path / "experiment.json")
    check_schema(path / "experiment.json", meta)
    for key in ("experiment", "repeats", "configurations", "runs"):
        if key not in meta:
            raise AnalysisError(f"{path / 'experiment.json'}: missing key '{key}'")
    configs = {c["name"]: Configuration(c["name"], dict(c.get("params", {})))
               for c in meta["configurations"]}
    for entry in meta["runs"]:
        if entry.get("status") != "completed":
            continue  # failed, interrupted and missing runs are not results (EC-20)
        config = configs.get(entry.get("config"))
        if config is None:
            raise AnalysisError(f"{path / 'experiment.json'}: run of unknown configuration "
                                f"'{entry.get('config')}'")
        config.runs.append(load_run(path / entry["path"]))
    return Experiment(path, str(meta["experiment"]), int(meta["repeats"]), list(configs.values()))


def runs_as_experiment(path: Path, run_dirs: list[Path]) -> Experiment:
    """Plain `frameflow run` folders: one configuration per run, named after the folder."""
    configs = []
    for run_dir in run_dirs:
        run = load_run(run_dir)
        configs.append(Configuration(run_dir.name, {"policy": run.policy}, [run]))
    return Experiment(path, path.name, 1, configs)


def discover(root: Path) -> list[Experiment]:
    if not root.is_dir():
        raise AnalysisError(f"{root}: no such folder")
    if (root / "experiment.json").is_file():
        return [load_experiment(root)]
    if (root / "summary.json").is_file():
        return [runs_as_experiment(root.parent, [root])]
    children = sorted(p for p in root.iterdir() if p.is_dir())
    experiments = [load_experiment(p) for p in children if (p / "experiment.json").is_file()]
    run_dirs = [p for p in children if (p / "summary.json").is_file()]
    if run_dirs:
        experiments.append(runs_as_experiment(root, run_dirs))
    if not experiments:
        raise AnalysisError(f"no runs found in {root} (hint: run frameflow bench first)")
    return experiments


# ---------------------------------------------------------------------------- bursty (D46)


def spike_windows(config: dict, end_s: float) -> list[tuple[float, float]]:
    """Spike [start, end) times in seconds from the run start, rebuilt from summary.json."""
    keys = ("spike_offset_s", "spike_len_s", "spike_every_s")
    if any(config.get(k) is None for k in keys):
        return []
    offset, length, every = (float(config[k]) for k in keys)
    windows = []
    start = offset
    while start < end_s:
        windows.append((start, start + length))
        start += every
    return windows


def recovery_ms(delivered: pd.DataFrame, spike_end_s: float, target_ms: float) -> float | None:
    """Spike end -> first delivered frame below target; 0 if already below; None if never."""
    before = delivered[delivered["t_out_s"] <= spike_end_s]
    if len(before) and before["latency_ms"].iloc[-1] < target_ms:
        return 0.0
    after = delivered[(delivered["t_out_s"] > spike_end_s) & (delivered["latency_ms"] < target_ms)]
    if after.empty:
        return None
    return (after["t_out_s"].iloc[0] - spike_end_s) * 1000.0


def target_ms_of(run: Run, default_target_ms: float) -> float:
    target = run.summary["config"].get("target_ms")
    return float(target) if target is not None else default_target_ms


def bursty_metrics(run: Run, default_target_ms: float) -> dict:
    """Mean recovery time and drops outside spikes, after warm-up; {} for non-bursty runs."""
    frames = read_csv(run.path / "frames.csv", FRAME_COLUMNS, ("t_cap_us", "t_out_us", "latency_ms"))
    end_s = float(frames["t_cap_us"].max()) / 1e6 if len(frames) else 0.0
    windows = spike_windows(run.summary["config"], end_s)
    if not windows:
        return {}
    delivered = frames[frames["latency_ms"].notna()].copy()
    delivered["t_out_s"] = delivered["t_out_us"] / 1e6
    delivered = delivered.sort_values("t_out_s")
    target = target_ms_of(run, default_target_ms)
    measured = [w for w in windows if w[0] >= run.warmup_s and w[1] <= end_s]
    recoveries = [r for r in (recovery_ms(delivered, end, target) for _, end in measured)
                  if r is not None]
    events = read_csv(run.path / "events.csv", EVENT_COLUMNS, ("t_us",))
    drop_s = events[events["type"] == "drop"]["t_us"] / 1e6
    drop_s = drop_s[drop_s >= run.warmup_s]
    near_spike = pd.Series(False, index=drop_s.index)
    for start, end in windows:  # "during and right after": [start, end + one spike length)
        near_spike |= (drop_s >= start) & (drop_s < end + (end - start))
    return {"recovery_ms": sum(recoveries) / len(recoveries) if recoveries else None,
            "drops_outside_spikes": int((~near_spike).sum())}


def nearest_rank(values: list[float], p: float) -> float | None:
    """FR-MET-03 nearest rank, as in the C++ statistics."""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(p / 100.0 * len(ordered) - 1e-9))
    return ordered[rank - 1]


def lateness_metrics(run: Run) -> dict:
    """PR-01: source lateness over every produced frame (D56)."""
    frames = read_csv(run.path / "frames.csv", FRAME_COLUMNS, ("lateness_us",))
    lateness_ms = [v / 1000.0 for v in frames["lateness_us"].dropna()]
    return {"lateness_p50_ms": nearest_rank(lateness_ms, 50),
            "lateness_p99_ms": nearest_rank(lateness_ms, 99)}


# ---------------------------------------------------------------------------- tables


def valid_runs(config: Configuration) -> list[Run]:
    return [r for r in config.runs if r.summary.get("valid") is True]


def metric_of(run: Run, metric: str):
    if metric in DERIVED_METRICS:
        return run.derived.get(metric)
    if metric in MEMORY_METRICS:
        return (run.summary.get("memory") or {}).get(metric)
    return run.summary["metrics"].get(metric)  # OPTIONAL_METRICS may be absent or null


def summary_rows(experiment: Experiment) -> pd.DataFrame:
    """One row per configuration: matrix params, run counts, median/min/max of each metric."""
    param_keys = []
    for config in experiment.configs:
        param_keys += [k for k in config.params if k not in param_keys]
    rows = []
    for config in experiment.configs:
        row = {"configuration": config.name}
        row.update({k: config.params.get(k, "") for k in param_keys})
        runs = valid_runs(config)
        row["runs"] = len(runs)
        row["invalid_runs"] = len(config.runs) - len(runs)
        for metric in TABLE_METRICS + OPTIONAL_METRICS + DERIVED_METRICS + MEMORY_METRICS:
            values = pd.Series([metric_of(r, metric) for r in runs], dtype="float64")
            values = values.dropna()  # null = could not be computed (EC-15), not 0
            row[f"{metric}_median"] = values.median() if len(values) else None
            row[f"{metric}_min"] = values.min() if len(values) else None
            row[f"{metric}_max"] = values.max() if len(values) else None
        rows.append(row)
    return pd.DataFrame(rows)


def fmt(value) -> str:
    if value is None or (isinstance(value, float) and pd.isna(value)):
        return "—"
    return f"{float(value):.1f}"


def markdown_table(experiment: Experiment, table: pd.DataFrame) -> str:
    metrics = ["p50_ms", "p95_ms", "p99_ms", "output_fps", "drop_rate_pct", "depth_max"]
    titles = ["p50 ms", "p95 ms", "p99 ms", "Out FPS", "Drop %", "Peak depth"]
    extra = (("recovery_ms", "Mean recovery ms"), ("drops_outside_spikes", "Drops outside spikes"),
             ("state_changes", "State changes"), ("shedding_pct", "Shedding %"),
             ("lateness_p50_ms", "Lateness p50 ms"), ("lateness_p99_ms", "Lateness p99 ms"),
             ("peak_rss_mb", "Peak RSS MB"), ("growth_pct", "RSS growth %"))
    for metric, title in extra:  # only when some configuration has the value
        if table[f"{metric}_median"].notna().any():
            metrics.append(metric)
            titles.append(title)
    lines = [f"# {experiment.name}", "",
             "Median (min–max) over the completed, valid repetitions of each configuration.", "",
             "| Configuration | Runs | " + " | ".join(titles) + " |",
             "| --- | --- | " + " | ".join("---" for _ in titles) + " |"]
    for _, row in table.iterrows():
        cells = [f"{fmt(row[f'{m}_median'])} ({fmt(row[f'{m}_min'])}–{fmt(row[f'{m}_max'])})"
                 for m in metrics]
        lines.append(f"| {row['configuration']} | {row['runs']} | " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def write_tables(experiment: Experiment, table: pd.DataFrame) -> list[Path]:
    csv_path = experiment.path / "summary.csv"
    md_path = experiment.path / "summary.md"
    table.to_csv(csv_path, index=False, float_format="%.3f")
    md_path.write_text(markdown_table(experiment, table), encoding="utf-8")
    return [csv_path, md_path]


# ---------------------------------------------------------------------------- graphs


def frames_of(run: Run) -> pd.DataFrame:
    return read_csv(run.path / "frames.csv", FRAME_COLUMNS, ("t_cap_us", "latency_ms"))


def depth_of(run: Run) -> pd.DataFrame:
    return read_csv(run.path / "depth.csv", DEPTH_COLUMNS, ("t_us", "depth"))


def footer(run: Run, note: str) -> str:
    env = run.summary.get("environment", {})
    return f"{note} · {env.get('cpu_model', 'unknown CPU')} · kernel {env.get('kernel', '?')}"


def policy_sort_key(policy: str) -> int:
    return POLICY_ORDER.index(policy) if policy in POLICY_ORDER else len(POLICY_ORDER)


def time_chart(path: Path, title: str, ylabel: str, series: list[tuple[Run, pd.Series, pd.Series]],
               note: str, step: bool = False) -> Path:
    """One line per policy (UIUX 6.3): x = time (s); warm-up shaded grey."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(10, 5), dpi=100)
    warmup = max((run.warmup_s for run, _, _ in series), default=0.0)
    if warmup > 0:
        ax.axvspan(0, warmup, color=WARMUP_COLOR, alpha=0.5, label="warm-up", linewidth=0)
    for run, x, y in sorted(series, key=lambda s: policy_sort_key(s[0].policy)):
        style = dict(POLICY_STYLE.get(run.policy, {"color": "#555555", "linestyle": "-"}))
        style.pop("marker", None)  # thousands of points: markers only in the legend
        if step:
            ax.step(x, y, where="post", label=run.policy, **style)
        else:
            ax.plot(x, y, label=run.policy, **style)
    ax.set_xlabel("time (s)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left")
    fig.text(0.01, 0.01, note, fontsize=8, color="#555555")
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    fig.savefig(path)
    plt.close(fig)
    return path


def latency_series(run: Run) -> tuple[Run, pd.Series, pd.Series]:
    frames = frames_of(run)
    delivered = frames[frames["latency_ms"].notna()]
    return run, delivered["t_cap_us"] / 1e6, delivered["latency_ms"]


def depth_series(run: Run) -> tuple[Run, pd.Series, pd.Series]:
    depth = depth_of(run)
    return run, depth["t_us"] / 1e6, depth["depth"]


def run_charts(run: Run) -> list[Path]:
    note = footer(run, f"{run.path.name} · policy {run.policy}")
    return [
        time_chart(run.path / "latency_over_time.png", f"Frame age · {run.path.parent.name}",
                   "frame age (ms)", [latency_series(run)], note),
        time_chart(run.path / "depth_over_time.png", f"Queue depth · {run.path.parent.name}",
                   "queue depth (frames)", [depth_series(run)], note, step=True),
    ]


def policy_groups(experiment: Experiment) -> dict[str, list[Configuration]]:
    """Configurations that differ only in policy share a chart; the key names the rest."""
    groups: dict[str, list[Configuration]] = {}
    for config in experiment.configs:
        if not config.runs:
            continue
        rest = [(k, v) for k, v in config.params.items() if k != "policy"]
        key = "_".join(f"{k.split('.')[-1]}-{v}" for k, v in rest)
        groups.setdefault(key, []).append(config)
    return groups


def tradeoff_scatter(experiment: Experiment, table: pd.DataFrame) -> Path | None:
    """UIUX 6.3: x = drop rate (%), y = p95 latency (ms), one labelled point per configuration."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    rows = table[table["p95_ms_median"].notna() & table["drop_rate_pct_median"].notna()]
    if rows.empty:
        return None
    policies = {c.name: (c.runs[0].policy if c.runs else "unknown") for c in experiment.configs}
    fig, ax = plt.subplots(figsize=(8, 6), dpi=100)
    seen = set()
    for _, row in rows.iterrows():
        policy = policies.get(row["configuration"], "unknown")
        style = POLICY_STYLE.get(policy, {"color": "#555555", "marker": "o"})
        ax.scatter(row["drop_rate_pct_median"], row["p95_ms_median"], color=style["color"],
                   marker=style.get("marker", "o"), s=60,
                   label=policy if policy not in seen else None, zorder=3)
        seen.add(policy)
        ax.annotate(row["configuration"], (row["drop_rate_pct_median"], row["p95_ms_median"]),
                    textcoords="offset points", xytext=(5, 5), fontsize=7)
    ax.set_xlabel("drop rate (%)")
    ax.set_ylabel("p95 latency (ms)")
    ax.set_title(f"Latency vs drops · {experiment.name}")
    ax.grid(True, alpha=0.3)
    handles, labels = ax.get_legend_handles_labels()
    order = sorted(range(len(labels)), key=lambda i: policy_sort_key(labels[i]))
    ax.legend([handles[i] for i in order], [labels[i] for i in order], loc="upper right")
    runs = int(rows["runs"].max())
    first = next(r for c in experiment.configs for r in c.runs)
    fig.text(0.01, 0.01, footer(first, f"median of {runs} runs"), fontsize=8, color="#555555")
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    path = experiment.path / "latency_vs_drops.png"
    fig.savefig(path)
    plt.close(fig)
    return path


def state_changes_chart(experiment: Experiment, table: pd.DataFrame) -> Path | None:
    """E5: median adaptive state changes per configuration (bars), adaptive configurations only."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    rows = table[table["state_changes_median"].notna()]
    if rows.empty:
        return None
    fig, ax = plt.subplots(figsize=(max(6, 0.8 * len(rows) + 2), 5), dpi=100)
    ax.bar(rows["configuration"], rows["state_changes_median"],
           color=POLICY_STYLE["adaptive"]["color"])
    ax.set_ylabel("state changes per run (median)")
    ax.set_title(f"Adaptive state changes · {experiment.name}")
    ax.tick_params(axis="x", labelrotation=45, labelsize=8)
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()
    path = experiment.path / "state_changes.png"
    fig.savefig(path)
    plt.close(fig)
    return path


def experiment_charts(experiment: Experiment) -> list[Path]:
    written = []
    for key, configs in policy_groups(experiment).items():
        suffix = f"_{key}" if key else ""
        first_runs = [c.runs[0] for c in configs]  # rep 1 of each configuration
        note = footer(first_runs[0], "rep 1 of each configuration")
        title = experiment.name + (f" · {key}" if key else "")
        written.append(time_chart(experiment.path / f"latency_over_time{suffix}.png",
                                  f"Frame age · {title}", "frame age (ms)",
                                  [latency_series(r) for r in first_runs], note))
        written.append(time_chart(experiment.path / f"depth_over_time{suffix}.png",
                                  f"Queue depth · {title}", "queue depth (frames)",
                                  [depth_series(r) for r in first_runs], note, step=True))
    return written


# ---------------------------------------------------------------------------- main


def analyze(root: Path, out: Path | None, charts: bool = True,
            target_ms: float = DEFAULT_TARGET_MS) -> list[Path]:
    written: list[Path] = []
    for experiment in discover(root):
        for config in experiment.configs:
            for run in config.runs:
                run.derived = {**bursty_metrics(run, target_ms), **lateness_metrics(run)}
        table = summary_rows(experiment)
        experiment_files = write_tables(experiment, table)
        if charts:
            for config in experiment.configs:
                for run in config.runs:
                    written += run_charts(run)
            experiment_files += experiment_charts(experiment)
            experiment_files += [p for p in (tradeoff_scatter(experiment, table),
                                             state_changes_chart(experiment, table)) if p]
        written += experiment_files
        if out is not None:
            target = out / experiment.name
            target.mkdir(parents=True, exist_ok=True)
            for path in experiment_files:
                written.append(Path(shutil.copy2(path, target / path.name)))
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Summary tables and graphs from FrameFlow results.")
    parser.add_argument("results", type=Path, help="experiment folder, run folder, or their parent")
    parser.add_argument("--out", type=Path, help="also copy experiment tables and graphs here")
    parser.add_argument("--no-charts", action="store_true", help="tables only")
    parser.add_argument("--target-ms", type=float, default=DEFAULT_TARGET_MS,
                        help="latency target for recovery time when a run has none (non-adaptive)")
    args = parser.parse_args(argv)
    try:
        for path in analyze(args.results, args.out, charts=not args.no_charts,
                            target_ms=args.target_ms):
            print(f"wrote {path}")
    except AnalysisError as e:
        print(f"analyze.py: error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
