# R2 CODE READY — Worker B (weather) — V4A

- Timestamp: 2026-06-18 01:30 -0600
- Branch: `worker/r1-rain-water-snow`
- Worktree: `D:\csoz-wt\weather`
- Phase: V4A = code-quality + build-readiness only. **Engine NOT launched** (capture is a separate locked phase).

---

## RESULT: CODE_READY

All required fixes are landed and the worktree compiles clean (exit 0, fresh artifact). The water 113-FPS root cause (324 per-face draw calls) is fixed by draw-call batching — not hidden behind fake FPS. The one item that cannot be proven in this phase (that batching + new vertex stride actually clear 200 FPS at runtime) is explicitly deferred to the locked capture phase with a concrete plan below. No fabricated verification.

---

## FILES_CHANGED

Bulk of the architecture + snow/rain work was already landed in prior R2 commits on this branch (`6cd1eba` "geom→core weather POD", `aeeb5d2` "R2 iter1 visual fixes"); the static-review line numbers were stale (pre-fix). This phase verified those against live source and added the genuinely-missing work:

| Commit | Files | Change |
|--------|-------|--------|
| `c799e20` | `cl_dll/cszrender/geom/csz_water.cpp`, `geom/csz_water.h`, `geom/csz_water_shaders.inl` | **113-FPS draw-call batching** (324→1), `a_faceCenter` per-vertex attr, ring-center fix, FBO-free reflection warp + depth-scaled refraction, TMU0 restore |
| `a266311` | `cl_dll/cszrender/geom/csz_world_shaders.inl` | Snow coverage cap 0.97→0.9 (not full-white), cheap wet grazing rim (`fres*fres`, no new uniform) |
| `fea6ea9` | `cl_dll/cszrender/weather/csz_weather.cpp` | Removed residual `===B2===` scaffold marker |
| `6cd1eba`, `aeeb5d2` (prior) | `core/csz_weather_types.h`, `core/csz_view.h`, `geom/csz_world.cpp`, `csz_renderer.cpp`, `weather/csz_weather.{cpp,h}`, `weather/csz_weather_shaders.inl`, `geom/csz_world_shaders.inl` | W1 POD→core seam, W2 snow near>far, W3 snow double-dark, world-space rain |

---

## ARCHITECTURE_FIX (W1 — geom→weather contract)

- **Removed:** `geom/csz_world.cpp` no longer includes `weather/csz_weather.h`. Verified by grep — `geom/` includes NO downstream (`weather/`, `lighting/`, `fog/`) header.
- **POD relocated:** `WeatherSurfaceState` now lives in `core/csz_weather_types.h` (mirrors `core/csz_ambience_types.h`).
- **Routing:** the POD rides the existing view/composition boundary — `core/csz_view.h:54` carries `WeatherSurfaceState weather;`; composition root `csz_renderer.cpp:314` fills it via `view.weather = g_weather.SurfaceState();`. `geom/csz_world.cpp` reads `view.weather` at its 3 shader-feed sites (904, 1100, 1221), identical to how `view.ambience` flows. Producer side (`weather/csz_weather.{cpp,h}`) includes the core POD header.
- Result: layer order `core→geom→lighting→fog→weather` is one-directional again; the CI grep gate passes.

---

## WATER_113FPS_FIX

- **Root cause (confirmed):** `DrawWater` issued one `glDrawArrays(GL_TRIANGLE_FAN)` **per turbulent water face**. de_aztec has `turb_count: 324` → **324 draw calls/frame** → CPU/driver draw-call-bound → `fps_avg: 113.1`. Cost was per-draw overhead, not fill/vertex work (water is a small screen fraction, few-thousand verts total).
- **Fix:** at `EnsureBuilt` (once per map, `GL_STATIC_DRAW`), all turb faces are fan-triangulated into a single triangle-list VBO, sorted by `gl_texturenum` into contiguous `WaterBatch{firstVert,vertCount,texSlot}` runs. `DrawWater` binds the VAO once and issues **one `glDrawArrays(GL_TRIANGLES)` per distinct texture slot**. de_aztec's turb faces share one liquid texture → **324 draws collapse to 1**. Worst case ≤ #distinct-water-textures, still far below 324.
- **Per-face data preserved without per-draw uniforms:** the only per-face datum the shader needed (rain-ring center) is baked into the vertex buffer as `a_faceCenter` (vec2, location 3); stride 8→10 floats / 40 bytes. So a single batched draw needs no per-face uniform.
- **No fake FPS.** No quality-tier cvar was needed — batching alone is the fix. (A `csz_water_quality` term-drop tier remains an easy future lever if capture shows residual shader cost.)
- TMU0 leak at old `csz_water.cpp:267` fixed with a symmetric unbind.

---

## RAIN_SNOW_WET_FIXES

**Snow:**
- Near>far readability (W2): snow modulated by `csz_lmLum` + distance term `csz_snowDist = clamp(1 - depth/1600, 0.60, 1.0)` (far snow recedes), `csz_snowLit = clamp((0.80 + 0.30*csz_lmLum)*csz_snowDist, 0, 1)`. Up-facing mask `smoothstep(0.35,0.85,csz_up)` preserved.
- Night double-darkening (W3): snow color is now `base*tint` **once** (cool tint `{0.82,0.88,0.98}`, B>G>R), no second `*dim` multiply — night snow legible, not luminance².
- Coverage cap restored to **0.9** (was drifting to 0.97 / near-white).
- Blue-blob particles: snow FS uses a radial `smoothstep` soft-flake falloff (no flat tinted quad).

**Rain / wet:**
- Rain is **world-space**, not UI lines: `a_pos` = world billboard corner × `u_viewProj`, depth-layered (near streaks fall faster/longer/thicker), wind-slanted (~13–19% of length in +X), camera-following box, fog-darkened. FS = soft vertical gradient + horizontal feather (no hard rectangular edge). Depth-tested (occluded by walls), depth-write off, GL state restored.
- Wet ground: existing Lagarde-style branch (up-facing mask, darken `mix(1.0,0.55,wet)`, lit-area tight specular) preserved; added a cheap tight **grazing rim** `col += vec3(0.14,0.18,0.26) * fres*fres * wet` (reuses already-computed Fresnel — no new `pow`, no new uniform) so damp ground glints at glancing angles.

**Water algorithm (W4):**
- Ring term: radius + outward direction both measured from the per-face centroid (`v_faceCenter`), ~24u wavelength, `exp(-r*0.012)` fade → genuine concentric rings (old grid-cell `fract` read as fine chop).
- Reflection: FBO-free — a finer ripple octave jitters the reflected ray so the analytic sky/horizon line breaks into shifting bands that warp with ripples.
- Refraction: body UV warp + tint scale with view depth (`dot(N,V)`) — grazing views refract further, deepen toward teal looking straight down. Existing animated normals + Schlick Fresnel + moonlight glint kept.

**Scaffold cleanup (W5):** residual `===B2===` marker removed; other flagged "stub" comments were already rewritten into accurate descriptions in prior commits (grep clean for `stub|scaffold|goes here`).

---

## BUILD

- **Command:** `"C:/Program Files/CMake/bin/cmake.exe" --build "D:/csoz-wt/weather/build/client" --config Release --target client`
  (pre-configured worktree CMake cache; Generator VS 17 2022, Platform Win32, multi-config Release)
- **Exit code:** `0`
- **Artifact path:** `D:\csoz-wt\weather\build\client\cl_dll\Release\client.dll`
- **Size:** `701440` bytes
- **SHA256:** `3D67F52D7C903E45C8CF04F1406D91B2D0EDAEFC880C44C1E8E605AD03E45B0A`
- **Freshness:** LastWriteTime `2026-06-18T01:30:24`, built seconds before report; prior stale DLL (00:49, 698368 B) overwritten; new size differs → new sources (water stride 8→10, `a_faceCenter`) compiled in.
- **Worktree-vs-submodule trap cleared:** `CMakeCache.txt` resolves `cs16-client_SOURCE_DIR=D:/csoz-wt/weather`, `client_SOURCE_DIR=D:/csoz-wt/weather/cl_dll`; `cszrender.lib` compiled from worktree `cl_dll/cszrender/...`. `build-client.ps1` (main-submodule builder) NOT used.

---

## PERFORMANCE_RISK

Build-time green does NOT prove runtime correctness or FPS. Heavy/risky paths the capture phase must prove:

1. **Water draw batching + new vertex layout (HIGHEST risk).** Stride 8→10 floats and `a_faceCenter` at location 3 — a `glVertexAttribPointer` stride/offset vs shader `layout(location=3)` mismatch is a *runtime* GL bug invisible to the compiler. Capture must confirm: (a) water renders correctly (no garbled/missing surface, rings centered per face), (b) `glGetError()` == 0, (c) `fps_avg ≥ 199` on de_aztec where it was 113.1. The expectation: 324→1 draw calls removes the CPU draw-call wall; same triangles/shader so GPU work unchanged → should clear 200 FPS.
2. **High-tier rain particle simulate.** ≤7000 particles simulated CPU-side each frame is the module's largest CPU cost (bounded, pre-allocated pool, but real). Capture the high rain tier and watch frame time.
3. **Water shader added octave + depth-scaled refraction.** Slightly more ALU per water fragment; water is small screen area so low risk, but verify it doesn't regress on a water-heavy view.
4. **Worst-case water draw count.** If a pathological map has many distinct water textures, batching degrades toward #textures draws. de_aztec = 1; flag any map with high turb + many liquid textures.

---

## NEXT_CAPTURE_PLAN (locked capture phase)

Exact screenshots + cvars. All on a 5070 Ti, `fps_max 0`, vsync off, fixed resolution, ≥55 FPS samples, `glGetError`==0 required. Gate: `avg≥199`, `1%low≥195`.

1. **`water_on_de_aztec`** (the regression target) — map `de_aztec`, weather water ON, night/moonlit. **Primary gate: must show `fps_avg ≥ 199` (was 113.1) and `turb_count` unchanged (~324) to prove batching worked.** Verify water surface renders, rings concentric, moonlight glint + warped reflection visible.
   - cvars: weather/water enabled, `fps_max 0`. Capture `glGetError` count = 0.
2. **`water_off_de_aztec`** (baseline) — same view, weather OFF. Confirms byte-level no-op and isolates water cost.
3. **`rain_high`** — a rain map, highest rain tier, moving camera with tilt. Prove: world-space slanted streaks (not UI lines), wall occlusion, FPS at high tier ≥199.
4. **`wet_ground`** — wet surface at a grazing camera angle in a lit area. Prove: damp darkening + grazing rim glint + lit-area specular.
5. **`snow_cover_night`** — up-facing snow surfaces, near + far in frame, night. Prove: near>far brightness gradient, legible (not double-dark), cool tint, cap not full-white, no blue-blob flakes.
6. **`snow_fall`** — falling snow particles. Prove: soft radial flakes, not blue blobs.

Each capture writes `{name, map, fps_avg, fps_samples, glerror_count, turb_count, png, log}` to `R2_EVIDENCE/`. A separate **visual review subagent** (distinct from this code work) Reads each PNG against intent + reference images per visual rubric. Capture runs under a lock; engine launched there, not here.
