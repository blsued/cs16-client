# M2C Studio Render-Completeness — INTEGRATION SPEC (worker/m2c-studio)

Worktree: `D:/csoz-wt/C`  ·  Branch: `worker/m2c-studio`  ·  Base: `feature/m2c-render-completeness` (ca883ca)
Build verified: `client.dll` Release Win32 OK (no new warnings; only pre-existing `pm_shared` C4244).

This worker OWNS the `csz_studio*` files. The renderer composition root
(`csz_renderer.cpp/.h`), cvar registration call-site, and transparent-domain
pass ordering are INTEGRATOR-owned seams. Below is everything the integrator
must review / adopt. All `csz_renderer.cpp` edits I made are marked
`INTEGRATION (worker/m2c-studio ...)` in-line and are also listed here.

---

## 1. New cvars (all default 1 = feature ON; all FCVAR_CLIENTDLL)

Registered inside `csz::StudioRegisterCvars()` (in `csz_studio.cpp`, which I own;
already called from `Renderer::OnHudInit()` — **no integrator change needed**):

| cvar | feature | read by |
|------|---------|---------|
| `csz_attach` | MUST 1 attachments | `csz_studio_bones.cpp` via `pfnGetCvarPointer` |
| `csz_studio_rendermode` | MUST 2 transparent rendermodes | `csz_studio.cpp` |
| `csz_renderfx` | MUST 3 render-fx + glowshell | `csz_studio.cpp` |
| `csz_chrome` | SHOULD 4 per-bone chrome | `csz_studio.cpp` (0 = legacy global) |
| `csz_bonelerp` | SHOULD 5 bone controllers | `csz_studio_bones.cpp` via `pfnGetCvarPointer` |
| `csz_blobshadow` | SHOULD 6 blob shadow | `csz_studio.cpp` |

All read sites FAIL-SAFE ON if the pointer is NULL.

## 2. New StudioRenderer pass functions + where they slot (csz_renderer.cpp)

Order inside the existing pass tree:

- **slot 12.5 (kTmStudio, after `DrawOpaque`)** — `g_studio.DrawGlowShells(view, m_frame.studio, m_frame.numStudio)`
  MUST 3 additive glowshell extrude pass. Needs opaque world+studio depth already written
  (occludes shells correctly); depth-write off, additive blend.
- **slot 13.9 (kTmTrans, first)** — `g_studio.DrawBlobShadows(view, m_frame.studio, m_frame.numStudio)`
  SHOULD 6. Darkens the opaque floor; alpha blend, depth-test on, depth-write off, polygon offset.
- **slot 14.0 (kTmTrans, before DrawSprites)** — `g_studio.DrawTransparent(view, m_frame.studio, m_frame.numStudio)`
  MUST 2 non-opaque studio entities, back-to-front sorted, per-rendermode blend state.

`DrawOpaque` / `DrawDepth` / `DrawLitAdditive` now SKIP `IsTransparentEntity()` entities
(rendermode != kRenderNormal) so transparent models are drawn ONLY by `DrawTransparent`
(no double-draw, no solid shadow, no opaque-style flashlight). Glowshell entities are
normally `kRenderNormal` so they stay in the opaque/depth/lit passes — correct.

These three calls are the ONLY edits to `csz_renderer.cpp` (2 hunks). No `csz_renderer.h`
change. No `FrameEntities` change — all three pass functions reuse the existing
`m_frame.studio` list.

## 3. ent->attachment[] usage (MUST 1)

`StudioCalcAttachments` runs at the end of `BuildWorldBones` (once per entity per frame via
the bone cache) and writes WORLD positions into `ent->attachment[0..3]`. This is the ONE
sanctioned mutation of `cl_entity_t`. The viewmodel path (`DrawSingle`) also computes its
attachments via the same `SetupBones` → `BuildWorldBones`.

**Follow-up for the viewmodel owner (not done here — different concern/file):**
`csz_viewmodel.cpp:199-202` currently DEFERS muzzle-flash/spark events (5001/5002/5011/
5021/5031) with the note "re-enable when a StudioCalcAttachments-equivalent lands." That
equivalent now lands. The defer can be removed; events fire ~1 frame before `DrawSingle`
recomputes attachments, so they read the prior frame's `ent->attachment[]` — standard
GoldSrc behavior, visually fine. Recommend the integrator/viewmodel owner remove that gate
and smoke-test muzzle flashes.

## 4. Shader uniform additions (base + lit studio programs)

- `u_studioRender` (int, default 0), `u_renderAmt` (float, default 1), `u_renderColor` (vec3) —
  MUST 2. Mode 0 = opaque (`fragColor = vec4(col,1)` byte-identical to pre-M2c). Modes 1/2/3
  selected per transparent entity. `BeginStudioPass` re-pins mode 0, so the opaque pass never
  inherits a stale transparent mode.
- `u_chromeMode` (int) + `u_camPos` in the VS — SHOULD 4. 0 = legacy global view-basis sphere
  map (byte-identical A/B), 1 = per-bone basis (camera→bone). Default ON.
- New programs: `csz_studio_shell` (glowshell), `csz_studio_blob` (blob shadow).

## 5. Honest cuts / deferrals

- **SHOULD 5 cross-sequence transition lerp + latched-state interpolation: DEFERRED.**
  Needs per-entity latched (prevsequence, prevframe, transition start time) state similar to
  the gait slots. Bone CONTROLLERS + mouth ARE implemented (the higher-value half). Sequence
  changes still snap (same as pre-M2c).
- **SHOULD 5 multi-seqgroup (seqgroup != 0): DEFERRED** (was the prior G-S1 cut). Still falls
  back to bind pose with the existing throttled warn.
- **MUST 3 renderfx Fade*/Solid* families: APPROXIMATED** (renderamt passthrough). True
  monotonic ramps need a per-entity latched spawn time the takeover does not track. Pulse /
  Strobe / Flicker are fully time-driven and correct.
- **MUST 3 niche renderfx (Distort/Hologram/Explode/Glasshull/DeadPlayer): not implemented**
  (explicitly out of scope per the task).
- **Glowshell** is a flat additive color shell (not the scrolling-chrome stock look) — the
  common powerup/zombie appearance; distance-capped at 2000u, fixed extrude 2.0u, alpha 0.5.

## 6. Observability ([CSZ:studio] Dev logs, 1s throttle) + kTmStudio timing

- opaque: `drawn N / M studio entities; attach K`  (K = MUST 1 attach count)
- transparent: `transparent N / M studio entities`
- glowshell: `glowshell N studio entities`
- blobshadow: `blobshadow N studio entities`

kTmStudio wraps `DrawOpaque` + `DrawGlowShells`; `DrawTransparent` + `DrawBlobShadows`
ride kTmTrans (the existing transparent-domain timer).

## 7. Not yet visually verified

Code-built + self-reviewed only (DIFFS_READY_UNVERIFIED). No engine smoke / frame capture was
run by this worker. Recommend an in-engine pass: a glowshell monster (zombie), a transparent
(kRenderTransAdd/Alpha) studio prop, blob shadows on a flat + sloped floor, and an A/B of
`csz_chrome 0/1` on a chrome model.
