# worldshader.md — geom/csz_world_shaders.inl (worker/r1-rain-water-snow)

## Finding (2026-06-18)
The W2 snow near>far fix and the wet-ground Lagarde branch were ALREADY
implemented in prior commits (6cd1eba, aeeb5d2). The R2_STATIC_REVIEW.md line
numbers handed to me (snow == "just mix(col,u_snowColor,snow)" @152-154) are
STALE / pre-fix. Current file already:
  - snow: modulates by csz_lmLum (line ~181: 0.80+0.30*csz_lmLum) AND a cheap
    distance falloff csz_snowDist (line ~180) -> near+lit snow brighter, far recedes.
  - wet: up mask smoothstep(0.35,0.85), darken mix(1,0.55), grazing fresnel,
    tight lit spec, self-lit cool rim. Matches Lagarde.

## Actionable deltas (compact, per task constraints)
1. Snow coverage cap had drifted to 0.97 (near full-white). Task explicitly
   requires the 0.9 cap ("don't go full white"). Restore cap to 0.9.
2. Wet grazing rim: cheaply sharpen the glancing-angle rim so damp ground reads
   glossy at grazing angles, WITHOUT regressing darken (line 143) or lit spec (158).

## Constraints honored
- Only fed uniforms used (u_snowColor/u_snowAmount/u_wetness/u_camPos/u_sunDir/
  u_sunColor/u_fog/u_ambTint — all confirmed in csz_world.cpp:776-784).
- No new uniforms.
- weather-off byte no-op preserved: snow under if(u_snowAmount>0), wet under
  if(u_wetness>0).
- lines 36-37 stub: NOT stale here (legit cross-file contract note); the W5
  ".inl:36-37" stub belongs to csz_water_shaders.inl (other agent). Left alone.
