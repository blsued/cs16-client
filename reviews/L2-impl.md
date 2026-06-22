# L2 — Moonlight light model (audit + expose downstream channels) — impl progress

Branch `feat/sky-base-d1`, base HEAD `a49459b` (L1). Goal: preserve the APPROVED
full-moon look; expose clean moon channels for L3 (cloud-dim) / L4 (light-shafts).

## Audit (first step — current state)
1. **Phase→intensity mapping**: NOT crude pow. Three INCONSISTENT phase responses:
   - Surface directional (`geom/csz_sky.cpp` `MoonLitFraction()` :119-128, applied :438):
     LINEAR illuminated fraction `0.5*(1+cos a)`, 0.25 floor → half-moon = 0.5.
   - Disc wash/glow (`csz_moon_phase_exp` default 2.7, `geom/csz_sunmoon.cpp`): `lit^2.7`
     → half ≈ 0.15. (This IS the "crude pow" the research warned of, but it is the
     APPROVED aesthetic for the disc glow, not the surface.)
   - Godray source (`geom/csz_sunmoon.cpp:686-702`): smoothstep(0,0.02,lit) presence GATE
     → essentially on/off, not a brightness curve.
   At default `csz_moon_phase -1` ALL return 1.0 → approved full-moon UNCHANGED.
   Surface is a defensible linear illuminated-fraction model (photometrically too bright
   at partial phases, but not crude). The cross-channel inconsistency is the real finding.
2. **Channel separation**: NOT separated. Surface N·L (world FS `csz_world_shaders.inl:163`,
   studio FS `csz_studio_shaders.inl:154`) AND per-pixel fog in-scatter glow (world FS
   :178-179, studio FS :162-163) BOTH read the SAME `u_sunColor` (=`amb.moonlightColor`,
   blended sun+moon premul) & `u_sunDir` (=`amb.moonlightDir`); fog glow only additionally
   ×`u_fogParams.y` (=`amb.sunGlow`). → exactly the codex "one scalar → ground black / air
   bright" coupling risk for L4. Disc sprite = separate pass. Screen godrays (Step4) =
   ALREADY independent (compute own color, do NOT read `amb.moonlightColor`).
3. **Step4 godrays source**: `CszGodraySource(view)` (`geom/csz_sunmoon.cpp:~640-740`)
   recomputes dir from phase (antipodal sun), picks dominant body, builds OWN color
   (moon {0.78,0.88,1.10}×csz_moon_gain×csz_moon_godray) + own phase gate. Fully decoupled
   from the published moonlight channel already.

## Delivery (minimal, additive, identity at all defaults)
- **A. Expose channels** — `core/csz_ambience_types.h`: new fields `moonSurfaceDirect[3]`,
  `moonFogInScatter[3]`, `moonFogInScatterIntensity`, + `CszMoonInScatter()` accessor.
  Populated in `csz_sky.cpp` PublishLighting. NOT read by any current shader → pure
  exposure → surface render output unchanged.
- **B. cloudDim hook** — `amb.cloudDim` (default 1.0 via AmbienceNeutral). Applied to
  moonLit in PublishLighting (`*cloudDim`, IEEE-exact identity at 1.0). For L3.
- **C. Phase LUT (opt-in)** — `csz_moonlight_v2` cvar (default "0" = OFF = legacy linear
  fraction). >0 → `pow(litFrac, v2)` photometric curve on the SURFACE moonlight only.
  Full moon = 1.0 for any exponent → approved look byte-identical. A/B knob.

## Status: code done, cszrender+client.dll build clean (0 err/0 warn). Visual self-test → TEST subagent.
