# csz_verify_freeze — verification-infra cvar (Deliverable A)

**Branch:** `worker/m2c-verifyfreeze` (off `feature/m2c-render-completeness`)
**Purpose:** machine-verify TIME-VARYING render effects under the takeover renderer
(`csz_renderer 1`). The capture harness freezes the *scene instant* (one
`cl.time` / `csz_sky_phase`), but several render terms still vary per *rendered
frame* or react to scene content, confounding effect-diff / A/B. `csz_verify_freeze`
(default `0`, `FCVAR_CLIENTDLL`) pins those terms to constants so two captures of the
same fixed instant are byte-near-identical and an effect-diff (toggle one effect's own
cvar) is a clean, grade-stable machine gate.

NOT a gameplay feature. Default `0` reproduces the exact gameplay path.

## What it pins (single source of truth: `SkyComposeVerifyFreeze()`)

| # | Term | File | Frozen behaviour |
|---|------|------|------------------|
| a | HDR adaptive eye-adaptation exposure | `geom/csz_atmos.cpp` | exposure input pinned to sun-elevation 0° → constant `adaptive=150`, decoupled from live sun position |
| b | resolve dither (OETF→dither tail) | `geom/csz_sky_compose.cpp` | `u_dither` forced 0 regardless of `csz_dither` |
| c | fog-volume animated-IGN frame offset | `fog/csz_fog_volume.cpp` | free-running `s_marchFrame` → `0` |
| c | star twinkle clock | `geom/csz_stars.cpp` | `u_time` → `0` (no per-star boil) |
| c | dust temporal integration | `lighting/csz_dust.cpp` | `dt=0` → motes pinned at hash-seeded positions |

`csz_sky_compose.{h,cpp}` owns the cvar + the `SkyComposeVerifyFreeze()` query;
the four effect modules already `#include` the compose header and read it live.

## Self-test (de_dust2, `csz_renderer 1`, dev instance `D:\csoz-run-dev-VF`)

Built Release/Win32 **GREEN** (no errors/warnings); `client.dll`
SHA256 `19EC8C29…64E`. Captures 640×480, `SkyPhase 0.5` (midnight). Pixel metric =
sum-of-|Δ| over RGB per pixel.

| Comparison | Max | Mean | %px differ | Verdict |
|---|---|---|---|---|
| `frozen_A` vs `frozen_B` (both freeze 1) | **0** | 0 | 0% | **byte-IDENTICAL** → determinism ✓ (headline criterion) |
| `dither1+freeze0` vs `dither1+freeze1` | 3 | 0.907 | **55.3%** | freeze **suppresses** the TPDF dither (±1 LSB/ch) → cvar genuinely WIRED, (b) ✓ |
| `dither1+freeze1` vs `frozen_A` (dither0) | 0 | 0 | 0% | under freeze, `csz_dither` is fully overridden ✓ |
| `baseline_A` vs `baseline_B` (both freeze 0) | 0 | 0 | 0% | harness is frame-count deterministic at a frozen instant |

### Honest limits of the demonstration
- **(a) adaptive exposure**: wired via the same `SkyComposeVerifyFreeze()` path
  (build-verified) but NOT pixel-demonstrable at frozen midnight — the night
  atmosphere radiance is ~0, so `150` vs the baseline ceiling `5000` both resolve to
  ~black (`sky_nofreeze` vs `sky_freeze` = 0 diff). At bright twilight the baseline
  elevation ≈ 0 ≈ the frozen value, so they converge instead. Its real value is
  decoupling exposure from sun-position drift under **live** phase and from
  scene-content reaction during an effect toggle — neither exercised by a single
  frozen instant.
- **(c) fog/stars/dust**: wired identically; because the harness is frame-count
  deterministic, these per-frame/time terms show no run-to-run delta at a frozen
  instant to begin with (baseline pair = 0). They matter when the captured instant is
  not bit-perfectly frozen (wall-clock `ClientTime` drift) and to keep an effect
  toggle from shifting unrelated animated content.

### Consumer note
`csz_verify_freeze` composes WITH harness instant-freezing — it does **not** freeze
the scene clock itself. For a stable A/B, freeze the instant (`-SkyPhase <v>`, fixed
pose) **and** set `csz_verify_freeze 1`. The dither-override result above is the
load-bearing proof that the cvar engages end-to-end.
