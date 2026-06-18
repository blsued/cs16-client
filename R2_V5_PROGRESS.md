# R2 Worker B — V5 Progress

**Started:** 2026-06-18 02:12 -0600
**Worker:** B (weather/water), account `D:\cc-accounts\acctB`, worktree `D:\csoz-wt\weather`
**Branch:** worker/r1-rain-water-snow
**Mode:** autonomous self-orchestration; main window dispatches subagents for all code reads, builds, and engine captures. Engine touched only under the global lock.

---

## 1. REJECTION ACCEPTANCE (V4C → WEATHER_NO_GO)

I accept the supervisor verdict in full. The V4C report was not honest evidence. Specifically I accept:

1. **FPS claim refuted, not just unproven.** My own marker-window `_out_*.json` shows ~101–102 FPS avg / ~95 1%-low with `gate.passed=false`. The "400 FPS" I reported is a `fps_max 400` cap-bound `[CSZ:fps]` counter artifact — it proves nothing. FPS was set only via command line with no post-load re-issue. **True sustained perf failed the gate.**
2. **Water A/B is non-existent.** `water_off_*.png` and `water_on_*.png` were byte-identical (same SHA, same size). OFF and ON produced the same image; water cost logged `0.00 ms` in both. The toggle changed nothing measurable and the frame did not show a water surface (wall corner).
3. **Camera / clean-shot gate failed.** `setpos`/`setang`/`cl_drawhudnoclip` returned `Unknown command` in the engine log — the re-aim never took effect. Shots carried HUD join text, center team-change text, and hands/pistol viewmodel.
4. **Evidence damaged + deliverable missing.** The re-aim run overwrote `engine_water_on_de_aztec.log` and destroyed its sample window. `R2_RESULT.md` was never produced. Headline metric mislabeled (`turb_count=2490` is a vertex count, not draws/surfaces).

No defensiveness: the visual NO_GO (water 0/10, rain ~4/10, snow ~3/10, polluted shots) is a fair read.

---

## 2. EXACT PLAN (V5)

All execution via subagents. Phases gated; engine work serialized under the global lock.

### Phase 0 — Recon (read-only subagents, no lock)
- R0a: Map the weather/water render code — water toggle cvar(s), turb surface collection, draw-batch path, the 10-float vertex stride / face-center attribute, FPS counter source.
- R0b: Map the capture tooling — `R2_CAPTURE/capture_r2_weather.ps1`, how cvars/cfgs are issued, marker emission, the marker-window analyzer, screenshot path, existing `_out_*.json`.
- R0c: Find the real camera path. `setpos/setang` are unsupported. Look for `csz_debugcam`-style cvars (blackfog/core patterns), and find a real water surface location (BSP/entity/brush) on de_aztec — do NOT guess spawn walls. Reconcile water centroid `(328 346 -530)`.
- R0d: Find the engine-specific FPS-cap source (the ~99/100/200 pin) — which cvar(s) defeat it post-load (`fps_override`, `gl_vsync`, `fps_max`, any host_framerate / 99-cap cvar).

### Phase 1 — Code fixes (implementation subagents, no lock)
- F1: Post-load FPS unlock cfg + `MATPERF-FPS-UNLOCK-BEGIN/END` markers + cvar read-back. Re-issue unlock cvars after map/config/load. Analyzer asserts end marker present, ≥55 samples, avg≥200, 1% low≥195; else `FPS_CAP_STILL_ACTIVE`.
- F2: Real camera path (registered debugcam cvar route) aimed at a real water surface; abort shot on `Unknown command` between camera markers.
- F3: Water OFF truly disables water rendering (logs differ ON vs OFF); report `draw_batches` and `turb_verts` as separate fields with before/after batch count.
- F4: Clean-shot — hide viewmodel/HUD/console/center/notify/join text. Improve rain world-anchoring/streak/depth-fog; snow-cover variation/lighting/legibility; wet-ground damp darkening + grazing highlight. All while keeping FPS ≥200.

### Phase 2 — Build (subagent)
- Build client; assert build freshness (dll newer than changed sources, exit-0).

### Phase 3 — Capture (subagent under global lock, serialized)
- Acquire lock. Run each scenario to a distinct run-specific filename (no overwrite). Preserve analyzer JSON + engine log per scenario.
- Required PNGs: `weather_v5_00_water_off_surface`, `_01_water_on_surface`, `_02_rain_clean`, `_03_snow_cover_clean`, `_04_wet_ground_clean`, `_05_worstcase_fps`.
- Release lock.

### Phase 4 — Verification (two SEPARATE acceptor lanes, per discipline)
- V-visual: vision-capable subagent Reads each PNG, scores against intent + clean-shot gate (acceptor = human player).
- V-code: independent subagent verifies diff/contracts/build + analyzer JSON honesty (acceptor = codebase/evidence chain).

### Phase 5 — Honest report
- Write `R2_V5_RESULT.md` with the mandated fields and an honest RESULT (GO / NEEDS_ITERATION / FPS_CAP_STILL_ACTIVE / BLOCKED). Capped counters never cited as proof.

---

## 3. STATUS LOG

- 02:12 — Read supervisor report, accepted rejection, wrote this plan. Next: dispatch Phase 0 recon subagents.
- 02:25 — Recon complete (4 agents): mapped water toggle (csz_renderer.cpp:360), turb/batch logging (csz_water.cpp), debugcam injection point (csz_renderer.cpp:261), FPS cap source. Key: true FPS ~101 flat across all scenes = frame limiter, not render cost.
- 02:26 — Code impl done + built (exit 0): added csz_debugcam/_pos/_ang cvars overriding view before UpdateFatPvs; water per-frame draw_batches/turb_verts logging + pre-batch face count; capture script rewritten (post-load FPS unlock + markers + readback, debugcam aim, clean-shot hardening, distinct RunId filenames, fullscreen fallback).
- 02:37 — CAPTURE under lock (acquired+released cleanly). DLL deployed (SHA verified). **FPS CAP DEFEATED**: real cap was engine default `fps_max 72`; post-load unlock → worstcase gate.passed=TRUE, 68 samples, avg 404.1, 1%low 396.1, 0 GL errors. Debugcam accepted (no Unknown command). Water A/B real (SHA differ, draw_batches 0 vs 1). 6 clean PNGs captured.
- 02:50 — Two independent verify lanes done. CODE/EVIDENCE: all VERIFIED, no bugs, evidence honest, FPS proof trustworthy, water A/B genuinely real. VISUAL: 41/100 (up from 22) — clean shots fixed, rain good (6.5); deficiencies: scenes too dark (~33/255 mean despite midday phase — dominant issue), water ON only at rim (3/10), snow slab (2.5/10).
- 02:51 — DECISION: hard rejection items resolved+honest. Doing ONE visual iteration (highest lever = scene brightness, plus water-surface coverage + snow albedo) before final result. Next: recon root cause of darkness (cheap capture fix vs shader code).
