# cszrender -- CSOZ full-frame takeover renderer

Static library housing the entire CSOZ renderer. Upstream client code sees
only the `CSZ_*` entry points declared in `csz_render_iface.h`; every upstream
edit is a one-line hook tagged `// CSOZ hook:` (see fork `docs/upstream-diff.md`).

Engine contract: RenderAPI version 37 (`common/render_api.h`). The engine calls
`HUD_GetRenderInterface`, we register `render_interface_t` callbacks, and
`GL_RenderFrame` returning 1 makes the client own the whole 3D scene while the
engine keeps 2D/HUD/sound/model loading (csoz plan section 2, notes-renderapi).

## Layout (one-way dependencies: core <- geom <- lighting <- composition root)

| Path | Role |
|---|---|
| `csz_render_iface.h/.cpp` | Composition root: `extern "C"` entries, handshake, render_api audit, ref_gl check, callback registration |
| `csz_renderer.h/.cpp` | Composition root: frame orchestration (pass order), per-frame entity lists, `csz_renderer` cvar, lazy GL init |
| `core/` | Zero-dependency base: engine access (`csz_engine`), logging facade (`csz_log`), fatal-exit (`csz_fatal`), vendored Khronos GL headers (`csz_glcorearb`/`csz_khrplatform`), GL loader (`csz_glfuncs`), capability probe (`csz_glcaps`); later: state wrappers, shaders, math, views |
| `geom/` | (T2+) world/BSP, lightmap atlas, studio, viewmodel, sprite rendering; depends on core only |
| `lighting/` | (T6+) light registry, spot shadow map, light passes; depends on core + geom public headers |

Rules of the house (enforced by review + grep gates):
- Only `core/csz_log.cpp` calls `gEngfuncs.Con_Printf`; everything logs via `CSZ_Log*`.
- Only `core/csz_fatal.cpp` may terminate the process (and include windows.h).
- All GL entry points load through `gRenderAPI.GL_GetProcAddress` via the
  `CSZ_GL_FUNCTIONS` X-macro; the required set is pinned in plan section 2.3.
- Init-chain failures are explicit (`CSZ_FatalInit`), never silent fallback.
- New sources must be added to `CMakeLists.txt` explicitly (no GLOB).
