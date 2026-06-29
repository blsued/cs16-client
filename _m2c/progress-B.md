# Worker B — World-surface dynamics (worker/m2c-B)

## Scope (priority order)
1. Animated lightstyles  (csz_lightstyle) — CPU R_BuildLightMap, dirty re-upload
2. Texture frame animation (csz_texanim) — R_TextureAnimation, anim_next chain + alternate
3. Scrolling/conveyor (csz_scroll) — SURF_CONVEYOR (BIT6), u_scroll in VS
4. Detail textures (csz_detail) — dt_texturenum, u_texDetail unit 3   [LOW]

## Key facts (grounded)
- lightstyle scale: 'a'=0 'm'=264 'z'=550 (×22), >>8 + LightToTexGamma. Matches existing
  style-0 bake (sample*264>>8). value from GetLightStyle(n)->pattern (canonical, no engine-scale dep).
- SURF_CONVEYOR = BIT(6)=0x40 (FWGS surface flags; matches existing kSurf* layout).
- Texture anim clock: (int)(ClientTime()*10) % anim_total; walk anim_next [anim_min,anim_max);
  alternate_anims when entity curstate.frame != 0.
- Detail: tex->dt_texturenum = slot to bind; GetDetailScaleForTexture(tex->gl_texturenum,&sx,&sy).
- TMU map: 0 diffuse, 1 lightmap, 2 shadow(lit only), 3 detail (new).
- ClientTime() = gEngfuncs.GetClientTime(). cvars self-register lazily (FeedTpFogGlow precedent).
- Per-frame hook = top of DrawOpaque (UpdateWorldDynamics) — no integrator wiring needed for the loop.

## Files
- core/csz_engine_bsp.h : +kSurfConveyor
- geom/csz_lightmap.h/.cpp : +UploadBlockLit (gamma-only, no 264)
- geom/csz_world_shaders.inl : VS u_scroll; FS detail sampler+modulate
- geom/csz_world.cpp : FaceRec fields, anim-face list, AnimateLightStyles, UpdateWorldDynamics,
   R_TextureAnimation, draw-loop texanim/scroll/detail + observability log

## Status: DONE — Win32 Release build green (cszrender.lib + client.dll). DIFFS_READY_UNVERIFIED (runtime shader+visual OWED to TEST). See INTEGRATION-SPEC-B.md
