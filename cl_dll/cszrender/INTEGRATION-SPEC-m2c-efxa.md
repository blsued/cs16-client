# M2c C-DEC + C-TRI — Integration Spec (worker/m2c-efxa)

Worker owns the NEW files (`geom/csz_decal.*`, `geom/csz_triapi.*`). Everything
below marked **[INTEGRATION]** is an edit to an integrator-owned / shared seam,
already applied in this worktree for build + self-test. The integrator should
graft exactly these onto the integration base.

Build: clean on VS2022 Win32 Release (`cszrender.lib` + `client.dll`). Fixed-
function GL blacklist (`glBegin/glMatrixMode/GL_FOG/glLoadMatrix/GL_DrawParticles/
DrawSingleDecal`) = zero hits in the new files. Honest state:
`DIFFS_READY_UNVERIFIED` — real smoke + same-session A/B (csz_renderer 0↔1) +
32-bot perf are OWED to the main window.

## Owned files (no integration action beyond CMake)
- `geom/csz_decal.h` / `geom/csz_decal.cpp` / `geom/csz_decal_shaders.inl`
- `geom/csz_triapi.h` / `geom/csz_triapi.cpp` / `geom/csz_triapi_shaders.inl`

Public API:
```cpp
// csz_decal.h
void DrawDecalsAlpha( const ViewSetup &view, const unsigned char *visibleFaces, int numFaces );
void DrawDecalsModulate( const ViewSetup &view );
void DecalRegisterCvars();   // csz_decal default 1
void DecalShutdown();
// csz_triapi.h
triangleapi_t *CszTriApiTable();
void CszTriApiBeginDispatch( const ViewSetup &view, triangleapi_t *engineTri );
void CszTriApiEndDispatch();
bool CszTriApiEnabled();      // csz_triapi != 0
void TriApiRegisterCvars();   // csz_triapi default 1
void TriApiShutdown();
```

## [INTEGRATION] 1 — `CMakeLists.txt`
Add to `CSZRENDER_SRC` (after `geom/csz_sprite.cpp`):
```
	geom/csz_decal.cpp
	geom/csz_triapi.cpp
```

## [INTEGRATION] 2 — `core/csz_glstate.h` / `.cpp` : new blend mode
`enum BlendMode` gains a trailing `kBlendModulate`. `SetBlend()` adds:
```cpp
case kBlendModulate:
	glEnable( GL_BLEND );
	glBlendFunc( GL_DST_COLOR, GL_SRC_COLOR );   // 2*src*dst (engine 2x decal blend; identity at src=0.5)
	break;
```
Pure addition; no existing mode changes; IEEE/behaviour-identical for all current callers.

## [INTEGRATION] 3 — `geom/csz_world.h` / `.cpp` : read-only visible-face accessor
```cpp
// csz_world.h (in class WorldRenderer, after IsBuilt())
const unsigned char *VisibleFaces( int *outCount ) const;
// csz_world.cpp (after WorldRenderer::IsBuilt)
const unsigned char *WorldRenderer::VisibleFaces( int *outCount ) const {
	if( outCount != NULL ) *outCount = ( s_world.visible != NULL ) ? s_world.numFaces : 0;
	return s_world.visible;
}
```
Returns the existing per-local-face visibility array `BuildVisibleSet` already
maintains (1 = visible; length == `nummodelsurfaces`). Read-only, no new state.
The decal module reads the BSP itself via the core `EngBsp` mirror, so it needs
no `csz_world.h` include — the composition root supplies `(ptr, count)`.

## [INTEGRATION] 4 — `csz_renderer.cpp` : composition root

a. Includes (after `geom/csz_sprite.h`):
```cpp
#include "geom/csz_decal.h"
#include "geom/csz_triapi.h"
```

b. Forward declarations of the client TriAPI entry points, at GLOBAL scope
(before `namespace csz {`):
```cpp
void HUD_DrawNormalTriangles( void );
void HUD_DrawTransparentTriangles( void );
```

c. `OnHudInit()` — register cvars (after `RegisterSpriteCommands();`):
```cpp
DecalRegisterCvars();
TriApiRegisterCvars();
```

d. `Shutdown()` — teardown inside the `if( m_glReady )` block (after
`g_studio.DestroyAll();`):
```cpp
DecalShutdown();
TriApiShutdown();
```

e. `RenderFrame()` pass slots. Timers reuse the EXISTING `kTmDecal` / `kTmTriapi`
enum members (no rename needed for this task; `kTmDelegate→kTmEfx` belongs to the
beam/particle C-BEAM/C-PAR task, not here).

- **Slot 11.6** — after `EndPass( kTmBrush )`, before `BeginPass( kTmStudio )`:
  ```cpp
  {
      int numVisFaces = 0;
      const unsigned char *visFaces = g_world.VisibleFaces( &numVisFaces );
      BeginPass( kTmDecal );
      DrawDecalsAlpha( view, visFaces, numVisFaces );   // also does the single BSP walk that builds BOTH classes
      EndPass( kTmDecal );
  }
  ```
- **Slot 13.x** — immediately after `EndPass( kTmLights )`:
  ```cpp
  BeginPass( kTmDecal );
  DrawDecalsModulate( view );      // draws the modulate batch slot 11.6 built (no-op if it didn't run)
  EndPass( kTmDecal );
  ```
- **Slot 13.9** — before `BeginPass( kTmTrans )` (opaque TriAPI = spectator overview):
  ```cpp
  if( CszTriApiEnabled()) {
      BeginPass( kTmTriapi );
      triangleapi_t *savedTri = gEngfuncs.pTriAPI;
      CszTriApiBeginDispatch( view, savedTri );
      gEngfuncs.pTriAPI = CszTriApiTable();
      HUD_DrawNormalTriangles();
      gEngfuncs.pTriAPI = savedTri;
      CszTriApiEndDispatch();
      EndPass( kTmTriapi );
  }
  ```
- **Slot 14.7** — after `EndPass( kTmTrans )`, before `BeginPass( kTmViewmodel )`
  (transparent TriAPI = particleman + g_Environment). Identical block but calling
  `HUD_DrawTransparentTriangles()`.

  Save/swap/restore is the integrator's bridge responsibility (the swap is the
  only place `gEngfuncs.pTriAPI` is touched; it is restored before the dispatch
  block returns, so the engine 2D HUD pass after `RenderFrame` keeps the real
  table). `CszTriApiEndDispatch()` already does per-dispatch GL state hygiene
  (blend/depth/cull/VAO/program reset); the existing `LeaveTakeover` (slot 16)
  still owns the final TMU `GL_CleanUpTextureUnits` before engine handoff — no
  extra TMU cleanup call is required at the dispatch sites.

## frametime / double-step (design §4)
Each `HUD_Draw*Triangles` is called EXACTLY ONCE per real frame, so
`g_pParticleMan->Update()` + `g_Environment.Update()` step their simulation once.
No `GetFrameTime()` zeroing is needed (these are not scene re-renders); do NOT add
a second call site.

## Tuning points / OWED (for the visual A/B + perf gates)
1. **Decal class signal** — `DecalIsAlphaClass()` currently uses `PARM_TEX_FLAGS &
   TF_HAS_ALPHA` (the design's literal "with alpha / no-alpha" wording), isolated
   in ONE function. If a map authors bullet holes WITH an alpha channel too, swap
   that helper to the `FDECAL_CUSTOM` flag (offset 22 in the `EngDecal` mirror).
   The same-session A/B (csz_renderer 0↔1 decal look) is the arbiter — OWED.
2. **Decal lightmap brightness** — samples the engine lightmap page
   (`PARM_TEX_LIGHTMAP[surf.lightmaptexturenum]`) at ×1, relying on the modulate
   class's 2× blend (engine parity). Verify against the engine path; if decals
   read dark/bright, a `u_lmScale` uniform is the knob. OWED.
3. **Spectator overview projection** — the opaque TriAPI dispatch feeds the world
   `matViewProj`; if `DrawOverview` sets its own ortho via `GetMatrix`/ortho
   bounds the overview inset may need a dedicated projection. Particleman/
   environment (the primary consumers, world-space verts) are correct. OWED.
4. **Legacy mod fog color scale** — `Tri_Fog` divides the color by 255 (classic
   TriAPI convention); only affects the secondary mod-fog fallback path (scene
   ambience fog is primary). OWED if a map drives `cl_fog_*`.
5. **Studio decals** (`R_CreateStudioDecalList`) — explicitly OUT OF SCOPE; OWED.
