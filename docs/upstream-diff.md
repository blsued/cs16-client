# upstream-diff.md -- CSOZ fork: upstream file modification registry

Rule (csoz code-standards R2/R6): upstream files take hook lines only, every
hook line ends with `// CSOZ hook: <reason>`; all logic lives in
`cl_dll/cszrender/`. Every upstream edit must be registered here in the same
PR. Baseline: upstream pin `e587679`.

| File | Lines added | What / why it cannot live in cszrender |
|---|---|---|
| `cl_dll/CMakeLists.txt` | 2 | `add_subdirectory(cszrender)` + `target_link_libraries(client PRIVATE cszrender)`. Build wiring must touch the upstream client target; kept additive (R4). |
| `cl_dll/cdll_int.cpp` | 5 | 1 include + 4 calls: `CSZ_GetRenderInterface` (inside `HUD_GetRenderInterface`, registers render_interface_t callbacks -- the engine hands the tables to this upstream export only), `CSZ_HudInit` (after `gHUD.Init()`, cvar/command registration + handshake sanity check), `CSZ_VidInit` (in `HUD_VidInit`, GPU resource invalidation on vid restart), `CSZ_Shutdown` (end of `HUD_Shutdown`, resource teardown). These lifecycle exports are upstream-owned entry points; cszrender cannot intercept them from inside the static lib. |

Planned (not yet applied):
- `cl_dll/entity.cpp`: 1 line `CSZ_AddEntity` inside `HUD_AddEntity` (M1 T3).
