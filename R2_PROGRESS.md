# R2 PROGRESS — Worker B (weather) — V4A code-quality + build-readiness

- Timestamp (start): 2026-06-18 01:21 -0600
- Branch: `worker/r1-rain-water-snow`
- Worktree: `D:\csoz-wt\weather`
- Account: `D:\cc-accounts\acctB`
- Phase: **V4A = code quality + build readiness only. NO engine launch.** Capture is a separate locked phase.
- Orchestration: main window dispatches subagents (IRON LAW). This file is written first, before long work.

## Inputs read
- `D:\csoz-wt\materialperf\R2_STATIC_REVIEW.md` (supervisor static review; findings W1–W5)
- `D:\csoz-wt\weather\R2_EVIDENCE\capture_results.json` → `fps_avg: 113.1`, `turb_count: 324`

## Root-cause of 113 FPS (water_on_de_aztec)
de_aztec has **324 turbulent water faces**. Current `csz_water.cpp` draws **one `GL_TRIANGLE_FAN` per face = 324 draw calls/frame**. This is a draw-call-bound CPU stall, NOT shader fill cost. Fix = batch all turb faces into a single VBO + single (or few) draw call(s). This is the real fix; FPS will not be faked.

## Work units (disjoint file ownership → safe parallel)

### Agent ARCH — weather module + geom→weather seam (owns: `weather/*`, `geom/csz_world.cpp`, `core/csz_weather_types.h` (new), `csz_renderer.{cpp,h}`)
- W1: remove `geom/csz_world.cpp` → `weather/csz_weather.h` include. Move POD `WeatherSurfaceState` to `core/csz_weather_types.h`; route wet/snow surface state through existing view/composition boundary (mirror `csz_ambience_types.h`).
- W3: fix snow-color night double-darkening in `csz_weather.cpp:231-241` (`base*tint` OR `base*dim`, not both).
- Rain: world-space layered particles (camera-follow box already present) — verify/strengthen; remove "blue blob" particle color.
- W5: remove stale stub/scaffold comments in `csz_weather.h:54-55` and `csz_weather_shaders.inl:36-37`.

### Agent WATER — water pass (owns: `geom/csz_water.cpp`, `geom/csz_water.h`, `geom/csz_water_shaders.inl`)
- **113 FPS fix**: batch 324 per-face `GL_TRIANGLE_FAN` draws into one pre-built VBO + single `glDrawArrays` (or indexed). Add a quality tier if needed. No fake FPS.
- W4: improve water algorithm — keep Fresnel + animated normals + moonlight glint; add credible low-cost reflection/refraction feel (screen-space/cubemap-style perturbation, NOT planar FBO). Fix ring-term period/center misalignment (`csz_water_shaders.inl:139-142`).
- W5: remove stale stub comment `csz_water.h:45`, `csz_water_shaders.inl:36-37`.

### Agent WORLDSHADER — world surface shaders (owns: `geom/csz_world_shaders.inl` ONLY)
- W2: snow near>far brightness — modulate snow by `csz_lmLum`/distance (mirror wet branch). `csz_world_shaders.inl:152-154`.
- Wet ground: confirm grazing highlights + damp darkening read correctly (already strong per review); strengthen grazing rim if cheap.
- Constraint: use ONLY existing uniforms already fed by `csz_world.cpp`. If a new uniform is truly required, STOP and report — do not half-wire.

## After agents return
- BUILD agent: cmake build in worktree, capture exit code + `client.dll` path + SHA256.
- Orchestrator writes `R2_CODE_READY.md` with RESULT/FILES_CHANGED/ARCHITECTURE_FIX/WATER_113FPS_FIX/RAIN_SNOW_WET_FIXES/BUILD/PERFORMANCE_RISK/NEXT_CAPTURE_PLAN.

## Status log
- [01:21] Progress written. Dispatching ARCH + WATER + WORLDSHADER in parallel (disjoint files).
- [01:35] All 3 agents returned. KEY: W1/W2/W3/rain were already landed in commits 6cd1eba + aeeb5d2; static-review line numbers were stale. New work this phase:
  - WATER c799e20: 113fps fix = batch 324 GL_TRIANGLE_FAN per-face draws into 1 glDrawArrays(GL_TRIANGLES) via texture-sorted WaterBatch; a_faceCenter vec2 baked per-vertex (stride 8→10 floats); ring center fixed to per-face centroid; FBO-free reflection-warp + depth-scaled refraction; TMU0 restored.
  - ARCH fea6ea9: verified geom→core POD seam clean (no geom→weather include), removed residual "===B2===" scaffold marker.
  - WORLDSHADER a266311: snow cap 0.97→0.9, wet grazing rim (fres*fres, no new uniform/pow).
- [01:35] Dispatching BUILD agent (cmake worktree build + SHA256). Highest risk: WATER vertex stride/attrib change.
- [01:31] BUILD green: exit 0, worktree-sourced client.dll 701440 B, SHA256 3D67F52D...45B0A, fresh (01:30:24). Stride/attrib change compiled+linked clean. Worktree-vs-submodule trap cleared via CMakeCache.
- [01:31] R2_CODE_READY.md written. RESULT=CODE_READY. Phase V4A complete (no engine launch, per policy). Runtime FPS/visual proof deferred to locked capture phase with concrete plan.
