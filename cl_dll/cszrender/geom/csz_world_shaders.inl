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
uniform vec4 u_fog;               // rgb = fog color (linear), w = extinction a (1/units); w<=0 -> off
uniform vec4 u_fogParams;         // x = height falloff b, y = sun glow, z = maxOpacity, w = reserved
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
out vec4 fragColor;
// Analytic base-fog transmittance (fog M1 spec 4.3): closed-form integral of an
// exponential-height density d(z)=a*e^(-b*z) along the camera->surface ray, with
// BOTH mandatory guards -- |b|<eps (uniform density, divide-by-b blowup) and
// |rd.z|<eps (near-horizontal ray, divide-by-rd.z blowup). World up axis is Z.
// Returns T in [0,1]; the server maxOpacity floor clamps the fog amount.
float cszFogT( vec3 worldPos, vec3 camPos, float a, float b, float maxOpacity )
{
	if( a <= 0.0 )
		return 1.0;                                          // density<=0 -> fog off (legacy parity)
	vec3 d = worldPos - camPos;
	float t = length( d );
	float rdz = ( t > 1e-4 ) ? d.z / t : 0.0;
	float F;
	if( abs( b ) < 1e-4 )
		F = a * t;                                           // b->0 uniform density (divide-by-b guard)
	else if( abs( rdz ) < 1e-4 )
		F = a * exp( -b * camPos.z ) * t;                    // near-horizontal ray (divide-by-rd.z guard)
	else
		F = ( a / b ) * exp( -b * camPos.z ) * ( 1.0 - exp( -b * t * rdz )) / rdz;
	float T = exp( -max( F, 0.0 ));
	return max( T, 1.0 - maxOpacity );                       // server reveal floor (silhouettes/blackout)
}
void main()
{
	vec4 base = texture( u_texDiffuse, v_uv );
	if( u_alphaTest > 0.0 && base.a < u_alphaTest )
		discard;
	vec3 lm = texture( u_texLightmap, v_lmuv ).rgb;
	vec3 col = base.rgb * lm * ( 2.0 * 128.0 / 192.0 );
	// Night flag, hoisted here so the indoor sky-occlusion block (below) and the
	// day-for-night grade (further down) share one derivation. Derived purely
	// from u_ambTint's cool-bias: IDENTITY at day (tint white, b-r=0) and at
	// sunset (tint warm, b-r<0 -> 0); only engages at night/dawn (b>r).
	float csz_night = smoothstep( 0.0, 0.10, u_ambTint.b - u_ambTint.r );
	// Indoor sky-occlusion (Chunk C, defect 8): the engine has no per-texel
	// sky-visibility term, so at night a sky-occluded (indoor) surface was as
	// bright as the open outdoors. Reuse the already-sampled baked style-0
	// lightmap luminance as a DAYTIME sky-access proxy: a brightly baked surface
	// saw sun/sky (outdoor -> csz_sky~1), a dimly baked one is sheltered
	// (indoor -> csz_sky~0). Gated by csz_night so DAYTIME stays EXACT identity
	// (the bake already encodes daytime occlusion -- no double-darkening).
	// All multiplicative, so it composes with Chunk A's phase-scaled moonlight
	// (which rides in via u_sunColor). User decision: DARK / flashlight-required.
	// Honest limitation: bake luma conflates skylight with author/artificial
	// fill light (a brightly lit indoor room reads as outdoor) -- mitigated by
	// the tunable window; a true sky-style lightmap is the deferred hi-fi upgrade.
	float csz_lmLum = dot( lm, vec3( 0.2126, 0.7152, 0.0722 ));
	const float CSZ_SKY_LO = 0.15, CSZ_SKY_HI = 0.50;   // bake-luma window: below=indoor, above=outdoor
	float csz_sky = smoothstep( CSZ_SKY_LO, CSZ_SKY_HI, csz_lmLum );
	const float CSZ_INDOOR_AMB = 0.25;                  // night indoor ambient floor (DARK end)
	float csz_amb = mix( 1.0, mix( CSZ_INDOOR_AMB, 1.0, csz_sky ), csz_night );
	// L3b: sky-ambient cloud dimming. SEPARATE scalar multiplied AFTER csz_night is
	// derived from the RAW u_ambTint above (line ~136) -- folding it into u_ambTint
	// would drift b-r and break the day-for-night gate. 1.0 = clear sky (identity);
	// CPU clamps the floor at 0.6 so the scene stays readable under heavy cloud.
	col *= u_ambTint * csz_amb * u_skyAmbScale;
	// Shadowless directional sun/moon (Option A, base pass only, pitfall 23):
	// add N.L on top of the baked lightmap before the fog mix. u_sunColor is 0
	// when the publisher hasn't enabled the light, so the term vanishes.
	// Gated by sky-access (scoped to night via the outer mix): outdoor gets the
	// full unshadowed moon/sun, indoor only a faint reflected fraction; dusk/dawn
	// indoor is NOT regressed (csz_night~0 there -> csz_moon=1, identity).
	const float CSZ_INDOOR_MOON = 0.08;                 // night indoor directional floor (DARK end)
	float csz_moon = mix( 1.0, mix( CSZ_INDOOR_MOON, 1.0, csz_sky ), csz_night );
	col += base.rgb * u_sunColor * max( dot( normalize( v_normal ), u_sunDir ), 0.0 ) * csz_moon;
	// Day-for-night grade: at night the ambient tint is cool (B>R); push warm
	// baked-lightmap/sandstone surfaces toward a cool blue-grey so the WORLD visibly
	// tracks the day/night timeline (not just the sky). Reuses csz_night (above).
	float csz_l = dot( col, vec3( 0.2126, 0.7152, 0.0722 ));
	vec3  csz_cool = vec3( csz_l ) * vec3( 0.75, 0.92, 1.25 );   // luminance pushed cool-blue
	col = mix( col, csz_cool, csz_night * 0.70 );                // 0.70 = grade strength (tunable)
	// Analytic base fog (fog M1 Step 2): single extinction T applied exactly once
	// (replaces the old per-pixel exp2). In-scatter = fog color plus a forward
	// directional sun/moon glow (cheap phase pow; sunGlow=0 -> plain fog mix, the
	// legacy look). Composited in linear HDR so it is correct with ACES on or off.
	vec3 toFrag = v_worldPos - u_camPos;
	float tLen = length( toFrag );
	vec3 rd = ( tLen > 1e-4 ) ? toFrag / tLen : vec3( 0.0 );    // guard normalize-of-zero (NaN)
	float T = cszFogT( v_worldPos, u_camPos, u_fog.w, u_fogParams.x, u_fogParams.z );
	float cosT = max( dot( rd, u_sunDir ), 0.0 );                // toward the moon = +1
	float glow = pow( cosT, 8.0 ) * u_fogParams.y;               // legacy cheap forward glow (unchanged)
	vec3 inscatter = u_fog.rgb + u_sunColor * glow;
	// fog M1 L4 -- OBVIOUS moonlight Tyndall: Henyey-Greenstein forward single
	// in-scatter (g=0.8) of the DEDICATED moon channel (u_moonInScatter, decoupled
	// from the surface-coupled u_sunColor so brightening the air never lifts the
	// ground). Looking toward the moon the fog visibly halos. Gated by the cloud-gap
	// shaftMask (thick cloud suppresses) and the csz_moonshaft master toggle. Energy
	// stays bounded: the whole in-scatter is weighted by (1-T), the out-scattered
	// fraction the base-fog extinction already removed, so sigma_s <= sigma_t holds
	// and brightness can never run past the fog's own opacity budget.
	const float CSZ_HG_G = 0.8;
	float hgDen = 1.0 + CSZ_HG_G * CSZ_HG_G - 2.0 * CSZ_HG_G * cosT;
	float hg = ( 1.0 - CSZ_HG_G * CSZ_HG_G ) / ( 4.0 * 3.14159265 * pow( max( hgDen, 1e-4 ), 1.5 ) );
	inscatter += ( u_moonShaft * u_shaftMask * hg ) * u_moonInScatter;
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
out vec2 v_uv;
out vec3 v_worldPos;
out vec3 v_worldNormal;
void main()
{
	v_uv = a_uv;
	v_worldPos = a_pos;
	v_worldNormal = a_normal;
	gl_Position = u_viewProj * vec4( a_pos, 1.0 );
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
