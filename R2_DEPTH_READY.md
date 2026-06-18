# R2 Worker A — V6 Depth-Correct Volumetric Flashlight Shaft — DEPTH READY

**Date:** 2026-06-18
**Worker:** A (acctA, worktree `D:\csoz-wt\blackfog`, branch `worker/r1-blackfog-flashlight`)
**Mode:** ENGINEERING ONLY — engine NOT launched, no capture lock taken, no screenshots this phase (per main-window V6 directive: fix code + evidence chain + build + report *before* any capture round).

---

## RESULT

**DEPTH_READY** — the depth-correct shaft is implemented and builds clean; the capture
evidence chain (DLL stage+SHA, FPS measurement, camera method) is fixed and scripted;
docs/comments match the code. The decisive **wall-between-camera-and-cone** occlusion is
now handled in code by re-enabled hardware depth-test (all tiers) plus a true
`min(exit, sceneDepth)` clamp (MED/HIGH default).

**Explicitly OWED to the later locked capture phase** (cannot be produced without the
engine, which this phase forbade): in-engine visual A/B + the wall-bleed shot, and
per-tier uncapped in-world FPS + `kTmVolume`. The new capture script
(`_scratch/capture_r2_v6.ps1`) is authored to produce exactly those under the engine lock.

---

## REJECT_FIXES (each prior rejection item addressed)

1. **Wall bleed "present by design" (depth-test OFF).** FIXED. The shaft draw is now
   **depth-test ON (`GL_LEQUAL`), depth-write OFF, back-face hull** (cull front). Because
   the volumetric pass (slot 13.5) runs after the opaque world/studio depth writes
   (slots 11/12) with **no depth re-clear**, the hardware depth-test now rejects every
   cone fragment behind an opaque surface — so a wall between camera and cone fully
   occludes the shaft at zero shader cost. On top of that, MED/HIGH apply a true
   `min(exit, sceneDepth)` clamp (see DEPTH_ALGORITHM). No deferral, no work-around.
2. **Broken capture evidence chain.** FIXED. New `_scratch/capture_r2_v6.ps1` stages the
   one canonical worktree-built `client.dll`, computes SHA256 of source and deployed
   copy, and **aborts (exit ≠ 0) on mismatch** before any capture. Single source of
   truth resolved and documented (see DLL_STAGE_SHA_PLAN): the three conflicting/stale
   SHAs are retired.
3. **FPS invalid (pinned `fps_max 200`).** FIXED. The pin is removed; FPS-measurement
   mode sets `fps_max 1000` (far above 200 so real headroom is observable). Per-tier
   matrix specified (see FPS_MEASUREMENT_PLAN).
4. **Docs contradict code (depth-test ON / occlusion claims).** FIXED. The shader header
   block, the "HONEST LIMITATION" block, and the C++ state-setup comments were rewritten
   to describe the real V6 behavior (depth-test ON, back-face hull, analytic entry/exit,
   scene-depth clamp at MED/HIGH).
5. **Camera method (`setpos`/`setang`).** Confirmed **UNSUPPORTED** in this build —
   logged "Unknown command: setpos / setang" in all 7 prior runs *and* an independent
   probe; every prior "evidence" shot was therefore taken at the default spawn, not the
   scripted viewpoint. Not reused. Replaced with the in-tree, registered `csz_debugcam`
   (see CAMERA_METHOD) plus a runtime log proof-gate.

---

## DEPTH_ALGORITHM

Files: `cl_dll/cszrender/lighting/csz_volumetric_shaders.inl` (fragment shader),
`cl_dll/cszrender/lighting/csz_volumetric.cpp` (pass/state/uniforms).

**Why a shader-side clamp was even a question:** this renderer has **no sampleable
scene-depth texture** anywhere (confirmed whole-tree; the only depth texture is the
shadow map). The main scene renders to the default framebuffer, and only `matViewProj`
(not `matView/matProj`, near/far) was previously uploaded to this shader. So two
mechanisms are combined:

1. **Hardware depth occlusion (all tiers).** Cone is drawn as a bounding hull,
   **back faces only** (`SetCull(true)+SetCullFront(true)` → `glCullFace(GL_FRONT)`,
   verified `csz_glstate.cpp:238-245`), **depth-test ON `GL_LEQUAL`, depth-write OFF**
   (`csz_volumetric.cpp:~476-478`). Back faces (not front) so the hull stays valid when
   the **camera is inside the cone**. The opaque scene depth from slots 11/12 is still in
   the buffer (not re-cleared before slot 13.5), so any cone fragment behind a wall is
   z-rejected by hardware — the decisive wall-between-camera-and-cone occlusion, free.

2. **Analytic ray-vs-cone entry/exit (all tiers).** In the FS, for ray `ro=u_camPos`,
   `rd=normalize(v_worldPos-ro)`: solve the closed-form infinite-cone quadratic
   (apex `u_lightOrigin`, axis `u_lightDir`, `k=cos²(half-angle)` from `u_cosOuter`),
   take roots → `[tEntry,tExit]`, then **clip to the axial slab `[0, u_radius]`** (solve
   the axial coordinate `s(t)` against `s=0` and `s=u_radius`, with a constant-axial
   perpendicular-ray branch and a `|A|<eps` grazing branch). `tEntry=max(tEntry,0)`
   (camera inside ⇒ integrate from camera); `tExit=min(tExit, length(v_worldPos-ro))`
   (never integrate past the rasterized hull). Inscatter is integrated **only over
   `[tEntry,tExit]`** with a *bounded* fixed step count `u_steps` (tier-controlled,
   clamped `[4,96]`) — no unbounded/`while` loops. This both fixes correctness and
   *shrinks* fill/integration vs. the old whole-ray march. (`csz_volumetric_shaders.inl`
   FS, ~lines 89-148.)

3. **True `min(exit, sceneDepth)` clamp (MED/HIGH only; default MED).** A
   `GL_DEPTH_COMPONENT24` texture is created **once** (resized only on viewport change,
   guarded in `EnsureSceneDepthCopy`, `csz_volumetric.cpp:~308-356` — **no FBO, no extra
   scene pass**). When `csz_volumetric_quality >= 2`, one `glCopyTexSubImage2D` copies the
   default-framebuffer depth into it at the top of `Render` (before any cone draw,
   `~:423-430`). In the FS the depth is sampled at `gl_FragCoord.xy/u_screenSize`,
   linearized with the newly-uploaded `u_zNear`/`u_zFar`
   (`2·zN·zF/(zF+zN−(2d−1)(zF−zN))`), converted to a ray parameter via a per-fragment
   ratio calibrated by the same fragment's own depth, and `tExit = min(tExit, tScene)`;
   integration runs only if `tExit>tEntry`. This clamps the cases hardware depth-test
   alone cannot (a wall *partially inside* the cone, or the spot landing on a far wall).
   It **degrades gracefully**: on any depth-texture creation/bind failure it latches
   `depthFailed`, sets `u_hasSceneDepth=0`, logs once, and falls back to mechanisms 1+2
   (which still pass the decisive wall-between test). OFF/LOW never touch the depth copy.

**How wall bleed is prevented, summarized:** wall *between camera and the whole cone* →
hardware depth-test rejects all cone fragments (every tier). Wall *inside / at the far
end of the cone* → `min(exit, sceneDepth)` clamp stops integration at the wall (MED/HIGH).

---

## PERFORMANCE_EXPECTATION

- **Bounded integration:** fixed `u_steps` over `[tEntry,tExit]` only; step count
  clamped `[4,96]`; no unbounded loops. Integrating only the in-cone chord (not the whole
  camera→hull ray) and drawing **back faces only** reduces wasted fill/integration vs. the
  rejected depth-test-off full-ray shaft — the depth fix and the perf fix point the same
  way.
- **Per-tier cost (`csz_volumetric_quality`):**
  - **OFF (0):** early-return, no shaft drawn, no depth copy → guaranteed 200-FPS floor
    (zero shaft cost). (`csz_volumetric.cpp:~393`)
  - **LOW (1):** 12 steps, HW depth-test + analytic FS only, **no depth copy** → cheapest
    shaft; designed to hold ≥200 with headroom.
  - **MED (2, default):** 24 steps + one full-screen `glCopyTexSubImage2D`/frame.
  - **HIGH (3):** 48 steps + depth copy + shadow-carve.
- **No per-frame heap allocation** (cone verts stay in the fixed
  `ConeVertex verts[kConeSegments*6]` array). **No per-frame program/FBO/texture
  creation** — the depth texture is created/resized only (once-init guard); the per-frame
  `glBufferData` orphan upload is the pre-existing, allowed one. No FBO is created at all.
- **200-FPS tier plan:** OFF and LOW are the perf-floor tiers (no depth copy) and must
  hold ≥200 with genuine headroom in-world; MED/HIGH carry the extra full-screen depth
  copy. To be **proven** in the locked capture phase (OWED), uncapped, with `kTmVolume`.

---

## DLL_STAGE_SHA_PLAN

- **Single source of truth (resolved):** `D:\csoz-wt\blackfog\build\cl_dll\Release\client.dll`
  — the canonical output of `cmake --build "D:/csoz-wt/blackfog/build" --config Release
  --target client` (confirmed via `build\CMakeCache.txt` `client_BINARY_DIR=build/cl_dll`
  and `client.vcxproj` `OutDir`). The tree **`D:\csoz-wt\blackfog\build\client\` is a
  SEPARATE STALE CMake tree and must be ignored/deleted** — it produced one of the
  conflicting DLLs and the stale `R2_EVIDENCE` copy.
- **All three previously-documented SHAs are stale** and are retired:
  `FF5EE7DB…` (683,520 B, old report) and `9E035FE9…` (683,008 B, old README) match
  nothing on disk; `f7fc7ad2…` matches only the **stale** `build\client` tree + the old
  `R2_EVIDENCE\client.dll` copy. The current canonical artifact is a **4th** hash (below).
- **Deploy target:** `D:\csoz-run-r2-blackfog\client\cstrike\cl_dlls\client.dll`
  (engine = `D:\csoz-run-r2-blackfog\client\xash3d.exe`).
- **SHA check logic (in `_scratch/capture_r2_v6.ps1`, `Stage-VerifyDll`)** — no hardcoded
  hash literal (that is exactly what went stale); the **path** is pinned and copy
  integrity is verified source-vs-deployed:
  ```
  if (-not (Test-Path $SrcDll)) { Write-Error ...; exit 1 }
  $srcHash = (Get-FileHash -Algorithm SHA256 $SrcDll).Hash
  Copy-Item $SrcDll -Destination $DstDll -Force
  $dstHash = (Get-FileHash -Algorithm SHA256 $DstDll).Hash
  if ($srcHash -ne $dstHash) { Write-Error "...ABORTING, no capture."; exit 2 }
  ```
  Both hashes + byte sizes are echoed and the verified SHA is written to
  `R2_EVIDENCE\client.deployed.sha256.txt` (timestamp + src + dst + size) so every shot
  in a batch is traceable. The gate runs and can abort **before** any capture.

---

## FPS_MEASUREMENT_PLAN

- **Uncapped-enough setting:** the old `fps_max 200` pin is removed. FPS-measurement mode
  (`-FpsMode`) sets **`fps_max 1000`** (far above 200 so true headroom is observable).
  (`fps_max 0`=uncapped could not be confirmed from this repo — it is an engine cvar — so
  1000 is used rather than guessing 0.)
- **Tier matrix the script drives** (`csz_volumetric_quality` clamped 0..3,
  `csz_volumetric.cpp:316-329`; master `csz_flashlight_volumetric 0|1` at `:310`):
  OFF(0) / LOW(1, 12 steps) / MED(2, 24) / HIGH(3, 48 + `csz_light_shadow 1` carve).
  **Acceptance:** OFF and LOW ≥200 with genuine headroom in an active in-world scene.
- **`kTmVolume` is queryable — no extra GPU query needed.** It is the volumetric-pass CPU
  timer (`csz_renderer.cpp:78`, bracketed by `BeginPass/EndPass(kTmVolume)` at `:355-357`
  using the engine QPC clock). With `developer 1`, `SampleFps` (`csz_renderer.cpp:102-160`)
  emits once/sec to `engine.log`:
  `[fps] fps=… avg_ms=… worst_ms=… frames=…` and
  `[fps] pass-ms avg: … volume=X.XX …` (the `volume=` field is the kTmVolume per-frame
  avg ms). The script greps both from the copied log. **Caveat (from source `:70-75`):**
  this is CPU-side only; GPU work is async, so suspicious numbers must be cross-checked
  against the ON/OFF A/B — which the per-tier matrix + `ab_on`/`ab_off` scenarios provide.

---

## CAMERA_METHOD

- **`setpos`/`setang` are UNSUPPORTED in this build** (logged "Unknown command" in all 7
  prior runs + an independent probe). **Not used.**
- **Supported method = `csz_debugcam`** (registered `FCVAR_CLIENTDLL` cvars in
  `core/csz_view.cpp:176-190`, registered via `RegisterViewDevCvars()` `csz_renderer.cpp:201`,
  so they are `set`-able from cfg and will not log "Unknown command"). Applied at the top
  of `BuildViewFromPass` (`csz_view.cpp:202-216`) before matrices/frustum/PVS, so the
  whole render follows it. Deterministic placement (explicit mode):
  - `csz_debugcam_pos "x y z"` (`:181`, parsed by `sscanf "%f %f %f"` `:88`; bad input
    never overrides)
  - `csz_debugcam_ang "pitch yaw roll"` (`:183`)
  - `csz_debugcam 1` (`:179`; mode 2 = auto-orbit the `csz_testlight` cone side-on,
    framing via `csz_debugcam_dist/_side/_height` `:185-189`)
- **Runtime proof gate:** the autoexec echoes `CSZCAM_BEGIN` before and `CSZCAM_END`
  after the camera commands; the script copies `engine.log` out and `Inspect-EngineLog`
  scans the lines strictly between the markers for "Unknown command" — WARN/abort the
  shot as INVALID if found, else "camera proof gate PASSED." This is exactly the check
  that would have caught the silent setpos/setang failure in all 7 prior runs.
- **Proof plan:** first locked-phase action is a one-shot run that issues the
  `csz_debugcam` commands and confirms the proof gate PASSES (no "Unknown command"
  between markers) before any A/B shot is trusted. `noclip` (proven working) remains the
  movement primitive for pinning the placeholder coordinates to real de_dust2 landmarks.

---

## BUILD

- **Command:** `cmake --build "D:/csoz-wt/blackfog/build" --config Release --target client`
- **Exit code:** `0` (clean compile + link)
- **Artifact:** `D:\csoz-wt\blackfog\build\cl_dll\Release\client.dll`
- **Size:** `691,200` bytes
- **SHA256:** `CE0AD3AFABEBA35EB5489590FF484A4E6CA215F3EACCE4FBE1C17B421F28A3E2`
- **Verified independently** by the main window (`Get-FileHash`, mtime 2026-06-18T01:51,
  post-build) — matches the build subagent's report.

---

## FILES_CHANGED

Source (the V6 deliverable):
- `cl_dll/cszrender/lighting/csz_volumetric_shaders.inl` — FS rewritten: analytic
  ray-vs-cone entry/exit, `[entry,exit]` bounded integration, scene-depth clamp, new
  uniforms; header + "HONEST LIMITATION" comment blocks rewritten to match real depth
  behavior. (untracked → committed)
- `cl_dll/cszrender/lighting/csz_volumetric.cpp` — depth-test ON + back-face hull;
  `EnsureSceneDepthCopy()` once-init depth texture + per-frame copy at quality≥2; new
  uniform uploads (`u_hasSceneDepth`, `u_sceneDepth` unit 3, `u_screenSize`, `u_zNear`,
  `u_zFar`); cleanup restores cull/face/depth; state comments corrected. (untracked → committed)
- `cl_dll/cszrender/lighting/csz_volumetric.h` — pass declarations. (untracked → committed)
- `cl_dll/cszrender/core/csz_glfuncs.h` — added `glCopyTexSubImage2D` and `glUniform2f`
  to the GL function X-macro table (only two entry points not previously loaded). (modified)
- `cl_dll/cszrender/flashlight/csz_flashlight.h` — flashlight header. (untracked → committed)
- `cl_dll/cszrender/fog/csz_fog.cpp`, `cl_dll/cszrender/fog/csz_fog.h` — pre-existing
  black-fog WIP on this branch (global per-pixel exp2 fog; already depth-correct, kept). (modified)
- `cl_dll/cszrender/CMakeLists.txt` — source list (flashlight + volumetric units). (modified)

New non-code deliverables:
- `_scratch/capture_r2_v6.ps1` — rewritten capture chain (stage+SHA-verify+abort, uncapped
  FPS mode, `csz_debugcam` + proof gate, per-tier + wall-bleed scenarios). Old
  `_scratch/capture_r2.ps1` left in place for reference.
- `R2_DEPTH_READY.md` — this report.

Superseded / stale (left untracked, NOT committed, documented as invalid): `R2_OFFLINE_READY.md`
and `READY_FOR_CAPTURE.md` (V4 docs, stale SHAs), `R2_EVIDENCE/` (7 default-spawn shots
taken with the unsupported camera + stale-tree DLL — invalid by provenance).

---

## COMMIT_OR_PROVENANCE

Committed on branch `worker/r1-blackfog-flashlight` (see the commit immediately following
this report's creation). The commit contains the source deliverable + the new capture
script + this report. The stale V4 docs and the invalid `R2_EVIDENCE/` shots are
deliberately left untracked. `csz_flashlight.cpp` is already tracked (committed in
`58f60da`), which is why it is not in the diff and why the link succeeds. The stale
`build\client\` CMake tree should be deleted to prevent future SHA confusion.

---

## NEXT_CAPTURE_PLAN (later locked phase — OWED, engine required)

Run `_scratch/capture_r2_v6.ps1` under the engine lock. Every run is gated on
source==deployed DLL SHA and on the camera proof gate. Pin the placeholder camera
coordinates to real de_dust2 landmarks (via `noclip`) so an opaque wall sits between
camera and the lit cone.

1. **Decisive wall-bleed shot** (`bf_v6_wall_bleed_on`, HIGH+shadow): wall between camera
   and lit cone — expectation: **no shaft bleed through the wall, wall face not
   brightened**. This is the gate the V4 shaft failed.
2. **A/B** (`bf_v6_ab_on` MED vs `bf_v6_ab_off` quality 0 + master 0), same camera — a
   visible, non-blown-out shaft ON, absent OFF.
3. **Per-tier FPS** (`bf_v6_tier0_off/1_low/2_med/3_high`) with `-FpsMode` (`fps_max 1000`,
   ~30 s hold so several `[fps]` + `volume=` lines land): report avg / 1%-low / 0 GL
   errors / ≥55 samples, in-world, with OFF and LOW ≥200 + headroom, plus the `kTmVolume`
   `volume=` ms per tier.
4. **Beauty/orbit** (`bf_v6_orbit_high`, `csz_debugcam 2` around the `csz_testlight` cone).

Record results in `R2_RESULT.md` with the images + FPS/kTmVolume table.
