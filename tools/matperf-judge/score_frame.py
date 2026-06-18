#!/usr/bin/env python3
"""
score_frame.py -- automated, reproducible screenshot scorer for a CS1.6-based
zombie-survival game with a self-built renderer.

PURPOSE
    Score rendering iterations per night/weather scene with an AUTOMATED,
    REPRODUCIBLE 0-10 number + an explicit failure list per screenshot.
    This is a *regression proxy*: it flags when a render iteration got worse
    on measurable axes (too dark, lost moonlight, blown highlights, etc.).
    The subjective "does it look good / immersive" final review still belongs
    to the user.

    UI / HUD is NOT scored: a configurable top band and bottom band are
    cropped off, and every metric runs only on the remaining center "WORLD"
    region.

USAGE
    python score_frame.py <image.png> [<image2.png> ...]
        [--scene night|rain|snow|day|auto]
        [--crop-top FRAC] [--crop-bottom FRAC]
        [--json OUT.json] [--quiet]

    Defaults: --scene auto, --crop-top 0.12, --crop-bottom 0.18.
    Exit code is always 0 -- this is a scorer, not a CI gate. Failures are
    reported in the data, not via the exit code.

METRIC ROBUSTNESS (honest assessment)
    Strong / calibrated against real reference frames:
        M1 night_readability, M2 cool_moon_consistency, M4 no_overexposure.
    Informational only (NOT gated, weight 0):
        M3 fog_depth -- no reference frame contains fog, so no threshold is
        calibrated; it only reports the far/near luma+contrast gradient.
    Weak / proxy, validated only against EXTERNAL reference photos:
        M5 rain_wet_ground, M6 snow_coverage -- rain/snow weather is not yet
        implemented in-engine, so these cannot be validated on real engine
        frames yet.
"""

import argparse
import json
import sys

import numpy as np
from PIL import Image

import metrics as M

# Be robust to non-UTF-8 consoles (e.g. Windows cp1252): image paths and the
# em-dash in the disclaimer must not crash printing.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

DISCLAIMER = ("AUTOMATED PROXY -- subjective visual final review belongs to the "
              "user; this score flags regressions, it is not ground truth.")

# Which metrics gate / weight per scene. (name, fn, weight) -- weight 0 = display
# only, excluded from the overall gate & weighted average.
SCENE_METRICS = {
    "night": [
        ("M1_night_readability", M.m1_night_readability, 1.0),
        ("M2_cool_moon_consistency", M.m2_cool_moon_consistency, 1.0),
        ("M3_fog_depth", M.m3_fog_depth, 0.0),
        ("M4_no_overexposure", M.m4_no_overexposure, 1.0),
    ],
    "rain": [
        ("M4_no_overexposure", M.m4_no_overexposure, 1.0),
        ("M5_rain_wet_ground", M.m5_rain_wet_ground, 1.0),
    ],
    "snow": [
        ("M4_no_overexposure", M.m4_no_overexposure, 1.0),
        ("M6_snow_coverage", M.m6_snow_coverage, 1.0),
    ],
    "day": [
        ("M1_brightness", M.m_day_brightness, 1.0),
        ("M4_no_overexposure", M.m4_no_overexposure, 1.0),
    ],
}


def load_world(path, crop_top, crop_bottom):
    """Load an image and return (world_rgb_float_array, (W,H), crop_dict)."""
    im = Image.open(path).convert("RGB")
    a = np.asarray(im, dtype=np.float64)
    h = a.shape[0]
    t = int(round(h * crop_top))
    b = int(round(h * crop_bottom))
    t = min(max(t, 0), h - 1)
    b = min(max(b, 0), h - 1 - t)
    world = a[t:h - b, :, :]
    crop = {"top_frac": crop_top, "bottom_frac": crop_bottom,
            "top_px": t, "bottom_px": b,
            "world_w": int(world.shape[1]), "world_h": int(world.shape[0])}
    return world, im.size, crop


def score_image(path, scene_arg, crop_top, crop_bottom):
    world, size, crop = load_world(path, crop_top, crop_bottom)

    if scene_arg == "auto":
        scene = M.detect_scene(world)
        scene_src = "auto-detected"
    else:
        scene = scene_arg
        scene_src = "user-specified"

    metric_defs = SCENE_METRICS[scene]
    metric_results = []
    failures = []
    weighted_sum = 0.0
    weight_total = 0.0

    for name, fn, weight in metric_defs:
        r = fn(world)
        entry = {
            "name": name,
            "value": r["value"],
            "score": round(r["score"], 1),
            "pass": bool(r["pass"]),
            "threshold": r["threshold"],
            "note": r["note"],
            "weight": weight,
        }
        # carry useful diagnostics through to JSON without polluting the table
        for k in ("crushed_black", "p50", "saturation", "p95", "mean"):
            if k in r:
                entry[k] = r[k]
        metric_results.append(entry)

        if weight > 0:
            weighted_sum += r["score"] * weight
            weight_total += weight
            if not r["pass"]:
                failures.append(name)

    overall = (weighted_sum / weight_total) if weight_total > 0 else 0.0
    # a hard fail on any gating metric cannot read as "good"
    if failures:
        overall = min(overall, 5.9)
    overall = round(overall, 1)

    return {
        "image": path,
        "size": {"w": size[0], "h": size[1]},
        "scene": scene,
        "scene_source": scene_src,
        "crop": crop,
        "overall_score": overall,
        "metrics": metric_results,
        "failures": failures,
        "disclaimer": DISCLAIMER,
    }


def fmt_value(v):
    if isinstance(v, dict):
        return ", ".join(f"{k}={val}" for k, val in v.items())
    return str(v)


def print_report(rep, quiet=False):
    print(f"\n=== {rep['image']}")
    print(f"    image {rep['size']['w']}x{rep['size']['h']}  "
          f"world {rep['crop']['world_w']}x{rep['crop']['world_h']}  "
          f"(cropped top {rep['crop']['top_px']}px / bottom {rep['crop']['bottom_px']}px)")
    print(f"    scene: {rep['scene']} ({rep['scene_source']})")
    applicable = [m["name"] + ("(info,w0)" if m["weight"] == 0 else "")
                  for m in rep["metrics"]]
    print(f"    applicable metrics: {', '.join(applicable)}")
    print()

    # table
    hdr = f"    {'metric':<26} {'value':<34} {'score':>6} {'result':>7}  note"
    print(hdr)
    print("    " + "-" * (len(hdr) - 4))
    for m in rep["metrics"]:
        val = fmt_value(m["value"])
        if len(val) > 33:
            val = val[:30] + "..."
        result = "PASS" if m["pass"] else "FAIL"
        if m["weight"] == 0:
            result = "INFO"
        note = m["note"]
        if not quiet and len(note) > 60:
            note = note[:57] + "..."
        elif quiet:
            note = ""
        print(f"    {m['name']:<26} {val:<34} {m['score']:>6.1f} {result:>7}  {note}")

    print()
    print(f"    OVERALL: {rep['overall_score']:.1f}/10")
    if rep["failures"]:
        print(f"    FAILURES: {', '.join(rep['failures'])}")
    else:
        print(f"    FAILURES: none")
    print(f"    {DISCLAIMER}")


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="score_frame.py",
        description="Automated 0-10 renderer-screenshot scorer (regression proxy). "
                    "Crops UI/HUD, scores only the WORLD region. " + DISCLAIMER,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("images", nargs="+", help="one or more screenshot PNGs")
    p.add_argument("--scene", choices=["night", "rain", "snow", "day", "auto"],
                   default="auto", help="scene type (default: auto-detect)")
    p.add_argument("--crop-top", type=float, default=0.12,
                   help="fraction of height removed from top (UI band, default 0.12)")
    p.add_argument("--crop-bottom", type=float, default=0.18,
                   help="fraction of height removed from bottom (HUD band, default 0.18)")
    p.add_argument("--json", metavar="OUT.json", default=None,
                   help="also write a structured JSON report")
    p.add_argument("--quiet", action="store_true",
                   help="terse table (suppress long note text)")
    args = p.parse_args(argv)

    reports = []
    for path in args.images:
        try:
            rep = score_image(path, args.scene, args.crop_top, args.crop_bottom)
        except FileNotFoundError:
            print(f"\n=== {path}\n    ERROR: file not found", file=sys.stderr)
            continue
        except Exception as e:  # robust: one bad image must not kill the batch
            print(f"\n=== {path}\n    ERROR: {type(e).__name__}: {e}", file=sys.stderr)
            continue
        reports.append(rep)
        print_report(rep, quiet=args.quiet)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(reports, f, indent=2, ensure_ascii=False)
        print(f"\n[wrote JSON report -> {args.json}]")

    return 0  # always 0 -- scorer, not a gate


if __name__ == "__main__":
    sys.exit(main())
