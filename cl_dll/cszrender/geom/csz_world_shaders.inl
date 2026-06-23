/*
 * csz_world_shaders.inl -- CSOZ renderer: world GLSL sources
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original work written for
 * CSOZ; no code in this file is copied or translated from PrimeXT, Paranoia,
 * Trinity, retail/leaked sources, or any other license-tainted source
 * (see csoz docs/provenance.md, section 6).
 * Clean-room implementation. Mechanism studied from PrimeXT (see
 * csoz docs/notes/primext-render-mechanisms.md); implemented by an agent
 * that has not read that source.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * In addition, as a special exception, the author gives permission to link
 * the code of this program with the Half-Life Game Engine ("HL Engine") and
 * Modified Game Libraries ("MODs") developed by Valve, L.L.C ("Valve").
 * You must obey the GNU General Public License in all respects for all of
 * the code used other than the HL Engine and MODs from Valve. If you modify
 * this file, you may extend this exception to your version of the file, but
 * you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */
// Included ONLY by csz_world.cpp (plan section 2.1). Attribute locations and
// uniform names are the cross-file contract of plan section 2.4.
// GLES3 equivalence assumption: nothing here beyond the GL3.3 core /
// GLES3 / WebGL2 intersection (code-standards section 7).

// World base pass: diffuse * lightmap(style 0) * overbright factor.
// Stock-parity factor measured against the pinned engine (T2 A/B, ratio was
// exactly 1.5 with a plain 2.0): the classic non-VBO overbright path blends
// the lightmap pass with glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR) (= x2) AND
// a 128/192 vertex color (= x2/3), so the net factor is 4/3 (gl_rsurf.c
// R_BlendLightmaps). Upload already applied the engine light gamma table
// (see csz_lightmap.cpp).
// u_model is the per-draw model->world transform: identity for the static
// world (vertices are baked in world space at build time), and a translate *
// rotate built from a brush entity's origin/angles for moving/rotating brush
// submodels (func_door, rotating brushes). The same base program draws both
// the world and opaque brush entities so brush surfaces eat fog/night-tint
// identically (pitfall 23); only u_model changes between them.
static const char kWorldVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec2 a_lmuv;
layout(location = 3) in vec3 a_normal;
layout(location = 4) in float a_skyVis;  // S1: geometric sky visibility [0,1] (1=open); baked sidecar
uniform mat4 u_viewProj;
uniform mat4 u_model;
out vec2 v_uv;
out vec2 v_lmuv;
out vec3 v_normal;
out vec3 v_worldPos;
out float v_skyVis;                       // S1: forwarded to FS (S2 replaces the lightmap-luma proxy with it)
void main()
{
	v_uv = a_uv;
	v_lmuv = a_lmuv;
	v_skyVis = a_skyVis;
	// World-space normal forwarded raw (BSP face plane normal, baked world-space
	// at build time, csz_world.cpp:330). u_model is not applied: the lit VS
	// (kWorldLitVs) likewise forwards a_normal unrotated, so the base directional
	// term matches the lit pass for moving brush submodels (parity choice).
	v_normal = a_normal;
	// World-space fragment position (u_model APPLIED, unlike the normal): the
	// analytic base fog (fog M1 Step 2) reconstructs the camera->surface ray from
	// it. Identity for the static world; the per-entity transform for brush ents.
	vec4 wp = u_model * vec4( a_pos, 1.0 );
	v_worldPos = wp.xyz;
	gl_Position = u_viewProj * wp;
}
)GLSL";

// Fog + night-tint block (plan 2.6 contract, M2a A1; analytic base fog fog M1
// Step 2): closed-form exponential height+distance extinction with a directional
// sun/moon in-scatter glow, replacing the old exp2 term. Base pass ONLY: the
// lit-additive and depth programs below stay fog-free (clean-room pitfall 23,
// whole-pipeline ruling).
static const char kWorldFs[] = R"GLSL(#version 330 core
in vec2 v_uv;
in vec2 v_lmuv;
in vec3 v_normal;
in vec3 v_worldPos;
in float v_skyVis;                // S1: geometric sky visibility [0,1], plumbed but NOT consumed yet
                                  // (S2 replaces the csz_lmLum/csz_sky luma proxy below with clamp(v_skyVis,0,1)).
uniform sampler2D u_texDiffuse;   // unit 0
uniform sampler2D u_texLightmap;  // unit 1
uniform float u_alphaTest;        // 0 = off, else discard threshold (0.25)
uniform vec4 u_fog;               // rgb = fog color (linear, BASE/back-light in-scatter), w = extinction a (1/units); w<=0 -> off
uniform vec4 u_fogParams;         // S3: x = height falloff b, y = HG asymmetry g, z = maxOpacity, w = fogStart distance
uniform vec4 u_fogParams2;        // S3: xyz = per-channel extinction tint (b_ch = a*tint), w = 2D noise amplitude
uniform vec4 u_fogParams3;        // S3: x = noise world-scale, y = wind speed, z = fogCutoff distance, w = fog time (drift clock)
uniform vec3 u_fogLit;            // S3: toward-body (sun/moon) fog in-scatter color; HG lobe blends fog color toward this
uniform vec3 u_camPos;            // camera world position (ray origin)
uniform vec3 u_ambTint;           // night tint; (1,1,1) neutral
uniform float u_skyAmbScale;      // L3b sky-ambient cloud dimmer; 1.0 neutral (>=0.6 floor on CPU)
uniform vec3 u_sunDir;            // surface -> dominant body, normalized; base pass only
uniform vec3 u_sunColor;          // intensity-premultiplied light color; (0,0,0) = off
uniform float u_brushAlpha;       // per-entity translucency (curstate.renderamt/255); 1.0 = opaque/world
// fog M1 L4 -- moonlight Tyndall air-glow (forward HG fog in-scatter). All three
// default to the no-op identity (enable*mask*color all 0 => exactly the pre-L4
// in-scatter), so at csz_moonshaft 0 the result is byte-for-byte the legacy look.
uniform vec3  u_moonInScatter;    // L2 moonFogInScatter * intensity (premultiplied, linear); (0,0,0)=off
uniform float u_shaftMask;        // L3a cloud-gap gating: gap=1, thick cloud=0; 1.0 neutral
uniform float u_moonShaft;        // csz_moonshaft master toggle: 1=enhanced glow, 0=exact pre-L4
// S2 physical night model (REWORK-SPEC §S2, findings 1,2,7,9; codex S2 red-team v1).
// Default-fed to reproduce the approved 3fd8b7e look: u_nightModel 0 = byte-identical
// pre-S2 path, u_nightness 0 (day/sunset) -> the approvedDay branch is byte-identical.
uniform float u_nightModel;       // 1 = new physical model, 0 = legacy 3fd8b7e (A/B revert)
uniform float u_nightness;        // explicit phase gate [0,1], phase-curve driven (codex P0; replaces u_ambTint.b-r)
uniform vec3  u_sunWarmColor;     // warm SUN-ONLY premul directional (ungated; codex P2b); dir = -u_moonDir (antipode)
uniform vec3  u_moonDir;          // surface -> moon (pure antipode L vector) for the gated night moon term
uniform vec3  u_moonColor;        // moon-only premultiplied directional color (0 when the moon is down)
uniform vec3  u_nightSky;         // night sky-ambient color (premul), modulated by pow(skyVis,k)
uniform vec3  u_nightFloor;       // competitive readable ambient floor (premul), skyVis-independent
uniform float u_nightK;           // skyVis exponent k for the world night ambient
uniform float u_nightMoon;        // gain on the skyVis-GATED moon directional (wallhack-fixed moonlight)
out vec4 fragColor;
// S3 procedural 2D value noise (finding 6: 2D only -- the GL function table has no
// glTexImage3D, so this is in-shader hash noise, pure ALU, NO texture binding). Two
// octaves of smooth value noise over a drifting world-XY lattice = a cheap tiling-free
// density field; modulates the fog extinction 0.7..1.3x so the medium is not a flat slab.
float cszHash21( vec2 p )
{
	p = fract( p * vec2( 0.1031, 0.1173 ));
	p += dot( p, p.yx + 33.33 );
	return fract(( p.x + p.y ) * p.x );
}
float cszVNoise( vec2 p )
{
	vec2 i = floor( p );
	vec2 f = fract( p );
	vec2 u = f * f * ( 3.0 - 2.0 * f );                      // smootherstep weights
	float a = cszHash21( i );
	float b = cszHash21( i + vec2( 1.0, 0.0 ));
	float c = cszHash21( i + vec2( 0.0, 1.0 ));
	float d = cszHash21( i + vec2( 1.0, 1.0 ));
	return mix( mix( a, b, u.x ), mix( c, d, u.x ), u.y );
}
float cszFogNoise( vec2 worldXY, float scale, float wind, float t )
{
	vec2 drift = vec2( wind * t, wind * t * 0.6 );           // slow wind translation (drift = animation)
	vec2 q = worldXY * scale + drift * scale;
	float n = 0.65 * cszVNoise( q ) + 0.35 * cszVNoise( q * 2.03 + 7.1 );
	return n;                                                // ~[0,1]
}
// S3 per-channel analytic transmittance (REWORK-SPEC §S3.1, IQ model). Closed-form
// integral of an exponential-height density d(z)=e^(-b*z) along the camera->surface ray,
// with BOTH guards -- |b|<eps (uniform density) and |rd.z|<eps (near-horizontal ray) --
// computed ONCE as an a-independent geometric path factor G, then scaled by the PER-CHANNEL
// extinction aRGB (blue scatters most -> blue T lowest -> distance reads cool). Start/Cutoff
// bound the integral to [t0,t1] so the near field (weapon/skybox) stays crisp and the far
// field plateaus instead of crushing to black. World up axis is Z. Returns T in [0,1]^3.
vec3 cszFogT3( vec3 worldPos, vec3 camPos, vec3 aRGB, float b, float maxOpacity,
	float startD, float cutoffD, float densMul )
{
	if( all( lessThanEqual( aRGB, vec3( 0.0 ))))
		return vec3( 1.0 );                                  // ALL channels' extinction<=0 -> fog off (per-channel safe: a red/blue-only medium still scatters)
	vec3 d = worldPos - camPos;
	float t = length( d );
	float rdz = ( t > 1e-4 ) ? d.z / t : 0.0;
	float t0 = clamp( startD, 0.0, t );                      // near fence (no fog within startD)
	float t1 = ( cutoffD > 0.0 ) ? min( t, cutoffD ) : t;    // far plateau
	if( t1 <= t0 )
		return vec3( 1.0 );
	float G;                                                 // a-independent path factor over [t0,t1]
	if( abs( b ) < 1e-4 )
		G = ( t1 - t0 );                                     // uniform density (divide-by-b guard)
	else if( abs( rdz ) < 1e-4 )
		G = exp( -b * camPos.z ) * ( t1 - t0 );              // near-horizontal ray (divide-by-rd.z guard)
	else
		G = ( 1.0 / b ) * exp( -b * camPos.z ) * ( exp( -b * t0 * rdz ) - exp( -b * t1 * rdz )) / rdz;
	G = max( G, 0.0 ) * densMul;                             // 2D-noise density modulation
	vec3 F = aRGB * G;
	vec3 T = exp( -max( F, vec3( 0.0 )));
	return max( T, vec3( 1.0 - maxOpacity ));                // server reveal floor (silhouettes/blackout)
}
void main()
{
	vec4 base = texture( u_texDiffuse, v_uv );
	if( u_alphaTest > 0.0 && base.a < u_alphaTest )
		discard;
	vec3 albedo = base.rgb;
	vec3 lm = texture( u_texLightmap, v_lmuv ).rgb;
	vec3 lit = albedo * lm * ( 2.0 * 128.0 / 192.0 );   // baked daytime radiance (lightmap * overbright)
	vec3 nrm = normalize( v_normal );
	vec3 col;
	// S2 variable darkness (REWORK-SPEC §S2). u_nightModel is the master A/B
	// (csz_night_model): 1 = the new physical incident-light model, 0 = the
	// byte-identical pre-S2 (3fd8b7e) tiled-multiplier night. Uniform branch =
	// fully coherent, so the legacy path is a provable one-knob revert.
	if( u_nightModel > 0.5 )
	{
		// === New physical night model (findings 1,2,7,9; codex P0/P1a/P1b/P2b) ===
		// Shared SPLIT directional (codex P2b): the WARM SUN is UNGATED (dusk/dawn,
		// finding 2 -- never kill it; direction = pure antipode -u_moonDir), the COOL
		// MOON is skyVis-GATED so it can NEVER leak through walls at ANY nightness.
		// Both branches below use this same pair -> no ungated moonlight anywhere.
		float sv = clamp( v_skyVis, 0.0, 1.0 );
		vec3  sunWarm  = albedo * u_sunWarmColor * max( dot( nrm, -u_moonDir ), 0.0 );             // warm sun, UNGATED
		vec3  moonTerm = albedo * u_moonColor    * max( dot( nrm,  u_moonDir ), 0.0 ) * sv * u_nightMoon; // cool moon, skyVis-GATED
		// (1) APPROVED DAY LOOK -- the 3fd8b7e ambient + indoor floor expression VERBATIM
		// (codex P1a: NO tint reconstruction / phase gain), with the directional REPLACED by
		// the split sunWarm+moonTerm above (codex P2b). NOTE (S4): the world-only day-for-night
		// COOL GRADE no longer lives here -- it was RETIRED and folded into the UNIFIED compose
		// post (see the RETIRED note at :229-234 below); approvedDay is now just ambient + floor
		// + split directional. The tint.b-r (csz_night) signal survives, but it drives ONLY the
		// indoor ambient floor (csz_amb) here, not a cool grade. At the nightness=0 phases
		// (sunset/day) the moon is below the horizon so u_moonColor=0 and sunWarm == the old
		// blended directional -> approvedDay byte-identical to model0.
		vec3  approvedDay = lit;
		float csz_night = smoothstep( 0.0, 0.10, u_ambTint.b - u_ambTint.r );
		float csz_lmLum = dot( lm, vec3( 0.2126, 0.7152, 0.0722 ));
		const float CSZ_SKY_LO = 0.15, CSZ_SKY_HI = 0.50;   // bake-luma window: below=indoor, above=outdoor
		float csz_sky = smoothstep( CSZ_SKY_LO, CSZ_SKY_HI, csz_lmLum );
		const float CSZ_INDOOR_AMB = 0.25;                  // night indoor ambient floor (DARK end)
		float csz_amb = mix( 1.0, mix( CSZ_INDOOR_AMB, 1.0, csz_sky ), csz_night );
		approvedDay *= u_ambTint * csz_amb * u_skyAmbScale;
		approvedDay += sunWarm + moonTerm;                  // P2b split directional (was blended u_sunColor * csz_moon)
		// S4 / codex finding8: the world-only day-for-night COOL GRADE that used to live HERE
		// (luma -> vec3(0.75,0.92,1.25), mix by csz_night*0.70) is RETIRED. Studio had no
		// equivalent, so world and studio cooled inconsistently at night. The cool desaturation
		// now lives in the UNIFIED compose post (scotopicShift, csz_purkinje) which applies to
		// world + studio + everything by local luminance -> one consistent night cool grade.
		// (No effect on day: this grade was already gated by csz_night, == 0 at day/sunset.)
		// (2) PHYSICAL NIGHT -- spatialized by the S1 geometric skyVis (finding 7
		// replaces the old lightmap-luma sky proxy; finding 9: world-specific k + floor).
		// Indoor (skyVis~0) collapses to the cool readable ambFloor (competitive: silhouettes
		// still visible, no pure black); open (skyVis~1) gets the night sky-ambient PLUS the
		// same skyVis-GATED moon directional (屋顶下 skyVis~0 -> no moon leak / wallhack fix).
		vec3  nightAmb = u_nightSky * pow( sv, u_nightK ) + u_nightFloor;
		vec3  physicalNight = albedo * nightAmb + sunWarm + moonTerm;
		// (3) variable darkness = transition approvedDay -> physicalNight by the EXPLICIT
		// phase-curve gate u_nightness (codex P0: no longer u_ambTint.b-r, so the cool dawn
		// keyframe is no longer mis-read as full night). nightness=0 -> exactly approvedDay.
		col = mix( approvedDay, physicalNight, clamp( u_nightness, 0.0, 1.0 ));
	}
	else
	{
		// === Legacy pre-S2 (3fd8b7e) -- byte-identical A/B fallback (csz_night_model 0) ===
		col = lit;
		// Night flag derived from u_ambTint's cool-bias (legacy path only): IDENTITY
		// at day (b-r=0) and sunset (b-r<0 -> 0); engages at night/dawn (b>r).
		float csz_night = smoothstep( 0.0, 0.10, u_ambTint.b - u_ambTint.r );
		// Indoor sky-occlusion via the baked style-0 lightmap luma as a DAYTIME
		// sky-access proxy (bright baked = outdoor, dim = sheltered). Gated by
		// csz_night so daytime stays exact identity.
		float csz_lmLum = dot( lm, vec3( 0.2126, 0.7152, 0.0722 ));
		const float CSZ_SKY_LO = 0.15, CSZ_SKY_HI = 0.50;   // bake-luma window: below=indoor, above=outdoor
		float csz_sky = smoothstep( CSZ_SKY_LO, CSZ_SKY_HI, csz_lmLum );
		const float CSZ_INDOOR_AMB = 0.25;                  // night indoor ambient floor (DARK end)
		float csz_amb = mix( 1.0, mix( CSZ_INDOOR_AMB, 1.0, csz_sky ), csz_night );
		col *= u_ambTint * csz_amb * u_skyAmbScale;
		// Shadowless directional sun/moon (Option A), sky-access scaled (indoor only a
		// faint reflected fraction at night; dusk/dawn not regressed).
		const float CSZ_INDOOR_MOON = 0.08;                 // night indoor directional floor (DARK end)
		float csz_moon = mix( 1.0, mix( CSZ_INDOOR_MOON, 1.0, csz_sky ), csz_night );
		col += albedo * u_sunColor * max( dot( nrm, u_sunDir ), 0.0 ) * csz_moon;
		// Day-for-night cool grade (legacy): push warm surfaces toward cool blue-grey.
		float csz_l = dot( col, vec3( 0.2126, 0.7152, 0.0722 ));
		vec3  csz_cool = vec3( csz_l ) * vec3( 0.75, 0.92, 1.25 );   // luminance pushed cool-blue
		col = mix( col, csz_cool, csz_night * 0.70 );                // 0.70 = grade strength (tunable)
	}
	// S3 analytic fog (REWORK-SPEC §S3, findings 5/6/12): ONE in-scatter equation,
	// composited in linear HDR. Per-channel extinction (blue scatters most -> distance
	// cools) + a drifting 2D-noise density field + an HG directional lobe that blends the
	// cool sky-coupled base fog color (u_fog.rgb) toward the brighter sun/moon "lit" color
	// (u_fogLit). The old achromatic-gray fold (CszApplyFogAmbient) is RETIRED upstream and
	// the old pow(cosT,8) sunGlow is REPLACED by the HG lobe (not added on top -- finding 5).
	vec3 toFrag = v_worldPos - u_camPos;
	float tLen = length( toFrag );
	vec3 rd = ( tLen > 1e-4 ) ? toFrag / tLen : vec3( 0.0 );      // guard normalize-of-zero (NaN)
	float dens = 1.0;
	if( u_fogParams2.w > 0.0 )                                    // 2D noise density modulation (drifts with wind)
		dens = mix( 1.0, 2.0 * cszFogNoise( v_worldPos.xy, u_fogParams3.x, u_fogParams3.y, u_fogParams3.w ), u_fogParams2.w );
	vec3 aRGB = u_fog.w * u_fogParams2.xyz;                       // per-channel extinction (b_ch = a * tint)
	vec3 T = cszFogT3( v_worldPos, u_camPos, aRGB, u_fogParams.x, u_fogParams.z,
		u_fogParams.w, u_fogParams3.z, dens );
	// Directional in-scatter: Henyey-Greenstein lobe toward the dominant body (u_sunDir =
	// surface->sun/moon ~ camera->body). g (u_fogParams.y) ~0.7 -> a soft forward lobe; the
	// fog color blends from the cool base toward the sun/moon color so looking toward the
	// body the fog visibly brightens & warms/cools to the body's light.
	float cosT = dot( rd, u_sunDir );
	float g = u_fogParams.y;
	float hgDen = 1.0 + g * g - 2.0 * g * cosT;
	float hg = ( 1.0 - g * g ) / pow( max( hgDen, 1e-4 ), 1.5 );  // unnormalized HG (isotropic == 1)
	float sunAmount = clamp(( hg - 1.0 ) * 0.5, 0.0, 1.0 );       // forward lobe -> toward lit color
	vec3 inscatter = mix( u_fog.rgb, u_fogLit, sunAmount );
	// fog M1 L4 moonshaft Tyndall (INDEPENDENT enhancement, gated by u_moonShaft -- default
	// 0 = no-op; finding 12 keeps it out of the base equation). Weighted by (1-T) below so
	// sigma_s <= sigma_t holds and the air-glow stays inside the fog's own opacity budget.
	const float CSZ_HG_G = 0.8;
	float hgD2 = 1.0 + CSZ_HG_G * CSZ_HG_G - 2.0 * CSZ_HG_G * max( cosT, 0.0 );
	float hg2 = ( 1.0 - CSZ_HG_G * CSZ_HG_G ) / ( 4.0 * 3.14159265 * pow( max( hgD2, 1e-4 ), 1.5 ));
	inscatter += ( u_moonShaft * u_shaftMask * hg2 ) * u_moonInScatter;
	col = col * T + inscatter * ( 1.0 - T );
	fragColor = vec4( col, base.a * u_brushAlpha );
}
)GLSL";

// World lit-additive pass (T6): per-light contribution, blended additively on
// top of the opaque pass at equal depth (LEQUAL). Spot uniform group and the
// falloff formula are the plan 2.4 contract (shared with the studio family).
static const char kWorldLitVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 3) in vec3 a_normal;
uniform mat4 u_viewProj;
// u_model maps local model space -> world space. World submodel-0 geometry is
// baked in world space and feeds u_model = identity (byte-identical to the
// pre-u_model shader). Brush submodels (func_*) are baked in LOCAL space and
// feed a per-entity matrix, so the FS gets a genuine world-space position +
// normal for its ndotl / attenuation / spot projection / shadow textureProj.
uniform mat4 u_model;
out vec2 v_uv;
out vec3 v_worldPos;
out vec3 v_worldNormal;
void main()
{
	vec4 worldPos = u_model * vec4( a_pos, 1.0 );
	v_uv = a_uv;
	v_worldPos = worldPos.xyz;
	v_worldNormal = mat3( u_model ) * a_normal;	// rigid rotation; identity for world
	gl_Position = u_viewProj * worldPos;
}
)GLSL";

static const char kWorldLitFs[] = R"GLSL(#version 330 core
in vec2 v_uv;
in vec3 v_worldPos;
in vec3 v_worldNormal;
uniform sampler2D u_texDiffuse;       // unit 0
uniform float u_alphaTest;            // 0 = off, else discard threshold (0.25)
uniform vec3 u_lightOrigin;
uniform vec3 u_lightDir;
uniform vec3 u_lightColor;
uniform float u_lightRadius;
uniform float u_cosInner;
uniform float u_cosOuter;
uniform mat4 u_matShadow;
uniform sampler2DShadow u_shadowMap;  // unit 2 (bound only when u_hasShadow != 0)
uniform int u_hasShadow;
// L5R crisp direct profile (csz_flashlight_v3). v3=0 -> legacy linear cone (A/B).
uniform float u_v3;              // 1 = analytic crisp profile, 0 = legacy linear cone
uniform float u_edgeExp;         // cone-edge sharpening exponent
uniform float u_hotspotGain;     // central hotspot peak gain
uniform float u_hotspotSharp;    // hotspot tightness (higher = smaller bright core)
uniform float u_directGain;      // direct light-pool brightness multiplier
out vec4 fragColor;
void main()
{
	vec4 base = texture( u_texDiffuse, v_uv );
	if( u_alphaTest > 0.0 && base.a < u_alphaTest )
		discard;
	vec3 L = u_lightOrigin - v_worldPos;
	float d = length( L );
	L /= max( d, 1e-4 );
	float atten = clamp( 1.0 - d / u_lightRadius, 0.0, 1.0 );
	atten *= atten;
	float cosAx = dot( -L, u_lightDir );                 // 1 on the spot axis, falling outward
	// Legacy linear cone (csz_flashlight_v3 0): the pre-L5R uniform-disc falloff, kept for A/B.
	float coneLegacy = clamp(( cosAx - u_cosOuter ) / max( u_cosInner - u_cosOuter, 1e-4 ), 0.0, 1.0 );
	// L5R analytic profile: a crisp pool (smoothstep cone band raised to edgeExp -> sharper
	// boundary) PLUS a genuine central hotspot. The hotspot is measured from the AXIS over the
	// whole cone (smoothstep cosOuter..1) -- NOT the cone band, which saturates to 1 across the
	// inner cone and would make the "hotspot" cover the whole pool instead of peaking at center.
	float edge   = pow( smoothstep( u_cosOuter, u_cosInner, cosAx ), u_edgeExp );
	float axial  = smoothstep( u_cosOuter, 1.0, cosAx );                 // 1 at axis -> 0 at outer rim
	float hotspot = 1.0 + u_hotspotGain * pow( axial, u_hotspotSharp );
	float shaped = mix( coneLegacy, edge * hotspot, u_v3 );
	float gain   = mix( 1.0, u_directGain, u_v3 );
	float ndotl = max( dot( normalize( v_worldNormal ), L ), 0.0 );
	float shadow = 1.0;
	if( u_hasShadow != 0 )
		shadow = textureProj( u_shadowMap, u_matShadow * vec4( v_worldPos, 1.0 ));
	fragColor = vec4( base.rgb * u_lightColor * ( atten * shaped * ndotl * shadow * gain ), 1.0 );
}
)GLSL";

// World depth pass (T7 shadow map): position-only, empty FS (plan 2.4 row 3;
// depth-only FBO has no color attachment, fence-texture alpha test is a
// recorded M1 gap -- masked surfaces cast solid shadows).
static const char kWorldDepthVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
uniform mat4 u_viewProj;
void main()
{
	gl_Position = u_viewProj * vec4( a_pos, 1.0 );
}
)GLSL";

static const char kWorldDepthFs[] = R"GLSL(#version 330 core
void main()
{
}
)GLSL";
