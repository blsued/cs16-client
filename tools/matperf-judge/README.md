# matperf-judge — CSOZ map-material realism + 200 FPS acceptance framework

Worker C deliverable. A **reusable, automated proxy** for scoring CSOZ rendering
iterations so the main window can tell whether three parallel render workers are
*actually* improving, instead of relying on verbal description.

> **Honesty contract.** Every score this framework emits is an **automated proxy
> that flags regressions — it is NOT ground truth.** The final subjective visual
> verdict (does it *look* right / feel atmospheric) always belongs to the user.
> The scorer prints this disclaimer on every run; do not strip it.

Lives in `tools/matperf-judge/` of the `worker/r1-material-perf-judge` branch.
Pure Python (PIL + numpy) + one PowerShell launcher. No engine changes. All
runtime artifacts (captures, logs, JSON) go to a run dir or `_scratch/`, never
into official source dirs.

---

## What it scores

| # | Metric | Scene(s) | Gates? | What it catches |
|---|--------|----------|--------|-----------------|
| M1 | `night_readability` | night | yes | "too dark to play" / "washed for night" (world mean luma, crushed-black %, p50) |
| M2 | `cool_moon_consistency` | night | yes | **lost moonlight / wrong (neutral) color temperature** — the single cleanest good-vs-bad night discriminator (B−R + saturation) |
| M3 | `fog_depth` | night | **no (informational, weight 0)** | far/near luma + contrast gradient. Reported only — *not* calibrated (no foggy reference exists yet) |
| M4 | `no_overexposure` | all | yes | blown highlights / over-bright (blown-white %, p95, mean) |
| M5 | `rain_wet_ground` | rain | yes | wet-ground plausibility = warm + flat low-contrast reflective sheet (proxy; see caveat) |
| M6 | `snow_coverage` | snow | yes | snow plausibility = mid-luma bluish-gray ground, near brighter than far (proxy; see caveat) |
| — | FPS gate | any | yes | avg fps / 1% low / per-pass ms / GL errors over a ≥60 s window |

That is **6 visual metrics + 1 perf gate** (requirement was ≥5).

### Metric trust levels (read this before quoting a score)

- **Strong / calibrated against real engine frames:** M1, M2, M4. These were
  tuned against two *actual* in-engine night captures (the M2-era goal-run
  reference + the current "problem" shot). **M2 is the workhorse** — it ranks the
  good night frame strictly above the regressed one (see calibration below).
- **Informational only:** M3 fog_depth. No reference frame contains fog, so no
  threshold is asserted; it only reports the gradient for the user to eyeball. It
  carries weight 0 in the overall score.
- **Directional proxies (external-photo-validated only):** M5 rain, M6 snow.
  Rain/snow weather is **not implemented in-engine yet (M3 milestone)**, so these
  are validated against external reference *photos*, not engine frames. The naive
  "bright specular dots" (rain) and "bright + low-saturation" (snow) models were
  **rejected** — they mostly count white window/UI chrome. Treat M5/M6 scores as
  directional until the weather actually exists in-engine.

---

## Quick start (the main-window-executable scoring process)

### A. Score a screenshot (offline — no game needed)

```powershell
python D:\csoz-wt\materialperf\tools\matperf-judge\score_frame.py `
    "<path-to-screenshot.png>" --scene night --json out.json
```

- `--scene night|rain|snow|day|auto` (default `auto`; auto-detect from luma + B−R).
- `--crop-top FRAC --crop-bottom FRAC` — fraction of frame height removed before
  scoring, so **HUD/UI is excluded from the score** (per requirement). Defaults
  `0.12` / `0.18` suit a fullscreen shot with the options menu at the bottom. For
  a **windowed** capture (title bar at the very top, thin HUD at the bottom) use
  `--crop-top 0.05 --crop-bottom 0.12`.
- Prints a per-image table (`metric | value | score/10 | PASS/FAIL/INFO | note`),
  an overall 0–10, the applicable-metric list, and the disclaimer. Exit code is
  **always 0** — failures live in the data, not the exit code (it is a scorer,
  not a gate).
- `--json` writes a structured report: `overall_score`, per-metric
  `{name,value,score,pass,threshold,note,weight}`, `failures[]`, `disclaimer`.

**To compare three render workers on the same scene:** run the scorer on each
worker's screenshot of the same map/phase/viewpoint and rank by `overall_score`,
then read `failures[]` to see *why* a low one lost. Highest M2 = best cool-moon
consistency. Always finish with a human eyeball — the score is the filter, not
the judge.

### B. Score performance (needs an engine.log from a real run)

```powershell
python D:\csoz-wt\materialperf\tools\matperf-judge\parse_fps.py `
    "<rundir>\client\engine.log" `
    --start-marker MATPERF-FPS-START --end-marker MATPERF-FPS-END `
    --fps-cap 200 --min-avg 199 --min-1low 195 --json fps.json
```

Slices the log between the two `echo` markers, parses the frozen
`[CSZ:fps] fps=.. avg_ms=.. worst_ms=.. frames=..` lines and the `pass-ms avg:`
per-pass breakdown, then prints a **PASS/FAIL gate**:

- **PASS** iff `avg fps ≥ min-avg (199)` **and** `1% low ≥ min-1low (195)` **and**
  zero GL/fatal errors (`GL error` / `Host_Error` / `CSZ_FatalInit` / `[CSZ…Error]`
  scanned over the **whole** log) **and** ≥55 samples (≈60 s window).
- Reports min/avg/max fps, **1% low** (mean of the worst ceil(1%) per-second
  samples), worst frame ms, mean avg_ms, and a per-pass ms table (which pass
  dominates the frame). Exit code **0 on PASS, 1 on FAIL** — this one *is* a gate.

Thresholds are CLI args; the dev-machine (RTX 5070 Ti) defaults match spec §8.6
(avg ≥199, 1% low ≥195). Override for other hardware.

### C. Capture real frames + perf in one shot (needs a deployed run dir)

`run_capture.ps1` deploys (optional), launches a headless dedicated server +
offscreen client, runs `capture_session.cfg` (connect → spawn → freeze look →
screenshot → 75 s FPS window → quit), then copies `engine.log` + `scrshots\*.png`
into your output dir and runs `parse_fps.py`.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File D:\csoz-wt\materialperf\tools\matperf-judge\run_capture.ps1 `
  -RunDir D:\csoz-run-r1-materialperf -Port 27023 `
  -ClientCfg D:\csoz-wt\materialperf\tools\matperf-judge\capture_session.cfg `
  -ServerCfg <server.cfg> -OutDir <out-dir> `
  -Deploy -BotQuota 31 -AssetSource "E:\CSSME_Evolution_Build3601(HLND)"
```

- `-Deploy` runs `D:\csoz\scripts\package.ps1` first (builds client+server, copies
  to `-Target`). Omit it if the run dir is already built. **Without `-Deploy` and
  no run dir, it errors clearly** instead of guessing.
- Artifact handling is **copy-only**; the only delete is the client's own stale
  `engine.log` before a fresh window. Process kills are scoped to xash3d under the
  run dir. Safe to dry-read.
- Edit `capture_session.cfg`'s per-scene block (`csz_sky_phase`, optional
  `noclip`/`setpos` to the open courtyard) per look you want to grab.

---

## Calibration evidence (verified, fresh re-run)

Independent re-run on the four reference images:

| Image | scene | overall | M2 cool_moon |
|-------|-------|---------|--------------|
| `qq-screenshot-realistic-night.png` (TARGET) | night | **10.0** | **10.0** (B−R = 16.1) |
| `QQ截图20260617093940.png` (regressed/PROBLEM) | night | **8.9** | **6.6** (B−R = 9.3) |
| `REFERENCE_RAIN_SCREENSHOT_01.png` | rain | 10.0 | — (M5 = 10.0, B−R −19.9) |
| `REFERENCE_SNOW_SCREENSHOT_01.png` | snow | 10.0 | — (M6 = 10.0, mean 135, B−R +46.5) |

The required ranking holds **strictly**: the realistic-night target scores higher
overall and on M2 than the regressed shot — the framework correctly flags the
moonlight/color-temperature loss that visually distinguishes them. FPS gate
verified on synthetic logs: a clean ~199 fps / 0-error log → PASS (exit 0); a log
with a deep dip + injected `GL error 0x0506` → FAIL (exit 1) with each failed
condition listed.

---

## Determinism notes (so scores are comparable across iterations)

- Freeze the look with `csz_sky_phase` (−1 live, 0.0 sunset, 0.5 midnight, 0.86
  dawn, 1.0 day) and `wait ~500` (~2.5 s) for the renderer's brightness
  auto-adapt **before** capturing.
- Capture all three workers from the **same map / phase / viewpoint**. The prior
  run found spawn-corner views unrepresentative — use `noclip; setpos` to the open
  courtyard for valid WORLD-region stats.
- Pixel-level A/B between two builds must be **same session** (cross-session has
  brightness drift). For that use `D:\csoz\_scratch\m2\diff_mean.py`.

## Files

```
tools/matperf-judge/
  README.md             this guide (the executable scoring process)
  metrics.py            pure metric functions (M1–M6, score_band, scene detect)
  score_frame.py        visual scorer CLI
  parse_fps.py          FPS/perf log parser + 200-fps gate
  capture_session.cfg   client capture wait-chain template
  run_capture.ps1       deploy/launch/collect/parse wrapper
```
