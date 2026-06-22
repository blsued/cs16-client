#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bake_panorama.py -- CSOZ MW rework: bake the sampled all-sky panorama backdrop.

RE-GRADE PASS (briefRegrade, 2026-06-21): same architecture (starless band + dim
starfield, brightest stars removed for the live BSC5 layer), but the bake is now
PER-LAYER to fix the dual-gate gaps:

  G1 STAR DENSITY: the anti-fog black-point is applied to the BAND layer ONLY (the
     diffuse galactic glow, so faint band edges do not read as fog). The discrete
     dim-star layer is preserved at FULL density across the whole sky (stars are
     points, the gaps between them are naturally black, so no fog results) and is
     faint-lifted to enrich the fine carpet in the dark navy regions (codex: "add
     more fine, small, low-contrast stars across the whole sky, esp. darker navy").
     The star layer is downsampled to 4K with a 2x2 block-MAX (preserves point-source
     peaks) instead of an area box filter (which would average single-pixel stars
     toward black = the 8-bit/downsample density loss the brief warns about).
  G3 BAND PALETTE: a cool white-balance + stronger blue-white arms + deeper Great
     Rift dust, with a RESTRAINED magenta/pink H-alpha core. NOT warmed / NOT
     saturation-cranked (both gates confirmed warming drives the rejected garish
     "fire" look).

The band + star layers are graded/processed SEPARATELY, then composited additively,
then tonemapped, then mipped + display-encoded + packed (same CSZP container).

Inputs (raw EXR, 32-bit float LINEAR HDR, 8192x4096 galactic-equirect):
    panorama_src/milkyway_2020_8k_gal.exr   -- STARLESS diffuse Milky Way band
    panorama_src/starmap_2020_8k_gal.exr    -- dense point stars + band

DETERMINISTIC: no RNG, no time -> byte-identical mw_panorama.bin. RGB8 only.
G2 (navy sky) is ATMOSPHERE, not baked here. No sky colour is baked in (anti-fog).
"""

import os
import struct
import hashlib
import json
import sys

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import numpy as np
import cv2

HERE       = os.path.dirname(os.path.abspath(__file__))
SRC_DIR    = os.path.join(HERE, "panorama_src")
MW_EXR     = os.path.join(SRC_DIR, "milkyway_2020_8k_gal.exr")   # STARLESS band
STAR_EXR   = os.path.join(SRC_DIR, "starmap_2020_8k_gal.exr")    # stars + band
OUT_DIR    = os.path.join(HERE, "out")
OUT_BIN    = os.path.join(OUT_DIR, "mw_panorama.bin")
OUT_SHA    = os.path.join(OUT_DIR, "mw_panorama.sha256")
OUT_STATS  = os.path.join(HERE, "bake_panorama_stats.json")

MAGIC      = b"CSZP"
TARGET_W   = 4096
TARGET_H   = 2048

# --- grade constants (the shipped DEFAULT look; re-bake to make variants) -----------
N_REMOVE         = 500    # brightest point sources removed from the texture (live layer owns them)
PEAK_FLOOR       = 0.02
BLACK_PCT        = 50.0   # ANTI-FOG black point -- BAND LAYER ONLY now (G1). Diffuse floor -> true black.
EXPOSURE_PCT     = 99.5
EXPOSURE_TARGET  = 0.85
REINHARD_WHITE   = 3.0
SAT_GLOBAL       = 1.26   # band saturation (G3: NOT cranked -- avoid garish/orange)
CORE_PINK        = 0.40   # restrained magenta/pink H-alpha core (G3)
CORE_SIGMA_DEG   = 28.0
ARM_BLUE         = 0.34   # G3: stronger cool blue-white arms (was 0.16) -- de-warm the band
DUST_GAMMA       = 1.30   # G3: deeper Great Rift dust lanes (was 1.15)
ENCODE_GAMMA     = 2.2
BAND_WB          = (0.86, 0.97, 1.18)   # G3 cool white-balance: kill the warm tan, push blue-white
STAR_FAINT_GAMMA = 0.82   # G1 <1 lifts faint stars (denser low-contrast carpet in dark regions)
STAR_FAINT_CAP   = 3.0    # cap the faint-lift amplification (avoid noise blow-up)
STAR_GAIN        = 1.0    # G1 overall star-layer scalar (density lever for re-bake)
POLE_PHI0_DEG    = 5.0    # POLE-PINCH FIX: azimuthally band-limit rows within this polar angle of
                          # each galactic pole (see smooth_poles). 0.0 disables the bake-side step.

PINK_DIR   = np.array([1.00, 0.40, 0.78], dtype=np.float64)   # magenta-pink H-alpha
BLUE_DIR   = np.array([0.72, 0.80, 1.00], dtype=np.float64)   # cool blue-white arm
BAND_WB_A  = np.array(BAND_WB, dtype=np.float64)
LUMA       = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)


def log(msg):
    print("[bake_panorama] " + msg, flush=True)


def read_exr_rgb(path):
    if not os.path.isfile(path):
        raise SystemExit("FATAL: missing EXR source: %s" % path)
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED | cv2.IMREAD_ANYDEPTH | cv2.IMREAD_ANYCOLOR)
    if img is None:
        raise SystemExit("FATAL: OpenCV could not decode EXR (OPENCV_IO_ENABLE_OPENEXR=1?): %s" % path)
    if img.ndim != 3 or img.shape[2] < 3:
        raise SystemExit("FATAL: EXR is not 3-channel: %s shape=%s" % (path, getattr(img, "shape", None)))
    rgb = img[:, :, :3][:, :, ::-1].astype(np.float64)   # BGR -> RGB, float64 for determinism
    return np.maximum(rgb, 0.0)


def luminance(rgb):
    return rgb @ LUMA


def remove_bright_peaks(residual, n_remove, floor):
    """Detect the brightest point sources in the star residual and zero them (plus a
    1px halo) so the LIVE BSC5 layer owns them -- no double-count. Deterministic."""
    lum = luminance(residual).astype(np.float32)
    dil = cv2.dilate(lum, np.ones((3, 3), np.uint8))
    is_peak = (lum >= dil) & (lum > floor)
    ys, xs = np.nonzero(is_peak)
    vals = lum[ys, xs]
    npk = int(vals.shape[0])
    if npk == 0:
        return residual, 0, 0.0
    k = min(n_remove, npk)
    order = np.argsort(vals)[::-1]
    sel = order[:k]
    thresh = float(vals[sel[-1]])
    mask = np.zeros(lum.shape, dtype=np.uint8)
    mask[ys[sel], xs[sel]] = 1
    mask = cv2.dilate(mask, np.ones((3, 3), np.uint8))
    out = residual.copy()
    out[mask.astype(bool)] = 0.0
    return out, k, thresh


def core_weight(h, w):
    """Smooth galactic-core proximity weight in galactic-equirect coords."""
    u = (np.arange(w, dtype=np.float64) + 0.5) / w
    v = (np.arange(h, dtype=np.float64) + 0.5) / h
    l_deg = (u - 0.5) * 360.0
    b_deg = (0.5 - v) * 180.0
    L, B = np.meshgrid(l_deg, b_deg)
    cosd = np.cos(np.radians(B)) * np.cos(np.radians(L))
    ang = np.degrees(np.arccos(np.clip(cosd, -1.0, 1.0)))
    return np.exp(-0.5 * (ang / CORE_SIGMA_DEG) ** 2)


def grade_band(band):
    """G3 palette grade on the BAND layer only: cool white-balance, global saturation,
    restrained magenta/pink core + stronger cool blue-white arms, deepened dust lanes."""
    band = band * BAND_WB_A                              # G3 cool white-balance (kill warm tan)
    h, w, _ = band.shape
    lum = luminance(band)[:, :, None]
    out = np.maximum(lum + (band - lum) * SAT_GLOBAL, 0.0)

    bl = luminance(out)
    bmax = float(np.percentile(bl, 99.9))
    if bmax <= 1e-6:
        bmax = 1.0
    bandw = np.clip(bl / bmax, 0.0, 1.0)[:, :, None]

    cw = core_weight(h, w)[:, :, None]
    core_t = CORE_PINK * cw * bandw                      # restrained magenta/pink H-alpha core
    out = out * (1.0 - core_t) + (luminance(out)[:, :, None] * PINK_DIR) * core_t
    arm_t = ARM_BLUE * (1.0 - cw) * bandw                # cool blue-white arms
    out = out * (1.0 - arm_t) + (luminance(out)[:, :, None] * BLUE_DIR) * arm_t

    if DUST_GAMMA != 1.0:
        n = np.clip(luminance(out) / bmax, 0.0, 1.0)
        boost = np.power(n, DUST_GAMMA) / np.maximum(n, 1e-6)
        out = out * boost[:, :, None]
    return np.maximum(out, 0.0)


def process_stars(stars):
    """G1 star layer: NO black point, NO dust crush. Faint-lift (gamma<1) so the fine
    low-contrast stars in the dark navy regions read; gaps (zero) stay black."""
    sl = luminance(stars)
    nz = sl[sl > 1e-6]
    ref = float(np.percentile(nz, 99.5)) if nz.size else 1.0
    if ref <= 1e-6:
        ref = 1.0
    n = np.clip(sl / ref, 0.0, 1.0)
    num = np.power(n, STAR_FAINT_GAMMA)
    factor = np.where(n > 1e-6, num / np.maximum(n, 1e-6), 0.0)
    factor = np.minimum(factor, STAR_FAINT_CAP)
    out = stars * factor[:, :, None] * STAR_GAIN
    return np.maximum(out, 0.0)


def downsample_max2(img):
    """Half-resolution by 2x2 block MAX (preserves point-source peaks = the dense
    faint-star carpet survives the downsample). Deterministic."""
    h, w, c = img.shape
    h2, w2 = h - (h % 2), w - (w % 2)
    img = img[:h2, :w2]
    out = img.reshape(h2 // 2, 2, w2 // 2, 2, c).max(axis=(1, 3))
    if out.shape[0] != TARGET_H or out.shape[1] != TARGET_W:
        out = cv2.resize(out.astype(np.float32), (TARGET_W, TARGET_H),
                         interpolation=cv2.INTER_NEAREST).astype(np.float64)
    return np.maximum(out, 0.0)


def circ_box(row, k):
    """Circular (wrap-in-u) uniform box average of width k over one equirect ROW (W,C).
    O(W) via cumulative sums; deterministic. k clamped to W (k>=W -> the row's circular mean,
    the physical limit at the pole). Centered window (offset -k//2)."""
    W, C = row.shape
    if k >= W:
        return np.repeat(row.mean(axis=0, keepdims=True), W, axis=0)
    half = k // 2
    ext  = np.concatenate([row, row[:k]], axis=0)                       # wrap pad (W+k, C)
    csum = np.concatenate([np.zeros((1, C), dtype=row.dtype), np.cumsum(ext, axis=0)], axis=0)
    starts = (np.arange(W) - half) % W                                 # window start per output col
    return (csum[starts + k] - csum[starts]) / float(k)


def smooth_poles(base, phi0_deg=POLE_PHI0_DEG):
    """POLE-PINCH FIX (bake side, deterministic). The equirect lat-long map is singular at the
    galactic poles: every longitude column of the top/bottom rows converges to one sky point, so
    sharp per-texel content there smears into radial 'starburst' streaks when sampled. The
    runtime anisotropic-filter change is the primary cure; this complements it by azimuthally
    band-limiting ONLY the near-pole rows so no high-frequency content survives at the exact
    singularity AF cannot fully average. For each row at polar angle phi (deg, to the nearest
    pole) with phi < phi0, circularly low-pass it in u with box width k = round(sin(phi0)/sin(phi))
    (applied only where k >= 2). The 1/sin(phi) growth matches the true azimuthal resolution
    collapse. Reach: k>=2 from phi <= ~phi0/2 (the top/bottom few degrees). Mid-latitude rows and
    the band at v=0.5 are bit-exact untouched. Circular wrap => no longitude-seam discontinuity."""
    if phi0_deg <= 0.0:
        return base
    H, W, C = base.shape
    out = base.copy()
    sin_phi0 = float(np.sin(np.radians(phi0_deg)))
    touched, max_k = 0, 0
    for r in range(H):
        v = (r + 0.5) / H
        phi_deg = min(v, 1.0 - v) * 180.0
        if phi_deg >= phi0_deg:
            continue
        sphi = float(np.sin(np.radians(phi_deg)))
        k = W if sphi <= 1e-9 else int(round(sin_phi0 / sphi))
        if k < 2:
            continue
        if k > W:
            k = W
        out[r] = circ_box(base[r], k)
        touched += 1
        max_k = max(max_k, k)
    reach_deg = phi0_deg if touched else 0.0
    log("  POLE band-limit: phi0=%.1f deg, rows smoothed=%d/pole-region, max box k=%d" %
        (phi0_deg, touched, max_k))
    smooth_poles.last = {"phi0_deg": phi0_deg, "rows_touched": int(touched), "max_box_k": int(max_k)}
    return np.maximum(out, 0.0)


def tonemap(rgb):
    """Exposure (deterministic percentile) + extended Reinhard shoulder -> [0,1] LINEAR."""
    lum = luminance(rgb)
    pivot = float(np.percentile(lum, EXPOSURE_PCT))
    if pivot <= 1e-6:
        pivot = 1.0
    exposure = EXPOSURE_TARGET / pivot
    x = rgb * exposure
    wp = REINHARD_WHITE
    x = x * (1.0 + x / (wp * wp)) / (1.0 + x)
    return np.clip(x, 0.0, 1.0), float(exposure)


def encode_u8(level_linear):
    enc = np.power(np.clip(level_linear, 0.0, 1.0), 1.0 / ENCODE_GAMMA)
    return np.clip(np.rint(enc * 255.0), 0, 255).astype(np.uint8)


def mip_dims(w, h):
    dims = [(w, h)]
    lw, lh = w, h
    while lw > 1 or lh > 1:
        lw = max(1, lw // 2)
        lh = max(1, lh // 2)
        dims.append((lw, lh))
    return dims


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    log("decoding EXR sources ...")
    mw   = read_exr_rgb(MW_EXR)
    star = read_exr_rgb(STAR_EXR)
    if mw.shape != star.shape:
        raise SystemExit("FATAL: milkyway/starmap dims differ: %s vs %s" % (mw.shape, star.shape))
    log("  milkyway %s  starmap %s" % (mw.shape, star.shape))

    residual = np.maximum(star - mw, 0.0)
    dim_stars, removed, peak_thresh = remove_bright_peaks(residual, N_REMOVE, PEAK_FLOOR)
    log("  removed %d brightest point sources (peak-lum threshold %.4f)" % (removed, peak_thresh))

    # --- G1 BAND layer: anti-fog black point HERE ONLY, then G3 palette grade ---
    band_black = float(np.percentile(luminance(mw), BLACK_PCT))
    mw_antifog = np.maximum(mw - band_black, 0.0)
    log("  BAND anti-fog black point (pct %.0f) = %.5f linear (band layer only)" % (BLACK_PCT, band_black))
    band_graded = grade_band(mw_antifog)

    # --- G1 STAR layer: full density, NO black point, faint-lift carpet ---
    star_layer = process_stars(dim_stars)

    # downsample SEPARATELY: band area-filtered, stars block-MAX (preserve peaks)
    band_4k = cv2.resize(band_graded.astype(np.float32), (TARGET_W, TARGET_H),
                         interpolation=cv2.INTER_AREA).astype(np.float64)
    band_4k = np.maximum(band_4k, 0.0)
    star_4k = downsample_max2(star_layer)
    composite = np.maximum(band_4k + star_4k, 0.0)       # additive composite at 4K

    tm, exposure = tonemap(composite)
    log("  tonemap exposure %.5f (pct %.1f -> %.2f), reinhard white %.1f" %
        (exposure, EXPOSURE_PCT, EXPOSURE_TARGET, REINHARD_WHITE))
    base = np.clip(tm.astype(np.float64), 0.0, 1.0)      # already TARGET_W x TARGET_H
    base = smooth_poles(base)                            # POLE-PINCH FIX (near-pole rows only)
    base = np.clip(base.astype(np.float64), 0.0, 1.0)

    dims = mip_dims(TARGET_W, TARGET_H)
    nLev = len(dims)
    log("  mip levels = %d : %s ... %s" % (nLev, dims[0], dims[-1]))
    payload = bytearray()
    payload += MAGIC
    payload += struct.pack("<iiii", TARGET_W, TARGET_H, 3, nLev)
    base_mean = []
    base_u8 = None
    for (lw, lh) in dims:
        if (lw, lh) == (TARGET_W, TARGET_H):
            lvl = base
        else:
            lvl = cv2.resize(base.astype(np.float32), (lw, lh), interpolation=cv2.INTER_AREA)
            lvl = np.clip(lvl.astype(np.float64), 0.0, 1.0)
        u8 = encode_u8(lvl)
        b = u8.tobytes()
        assert len(b) == lw * lh * 3, "level byte size mismatch %dx%d" % (lw, lh)
        payload += b
        if (lw, lh) == (TARGET_W, TARGET_H):
            base_mean = [float(u8[:, :, c].mean()) for c in range(3)]
            base_u8 = u8

    with open(OUT_BIN, "wb") as f:
        f.write(payload)
    digest = hashlib.sha256(payload).hexdigest()
    with open(OUT_SHA, "w") as f:
        f.write("%s  mw_panorama.bin\n" % digest)

    # density metrics (G1 evidence): per-pixel luma in 8-bit, whole base level
    lum8 = (base_u8.astype(np.float64) @ LUMA)
    frac_pure_black   = float(np.mean(lum8 < 2.0))
    frac_faint_carpet = float(np.mean((lum8 >= 2.0) & (lum8 < 40.0)))
    frac_lit          = float(np.mean(lum8 >= 2.0))

    stats = {
        "magic": "CSZP", "width": TARGET_W, "height": TARGET_H, "channels": 3,
        "nLevels": nLev, "bytes": len(payload), "sha256": digest,
        "encode_gamma": ENCODE_GAMMA,
        "regrade": "per-layer bake: band anti-fog+G3 palette; star full-density faint carpet (block-max downsample)",
        "pole_pinch_fix": getattr(smooth_poles, "last", {"phi0_deg": POLE_PHI0_DEG, "rows_touched": 0, "max_box_k": 0}),
        "bright_stars_removed": removed, "bright_peak_lum_threshold": peak_thresh,
        "band_anti_fog_black_point": band_black, "exposure": exposure,
        "base_level_mean_rgb8": base_mean,
        "density": {
            "frac_pure_black_lt2": frac_pure_black,
            "frac_faint_carpet_2to40": frac_faint_carpet,
            "frac_lit_ge2": frac_lit,
            "note": "prior single-layer bake off-band frac<2/255 was ~0.580; lower frac_pure_black = denser carpet",
        },
        "grade": {
            "N_REMOVE": N_REMOVE, "SAT_GLOBAL": SAT_GLOBAL, "CORE_PINK": CORE_PINK,
            "CORE_SIGMA_DEG": CORE_SIGMA_DEG, "ARM_BLUE": ARM_BLUE, "DUST_GAMMA": DUST_GAMMA,
            "BAND_WB": list(BAND_WB), "STAR_FAINT_GAMMA": STAR_FAINT_GAMMA,
            "STAR_FAINT_CAP": STAR_FAINT_CAP, "STAR_GAIN": STAR_GAIN,
            "EXPOSURE_PCT": EXPOSURE_PCT, "EXPOSURE_TARGET": EXPOSURE_TARGET, "REINHARD_WHITE": REINHARD_WHITE,
        },
        "determinism": "no RNG/time; same input EXR bytes -> byte-identical CSZP",
    }
    with open(OUT_STATS, "w") as f:
        json.dump(stats, f, indent=2)

    log("WROTE %s (%d bytes, %d mips)" % (OUT_BIN, len(payload), nLev))
    log("  sha256 %s" % digest)
    log("  base level mean RGB8 = %s" % base_mean)
    log("  density frac<2=%.4f  carpet[2,40)=%.4f  lit>=2=%.4f" %
        (frac_pure_black, frac_faint_carpet, frac_lit))
    return 0


if __name__ == "__main__":
    sys.exit(main())
