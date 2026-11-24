#!/usr/bin/env python3
"""Sequential, synthetic-only KEEFI P5 measurement and before/after comparison."""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(repo: Path, *args: str) -> str:
    return subprocess.check_output(["git", *args], cwd=repo, text=True).strip()


def stats(values: list[float]) -> dict | None:
    if not values:
        return None
    ordered = sorted(values)
    def at(q: float) -> float:
        return ordered[max(0, math.ceil(q * len(ordered)) - 1)]
    return {"n": len(ordered), "p50": at(.5), "p90": at(.9), "p95": at(.95), "max": ordered[-1]}


def split_samples(value: str) -> list[float]:
    return [float(x) for x in value.split("|") if x and float(x) >= 0]


def summarize(path: Path) -> dict:
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    result = {}
    for operation in sorted({row["operation"] for row in rows}):
        selected = [row for row in rows if row["operation"] == operation]
        first = [float(row["first_paint_end_ms"]) for row in selected if float(row["first_paint_end_ms"]) >= 0]
        paints = [x for row in selected for x in split_samples(row["paint_wall_samples_ms"])]
        cpu_paints = [x for row in selected for x in split_samples(row["paint_cpu_samples_ms"])]
        gaps = [x for row in selected for x in split_samples(row["event_loop_gaps_ms"])]
        counters = {key: sum(int(row[key]) for row in selected) for key in (
            "paint_count", "ticks", "quote_batches", "chart_resets", "chart_inserted", "chart_removed", "chart_changed",
            "explore_resets", "explore_inserted", "explore_removed", "portfolio_resets", "portfolio_inserted", "portfolio_removed")}
        input_goal = operation.startswith("tab_") or operation == "symbol_chart"
        motion_goal = operation in {"wheel_zoom", "drag_pan", "watchlist_wheel"}
        fraction = sum(x <= 16 for x in paints) / len(paints) if paints else None
        result[operation] = {
            "samples": len(selected), "first_paint_ms": stats(first), "no_paint_samples": len(selected) - len(first),
            "sync_ms": stats([float(row["sync_ms"]) for row in selected]),
            "cpu_ms": stats([float(row["cpu_ms"]) for row in selected]),
            "paint_wall_ms": stats(paints), "paint_cpu_ms": stats(cpu_paints), "event_loop_gap_ms": stats(gaps),
            "paint_at_most_16ms_fraction": fraction, "model_deltas": counters,
            "first_paint_100ms_goal": (max(first) <= 100 if len(first) == len(selected) else None) if input_goal else None,
            "paint_90percent_16ms_goal": (fraction >= .90 if fraction is not None else None) if motion_goal else None,
        }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--phase", choices=("baseline", "after"), required=True)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--baseline", type=Path, help="Baseline summary.json for the after comparison")
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--themes", default="light,dark")
    parser.add_argument("--sizes", default="1800x1125,1440x900,720x900")
    parser.add_argument("--mode", choices=("ticks", "idle"), default="ticks")
    args = parser.parse_args()
    repo, probe, output = args.repo.resolve(), args.probe.resolve(), args.output.resolve()
    head = git(repo, "rev-parse", "HEAD")
    expected = git(repo, "rev-parse", args.expected_commit)
    if head != expected:
        parser.error(f"HEAD differs from the explicit measured commit: {head}")
    product_dirty = git(repo, "diff", "--name-only", "HEAD", "--", "fincept-qt/src/keefi")
    if args.phase == "baseline" and product_dirty:
        parser.error("Baseline must use committed P4 product sources; source changes are pending")
    if not probe.is_file() or not 1 <= args.rounds <= 100:
        parser.error("Probe must exist and rounds must be in 1..100")
    conditions = []
    for theme in args.themes.split(","):
        if theme not in {"light", "dark"}:
            parser.error("Only light/dark themes are supported")
        for size in args.sizes.split(","):
            width, height = map(int, size.split("x"))
            if width < 720 or height < 720:
                parser.error("Minimum measured size is 720x720")
            conditions.append((theme, width, height))
    output.mkdir(parents=True, exist_ok=False)
    build = next((path for path in probe.parents if (path / "libkeefi_ui.a").is_file()), None)
    if build is None:
        parser.error("Cannot locate the linked KEEFI build libraries")
    libraries = {str(path): sha(path) for path in (build / "libkeefi_ui.a", build / "libkeefi_trading.a", build / "libkeefi_vault.a") if path.exists()}
    manifest = {
        "phase": args.phase, "head": head, "probe": str(probe), "probe_sha256": sha(probe), "libraries": libraries,
        "source_changes": product_dirty.splitlines(), "source_diff_sha256": hashlib.sha256(git(repo, "diff", "--binary", "HEAD", "--", "fincept-qt/src/keefi").encode()).hexdigest(), "status": git(repo, "status", "--short").splitlines(),
        "workload": {"holdings": 10, "watchlist": 50, "native_closed_candles": 200, "forming": args.mode == "ticks", "candle_arrival_hz": 10 if args.mode == "ticks" else 0,
                     "public_quote_arrival_hz": 10 if args.mode == "ticks" else 0, "public_quote_ui_flush_hz": 1, "rounds": args.rounds, "mode": args.mode, "symbol_input_protocol":"visible-mouse-wide_qt-row-activation-with-visible-chart-narrow"},
        "goals": {"tab_symbol_first_paint_all_observed_ms": 100, "motion_target_paint_ms": 16, "motion_fraction": .90},
        "scope": "Native Cocoa target QWidget Paint handler completion. Physical input/compositor latency and personal-profile behavior are unmeasured.",
        "conditions": {},
    }
    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = "cocoa"
    env.pop("KEEFI_G8_LIVE_PUBLIC", None)
    summaries = {}
    for theme, width, height in conditions:
        name = f"{theme}-{width}x{height}"
        raw, log, capture = output / f"{name}.csv", output / f"{name}.log", output / f"{name}.png"
        cmd = [str(probe), "--width", str(width), "--height", str(height), "--theme", theme, "--rounds", str(args.rounds),
               "--mode", args.mode, "--output", str(raw), "--capture", str(capture)]
        started = time.monotonic()
        with log.open("w") as stream:
            result = subprocess.run(cmd, cwd=repo, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=240)
        entry = {"command": cmd, "returncode": result.returncode, "wall_seconds": time.monotonic() - started}
        manifest["conditions"][name] = entry
        fixture_line = next((line[len("P5_FIXTURE "):] for line in log.read_text().splitlines() if line.startswith("P5_FIXTURE ")), None)
        if result.returncode != 0 or fixture_line is None:
            (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
            print(f"FAILED {name}: see {log}", file=sys.stderr)
            return 1
        fixture = json.loads(fixture_line)
        entry["fixture"] = fixture
        required = {"platform": "cocoa", "holdings": 10, "chartHoldings": 10, "watchlist": 50, "chartRows": 50, "width": width, "height": height, "injectedLiveTransports": 0, "dormantNativeFinished": 0, "temporarySyntheticProfile": True}
        if any(fixture.get(key) != value for key, value in required.items()):
            (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
            print(f"FAILED fixture {name}: {fixture}", file=sys.stderr)
            return 1
        summaries[name] = summarize(raw)
        entry["raw_sha256"], entry["capture_sha256"] = sha(raw), sha(capture)
        (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        print(f"MEASURED {name}: {len(summaries[name])} operation groups", flush=True)
    summary = {"phase": args.phase, "head": head, "workload": manifest["workload"], "conditions": summaries}
    (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    if args.baseline:
        baseline = json.loads(args.baseline.read_text())
        if baseline["workload"] != summary["workload"]:
            parser.error("Before/after workloads differ; do not compare them")
        comparison = {}
        for condition, operations in summaries.items():
            comparison[condition] = {}
            for operation, after in operations.items():
                before = baseline["conditions"][condition][operation]
                fields = {}
                for field in ("first_paint_ms", "cpu_ms", "paint_wall_ms", "paint_cpu_ms", "event_loop_gap_ms"):
                    a, b = after[field], before[field]
                    fields[field] = {"before": b, "after": a, "p95_delta_ms": a["p95"] - b["p95"] if a and b else None}
                comparison[condition][operation] = {"timing": fields, "before_model_deltas": before["model_deltas"], "after_model_deltas": after["model_deltas"],
                                                   "no_paint_before": before["no_paint_samples"], "no_paint_after": after["no_paint_samples"]}
        (output / "comparison.json").write_text(json.dumps(comparison, ensure_ascii=False, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
