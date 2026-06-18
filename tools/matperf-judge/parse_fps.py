#!/usr/bin/env python3
"""
parse_fps.py - CSOZ renderer FPS/perf log parser + pass/fail gate.

Parses the CSZ renderer's engine.log (written once per second when the engine
runs with `developer 1`/`-dev 2` AND launch flag `-log`). Slices the sampling
window delimited by console `echo` markers, computes FPS statistics (avg / min /
max / 1% low / worst frame ms), aggregates per-pass ms timings, scans the FULL
log for GL/fatal errors, and applies a PASS/FAIL gate.

FROZEN log line formats this parser matches (robustly, tolerating extra
whitespace):
  [CSZ:fps] fps=%.1f avg_ms=%.2f worst_ms=%.2f frames=%d
  [CSZ:fps] pass-ms avg: shadow=.. sky=.. world=.. brush=.. decal=.. studio=..
            lights=.. volume=.. trans=.. delegate=.. triapi=.. viewmodel=..

CLI:
  python parse_fps.py <engine.log>
      [--start-marker MATPERF-FPS-START] [--end-marker MATPERF-FPS-END]
      [--fps-cap 200] [--min-avg 199] [--min-1low 195] [--json OUT.json]

Exit code: 0 on PASS, 1 on FAIL (or fatal usage error). THIS IS A GATE.
"""
import argparse
import json
import math
import re
import sys

# --- FROZEN line formats -----------------------------------------------------
# fps summary line. Numbers captured loosely; whitespace tolerated.
RE_FPS = re.compile(
    r"\[CSZ:fps\].*?\bfps\s*=\s*([\d.]+)\s+"
    r"avg_ms\s*=\s*([\d.]+)\s+"
    r"worst_ms\s*=\s*([\d.]+)\s+"
    r"frames\s*=\s*(\d+)",
    re.IGNORECASE,
)
# pass-ms line. We pull every key=value pair after "pass-ms avg:" generically so
# that if a pass column is added/removed the parser still works.
RE_PASSMS_LINE = re.compile(r"\[CSZ:fps\].*?pass-ms\s+avg\s*:", re.IGNORECASE)
RE_KV = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([-\d.]+)")

# Canonical pass ordering (for stable table output); any extras appended after.
PASS_ORDER = [
    "shadow", "sky", "world", "brush", "decal", "studio",
    "lights", "volume", "trans", "delegate", "triapi", "viewmodel",
]

# Error / fatal patterns scanned over the WHOLE log (not just the window).
ERROR_PATTERNS = [
    re.compile(r"GL error", re.IGNORECASE),
    re.compile(r"Host_Error"),
    re.compile(r"CSZ_FatalInit"),
    re.compile(r"\[CSZ.*?[Ee]rror"),
]


def slice_window(lines, start_marker, end_marker):
    """Return (window_lines, used_markers_bool, warning_or_None).

    Window = lines strictly between the first START marker and the next END
    marker. If markers are absent, returns the whole file with a warning.
    """
    start_idx = None
    end_idx = None
    for i, ln in enumerate(lines):
        if start_idx is None and start_marker in ln:
            start_idx = i
            continue
        if start_idx is not None and end_marker in ln:
            end_idx = i
            break
    if start_idx is None and end_idx is None:
        return lines, False, (
            f"markers not found (start='{start_marker}', end='{end_marker}'); "
            "parsing WHOLE file"
        )
    if start_idx is None:
        return lines, False, (
            f"start marker '{start_marker}' not found; parsing WHOLE file"
        )
    if end_idx is None:
        # START found but no END: take everything after START.
        return lines[start_idx + 1:], True, (
            f"end marker '{end_marker}' not found; parsing from START to EOF"
        )
    return lines[start_idx + 1:end_idx], True, None


def parse_fps_lines(window_lines):
    """Return (fps[], avg_ms[], worst_ms[], frames[])."""
    fps, avg_ms, worst_ms, frames = [], [], [], []
    for ln in window_lines:
        m = RE_FPS.search(ln)
        if m:
            fps.append(float(m.group(1)))
            avg_ms.append(float(m.group(2)))
            worst_ms.append(float(m.group(3)))
            frames.append(int(m.group(4)))
    return fps, avg_ms, worst_ms, frames


def parse_pass_ms(window_lines):
    """Return (per_pass_mean: dict, pass_order: list).

    Aggregates the mean of each pass column over all pass-ms lines in window.
    """
    cols = {}  # name -> list of floats
    order = []  # discovery order for any non-canonical extras
    for ln in window_lines:
        if not RE_PASSMS_LINE.search(ln):
            continue
        # only consider the substring after "pass-ms avg:" to avoid picking up
        # other key=value noise earlier in the line.
        tail = re.split(r"pass-ms\s+avg\s*:", ln, maxsplit=1, flags=re.IGNORECASE)[-1]
        for name, val in RE_KV.findall(tail):
            try:
                fval = float(val)
            except ValueError:
                continue
            if name not in cols:
                cols[name] = []
                order.append(name)
            cols[name].append(fval)
    means = {k: (sum(v) / len(v) if v else 0.0) for k, v in cols.items()}
    # stable ordering: canonical first (if present), then any extras
    ordered = [p for p in PASS_ORDER if p in means]
    ordered += [p for p in order if p not in PASS_ORDER]
    return means, ordered


def scan_errors(all_lines):
    """Return (count, sample_lines[]) of error/fatal matches over whole log."""
    matches = []
    for ln in all_lines:
        for pat in ERROR_PATTERNS:
            if pat.search(ln):
                matches.append(ln.rstrip())
                break
    return len(matches), matches[:8]


def compute_stats(fps, avg_ms, worst_ms):
    n = len(fps)
    if n == 0:
        return None
    sv = sorted(fps)
    k = max(1, math.ceil(n * 0.01))
    low1 = sum(sv[:k]) / k
    return {
        "samples": n,
        "duration_s_approx": n,  # ~1 sample/sec
        "avg_fps": sum(fps) / n,
        "min_fps": sv[0],
        "max_fps": sv[-1],
        "low_1pct_fps": low1,
        "low_1pct_n": k,
        "worst_frame_ms": max(worst_ms) if worst_ms else 0.0,
        "mean_avg_ms": sum(avg_ms) / n if avg_ms else 0.0,
    }


def build_report(args):
    try:
        with open(args.engine_log, encoding="utf-8", errors="replace") as f:
            all_lines = f.read().splitlines()
    except OSError as e:
        return None, [f"cannot read '{args.engine_log}': {e}"]

    warnings = []
    window, used_markers, win_warn = slice_window(
        all_lines, args.start_marker, args.end_marker
    )
    if win_warn:
        warnings.append(win_warn)

    fps, avg_ms, worst_ms, frames = parse_fps_lines(window)
    pass_means, pass_order = parse_pass_ms(window)
    err_count, err_samples = scan_errors(all_lines)
    stats = compute_stats(fps, avg_ms, worst_ms)

    report = {
        "engine_log": args.engine_log,
        "used_markers": used_markers,
        "start_marker": args.start_marker,
        "end_marker": args.end_marker,
        "thresholds": {
            "fps_cap": args.fps_cap,
            "min_avg": args.min_avg,
            "min_1low": args.min_1low,
            "min_samples": 55,
        },
        "stats": stats,
        "pass_ms": {"order": pass_order, "means": pass_means},
        "errors": {"count": err_count, "samples": err_samples},
        "warnings": warnings,
    }
    return report, []


def evaluate_gate(report):
    """Return (passed: bool, reasons: list[str], soft_warnings: list[str])."""
    reasons = []          # explicit fail reasons
    soft = []             # warn-only
    th = report["thresholds"]
    stats = report["stats"]
    errs = report["errors"]["count"]

    if stats is None:
        reasons.append("NO FPS SAMPLES parsed from window")
        return False, reasons, soft

    n = stats["samples"]
    avg = stats["avg_fps"]
    low1 = stats["low_1pct_fps"]

    if avg < th["min_avg"]:
        reasons.append(f"avg fps {avg:.1f} < min-avg {th['min_avg']}")
    if low1 < th["min_1low"]:
        reasons.append(f"1% low {low1:.1f} < min-1low {th['min_1low']}")
    if errs > 0:
        reasons.append(f"{errs} GL/fatal error line(s) in engine.log")
    if n < th["min_samples"]:
        reasons.append(f"only {n} samples < required {th['min_samples']} (<~60s window)")

    # soft warnings
    if avg > th["fps_cap"] + 5:
        soft.append(
            f"avg fps {avg:.1f} exceeds fps-cap+5 ({th['fps_cap']}+5) — "
            "fps cap may not be applied"
        )
    if n < th["min_samples"]:
        soft.append(f"window only {n} samples (<55)")

    return (len(reasons) == 0), reasons, soft


def print_report(report, passed, reasons, soft):
    stats = report["stats"]
    print("=" * 64)
    print("CSOZ FPS / PERF REPORT")
    print("=" * 64)
    print(f"  engine.log : {report['engine_log']}")
    print(f"  markers    : start='{report['start_marker']}' "
          f"end='{report['end_marker']}' used={report['used_markers']}")
    for w in report["warnings"]:
        print(f"  WARN       : {w}")
    print("-" * 64)
    if stats:
        print(f"  samples         : {stats['samples']}  "
              f"(~{stats['duration_s_approx']}s window)")
        print(f"  avg fps         : {stats['avg_fps']:.2f}")
        print(f"  min fps         : {stats['min_fps']:.1f}")
        print(f"  max fps         : {stats['max_fps']:.1f}")
        print(f"  1% low fps      : {stats['low_1pct_fps']:.2f} "
              f"(mean of worst {stats['low_1pct_n']})")
        print(f"  worst frame ms  : {stats['worst_frame_ms']:.2f}")
        print(f"  mean avg_ms     : {stats['mean_avg_ms']:.2f}")
    else:
        print("  NO FPS SAMPLES")
    print("-" * 64)

    pm = report["pass_ms"]
    if pm["order"]:
        print("  per-pass ms (mean over window, sorted by cost):")
        rows = sorted(
            ((name, pm["means"][name]) for name in pm["order"]),
            key=lambda x: x[1], reverse=True,
        )
        for name, val in rows:
            bar = "#" * min(40, int(val * 8))
            print(f"    {name:<10} {val:7.3f} ms  {bar}")
    else:
        print("  per-pass ms : (no pass-ms lines found)")
    print("-" * 64)

    ec = report["errors"]["count"]
    print(f"  GL/fatal errors : {ec}")
    for s in report["errors"]["samples"]:
        print(f"      | {s}")
    print("-" * 64)

    for w in soft:
        print(f"  WARN : {w}")
    if passed:
        print("  GATE : PASS")
    else:
        print("  GATE : FAIL")
        for r in reasons:
            print(f"      - {r}")
    print("=" * 64)


def main(argv=None):
    p = argparse.ArgumentParser(
        description="CSOZ renderer FPS/perf log parser + PASS/FAIL gate.",
    )
    p.add_argument("engine_log", help="path to engine.log")
    p.add_argument("--start-marker", default="MATPERF-FPS-START")
    p.add_argument("--end-marker", default="MATPERF-FPS-END")
    p.add_argument("--fps-cap", type=float, default=200.0)
    p.add_argument("--min-avg", type=float, default=199.0)
    p.add_argument("--min-1low", type=float, default=195.0)
    p.add_argument("--json", default=None, help="write structured report JSON here")
    args = p.parse_args(argv)

    report, fatal = build_report(args)
    if fatal:
        for f in fatal:
            print(f"ERROR: {f}", file=sys.stderr)
        return 1

    passed, reasons, soft = evaluate_gate(report)
    report["gate"] = {"passed": passed, "fail_reasons": reasons, "soft_warnings": soft}

    print_report(report, passed, reasons, soft)

    if args.json:
        try:
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump(report, f, indent=2)
            print(f"  (wrote JSON report -> {args.json})")
        except OSError as e:
            print(f"WARN: could not write JSON '{args.json}': {e}", file=sys.stderr)

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
