"""
metrics.py -- pure, importable, unit-testable metric functions for the
matperf screenshot judge.

All metrics operate on a "WORLD" crop (a numpy float64 RGB array, shape
[H, W, 3], values 0..255) that the caller has already produced by cropping
the UI/HUD bands off the top and bottom of the screenshot.

Luma is Rec.709: 0.2126*R + 0.7152*G + 0.0722*B, range 0..255.

Each metric returns a dict:
    {value, score(float 0..10), pass(bool), threshold(str), note(str)}
plus optional extra diagnostic keys.

NOTE on honesty: these are PROXIES for a renderer regression check. The
subjective "does it look good" judgement still belongs to a human. Some
metrics are well calibrated against real reference frames (night
readability, moon color temperature, overexposure); others (fog, rain,
snow) are weak / informational / validated only against external photos.
See module README in score_frame.py docstring and the per-metric notes.
"""

import numpy as np

# ---------------------------------------------------------------------------
# Low-level helpers
# ---------------------------------------------------------------------------

def luma709(arr):
    """Rec.709 luma (0..255) for an RGB float array shaped [...,3]."""
    return 0.2126 * arr[..., 0] + 0.7152 * arr[..., 1] + 0.0722 * arr[..., 2]


def hsv_saturation(arr):
    """HSV saturation scaled to 0..255 for an RGB float array [...,3]."""
    mx = arr.max(axis=-1)
    mn = arr.min(axis=-1)
    s = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0.0)
    return s * 255.0


def mean_br(arr):
    """Color-temperature proxy: mean(B) - mean(R)."""
    return float(arr[..., 2].mean() - arr[..., 0].mean())


def score_band(x, fail_lo, ideal_lo, ideal_hi, fail_hi):
    """Smooth piecewise-linear 0..10 scoring curve.

    Shape (left to right):
        x <= fail_lo                  -> 0
        fail_lo .. ideal_lo           -> ramp 0 -> 10
        ideal_lo .. ideal_hi          -> 10 (flat ideal band)
        ideal_hi .. fail_hi           -> ramp 10 -> 0
        x >= fail_hi                  -> 0

    Any boundary may be None to make that side open / unbounded:
        fail_lo None  -> no lower fail edge (10 from -inf up to ideal_hi)
        ideal_lo None -> no lower ramp (flat 10 starts at -inf or fail_lo edge)
        ideal_hi None -> flat 10 continues to +inf (no upper ramp/fail)
        fail_hi None   -> no upper fail edge (ramp from ideal_hi never reaches 0;
                          in practice pair ideal_hi=None with this for fully open top)

    Returns a float in [0, 10].
    """
    x = float(x)

    # ----- lower side -----
    if ideal_lo is not None:
        if x <= (fail_lo if fail_lo is not None else -np.inf):
            return 0.0
        if fail_lo is not None and x < ideal_lo:
            # ramp 0 -> 10 across [fail_lo, ideal_lo]
            return 10.0 * (x - fail_lo) / max(ideal_lo - fail_lo, 1e-9)
        if fail_lo is None and x < ideal_lo:
            # no fail edge but a defined ideal_lo: treat as flat 10 below ideal_lo
            return 10.0
    # if ideal_lo is None there is no lower ramp; fall through to flat/upper logic

    # ----- upper side -----
    if ideal_hi is not None and x > ideal_hi:
        if fail_hi is None:
            return 10.0  # open top, never penalised above ideal_hi
        if x >= fail_hi:
            return 0.0
        # ramp 10 -> 0 across [ideal_hi, fail_hi]
        return 10.0 * (fail_hi - x) / max(fail_hi - ideal_hi, 1e-9)

    # inside ideal band (or below an open lower side)
    return 10.0


def _clamp(v, lo, hi):
    return max(lo, min(hi, v))


def _region_stats(world):
    """Return commonly used scalar stats of a world/region crop."""
    luma = luma709(world)
    return {
        "mean": float(luma.mean()),
        "p50": float(np.percentile(luma, 50)),
        "p95": float(np.percentile(luma, 95)),
        "std": float(luma.std()),
        "crushed": float(np.mean(luma < 8)),
        "blown": float(np.mean(luma > 247)),
        "br": mean_br(world),
        "sat": float(hsv_saturation(world).mean()),
        "luma": luma,
    }


def vertical_thirds(world):
    """Split a world crop into top / mid / bottom vertical thirds (luma)."""
    luma = luma709(world)
    h = luma.shape[0]
    th = max(h // 3, 1)
    top = luma[:th]
    mid = luma[th:2 * th]
    bot = luma[2 * th:]
    return top, mid, bot


def ground_band(world, frac=0.40):
    """Bottom `frac` of the world crop -> the 'ground' region (RGB array)."""
    h = world.shape[0]
    start = int(round(h * (1.0 - frac)))
    start = min(max(start, 0), h - 1)
    return world[start:]


# ---------------------------------------------------------------------------
# M1 -- night readability
# ---------------------------------------------------------------------------

def m1_night_readability(world):
    s = _region_stats(world)
    mean, crushed, p50 = s["mean"], s["crushed"], s["p50"]

    score = score_band(mean, 8, 14, 45, 90)
    passed = True
    note = "readable night exposure"

    if mean < 12 or crushed > 0.35 or p50 < 10:
        passed = False
        note = "too dark to play"
    elif mean > 70:
        passed = False
        note = "washed/too bright for night"

    return {
        "value": round(mean, 1),
        "score": round(float(score), 4),
        "pass": passed,
        "threshold": "mean luma ideal 14-45; fail <12 dark / >70 washed",
        "note": note,
        "crushed_black": round(crushed, 4),
        "p50": round(p50, 1),
    }


# ---------------------------------------------------------------------------
# M2 -- cool moon color-temperature consistency  (the key discriminator)
# ---------------------------------------------------------------------------

def m2_cool_moon_consistency(world):
    s = _region_stats(world)
    br, sat = s["br"], s["sat"]

    base = score_band(br, 0, 13, 60, None)  # 0 at BR<=0, 10 at BR>=13, open top
    # saturation penalty: desaturated moonlight reads as "wrong color temp"
    if sat < 120:
        factor = _clamp(sat / 120.0, 0.5, 1.0)
        score = base * factor
    else:
        score = base

    passed = True
    note = "cool moonlit color temperature present"
    if br < 8:
        passed = False
        note = "lost moonlight / wrong (too neutral) color temperature"

    return {
        "value": round(br, 1),
        "score": round(float(score), 4),
        "pass": passed,
        "threshold": "BR=meanB-meanR ideal>=13; fail <8; sat<120 penalised",
        "note": note,
        "saturation": round(sat, 1),
    }


# ---------------------------------------------------------------------------
# M3 -- fog depth  (INFORMATIONAL, non-gating, weight 0)
# ---------------------------------------------------------------------------

def m3_fog_depth(world):
    top, mid, bot = vertical_thirds(world)
    top_luma, top_std = float(top.mean()), float(top.std())
    bot_luma, bot_std = float(bot.mean()), float(bot.std())

    far_atten = bot_luma - top_luma             # >0 means far (top) is darker
    contrast_ratio = top_std / max(bot_std, 1e-6)  # <1 means far is flatter

    # soft 0..10: reward far darker (far_atten>0) AND far flatter (ratio<1)
    atten_score = score_band(far_atten, 0, 12, 200, None)        # darker far -> higher
    flat_score = score_band(contrast_ratio, None, 0, 0.85, 1.6)  # flatter far -> higher
    score = 0.5 * atten_score + 0.5 * flat_score

    return {
        "value": {
            "top_luma": round(top_luma, 1), "top_std": round(top_std, 1),
            "bottom_luma": round(bot_luma, 1), "bottom_std": round(bot_std, 1),
            "far_atten": round(far_atten, 1),
            "contrast_ratio": round(contrast_ratio, 3),
        },
        "score": round(float(score), 4),
        "pass": True,  # never gates
        "threshold": "none calibrated (informational)",
        "note": ("INFORMATIONAL -- not gated; none of the calibration references "
                 "contain fog, so no fog threshold is calibrated. Reports far/near "
                 "luma+contrast gradient for the user to judge."),
        "weight": 0,
    }


# ---------------------------------------------------------------------------
# M4 -- no overexposure / blown highlights  (ALL scenes)
# ---------------------------------------------------------------------------

def m4_no_overexposure(world):
    s = _region_stats(world)
    blown, p95, mean = s["blown"], s["p95"], s["mean"]

    score = score_band(blown * 100.0, None, 0, 0.5, 4)  # 10 at 0%, 0 by ~4%
    passed = True
    note = "no blown highlights"
    if blown > 0.02 or p95 > 240 or mean > 150:
        passed = False
        note = "over-exposed / blown highlights"

    return {
        "value": round(blown, 4),
        "score": round(float(score), 4),
        "pass": passed,
        "threshold": "blown(luma>247) ideal 0%; fail >2% or p95>240 or mean>150",
        "note": note,
        "p95": round(p95, 1),
        "mean": round(mean, 1),
    }


# ---------------------------------------------------------------------------
# M5 -- rain wet ground  (plausibility proxy)
# ---------------------------------------------------------------------------

def m5_rain_wet_ground(world):
    """Wet ground here is a DARK, low-std, WARM reflective sheet -- NOT bright
    specular dots (that naive proxy is wrong for this renderer)."""
    g = ground_band(world, 0.40)
    gs = _region_stats(g)
    g_br, g_std, g_blown = gs["br"], gs["std"], gs["blown"]

    temp_score = score_band(-g_br, 3, 8, 40, None)   # want BR negative (warm cast)
    flat_score = score_band(g_std, 3, 8, 30, 55)     # low/flat sheen
    notblown_score = score_band(g_blown * 100.0, None, 0, 0.5, 4)

    score = (temp_score + flat_score + notblown_score) / 3.0

    temp_pass = (g_br < -5)
    flat_pass = (8 <= g_std <= 35)
    blown_pass = (g_blown < 0.02)
    passed = temp_pass and flat_pass and blown_pass

    return {
        "value": {
            "ground_br": round(g_br, 1),
            "ground_std": round(g_std, 1),
            "ground_blown": round(g_blown, 4),
        },
        "score": round(float(score), 4),
        "pass": passed,
        "threshold": "warm BR<-5, flat std 8-35, blown<2% (plausibility proxy)",
        "note": ("PLAUSIBILITY PROXY -- wet ground modelled as a dark, low-std, "
                 "warm reflective sheet (the naive bright-specular-dots proxy was "
                 "rejected). Validated only vs an external reference photo."),
    }


# ---------------------------------------------------------------------------
# M6 -- snow coverage  (plausibility proxy)
# ---------------------------------------------------------------------------

def m6_snow_coverage(world):
    """Reference snow is mid-luma BLUISH-GRAY, brighter in the near/ground band.
    The naive bright+low-sat proxy mostly counts white UI chrome -> rejected."""
    g = ground_band(world, 0.40)
    gs = _region_stats(g)
    g_mean, g_br, g_blown = gs["mean"], gs["br"], gs["blown"]

    top, mid, bot = vertical_thirds(world)
    top_luma, bot_luma = float(top.mean()), float(bot.mean())
    near_brighter = bot_luma > top_luma

    mean_score = score_band(g_mean, 60, 95, 200, 250)
    temp_score = score_band(g_br, 5, 20, 80, None)
    gradient_score = 10.0 if near_brighter else 0.0
    notblown_score = score_band(g_blown * 100.0, None, 0, 0.5, 4)

    score = (mean_score + temp_score + gradient_score + notblown_score) / 4.0

    passed = (g_mean > 90) and (g_br > 18) and (g_blown < 0.02)

    return {
        "value": {
            "ground_mean": round(g_mean, 1),
            "ground_br": round(g_br, 1),
            "top_luma": round(top_luma, 1),
            "bottom_luma": round(bot_luma, 1),
            "near_brighter": bool(near_brighter),
            "ground_blown": round(g_blown, 4),
        },
        "score": round(float(score), 4),
        "pass": passed,
        "threshold": "ground mean>90, cool BR>18, near>far, blown<2% (proxy)",
        "note": ("PLAUSIBILITY PROXY -- snow modelled as mid-luma bluish-gray, "
                 "brighter in the near/ground band. The old bright+low-sat proxy "
                 "was REJECTED (counts white UI chrome). Validated only vs an "
                 "external reference photo."),
    }


# ---------------------------------------------------------------------------
# day brightness (reuses M1 slot with a day-tuned band)
# ---------------------------------------------------------------------------

def m_day_brightness(world):
    s = _region_stats(world)
    mean = s["mean"]
    score = score_band(mean, 8, 40, 160, 230)
    passed = (8 < mean < 230)
    note = "daylight exposure in range"
    if mean <= 8:
        passed, note = False, "too dark for day"
    elif mean >= 230:
        passed, note = False, "blown / over-bright for day"
    return {
        "value": round(mean, 1),
        "score": round(float(score), 4),
        "pass": passed,
        "threshold": "mean luma ideal 40-160 (day)",
        "note": note,
    }


# ---------------------------------------------------------------------------
# Scene auto-detection (on the WORLD crop)
# ---------------------------------------------------------------------------

def detect_scene(world):
    s = _region_stats(world)
    mean, br = s["mean"], s["br"]
    if mean < 40 and br > 4:
        return "night"
    if mean > 85 and br > 18:
        return "snow"
    if 35 <= mean <= 110 and br < -4:
        return "rain"
    return "day"
