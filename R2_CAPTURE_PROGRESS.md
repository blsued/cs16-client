# R2 CAPTURE PROGRESS — Worker B (weather) — V4C

- Start timestamp: **2026-06-18 01:42 -0600**
- Branch: `worker/r1-rain-water-snow`
- Worktree: `D:\csoz-wt\weather`
- Phase: V4C = **locked runtime capture / verification only**. No rendering-code edits (capture-script-only fixes if strictly required).
- Prior V4B run was killed for ~8 min of silent no-progress. This run is **background + heartbeat-monitored** so progress is visible at every step.

---

## CAPTURE PLAN (exact)

### Prerequisites verified before any launch
1. Build artifact `D:\csoz-wt\weather\build\client\cl_dll\Release\client.dll`
   exists and SHA256 == `3D67F52D7C903E45C8CF04F1406D91B2D0EDAEFC880C44C1E8E605AD03E45B0A`.
2. Engine exe `D:\csoz-run-r1-weather\client\xash3d.exe` exists.
3. Engine lock currently free (or stealable-stale). Lock dir:
   `C:\Users\Administrator\Documents\Codex\2026-06-17\claude-max20-xash3d-200\work\claude_dispatch\engine_lock`.

### Capture tooling
- Driver: `D:\csoz-wt\weather\R2_CAPTURE\capture_r2_weather.ps1` (existing, proven anti-menu + FPS-hold + turb/FPS scrape + 3-min no-progress abort + always-release-lock `finally`).
- The script overlays the WIP dll into the run dir and re-verifies SHA before launch.
- FPS: `+fps_max 400 +fps_override 1 +gl_vsync 0` on the command line (authoritative, post-config-reexec). No menu FPS, no host_framerate, no fake FPS.

### Scenario -> required deliverable PNG mapping
| Required deliverable PNG | Script scenario | Map | Weather | Hold(FPS window) |
|---|---|---|---|---|
| weather_v4c_00_water_off_de_aztec | water_off_de_aztec | de_aztec | off, water 0 | no |
| weather_v4c_01_water_on_de_aztec  | water_on_de_aztec  | de_aztec | off, water 1 | yes (FPS gate) |
| weather_v4c_02_rain_high_de_dust2 | rain_far_de_dust2  | de_dust2 | rain q2 i1.0 | yes |
| weather_v4c_03_snow_cover_night   | snow_cover_de_dust2| de_dust2 | snow q2 i1.0 | no |
| weather_v4c_04_fps_worst_case     | water_rain_de_aztec| de_aztec | water1 + rain q2 | yes (worst case) |

After capture, evidence PNGs are copied to the required `weather_v4c_*` names in `R2_EVIDENCE\`.

### Hard gates checked from logs (no fabrication)
- de_aztec water ON: `turb_count` ~ 324 AND `fps_avg >= 199`.
- GL errors = 0; fps_samples >= 55; 1% low >= 195 if derivable.
- Water not garbled (new 10-float stride) — confirmed by visual-review subagent reading the PNG.
- Clean map shots only (HUD/crosshair/console/showfps all off in autoexec).

### Abort / blocker rule
- Lock held only for package/launch/screenshot/FPS.
- If no `xash3d.exe` and no screenshot/log within 3 min of lock acquisition -> release lock, write `BLOCKED` in `R2_RESULT.md` + `R2_EVIDENCE\BLOCKER.txt` with exact blocker.
- `finally` block always releases lock and kills stray xash3d.

---

## HEARTBEAT LOG
- 01:42 -0600 — progress file written; reading prerequisites (DLL SHA / exe / lock state).
- 01:43 -0600 — PREREQS OK: DLL SHA matches `3d67f52...e45b0a`; xash3d.exe present; cl_dlls target present; **lock FREE** (no lock file). Launching background smoke of regression-target A/B pair (`water_off_de_aztec`, `water_on_de_aztec`) — proves engine launch + clean shot + FPS delta before committing to the full set.
- 01:45-01:46 — two false starts: `-Scenarios` array did not bind via `-File` (comma-joined token / spilled to `$RunDir`). Both failed in pre-flight **before** lock acquisition — no lock taken, no engine. Fixed by invoking via `-Command "& script -Scenarios @('a','b')"`.
- 01:46:46-01:48 — **SMOKE SUCCESS (pipeline proven).** Lock acquired (`engine-capture.lock` by weather) → dll overlaid (SHA reverified) → de_aztec launched → 2 clean shots → lock RELEASED. No stale lock/engine.
  - `water_off_de_aztec`: SUCCESS, png saved.
  - `water_on_de_aztec`: SUCCESS, **fps avg 398.7, 1%-low 357, min 357, 68 samples** (fps_max 400 cap binds top; true ceiling higher). Was 113 → regression GONE.
  - Raw log: `built maps/de_aztec.bsp: 2490 turb verts, **1 draw batch(es)**` — 324 surfaces collapsed to 1 draw call. The script's `turb_count=2490` is the VERT count (regex matches `N turb`); the batching metric is "1 draw batch(es)". **Gate "~324" reconciled: 324 surfaces → 1 batch is the intended result.**
  - GL: only `GL_BuildLightmaps ... dirty` info line; no GL error lines = 0 errors logged.
- 01:48 -0600 — added `csz_sky_phase 0.0` (night) to `snow_cover_de_dust2` (capture-tooling one-liner) for the snow_cover_night deliverable. Launching remaining set: rain_far / rain_wet_ground / snow_fall / snow_cover / weather_off_baseline / water_rain (worst case).
- 01:50-01:54 — **REST RUN 6/6 SUCCESS.** All clear ≥199 FPS by wide margin: rain_high(rain_far) 397.8 avg / 271.5 1%-low / 68 samp; worst-case water_rain 399.1 avg / 359.5 1%-low / 68 samp; both aztec water = "2490 turb verts, 1 draw batch". GL errors: NONE (only non-render `R_NewMap` info + a `VoiceCapture` DirectSound audio-device failure — no mic, irrelevant). Lock released clean.
- 01:55 -0600 — copied 5 deliverable PNGs to `weather_v4c_*` names.
- 01:56 -0600 — **VISUAL REVIEW (separate render-acceptor subagent) = captures visually INVALID.** (a) Every frame has weapon viewmodel + HUD/notify text → clean-shot HARD GATE FAILED. (b) de_aztec camera framed a wall — **water pool never in frame**, so the 10-float-stride garbled-water question is UNANSWERED. Rain streaks/snow flakes that ARE visible look plausible. Perf proof stands (camera-independent log facts).
- 01:57 -0600 — capture-tooling iteration: `r_drawviewmodel 0`, settle waits 120→2500 (let join TextMsg fade), `developer 0`+`con_notifytime 0.05` (kill on-screen text), and **re-aimed de_aztec water at logged centroid (328 346 -530)**: `setpos 328 -150 -210 / setang 40 90 0`. Re-running clean A/B pair (water_off, water_on) — visual-only (FPS already proven).
- 02:01-02:02 — reaim #1 done. **Clean-shot fix WORKS**: viewmodel + HUD gone (only faint bottom notify strip). But camera still on dry floor — water not framed; ON==OFF byte-identical (no water in frustum).
- 02:03-02:05 — diagnosed: camera sat 300+ units ABOVE local water height (Z~-530), looking at surrounding floor. reaim #2 dropped camera to `setpos 328 100 -470 / setang 12 90 0` (~60u above surface). Still no water in frame (3rd miss). Clean shot confirmed again.
- 02:05 -0600 — **STOP framing grind (3 attempts = discipline limit on blind coord-guessing).** Final state verified clean: no xash3d, no lock file, orch lock absent.
- 02:06 -0600 — wrote `R2_RESULT.md`. **RESULT = NEEDS_ITERATION**: runtime/perf/batching/GL all PROVEN (GO-level); the single owed gate is visual confirmation the 10-float water isn't garbled (de_aztec water never framed). Next iteration = map-aware water framing + clean re-shoot of de_dust2 rain/snow.
