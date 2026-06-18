# R2 RESULT — Worker B (weather) — V4C (locked runtime capture)

- Timestamp: 2026-06-18 02:05 -0600
- Branch: `worker/r1-rain-water-snow`  | Worktree: `D:\csoz-wt\weather`
- Engine: Xash3D, run dir `D:\csoz-run-r1-weather\client`, 1280x720 windowed, `+fps_max 400 +fps_override 1 +gl_vsync 0` (far above 200; no menu/host_framerate/fake FPS).

## RESULT: **NEEDS_ITERATION**

Runtime is **proven and the 113-FPS water regression is fixed** — engine launches, water draw-call batching works (324 turb surfaces → **1 draw batch**), FPS is far above 200 everywhere, GL errors = 0, lock hygiene clean. The one unmet gate is **visual confirmation that the new 10-float-stride water is not garbled**: the de_aztec water surface could not be framed in 3 camera attempts (water never entered the frustum), so there is no direct visual proof. Perf/runtime ⇒ GO-level; visual water gate ⇒ owed. Hence NEEDS_ITERATION, not GO.

---

## DLL_STAGING

- Source artifact: `D:\csoz-wt\weather\build\client\cl_dll\Release\client.dll`
- SHA256 = `3D67F52D7C903E45C8CF04F1406D91B2D0EDAEFC880C44C1E8E605AD03E45B0A` — **matches the required worktree SHA.**
- Staged into run dir by `capture_r2_weather.ps1`: copied to `...\cstrike\cl_dlls\client.dll`, then **re-hashed at both ends and asserted equal before every launch** (script lines 545-549). Overlay SHA echoed each run = same value. No submodule/worktree mixup.

## SCREENSHOT_TABLE

All PNGs in `D:\csoz-wt\weather\R2_EVIDENCE\`. "Clean" = no viewmodel/HUD/crosshair/console.

| Deliverable PNG | Scenario / map | Captured | Clean shot? | Water framed? | Effect visible | Notes |
|---|---|---|---|---|---|---|
| weather_v4c_00_water_off_de_aztec | water_off_de_aztec / de_aztec | reaim2 (clean) | **YES** (faint corner notify only) | NO | n/a | camera missed water pool |
| weather_v4c_01_water_on_de_aztec | water_on_de_aztec / de_aztec | reaim2 (clean) | **YES** (faint corner notify only) | **NO** | none (ON==OFF byte-identical → no water in frustum, not a garble) | **garbled-check UNVERIFIED** |
| weather_v4c_02_rain_high_de_dust2 | rain_far_de_dust2 / de_dust2 | rest-run | NO (viewmodel+HUD) | n/a | rain: slanted 3D world-space streaks, plausible | re-shoot clean next iter |
| weather_v4c_03_snow_cover_night | snow_cover_de_dust2 / de_dust2 (night) | rest-run | NO (viewmodel+HUD) | n/a | night scene + soft round flakes; floor near-crushed dark | legibility marginal; re-shoot clean |
| weather_v4c_04_fps_worst_case | water_rain_de_aztec / de_aztec | rest-run | NO (viewmodel+HUD) | NO | rain streaks present; water not framed | FPS gate met (below) |
| rain_wet_ground_de_dust2 (extra) | de_dust2 | rest-run | NO | n/a | damp darkening; grazing rim weak/ambiguous | |
| snow_fall_de_dust2 (extra) | de_dust2 | rest-run | NO | n/a | soft round flakes (NOT hard blue squares) — passes | |
| weather_off_baseline_de_dust2 (extra) | de_dust2 | rest-run | NO | n/a | baseline | |

Minimum required set: **all 5 present.** Full desired set (water_rain, wet_ground, snow_fall): **also present.**

## FPS_200_EVIDENCE

From engine-log `[CSZ:fps]` samples inside the `MATPERF-FPS-START..END` window (FPS-hold scenarios; `developer 1`). Top is bounded by the `fps_max 400` cap → true ceiling is higher.

| Scenario | samples | avg | 1%-low | min | max | ≥199 avg | ≥195 1%low | ≥55 samp |
|---|---|---|---|---|---|---|---|---|
| water_on_de_aztec (regression target) | 68 | **398.7** | **357** | 357 | 400.4 | ✅ | ✅ | ✅ |
| water_rain_de_aztec (worst case) | 68 | **399.1** | **359.5** | 359.5 | 400 | ✅ | ✅ | ✅ |
| rain_far_de_dust2 (rain high) | 68 | **397.8** | **271.5** | 271.5 | 399.9 | ✅ | ✅ | ✅ |

Baseline `water_off_de_aztec` was 113.1 FPS pre-fix → now batched. The clean visual re-shoots used `developer 0` (no FPS window) and so are not the FPS evidence; the FPS evidence above is from the FPS-hold captures. All FPS gates pass with large margin. `glGetError`/GL-error lines: **none** across all 8 logs (only normal `R_NewMap: world invalidated` info and an unrelated `VoiceCapture_Init` DirectSound audio-capture-device failure — no microphone in headless; not a render error).

## WATER_BATCH_RUNTIME_VERDICT

**Batching CONFIRMED at runtime.** Both de_aztec water-ON captures log:
`[CSZ:water] Info: built maps/de_aztec.bsp: 2490 turb verts, 1 draw batch(es)`
→ the ~324 turbulent water **surfaces** are collapsed into **1 draw call**. (The script's `turb_count=2490` field is the *vertex* count — its regex matches `N turb` and grabs "verts"; the batching metric is "1 draw batch(es)". The hard-gate "turb_count ~324" refers to the old per-surface/per-draw count, which is exactly what is now batched to 1.) With water ON the frame holds **398.7 FPS avg** (was 113.1) and **0 GL errors**, so the new 10-float vertex stride / `a_faceCenter` attribute is not catastrophically mis-bound (a bad `glVertexAttribPointer` would typically throw GL_INVALID_OPERATION or crash). **Caveat:** this is build/perf/GL evidence — it is NOT visual proof the water *looks* correct; that requires framing the surface (see NEXT_ITERATION).

## VISUAL_SELF_SCORE

- Water (de_aztec): **UNSCORED / owed** — surface never framed in 3 attempts; garbled-vs-coherent unanswered. Indirect health only (builds, 1 batch, 0 GL errors, 398 FPS).
- Rain high (de_dust2): ~6/10 — believable slanted world-space streaks, but judged on a dirty (viewmodel/HUD) frame.
- Snow cover night: ~4/10 — reads as night with soft round flakes, but floor near double-dark/crushed; near>far gradient hard to judge; dirty frame.
- Snow fall: ~6/10 — soft round flakes (passes the "no hard blue blob" check).
- Wet ground grazing: ~4/10 — damp darkening plausible; grazing-rim glint weak/ambiguous.
- Clean-shot pipeline: **fixed and verified** on de_aztec (viewmodel + HUD removed via `r_drawviewmodel 0` + `developer 0` + longer settle; only a faint corner notify line remains).
- **Overall self-score: 5/10** — perf/runtime excellent and proven; visual evidence incomplete (water unframed; rain/snow shots dirty).

## NEXT_ITERATION

1. **Frame the de_aztec water surface** (the one blocking gate). Logged geometry: `turb centroid=(328 346 -530)`, `min=(-3776 -1856 -544)`, `max=(2208 1984 -344)` — a large, low water body. 3 blind camera guesses missed (camera kept landing on surrounding dry floor/walls). Needs map-aware aiming or interactive positioning; confirm the water-ON frame **differs** from water-OFF, then judge coherent-vs-garbled. Consider a map with an obvious central water body if de_aztec's turb water is awkward to frame.
2. **Re-shoot de_dust2 rain / snow-night / wet-ground with the now-fixed clean-shot settings** (`r_drawviewmodel 0`, `developer 0`/`con_notifytime`, 2500-frame settle) so visual gates are judged on clean frames. Also drop the faint corner notify line if possible.
3. **Snow-night legibility**: the night floor reads near-crushed; verify the W3 single-dim / near>far gradient is actually legible at `csz_sky_phase 0.0` or pick a slightly-lifted night phase for the shot.
4. Optional: capture one pass with `fps_max 0` to report the true uncapped ceiling (currently pinned at the 400 cap).

## Capture-tooling changes made this session (no rendering code touched)
- `R2_CAPTURE/capture_r2_weather.ps1`: added `r_drawviewmodel 0`; settle waits 120→2500 (let join/team TextMsg fade); added night `csz_sky_phase 0.0` to snow_cover; re-aimed de_aztec water coords + `developer 0`/`con_notifytime 0.05` for clean visual shots. Invocation fix: array `-Scenarios` must be passed via `-Command "& script -Scenarios @(...)"` (not `-File`, which mis-binds the array).

## Lock hygiene
Every run acquired the lock, ran, and **released it in the `finally` block**; final state verified: `xash3d=0`, no lock file in the lock dir, `D:\csoz\_orch\engine-capture.lock` absent. No stale lock or stale engine left behind.
