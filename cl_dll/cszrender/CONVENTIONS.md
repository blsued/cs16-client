# CSOZ renderer conventions (`cl_dll/cszrender/`)

Versioned with the renderer. These are the cross-layer rules every effect layer
(sky, fog, lighting, atmosphere, stars, …) follows so layers stay independently
buildable, isolable, and acceptance-testable.

## 1. Per-layer master cvar

Each effect layer ships **exactly one** `csz_<layer>` master switch whose **default
selects the current / baseline behavior**. Turning every other layer to its
baseline and toggling just one master lets that layer be isolated for visual
acceptance (A/B the layer on vs off, nothing else moving).

Naming + idiom (single source of truth — copy this, don't reinvent):

- Name: `csz_<layer>` (e.g. `csz_atmos`, `csz_stars`, `csz_pano`, `csz_hdr`).
- Register once at HUD init in the layer's `…RegisterCvars()` / `RegisterDevCvars()`,
  itself called from `Renderer::OnHudInit()` (`csz_renderer.cpp`).
- Store the pointer in a file-static `cvar_t*`; register with
  `gEngfuncs.pfnRegisterVariable( "csz_<layer>", "<baseline-default>", FCVAR_CLIENTDLL )`.
- Read **live** each frame via a small `ReadCvar( cv, fallback )` helper so an
  in-session toggle takes effect next frame.
- Reference implementation: `geom/csz_sky.cpp:160` (`RegisterDevCvars`) and the
  `ReadCvar` helpers in `geom/csz_sky_compose.cpp:167` / `fog/csz_fog_volume.cpp:100`.

## 2. Zero-visual-change rule for infrastructure layers

Any **observability** or **decoupling** addition (perf counters, seams, hooks,
debug dumps) MUST be **default-off / default-identity** and **A/B pixel-identical
to baseline** with default cvars. The capture gate rejects any pixel delta under
defaults. Reading a passive GPU/CPU timer or printing a console line is allowed
(it changes no draw); changing a uniform, blend, clear, or draw order is not —
unless gated behind a non-default cvar value.

## 3. One-way module boundary (spec 4.6)

`fog/` never includes (and is never included by) `lighting/` or `geom/`. The
server-authoritative ambience POD (`core/csz_ambience_types.h`) is the shared
contract; `core/` is the only thing all three include. Cross-cutting per-frame
scalars belong on `ViewSetup` (a render-side struct) or `AmbienceParams`, resolved
once in the composition root (`Renderer::RenderFrame`), never by reaching across
the boundary from a consumer.

## 4. L0 infrastructure interfaces (this layer)

### `csz_perf_dump` (default `0`) — perf observability
When `!= 0`, `Renderer::SampleFps` (`csz_renderer.cpp`) emits **one parseable line
per ~1 s window** via the always-visible `CSZ_LogInfo` facade:

```
[csz_perf] gpu_frame_ms=%.3f shadow=%.3f sky=%.3f world=%.3f brush=%.3f decal=%.3f studio=%.3f lights=%.3f volume=%.3f trans=%.3f delegate=%.3f triapi=%.3f viewmodel=%.3f
```

- `gpu_frame_ms` = the whole-frame **in-scene** GPU span, read from the **single**
  `GL_TIME_ELAPSED` query that `csz_sky_compose.cpp` already wraps around
  BeginScene…Resolve (`SkyComposeLastGpuMs()`). It is `-1.0` until the first ring
  result resolves. **`GL_TIME_ELAPSED` cannot nest — never add a second GPU timer
  inside the compose span.** Per-pass GPU timing is therefore not available; the
  per-pass numbers are CPU ms.
- Each pass value = the per-frame-averaged **CPU** ms from `BeginPass/EndPass`
  (`s_passAccumMs[]`), the same numbers the Dev `fps` line prints.
- `csz_perf_dump != 0` only *forces the existing passive query on* (via
  `ComposeTimingActive`) and prints text — **no draw changes**, frame stays
  pixel-identical.

### `csz_fog_server_mask` (default `1`) — black-fog decouple seam
Render-side fog visibility is consumed as
`fogDensityConsumed = clientDensity (clientVisibilityFactor) × serverFogMask`.
`serverFogMask` is applied at the **single chokepoint** `RenderFrame` where
`view.ambience.fogDensity` is finalized — every fog consumer (world/studio/sprite
uniforms via `CszFogUniformVecs`, the sky fog band, the volumetric march) reads
that field, so one multiply covers them all consistently.

- Default `1.0` ⇒ `density * 1.0f` is an IEEE-exact identity ⇒ pixel-for-pixel
  the pre-L0 fog.
- Client API (`fog/csz_fog.h`): `CszFogServerMask()` (live `[0,1]`),
  `CszFogSetServerMask(float)` (reserved future server drive; `m<0` clears),
  `CszFogRegisterCvars()`.
- **Reserved server hook**: a future server-authoritative blackout maps its intent
  to `CszFogSetServerMask()` from `cl_dll/hud_msg.cpp` `MsgFunc_Fog` (gmsgFog). Not
  wired this period — the cvar is the sole source and stays at identity.
