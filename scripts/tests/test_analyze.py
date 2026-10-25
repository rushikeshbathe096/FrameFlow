"""Tests for scripts/analyze.py (D7: pytest). Fixtures are written into tmp_path by helpers, so
every test states exactly the data it depends on. One test runs the real binary when
FRAMEFLOW_BINARY is set (ctest sets it)."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pandas as pd
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import analyze  # noqa: E402

FRAMES_HEADER = ("id,t_cap_us,t_push_us,t_pop_us,t_proc_start_us,t_proc_end_us,t_out_us,"
                 "latency_ms,lateness_us,dropped,reason,warmup\n")


def metrics(p95: float | None, **extra) -> dict:
    base = {name: 1.0 for name in analyze.TABLE_METRICS}
    base["p95_ms"] = p95
    base.update(extra)
    return base


def write_run(path: Path, policy: str = "blocking", p95: float | None = 10.0, valid: bool = True,
              frames: str | None = None, warmup_s: float = 0.0, config: dict | None = None,
              events: str = "", **metric_overrides) -> Path:
    path.mkdir(parents=True)
    if frames is None:
        frames = FRAMES_HEADER + "".join(
            f"{i},{i * 20000},{i * 20000},,,,{i * 20000 + 5000},{5 + i},0,0,,0\n" for i in range(50))
    (path / "frames.csv").write_text(frames)
    (path / "depth.csv").write_text("t_us,depth\n0,1\n10000,0\n20000,1\n")
    (path / "events.csv").write_text("t_us,type,frame_id,detail\n" + events)
    summary = {"schema_version": 1, "frameflow_version": "0.1.0", "git_commit": "abc",
               "valid": valid,
               "config": {"policy": policy, "warmup_s": warmup_s, **(config or {})},
               "environment": {"cpu_model": "Test CPU", "kernel": "6.8"},
               "metrics": metrics(p95, **metric_overrides)}
    (path / "summary.json").write_text(json.dumps(summary))
    return path


def write_experiment(path: Path, configs: dict[str, dict], reps: dict[str, list[dict]],
                     statuses: dict[tuple[str, int], str] | None = None) -> Path:
    """configs: name -> params; reps: name -> list of write_run kwargs (one per repetition)."""
    statuses = statuses or {}
    runs = []
    for name, rep_list in reps.items():
        for rep, kwargs in enumerate(rep_list, start=1):
            status = statuses.get((name, rep), "completed")
            if status != "missing":
                write_run(path / name / f"rep_{rep}", **kwargs)
            runs.append({"config": name, "rep": rep, "path": f"{name}/rep_{rep}",
                         "status": status})
    meta = {"schema_version": 1, "experiment": path.name, "repeats": 3,
            "configurations": [{"name": n, "params": p} for n, p in configs.items()],
            "runs": runs}
    (path / "experiment.json").write_text(json.dumps(meta))
    return path


def two_policy_experiment(tmp_path: Path) -> Path:
    return write_experiment(
        tmp_path / "e2",
        {"policy-blocking": {"policy": "blocking"}, "policy-drop-oldest": {"policy": "drop-oldest"}},
        {"policy-blocking": [{"p95": 30.0}, {"p95": 10.0}, {"p95": 20.0}],
         "policy-drop-oldest": [{"policy": "drop-oldest", "p95": 5.0},
                                {"policy": "drop-oldest", "p95": 7.0}]})


def error_of(tmp_path: Path, capsys, target: Path) -> str:
    assert analyze.main([str(target), "--no-charts"]) == 1
    return capsys.readouterr().err


# ---------------------------------------------------------------------------- tables (P3.6)


def test_median_min_max_per_configuration(tmp_path):
    experiment = analyze.discover(two_policy_experiment(tmp_path))[0]
    table = analyze.summary_rows(experiment).set_index("configuration")
    assert table.loc["policy-blocking", "p95_ms_median"] == 20.0  # odd count: middle value
    assert table.loc["policy-blocking", "p95_ms_min"] == 10.0
    assert table.loc["policy-blocking", "p95_ms_max"] == 30.0
    assert table.loc["policy-drop-oldest", "p95_ms_median"] == 6.0  # even: mean of the middle two
    assert table.loc["policy-blocking", "runs"] == 3


def test_tables_written_as_csv_and_markdown(tmp_path):
    path = two_policy_experiment(tmp_path)
    analyze.main([str(path), "--no-charts"])
    csv = pd.read_csv(path / "summary.csv")
    assert list(csv["configuration"]) == ["policy-blocking", "policy-drop-oldest"]
    assert "p95_ms_median" in csv.columns and "policy" in csv.columns
    md = (path / "summary.md").read_text()
    assert "| policy-blocking | 3 | 1.0 (1.0–1.0) | 20.0 (10.0–30.0) |" in md


def test_invalid_and_unfinished_runs_are_excluded(tmp_path):
    path = write_experiment(
        tmp_path / "e",
        {"policy-blocking": {"policy": "blocking"}},
        {"policy-blocking": [{"p95": 10.0}, {"p95": 99.0, "valid": False}, {"p95": 50.0},
                             {"p95": 70.0}]},
        statuses={("policy-blocking", 3): "interrupted", ("policy-blocking", 4): "missing"})
    row = analyze.summary_rows(analyze.discover(path)[0]).iloc[0]
    assert row["runs"] == 1
    assert row["invalid_runs"] == 1
    assert row["p95_ms_median"] == 10.0


def test_null_metric_is_not_zero(tmp_path):
    path = write_experiment(tmp_path / "e", {"default": {}}, {"default": [{"p95": None}]})
    analyze.main([str(path), "--no-charts"])
    assert "| default | 1 | 1.0 (1.0–1.0) | — (—–—) |" in (path / "summary.md").read_text()


def test_discovers_experiments_and_plain_runs_under_a_parent(tmp_path):
    two_policy_experiment(tmp_path)
    write_run(tmp_path / "run_001", policy="drop-newest")
    names = [e.name for e in analyze.discover(tmp_path)]
    assert names == ["e2", tmp_path.name]


def test_single_run_folder(tmp_path):
    run = write_run(tmp_path / "run_001")
    experiments = analyze.discover(run)
    assert len(experiments) == 1 and experiments[0].configs[0].runs[0].path == run


# ---------------------------------------------------------------------------- errors (ER-09)


def test_empty_folder_says_no_runs(tmp_path, capsys):
    err = error_of(tmp_path, capsys, tmp_path)
    assert f"no runs found in {tmp_path} (hint: run frameflow bench first)" in err


def test_missing_result_file_is_named(tmp_path, capsys):
    path = two_policy_experiment(tmp_path)
    (path / "policy-blocking" / "rep_2" / "events.csv").unlink()
    err = error_of(tmp_path, capsys, path)
    assert f"{path / 'policy-blocking' / 'rep_2' / 'events.csv'}: file is missing" in err


def test_missing_column_names_file_and_column(tmp_path, capsys):
    run = write_run(tmp_path / "run_001", frames="id,t_cap_us\n0,0\n")
    err = error_of(tmp_path, capsys, run)
    assert f"{run / 'frames.csv'}: missing column 't_push_us'" in err


def test_non_numeric_value_names_column_and_line(tmp_path, capsys):
    frames = FRAMES_HEADER + "0,0,0,,,,5000,5,0,0,,0\n1,20000,20000,,,,25000,abc,0,0,,0\n"
    run = write_run(tmp_path / "run_001", frames=frames)
    assert analyze.main([str(run)]) == 1  # charts read the full CSV
    err = capsys.readouterr().err
    assert f"{run / 'frames.csv'}: column 'latency_ms' line 3: 'abc' is not a number" in err


def test_wrong_schema_version_names_expected_version(tmp_path, capsys):
    run = write_run(tmp_path / "run_001")
    summary = json.loads((run / "summary.json").read_text())
    summary["schema_version"] = 0
    (run / "summary.json").write_text(json.dumps(summary))
    err = error_of(tmp_path, capsys, run)
    assert "schema_version is 0, expected 1" in err


def test_malformed_json_is_named(tmp_path, capsys):
    run = write_run(tmp_path / "run_001")
    (run / "summary.json").write_text("{not json")
    err = error_of(tmp_path, capsys, run)
    assert f"{run / 'summary.json'}: not valid JSON" in err


def test_missing_metric_key_is_named(tmp_path, capsys):
    run = write_run(tmp_path / "run_001")
    summary = json.loads((run / "summary.json").read_text())
    del summary["metrics"]["depth_max"]
    (run / "summary.json").write_text(json.dumps(summary))
    assert "missing key 'metrics.depth_max'" in error_of(tmp_path, capsys, run)


# ---------------------------------------------------------------------------- graphs (P3.7)

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def assert_png(path: Path) -> None:
    assert path.is_file(), path
    data = path.read_bytes()
    assert data.startswith(PNG_SIGNATURE), path
    assert len(data) > 10_000, f"{path} is suspiciously small ({len(data)} bytes)"


def test_charts_per_run_and_per_experiment(tmp_path, capsys):
    path = two_policy_experiment(tmp_path)
    assert analyze.main([str(path)]) == 0
    assert_png(path / "latency_over_time.png")
    assert_png(path / "depth_over_time.png")
    for rep in ("rep_1", "rep_2", "rep_3"):
        assert_png(path / "policy-blocking" / rep / "latency_over_time.png")
    assert f"wrote {path / 'latency_over_time.png'}" in capsys.readouterr().out  # UIUX 9.1


def test_one_chart_per_non_policy_group(tmp_path):
    configs = {f"cost_ms-{c}_policy-{p}": {"cost_ms": str(c), "policy": p}
               for c in (40, 50) for p in ("blocking", "drop-oldest")}
    reps = {name: [{"policy": params["policy"]}] for name, params in configs.items()}
    path = write_experiment(tmp_path / "e2", configs, reps)
    analyze.main([str(path)])
    assert_png(path / "latency_over_time_cost_ms-40.png")
    assert_png(path / "depth_over_time_cost_ms-50.png")
    assert not (path / "latency_over_time.png").exists()


def test_out_copies_experiment_files(tmp_path):
    path = two_policy_experiment(tmp_path / "results")
    out = tmp_path / "docs_images"
    analyze.main([str(path), "--out", str(out)])
    for name in ("summary.csv", "summary.md", "latency_over_time.png", "depth_over_time.png"):
        assert (out / "e2" / name).is_file(), name


def test_warmup_is_shaded(tmp_path):
    run = write_run(tmp_path / "run_001", warmup_s=0.3)
    analyze.main([str(run)])
    assert_png(run / "latency_over_time.png")


# ---------------------------------------------------------------------------- P4.6 (D46)

BURSTY = {"spike_offset_s": 1.0, "spike_len_s": 1.0, "spike_every_s": 4.0}


def frame_rows(rows: list[tuple[float, float]]) -> str:
    """(t_cap_s, latency_ms) per delivered frame -> frames.csv text."""
    text = FRAMES_HEADER
    for i, (t_cap, latency) in enumerate(rows):
        cap = int(t_cap * 1e6)
        out = cap + int(latency * 1000)
        text += f"{i},{cap},{cap},,,,{out},{latency},0,0,,0\n"
    return text


def test_spike_windows_from_summary():
    assert analyze.spike_windows(BURSTY, 10.0) == [(1.0, 2.0), (5.0, 6.0), (9.0, 10.0)]
    assert analyze.spike_windows({"spike_offset_s": None}, 10.0) == []


def test_recovery_time_after_a_spike(tmp_path):
    # Latency 300 ms at the spike end (2 s), back below 100 ms for the frame delivered at 2.45 s.
    rows = [(0.5, 10), (1.5, 300), (1.9, 250), (2.3, 150), (2.4, 50), (3.0, 10), (3.5, 10)]
    run = analyze.load_run(write_run(tmp_path / "r", frames=frame_rows(rows), config=BURSTY))
    metrics = analyze.bursty_metrics(run, 100.0)
    assert metrics["recovery_ms"] == pytest.approx(450.0)  # t_out 2.45 s - spike end 2.0 s


def test_recovery_is_zero_when_latency_stays_below_target(tmp_path):
    rows = [(t / 10, 20) for t in range(40)]
    run = analyze.load_run(write_run(tmp_path / "r", frames=frame_rows(rows), config=BURSTY))
    assert analyze.bursty_metrics(run, 100.0)["recovery_ms"] == 0.0


def test_adaptive_run_uses_its_own_target(tmp_path):
    rows = [(1.5, 300), (2.1, 80), (2.3, 40), (3.5, 10)]
    config = dict(BURSTY, target_ms=50.0)
    run = analyze.load_run(write_run(tmp_path / "r", policy="adaptive", frames=frame_rows(rows),
                                     config=config))
    # T = 50: the 80 ms frame (out 2.18 s) is above it; the 40 ms frame goes out at 2.34 s.
    assert analyze.bursty_metrics(run, 100.0)["recovery_ms"] == pytest.approx(340.0)


def test_drops_outside_spikes(tmp_path):
    # Spikes [1, 2) and [5, 6): drops count as "near" until end + 1 s, i.e. [1, 3) and [5, 7).
    events = "".join(f"{int(t * 1e6)},drop,{i},stale\n" for i, t in enumerate((1.5, 2.9, 3.5, 4.0, 6.5)))
    events += "1200000,state_change,,NORMAL->SHEDDING L_ms=120.000\n"
    rows = [(t / 10, 20) for t in range(80)]
    run = analyze.load_run(write_run(tmp_path / "r", frames=frame_rows(rows), config=BURSTY,
                                     events=events))
    assert analyze.bursty_metrics(run, 100.0)["drops_outside_spikes"] == 2  # 3.5 s and 4.0 s


def test_drops_in_warmup_are_not_counted(tmp_path):
    events = "500000,drop,0,oldest\n"
    rows = [(t / 10, 20) for t in range(40)]
    run = analyze.load_run(write_run(tmp_path / "r", frames=frame_rows(rows), config=BURSTY,
                                     events=events, warmup_s=0.8))
    assert analyze.bursty_metrics(run, 100.0)["drops_outside_spikes"] == 0


def test_fixed_rate_runs_have_no_bursty_metrics(tmp_path):
    run = analyze.load_run(write_run(tmp_path / "r"))
    assert analyze.bursty_metrics(run, 100.0) == {}


def test_extra_columns_only_when_present(tmp_path):
    plain = two_policy_experiment(tmp_path / "a")
    analyze.main([str(plain), "--no-charts"])
    assert "State changes" not in (plain / "summary.md").read_text()
    adaptive = write_experiment(tmp_path / "b", {"policy-adaptive": {"policy": "adaptive"}},
                                {"policy-adaptive": [{"policy": "adaptive", "state_changes": 4,
                                                      "shedding_pct": 25.0}]})
    analyze.main([str(adaptive), "--no-charts"])
    md = (adaptive / "summary.md").read_text()
    assert "State changes" in md and "Shedding %" in md
    assert "| 4.0 (4.0–4.0) | 25.0 (25.0–25.0) |" in md


def test_tradeoff_scatter_and_state_change_chart(tmp_path):
    path = write_experiment(
        tmp_path / "e5",
        {"target_ms-50": {"adaptive.target_ms": "50"}, "target_ms-100": {"adaptive.target_ms": "100"}},
        {"target_ms-50": [{"policy": "adaptive", "state_changes": 6, "drop_rate_pct": 20.0}],
         "target_ms-100": [{"policy": "adaptive", "state_changes": 2, "drop_rate_pct": 10.0}]})
    analyze.main([str(path)])
    assert_png(path / "latency_vs_drops.png")
    assert_png(path / "state_changes.png")


def test_no_state_change_chart_without_adaptive(tmp_path):
    path = two_policy_experiment(tmp_path)
    analyze.main([str(path)])
    assert_png(path / "latency_vs_drops.png")
    assert not (path / "state_changes.png").exists()


# ---------------------------------------------------------------------------- P5.9 (D56)


def test_lateness_percentiles_use_nearest_rank(tmp_path):
    frames = FRAMES_HEADER + "".join(
        f"{i},{i * 1000},,,,,,,{(i + 1) * 100},0,,0\n" for i in range(100))  # 0.1 .. 10 ms
    run = analyze.load_run(write_run(tmp_path / "r", frames=frames))
    metrics = analyze.lateness_metrics(run)
    assert metrics["lateness_p50_ms"] == pytest.approx(5.0)
    assert metrics["lateness_p99_ms"] == pytest.approx(9.9)


def test_memory_metrics_in_tables(tmp_path):
    path = write_experiment(tmp_path / "e", {"default": {}}, {"default": [{}, {}, {}]})
    for rep, peak in ((1, 100.0), (2, 110.0), (3, 120.0)):
        summary_path = path / "default" / f"rep_{rep}" / "summary.json"
        summary = json.loads(summary_path.read_text())
        summary["memory"] = {"peak_rss_mb": peak, "growth_pct": 0.5}
        summary_path.write_text(json.dumps(summary))
    analyze.main([str(path), "--no-charts"])
    table = pd.read_csv(path / "summary.csv").set_index("configuration")
    assert table.loc["default", "peak_rss_mb_median"] == 110.0
    assert "Peak RSS MB" in (path / "summary.md").read_text()


# ---------------------------------------------------------------------------- real output


@pytest.mark.skipif("FRAMEFLOW_BINARY" not in os.environ, reason="needs the built frameflow")
def test_real_bench_output(tmp_path):
    (tmp_path / "e.yaml").write_text(
        "experiment: real\nsource: { type: synthetic, resolution: 32x32, fps: 100 }\n"
        "frames: 40\nwarmup_s: 0\ncost_ms: 15\nrepeats: 2\n"
        "matrix:\n  policy: [blocking, drop-newest]\n")
    subprocess.run([os.environ["FRAMEFLOW_BINARY"], "bench", f"--config={tmp_path / 'e.yaml'}",
                    f"--out={tmp_path}"], check=True, capture_output=True)
    assert analyze.main([str(tmp_path / "real")]) == 0
    table = pd.read_csv(tmp_path / "real" / "summary.csv").set_index("configuration")
    assert table.loc["policy-blocking", "runs"] == 2
    assert table.loc["policy-blocking", "drop_rate_pct_median"] == 0.0
    assert table.loc["policy-drop-newest", "drop_rate_pct_median"] > 0.0
    assert_png(tmp_path / "real" / "latency_over_time.png")


@pytest.mark.skipif("FRAMEFLOW_BINARY" not in os.environ, reason="needs the built frameflow")
def test_real_bursty_adaptive_output(tmp_path):
    (tmp_path / "e.yaml").write_text(
        "experiment: bursty\nsource: { type: synthetic, resolution: 32x32, fps: 30 }\n"
        "duration_s: 4\nwarmup_s: 0.5\nrepeats: 1\ncost_mode: bursty\ncost_ms: 20\n"
        "spike_ms: 120\nspike_len: 500ms\nspike_every: 1s\n"
        "adaptive: { target_ms: 100, window: 10 }\n"
        "matrix:\n  policy: [blocking, adaptive]\n")
    subprocess.run([os.environ["FRAMEFLOW_BINARY"], "bench", f"--config={tmp_path / 'e.yaml'}",
                    f"--out={tmp_path}"], check=True, capture_output=True)
    assert analyze.main([str(tmp_path / "bursty")]) == 0
    table = pd.read_csv(tmp_path / "bursty" / "summary.csv").set_index("configuration")
    assert pd.notna(table.loc["policy-adaptive", "state_changes_median"])
    assert pd.isna(table.loc["policy-blocking", "state_changes_median"])
    assert pd.notna(table.loc["policy-blocking", "drops_outside_spikes_median"])
    assert_png(tmp_path / "bursty" / "latency_vs_drops.png")
    assert_png(tmp_path / "bursty" / "state_changes.png")
