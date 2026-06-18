# R2 Worker B — V6 Progress

**Started:** 2026-06-18 03:05 -0600
**Branch:** worker/r1-rain-water-snow
**Worktree:** D:\csoz-wt\weather
**Account:** acctB

## V5 rejection — accepted (in my own words)

The user looked at V5 and rejected it. I accept this verdict without defense. Concretely:

1. **It looks fake, and it is NOT a parameter problem — it is a particle/system problem.**
   The fake look does not come from wrong count/color/alpha/size. It comes from the
   underlying weather system being wrong at the bottom level. Tuning the old particle
   parameters is meaningless and is explicitly forbidden for V6. The fix has to be in the
   underlying pipeline (particle generation, simulation, depth policy, sorting/blending,
   material response, water surface), not in knob values.

2. **The "water mirror" in V5 was never water.** What looked like a reflective water surface
   was the camera having flown OUTSIDE the map / into the void, and the blue region was the
   NIGHT-SKY / skybox color — not a water body. So every V5 "water" screenshot is invalid as
   water evidence: wrong camera position, no real water geometry in frustum.

3. Therefore: **V5 was NOT close to GO.** I will not write that. V5 is rejected.

## V6 mandate (root fixes, not tuning)

- The custom rendering pipeline is fully ours to change. Old Xash/GoldSrc renderer and the
  existing CSOZ particle path are NOT fixed and may be replaced.
- Target quality: at least Source-engine level; approach Unreal-style where the 200 FPS budget
  allows. Old-engine overlay particles / flat sky-colored planes / outside-map screenshots = FAIL
  even at high FPS.
- **Camera legitimacy gate is a HARD precondition** before any visual claim:
  - prove camera is inside valid world space (not void/skybox/outside map);
  - prove the frustum actually contains real water draw geometry for water shots;
  - log camera pos/angles + contents/leaf/PVS/BSP validity;
  - log `visible_water_faces` (post frustum+depth cull), not just the turb centroid.

## Honesty contract for this run

- No "verified" without real evidence (build + smoke + in-frustum capture + Read of frames).
- Visual/audience review and code/engineering review are SEPARATE subagents, never merged.
- If a claim can't be verified: write UNKNOWN / BLOCKED, never optimism.
- Engine runs only under the global lock.

## Log

- 03:05 — Wrote this file. Dispatched 3 read-only recon agents (code map / V5 forensics / acceptor discovery).
- 03:20 — Recon complete. Confirmed root causes:
  - **Water reflection + refraction are 100% in-shader analytic fakes** (procedural sky gradient + UV-warp of
    own diffuse). NO FBO/RTT/cubemap anywhere in the water path. → genuine bottom-level architecture gap.
  - **Camera has ZERO legitimacy validation** (csz_renderer.cpp:289-334 slams debugcam coords into view, logs
    only pos/ang). The only water log is a BUILD-TIME centroid, never a runtime in-frustum count. V5 "water"
    pose (328,-600,-300) was over the map edge → blue region = skybox/void, exactly as user said.
  - **Rain/snow** are world-space depth-tested billboards (NOT a screen overlay in code), but read as fake:
    uniform streaks blanketing the sky, texture-free, no parallax legibility, no splash interaction.
  - Acceptor = user judging by eye vs "Source-min / Unreal-ideal". Rubric captured (7 scorable visual dims,
    camera-inside is a hard GATE on all water claims). External reference set = OWED.
  - Build: CMake preset win32-release-amd64 → build\client\cl_dll\Release\client.dll, overlaid into
    D:\csoz-run-r1-weather\client\cstrike\cl_dlls\. Engine run gated by directory-mutex lock
    D:\csoz\_orch\engine-capture.lock via Acquire/Release-CsozEngineLock.ps1.

## V6 plan (bottom-level, not tuning)

- **Phase 1 (parallel, non-overlapping files):**
  - **A — Camera legitimacy gate:** PM_PointContents on eye (reuse csz_sky.cpp:275-277 pattern) + world-AABB
    inside test → `[CSZ:camgate]` log + CameraLegit flag; `csz_debugcam 2` auto-frame-water (search candidate
    poses around logged water AABB, pick first with valid contents + water-in-frustum, single launch);
    `visible_water_faces` per-frame frustum count in csz_water; capture script FAILS any scenario whose camera
    is SOLID/SKY/outside-world, and any water scenario with visible_water_faces==0.
  - **B — Rain/snow realism:** within csz_weather.* — kill the uniform full-frame look, real near/far parallax
    legibility, motion-correct streak length, fade over sky, add ground/water splash interaction, snow depth
    distribution. Bottom-level appearance/integration, not param tuning.
- **Phase 2 (sequential, after A):**
  - **C — Real water:** planar reflection FBO (mirrored scene pass) + refraction (framebuffer copy) sampled in
    the water shader via Fresnel — Source-style real reflection/refraction of actual scene geometry. Replaces
    the analytic fake. Honest FPS gate (200 budget; half-res FBO).
- **Phase 3:** build → capture under lock → SEPARATE visual-review (frames) & code-review (diffs) subagents.
- Honesty: any phase that won't build/work → reported BLOCKED, never optimism.
