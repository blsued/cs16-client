# upstream-diff.md -- CSOZ fork: upstream file modification registry

Rule (csoz code-standards R2/R6): upstream files take hook lines only, every
hook line ends with `// CSOZ hook: <reason>`; all logic lives in
`cl_dll/cszrender/`. Every upstream edit must be registered here in the same
PR. Baseline: upstream pin `e587679`.

| File | Lines added | What / why it cannot live in cszrender |
|---|---|---|
| `cl_dll/CMakeLists.txt` | 2 | `add_subdirectory(cszrender)` + `target_link_libraries(client PRIVATE cszrender)`. Build wiring must touch the upstream client target; kept additive (R4). |
| `cl_dll/cdll_int.cpp` | 5 | 1 include + 4 calls: `CSZ_GetRenderInterface` (inside `HUD_GetRenderInterface`, registers render_interface_t callbacks -- the engine hands the tables to this upstream export only), `CSZ_HudInit` (after `gHUD.Init()`, cvar/command registration + handshake sanity check), `CSZ_VidInit` (in `HUD_VidInit`, GPU resource invalidation on vid restart), `CSZ_Shutdown` (end of `HUD_Shutdown`, resource teardown). These lifecycle exports are upstream-owned entry points; cszrender cannot intercept them from inside the static lib. |

| `cl_dll/entity.cpp` | 2 | 1 include + 1 call: `CSZ_AddEntity` inside `HUD_AddEntity` before `return 1` (after the spectator in-eye `return 0` filter, so filtered entities stay undrawn). The engine dispatches visible entities to this upstream export every frame; the CSZ renderer builds its draw lists from it (M1 T3). |
| `cl_dll/hud/MOTD.cpp` | 5 | 1 include + 1 guarded early-out in `MsgFunc_MOTD`: `CSZ_MotdContentIsBlank` + `Reset()` + `return 1` (blank-MOTD suppression, M1 defect batch 2 #14 -- CSOZ ships an empty motd.txt and wants no join announcement window; an empty MOTD would otherwise still draw the window frame). The HUD message handler is upstream-owned; only the blank check lives in cszrender. |
