# READY_FOR_CAPTURE — Worker B (weather): rain / wetland / water / snow

- Date: 2026-06-17
- Worktree: `D:\csoz-wt\weather`  | branch: `worker/r1-rain-water-snow`
- Run dir (per ENGINE_CAPTURE_PROTOCOL): `D:\csoz-run-r1-weather`  | port: `27022`
- Phase-1 status: **BUILD GREEN, NOT YET CAPTURED.** WIP is uncommitted (working-tree only).
- This worker is blocked on screenshots only. All offline work (read/fix/build) is done. Engine was NOT started here.

## 1. Build evidence (freshly compiled, this session)
- Result: **PASS**, exit 0, zero warnings, zero errors. WIP compiled clean **as-is — no source edits were needed.**
- Artifact: `D:\csoz-wt\weather\build\client\cl_dll\Release\client.dll`
- SHA256: `250456FBFDC1339F72DE9134DE4DCC39F1E35907B1D4A317AAF4A8002A693160`
- Size: `690688` bytes
- Built via worktree-local CMake tree `build\client\` (VS2022 generator, Release). NOTE: the repo `scripts\build-client.ps1` targets `D:\csoz\src\cs16-client`, **not** this worktree — do not use it for this WIP.

## 2. What ships in this WIP (all compile-wired)
Three effect families in `cl_dll/cszrender/`:
- **Precipitation particles** — `weather/csz_weather.cpp` (rain streaks + snow flakes, camera-following 700u box).
- **Ground wet / snow-cover grade** — spliced into the world FS (`geom/csz_world_shaders.inl`, fed from `geom/csz_world.cpp`); up-facing surfaces only, byte-exact no-op when weather off.
- **Animated water surface** — `geom/csz_water.cpp` over BSP turb faces (animated ripple + fake-Fresnel "reflection feel" + moon specular). **Not** a true planar reflection/refraction.

There is **NO blood-rain** in this WIP (mode is only 0/1/2). If blood-rain is in scope for R1, it is a future iteration, not capturable now.

## 3. CVARs (set AFTER spawn so they stick)
| cvar | default | range | effect |
|---|---|---|---|
| `csz_weather` | `0` | 0=off, 1=rain, 2=snow | master selector; drives particles + world wet/snow + water ripple lift |
| `csz_weather_intensity` | `0.7` | 0..1 | density + wetness/snow coverage + rain-ripple amplitude |
| `csz_weather_quality` | `1` | 0/1/2 | particle COUNT only (rain {1200,3500,7000}, snow {800,2000,4000}); never gates presence |
| dev cmd `csz_devweather <0\|1\|2> [intensity]` | — | — | convenience: sets the two cvars above |

Day/night appearance (snow color, water/wet reflection tint) tracks `csz_sky_phase` (default `-1` live; freeze `0.5`=day, `0.0`/`0.9`=night for deterministic A/B).

**Default load shows NO weather** (`csz_weather 0`): no rain, no snow, no wet/snow grade. Water surface still animates on water maps but with no rain sparkle. You MUST set `csz_weather 1` or `2` after spawn to see the feature.

## 4. Map requirement — IMPORTANT
- Rain / snow / ground-wet / snow-cover: visible on **de_dust2** (open ground, crate tops). Fine for shots 1–4.
- **Water surface needs a map with BSP turb (water) surfaces. de_dust2 / de_dust have NONE → water pass silently early-returns (no error, nothing drawn).**
  - Use **`de_aztec`** (central water channel) for the water shot. Verify on map load: console info log `built %s: N turb surfaces` (from `csz_water.cpp`) — N must be > 0. If aztec logs 0 turb, fall back to `cs_militia` or `de_inferno` puddles.

## 5. Capture matrix (serial, one worker, lock per protocol)
Harness (reuse the sky-goal harness):
```
xash3d.exe -log -console -game cstrike -windowed -width 1280 -height 720 +sv_lan 1 +maxplayers 1
```
Spawn: `setinfo _vgui_menus 0; chooseteam; jointeam 2; joinclass 1`
Freeze: `host_framerate 0.05`  •  fly: `noclip`  •  shot: `screenshot` → `cstrike\scrshots\*.png`

| # | effect | map | cvars (after spawn) | viewpoint |
|---|---|---|---|---|
| 1 | rain streaks | de_dust2 | `csz_weather 1; csz_weather_intensity 1; csz_weather_quality 2` | open courtyard, look slightly up across open space; dark wall/shadow backdrop reads streaks best |
| 2 | ground wetness | de_dust2 | `csz_weather 1; csz_weather_intensity 1` (opt `csz_sky_phase 0.5`) | grazing-angle down at flat LIT ground; expect darkened + specular glint |
| 3 | snow falling | de_dust2 | `csz_weather 2; csz_weather_intensity 1; csz_weather_quality 2` | open area, look across; flakes small (1.6–3.8u), frame vs dark backdrop |
| 4 | snow cover | de_dust2 | `csz_weather 2; csz_weather_intensity 1` | ground + crate tops; up-facing surfaces tint cool white |
| 5 | water surface | **de_aztec** | baseline `csz_weather 0`, then `csz_weather 1; csz_weather_intensity 1` | stand over/beside the central water trough, grazing angle on the water plane |
| 6 | default/off baseline | de_dust2 | none | confirms nothing precipitates by default |

Optional night/day variants of 3–5: repeat with `csz_sky_phase 0.0` (night) vs `0.5` (day) to capture color shift.

Expected shot count: **6 core** (+ up to ~4 day/night variants of 3–5). Est. capture time ~15–25 min serial.

## 6. Deploy step the main window must do first (capture prep)
The fresh `client.dll` is in the **worktree build tree**, not in any run dir. To capture:
1. Acquire engine-capture lock (`Acquire-CsozEngineLock.ps1 -Owner weather`).
2. Build/refresh run dir `D:\csoz-run-r1-weather` via `package.ps1 -Target D:\csoz-run-r1-weather -AssetSource "E:\CSSME_Evolution_Build3601(HLND)" -BotQuota 0` (or the standard asset source).
3. **Overlay this WIP's client.dll** over the packaged one: copy `D:\csoz-wt\weather\build\client\cl_dll\Release\client.dll` (SHA `250456FB…3160`) into `D:\csoz-run-r1-weather\client\cstrike\cl_dlls\client.dll` (confirm the in-run path/name matches how the sky goal overlaid its dev dll). Verify SHA matches `250456FB…3160` after copy.
4. Launch on port `27022`. Capture per matrix. Release lock.

(Worker B did NOT touch any run dir per Phase-1 rules — this overlay is the main window's serial step.)

## 7. Risks / honesty notes
- **Water on de_dust2 = invisible** (no turb). The #1 "compiled but shows nothing" trap — must use a water map. Verify turb count in the log.
- Water is **opaque** (depth-write ON, replaces engine water on those faces) and the "reflection" is a Fresnel sheen, **not** a mirror — describe it as a sheen, not true reflection.
- Particle box follows camera (no world rain volume) → rain/snow appears everywhere incl. indoors (depth-occluded by walls); indoor shots may show near-camera streaks clipping ceilings. Prefer outdoor viewpoints.
- Wet/snow grade is gated to lightmap-LIT areas; fully-shadowed floor shows little. Capture lit ground.
- Stale comments in `csz_weather.h` / `csz_water.h` / the `.inl` headers still say "stub/scaffold only" — these are inaccurate; the bodies are fully implemented. (Cleanup nit, not a blocker.)
- No acceptance rubric for weather/water exists yet (the documented rubric is the sky goal's). The acceptor (user) judges visual realism of rain/snow/wet/water against intent once captured.
