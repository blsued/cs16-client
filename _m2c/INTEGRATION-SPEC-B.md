# INTEGRATION SPEC — Worker B: World-surface dynamics

Branch `worker/m2c-B` (worktree of cs16-client submodule). Win32 Release build is
**green** (`cszrender.lib` + `client.dll` link clean). Status: **DIFFS_READY_UNVERIFIED**
— compiles & links; runtime shader compile + visual/手感 verification is OWED to a
TEST agent / main window (I cannot launch the engine).

## Files changed (all worker-owned)
- `core/csz_engine_bsp.h` — `+kSurfConveyor = (1<<6)` (SURF_CONVEYOR / flowing flag).
- `geom/csz_lightmap.h` / `.cpp` — `+UploadBlockLit()` (gamma-only block upload; the
  animated path pre-accumulates style*value>>8 itself, so it must NOT re-apply the
  static path's ×264). Refactored shared gamma into `GammaByte()`.
- `geom/csz_world_shaders.inl` — base **VS**: `uniform vec2 u_scroll; v_uv = a_uv + u_scroll;`
  base **FS**: detail sampler `u_texDetail` (unit 3) + `u_hasDetail` + `u_detailScale`,
  modulating albedo ×2. Lit/depth programs untouched (base pass only).
- `geom/csz_world.cpp` — FaceRec fields (`globalSurf, animTex, flowing, animLm,
  detailSlot, detailScale*`); animated-lightmap ref list; the dynamics helpers
  (`AnimateLightStyles`, `UpdateAnimatedLightmaps`, `BuildAndUploadAnimFace`,
  `RestoreFaceBaseLightmap`, `TextureAnimationSlot`, `ScrollOffset`,
  `LookupDynamicsCvars`); per-frame wiring inside `DrawOpaque` + the texanim/scroll/
  detail feeds in the world and brush draw loops; observability log.

## What the integrator MUST do: **nothing is strictly required.**
The seam is self-contained on purpose:
- **Cvars self-register lazily** (`LookupDynamicsCvars`, first `DrawOpaque`), exactly
  like the existing `FeedTpFogGlow` (`csz_tpfog*`). Names/defaults:
  `csz_lightstyle 1`, `csz_texanim 1`, `csz_scroll 1`, `csz_detail 1` (all `FCVAR_CLIENTDLL`).
- **Per-frame hook is folded into `WorldRenderer::DrawOpaque`** (frame slot 11, called
  every takeover frame). It runs `UpdateAnimatedLightmaps()` before drawing and applies
  texanim/scroll/detail in the draw loop. Brush passes (`DrawBrushOpaque/Transparent`)
  inherit texanim/scroll/detail through the shared base program automatically.

### OPTIONAL (if you prefer central cvar ownership at the composition root)
Register the four cvars (same names/defaults) anywhere before the first frame.
`pfnRegisterVariable` is idempotent — the lazy lookup will just receive the existing
pointers. No call-site changes needed. There is **no** new init function to call and
**no** new per-frame function to call from `csz_renderer.cpp`.

## Observability (for the TEST agent)
Set `developer 1`. Each second `DrawOpaque` emits:
```
[CSZ:world] DEV: dynamics lightstyle=<N animated-lm faces> texanim=<faces drawn> flowing=<faces drawn> detail=<faces drawn>
```
- `lightstyle>0` ⇒ the map has switchable/animated lightstyles under management (toggle
  `csz_lightstyle 0/1` to A/B the flicker; off restores the static bake).
- `texanim>0` ⇒ animated `+`-textures are being frame-resolved this frame.
- `flowing>0` ⇒ SURF_CONVEYOR faces are scrolling.
- `detail>0` ⇒ detail-texture overlay active (usually 0; HL detail textures are opt-in).

## Assumptions worth a sanity check during verification
1. **SURF_CONVEYOR = BIT(6)=0x40.** Consistent with the existing FWGS surface-flag
   layout already mirrored (`kSurfDrawSky`=2, `kSurfDrawTurb`=4). If a conveyor map
   shows `flowing=0`, this bit is the one thing to re-confirm against the pinned engine
   (`docs/engine-pin.md`); it is a 1-line fix in `csz_engine_bsp.h`.
2. **Lightstyle scale 'm'=264** (matches the existing static style-0 bake). Sampled from
   `GetLightStyle(n)->pattern` at 10 Hz — no dependence on the engine's internal `.value`.
3. **Conveyor speed** is a fixed `kScrollSpeed = 0.5` tex-widths/sec (world surfaces carry
   no per-entity speed). Tune in `csz_world.cpp` if a faster belt is wanted.

## Known minor gaps (deliberate, in scope notes)
- The additive **lit pass** (flashlight) samples diffuse un-scrolled / no detail (separate
  program, base-pass-only contract). A conveyor under the flashlight beam scrolls in the
  base layer but not the additive light layer — edge case, left for parity simplicity.
- Detail/lightstyle are not applied in the depth (shadow) pass — correct (geometry only).
