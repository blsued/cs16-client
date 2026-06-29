# INTEGRATION SPEC — m2c C-EFXB (beam + particle/tracer + efx shim)

Worker branch: `worker/m2c-efxb` (off `feature/m2c-render-completeness`).
Worker OWNS (new files, no integrator action beyond the build list):
`geom/csz_beam.{h,cpp}` + `csz_beam_shaders.inl`,
`geom/csz_particle.{h,cpp}` + `csz_particle_shaders.inl`,
`geom/csz_efx_shim.{h,cpp}`.

Everything below touches **integrator-owned shared seams**. Each edit in the
worktree is tagged `[INTEGRATION SPEC: m2c-efxb]` so it is greppable. The worktree
wiring is functional (client.dll builds + links clean, VS2022 Win32 Release); the
integrator may keep it as-is or re-apply onto the canonical composition root.

## 1. CMakeLists.txt
Add three sources to `CSZRENDER_SRC` (after `geom/csz_sprite.cpp`):
```
geom/csz_beam.cpp
geom/csz_particle.cpp
geom/csz_efx_shim.cpp
```

## 2. csz_renderer.cpp — includes
After `#include "geom/csz_sprite.h"`:
```
#include "geom/csz_beam.h"
#include "geom/csz_particle.h"
#include "geom/csz_efx_shim.h"
```

## 3. csz_renderer.cpp — `OnHudInit()` (cvar registration)
After `DustRegisterCvars();`:
```
BeamRegisterCvars();      // csz_beam       (default 1)
ParticleRegisterCvars();  // csz_particle   (default 1)
EfxShimRegisterCvars();   // csz_efx_shim   (default 1; 0 = engine real table = full revert)
```

## 4. csz_renderer.cpp — `RenderFrame()` SHIM INSTALL/UNINSTALL (takeover boundary)
Replace the `csz_renderer` escape-hatch check (slot 1) with a reconcile tied to
**takeover MODE**, NOT a per-pass `RF_DRAW_WORLD` check:
```
bool takeoverMode = ( m_cvarEnable == NULL || m_cvarEnable->value != 0.0f );
EfxShimSetActive( takeoverMode );
if( !takeoverMode )
    return 0;
```
Rationale (critical): the pEfxAPI swap must stay installed **across consecutive
taken-over frames** so emit events that fire *between* RenderFrame calls (weapon
prediction / packet processing) are captured. `EfxShimSetActive` is idempotent, so
the value is stable across this frame's sub-passes — no install/restore churn. The
shim is installed iff `csz_renderer != 0 && csz_efx_shim != 0`.

## 5. csz_renderer.cpp — DRAW PASS (slot 14.5)
Between `EndPass( kTmTrans );` and `BeginPass( kTmViewmodel );`:
```
BeginPass( kTmDelegate );   // design renames kTmDelegate -> kTmEfx (OWED)
BeamDraw( view );
ParticleDraw( view );
EndPass( kTmDelegate );
```
Slot 14.5 per design §4: transparent domain, after water/sprites/brush-trans,
before viewmodel.

**Single-step guard (§4):** neither `BeamDraw` nor `ParticleDraw` read the engine
frametime. Each advances its sim from its own `ClientTime()` delta, updated exactly
once per call. RenderFrame's main transparent pass is the ONLY caller, so there is
no double advance; a hypothetical zero-dt sub-pass re-entry would compute dt≈0
anyway. Do NOT also call them from any frametime=0 sub-pass.

**State hygiene:** both passes restore the EnterTakeover baseline before returning
(`SetBlend(kBlendNone)`, depth test+write ON, VAO/program unbound; particle also
`SkyComposeRestoreTmus()` on the soft path). Safe to enter the viewmodel pass after.

## 6. csz_renderer.cpp — `NewMap()`
After `g_world.Destroy();`:
```
BeamNewMap();       // drop live beams (no cross-map carry)
ParticleNewMap();   // drop live particles/tracers
```

## 7. csz_renderer.cpp — `Shutdown()`
Inside the `if( m_glReady )` block (after `DustShutdown();`):
```
BeamShutdown();      // GL teardown (generation-safe)
ParticleShutdown();  // GL teardown (generation-safe)
```
**Outside / after** the `m_glReady` block (pointer restore only, no GL — must
always run so the engine efx table is never left swapped):
```
EfxShimShutdown();
```

## 8. cvars added
`csz_beam` (1), `csz_particle` (1), `csz_efx_shim` (1; 0 = restore engine real
table = full A/B revert). Pass timers reuse `kTmDelegate` (design: rename to
`kTmEfx`, update the r_speeds / fps pass-ms print strings — integrator's call).

## 9. Behaviour / scope notes the integrator + reviewers must know
- **beam_s ABI:** the beam pool slot embeds a real `beam_s` as its first member
  and returns `&slot.pub` so the egon site (`ev_hldm.cpp:1462-1495`) can write
  `pBeam->flags |= FBEAM_SINENOISE` and `pBeam->die = 0` (stop). `BeamDraw`
  honours a client-set `die` (frees the slot when `now >= die`).
- **NOT redirected by the shim (correct, do not "fix"):** `R_DecalShoot` (engine
  decal path, decision A), `R_MuzzleFlash` (CL_AllocDlight mirror), `R_Sprite_Trail`
  (FTENT sprite tempents already drawn via HUD_AddEntity — redirecting would
  double-draw). These pass through.
- **OWED long tail:** `R_BeamEnts/Follow/Ring/CirclePoints/Lightning`,
  `R_RunParticleEffect` pass through to the engine + log once (`CSZ_LogDev`,
  token `efxshim`); under takeover the engine does not draw them → registered
  not-yet-visible, NOT claimed complete.
- **Beam texture:** best-effort sprite-strip resolve (`gRenderAPI.pfnGetModel`);
  falls back to a procedural additive core. One texture per batch (mixed
  textured/procedural beams OWED). Beam width/amplitude visual calibration is a
  tunable left for the visual TEST agent.
- **Particle soft fade:** uses the SkyCompose depth texture when `csz_hdr` is on
  (dust contract: depth test/write off + in-shader soft intersection); else falls
  back to a hard depth-tested additive draw into the bound FBO. Genuine smoke
  (alpha) particles for `R_BulletImpactParticles`/trails are sorted back-to-front.

## 10. Honest terminal state
`DIFFS_READY_UNVERIFIED`. Local build links clean; fixed-pipeline blacklist zero
hits in the new files; geom does not reverse-include lighting. OWED to the main
window: live invisible-test smoke (open-fire tracer/spark/impact-puff visible,
gauss/egon beam visible, fog darkens particles), same-session A/B vs `csz_efx_shim 0`,
32-bot perf sample, and the visual/audience acceptance pass (separate TEST agent).
