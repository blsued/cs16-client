/*
 * csz_studio_shaders.inl -- CSOZ renderer: studio GLSL sources
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
// Included ONLY by csz_studio.cpp (plan section 2.1). Attribute locations
// and uniform names follow the plan 2.4 studio contract; u_alphaTest /
// u_chrome / u_viewRight / u_viewUp are T3 additions documented in
// progress-t3.md (STUDIO_NF_MASKED alpha test + chrome approximation).
// GLES3 equivalence assumption: GL3.3 core / GLES3 / WebGL2 intersection
// only (code-standards section 7).

// GPU skinning: u_bones holds 3 vec4 rows per bone (world-from-bone 3x4,
// row vectors); 128 bones x 3 = 384 vec4 (glcaps floor 1664 components).
static const char kStudioVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in int a_bone;
uniform mat4 u_viewProj;
uniform vec4 u_bones[384];
uniform int u_chrome;       // chrome texture: sphere-map UV from view basis
uniform vec3 u_viewRight;
uniform vec3 u_viewUp;
out vec2 v_uv;
out vec3 v_normal;
out vec3 v_worldPos;
void main()
{
	int b = a_bone * 3;
	vec4 p = vec4( a_pos, 1.0 );
	vec3 worldPos = vec3( dot( u_bones[b], p ), dot( u_bones[b + 1], p ), dot( u_bones[b + 2], p ));
	v_worldPos = worldPos;	// analytic base fog (fog M1 Step 2): camera->surface ray origin
	vec3 n = vec3( dot( u_bones[b].xyz, a_normal ),
	               dot( u_bones[b + 1].xyz, a_normal ),
	               dot( u_bones[b + 2].xyz, a_normal ));
	v_normal = n;
	if( u_chrome != 0 )
	{
		vec3 nn = normalize( n );
		v_uv = vec2( 0.5 + 0.5 * dot( nn, u_viewRight ), 0.5 - 0.5 * dot( nn, u_viewUp ));
	}
	else
	{
		v_uv = a_uv;
	}
	gl_Position = u_viewProj * vec4( worldPos, 1.0 );
}
)GLSL";

// Classic two-term lambert (plan 2.4): tex * (ambient + shade * max(N.L, 0)).
// Fullbright meshes are drawn with u_ambient=1 / u_shadeColor=0 (no extra
// uniform). Light color already carries the world-parity gamma + overbright
// factor (see csz_studio.cpp SampleEntityLight).
// Fog + night-tint block (plan 2.6 contract, M2a A1) on the base pass ONLY:
// the lit-additive and depth programs below stay fog-free (clean-room pitfall
// 23, whole-pipeline ruling). The viewmodel rides this same program and gets
// fog for free (fogDepth ~ 0 -> visually fog-free at arm's reach).
static const char kStudioFs[] = R"GLSL(#version 330 core
in vec2 v_uv;
in vec3 v_normal;
in vec3 v_worldPos;
uniform sampler2D u_texDiffuse;   // unit 0
uniform float u_alphaTest;        // 0 = off, else discard threshold (0.25)
uniform vec3 u_ambient;
uniform vec3 u_shadeColor;
uniform vec3 u_shadeDir;
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
uniform float u_skyVis;           // S1: per-entity geometric sky visibility [0,1] (1=outdoor); sampled at
                                  // the entity origin. Plumbed but NOT consumed yet (S2 replaces the
                                  // u_ambient luma proxy below with it); defaults to 1.0 (fail-safe).
// fog M1 L4 -- moonlight Tyndall air-glow (forward HG fog in-scatter); mirrors the
// world base pass so both surfaces scatter identically. All three default to the
// no-op identity so at csz_moonshaft 0 the in-scatter is byte-for-byte pre-L4.
uniform vec3  u_moonInScatter;    // L2 moonFogInScatter * intensity (premultiplied, linear); (0,0,0)=off
uniform float u_shaftMask;        // L3a cloud-gap gating: gap=1, thick cloud=0; 1.0 neutral
uniform float u_moonShaft;        // csz_moonshaft master toggle: 1=enhanced glow, 0=exact pre-L4
// S2 physical night model (REWORK-SPEC §S2, findings 1,2,7,9; codex S2 red-team v1).
// Mirrors the world base pass with STUDIO-specific calibration (finding 9). Defaults
// reproduce 3fd8b7e: u_nightModel 0 = legacy path, u_nightness 0 (day/sunset) -> approvedDay identical.
uniform float u_nightModel;       // 1 = new physical model, 0 = legacy 3fd8b7e (A/B revert)
uniform float u_nightness;        // explicit phase gate [0,1], phase-curve driven (codex P0; replaces u_ambTint.b-r)
uniform vec3  u_sunWarmColor;     // warm SUN-ONLY premul directional (ungated; codex P2b); dir = -u_moonDir (antipode)
uniform vec3  u_moonDir;          // surface -> moon (pure antipode L vector) for the gated night moon term
uniform vec3  u_moonColor;        // moon-only premultiplied directional color (0 when the moon is down)
uniform vec3  u_nightSky;         // studio night sky-ambient color (premul), modulated by pow(u_skyVis,k)
uniform vec3  u_nightFloor;       // studio competitive readable ambient floor (premul)
uniform float u_nightK;           // skyVis exponent k for studio (per-entity)
uniform float u_nightMoon;        // gain on the skyVis-GATED moon directional (wallhack-fixed moonlight)
out vec4 fragColor;
// S3 procedural 2D value noise (finding 6: 2D only, no glTexImage3D -> in-shader hash
// noise, pure ALU). Identical to the world base pass so both surfaces eat fog the same way.
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
	vec2 u = f * f * ( 3.0 - 2.0 * f );
	float a = cszHash21( i );
	float b = cszHash21( i + vec2( 1.0, 0.0 ));
	float c = cszHash21( i + vec2( 0.0, 1.0 ));
	float d = cszHash21( i + vec2( 1.0, 1.0 ));
	return mix( mix( a, b, u.x ), mix( c, d, u.x ), u.y );
}
float cszFogNoise( vec2 worldXY, float scale, float wind, float t )
{
	vec2 drift = vec2( wind * t, wind * t * 0.6 );
	vec2 q = worldXY * scale + drift * scale;
	return 0.65 * cszVNoise( q ) + 0.35 * cszVNoise( q * 2.03 + 7.1 );
}
// S3 per-channel analytic transmittance (REWORK-SPEC §S3.1, IQ model) -- closed-form
// exponential height+distance with the |b|<eps and |rd.z|<eps guards, a-independent path
// factor G scaled by PER-CHANNEL extinction, Start/Cutoff bounds. World up axis is Z.
// (Identical to the world base pass so both surfaces eat fog the same way.)
vec3 cszFogT3( vec3 worldPos, vec3 camPos, vec3 aRGB, float b, float maxOpacity,
	float startD, float cutoffD, float densMul )
{
	if( aRGB.g <= 0.0 )
		return vec3( 1.0 );
	vec3 d = worldPos - camPos;
	float t = length( d );
	float rdz = ( t > 1e-4 ) ? d.z / t : 0.0;
	float t0 = clamp( startD, 0.0, t );
	float t1 = ( cutoffD > 0.0 ) ? min( t, cutoffD ) : t;
	if( t1 <= t0 )
		return vec3( 1.0 );
	float G;
	if( abs( b ) < 1e-4 )
		G = ( t1 - t0 );                                     // uniform density (divide-by-b guard)
	else if( abs( rdz ) < 1e-4 )
		G = exp( -b * camPos.z ) * ( t1 - t0 );              // near-horizontal ray (divide-by-rd.z guard)
	else
		G = ( 1.0 / b ) * exp( -b * camPos.z ) * ( exp( -b * t0 * rdz ) - exp( -b * t1 * rdz )) / rdz;
	G = max( G, 0.0 ) * densMul;
	vec3 F = aRGB * G;
	vec3 T = exp( -max( F, vec3( 0.0 )));
	return max( T, vec3( 1.0 - maxOpacity ));
}
void main()
{
	vec4 base = texture( u_texDiffuse, v_uv );
	if( u_alphaTest > 0.0 && base.a < u_alphaTest )
		discard;
	vec3 n = normalize( v_normal );
	float ndl = max( dot( n, u_shadeDir ), 0.0 );
	vec3 albedo = base.rgb;
	vec3 modelLit = albedo * ( u_ambient + u_shadeColor * ndl );   // baked two-term lambert
	vec3 col;
	// S2 variable darkness (REWORK-SPEC §S2). u_nightModel master A/B: 1 = new
	// physical model, 0 = byte-identical pre-S2 (3fd8b7e). Uniform branch = coherent.
	if( u_nightModel > 0.5 )
	{
		// === New physical night model (findings 1,2,7,9; codex P0/P1a/P2b) ===
		// Shared SPLIT directional (codex P2b): WARM SUN ungated (dusk/dawn, finding 2;
		// dir = pure antipode -u_moonDir), COOL MOON skyVis-GATED (no wall leak at any
		// nightness). Both branches below use this same pair -> no ungated moonlight.
		float sv = clamp( u_skyVis, 0.0, 1.0 );
		vec3  sunWarm  = albedo * u_sunWarmColor * max( dot( n, -u_moonDir ), 0.0 );             // warm sun, UNGATED
		vec3  moonTerm = albedo * u_moonColor    * max( dot( n,  u_moonDir ), 0.0 ) * sv * u_nightMoon; // cool moon, skyVis-GATED
		// (1) APPROVED DAY LOOK -- the 3fd8b7e studio ambient + indoor floor expression
		// VERBATIM (codex P1a: NO tint reconstruction / phase gain), with the directional
		// REPLACED by the split sunWarm+moonTerm above (codex P2b). Studio has NO
		// day-for-night cool grade in 3fd8b7e (finding 8 unifies that in S4 as a post). At
		// the nightness=0 phases the moon is down so u_moonColor=0 and sunWarm == the old
		// blended directional -> approvedDay byte-identical to model0.
		vec3  approvedDay = modelLit;
		float csz_night = smoothstep( 0.0, 0.10, u_ambTint.b - u_ambTint.r );
		float csz_lmLum = dot( u_ambient, vec3( 0.2126, 0.7152, 0.0722 ));
		const float CSZ_SKY_LO = 0.25, CSZ_SKY_HI = 0.90;   // u_ambient window (overbright): below=indoor, above=outdoor
		float csz_sky = smoothstep( CSZ_SKY_LO, CSZ_SKY_HI, csz_lmLum );
		const float CSZ_INDOOR_AMB = 0.25;                  // night indoor ambient floor (DARK end)
		float csz_amb = mix( 1.0, mix( CSZ_INDOOR_AMB, 1.0, csz_sky ), csz_night );
		approvedDay *= u_ambTint * csz_amb * u_skyAmbScale;
		approvedDay += sunWarm + moonTerm;                  // P2b split directional (was blended u_sunColor * csz_moon)
		// (2) PHYSICAL NIGHT -- spatialized by the per-entity geometric u_skyVis (finding 7
		// replaces the u_ambient luma proxy; finding 9: STUDIO-specific k + floor, NOT
		// shared with the world). Indoor (skyVis~0) -> cool readable floor so enemy models
		// stay discernible; open -> night sky-ambient + the same skyVis-GATED moon directional.
		vec3  nightAmb = u_nightSky * pow( sv, u_nightK ) + u_nightFloor;
		vec3  physicalNight = albedo * nightAmb + sunWarm + moonTerm;
		// (3) variable darkness via the explicit phase-curve gate u_nightness (codex P0).
		col = mix( approvedDay, physicalNight, clamp( u_nightness, 0.0, 1.0 ));
	}
	else
	{
		// === Legacy pre-S2 (3fd8b7e) -- byte-identical A/B fallback ===
		col = modelLit;
		// Indoor sky-occlusion via the per-entity baked u_ambient luma as the DAYTIME
		// sky-access proxy (window shifted up because u_ambient carries overbright).
		float csz_night = smoothstep( 0.0, 0.10, u_ambTint.b - u_ambTint.r );
		float csz_lmLum = dot( u_ambient, vec3( 0.2126, 0.7152, 0.0722 ));
		const float CSZ_SKY_LO = 0.25, CSZ_SKY_HI = 0.90;   // u_ambient window (overbright): below=indoor, above=outdoor
		float csz_sky = smoothstep( CSZ_SKY_LO, CSZ_SKY_HI, csz_lmLum );
		const float CSZ_INDOOR_AMB = 0.25;                  // night indoor ambient floor (DARK end)
		float csz_amb = mix( 1.0, mix( CSZ_INDOOR_AMB, 1.0, csz_sky ), csz_night );
		col *= u_ambTint * csz_amb * u_skyAmbScale;
		// Shadowless directional sun/moon (Option A), sky-access scaled.
		const float CSZ_INDOOR_MOON = 0.08;                 // night indoor directional floor (DARK end)
		float csz_moon = mix( 1.0, mix( CSZ_INDOOR_MOON, 1.0, csz_sky ), csz_night );
		col += albedo * u_sunColor * max( dot( n, u_sunDir ), 0.0 ) * csz_moon;
	}
	// S3 analytic fog (REWORK-SPEC §S3, findings 5/6/12): ONE in-scatter equation, mirrors
	// the world base pass so players/models fog identically. Per-channel extinction + 2D
	// noise density + HG directional lobe blending the cool base fog color toward the sun/
	// moon lit color. fogStart keeps the viewmodel (arm's reach) crisp. Retires the old
	// achromatic-gray fold and the pow(cosT,8) sunGlow (HG replaces it -- finding 5).
	vec3 toFrag = v_worldPos - u_camPos;
	float tLen = length( toFrag );
	vec3 rd = ( tLen > 1e-4 ) ? toFrag / tLen : vec3( 0.0 );      // guard normalize-of-zero (NaN)
	float dens = 1.0;
	if( u_fogParams2.w > 0.0 )
		dens = mix( 1.0, 2.0 * cszFogNoise( v_worldPos.xy, u_fogParams3.x, u_fogParams3.y, u_fogParams3.w ), u_fogParams2.w );
	vec3 aRGB = u_fog.w * u_fogParams2.xyz;                       // per-channel extinction (b_ch = a * tint)
	vec3 T = cszFogT3( v_worldPos, u_camPos, aRGB, u_fogParams.x, u_fogParams.z,
		u_fogParams.w, u_fogParams3.z, dens );
	float cosT = dot( rd, u_sunDir );
	float g = u_fogParams.y;
	float hgDen = 1.0 + g * g - 2.0 * g * cosT;
	float hg = ( 1.0 - g * g ) / pow( max( hgDen, 1e-4 ), 1.5 );  // unnormalized HG (isotropic == 1)
	float sunAmount = clamp(( hg - 1.0 ) * 0.5, 0.0, 1.0 );
	vec3 inscatter = mix( u_fog.rgb, u_fogLit, sunAmount );
	// fog M1 L4 moonshaft Tyndall (independent enhancement, gated by u_moonShaft -- default
	// 0 = no-op; finding 12). Weighted by (1-T) so sigma_s<=sigma_t (no air-glow runaway).
	const float CSZ_HG_G = 0.8;
	float hgD2 = 1.0 + CSZ_HG_G * CSZ_HG_G - 2.0 * CSZ_HG_G * max( cosT, 0.0 );
	float hg2 = ( 1.0 - CSZ_HG_G * CSZ_HG_G ) / ( 4.0 * 3.14159265 * pow( max( hgD2, 1e-4 ), 1.5 ));
	inscatter += ( u_moonShaft * u_shaftMask * hg2 ) * u_moonInScatter;
	col = col * T + inscatter * ( 1.0 - T );
	fragColor = vec4( col, 1.0 );
}
)GLSL";

// Studio lit-additive pass (T6): same skinning/attributes as the base pass
// plus a world-position varying; FS is the plan 2.4 spot formula (same
// implementation as the world family).
static const char kStudioLitVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in int a_bone;
uniform mat4 u_viewProj;
uniform vec4 u_bones[384];
uniform int u_chrome;       // chrome texture: sphere-map UV from view basis
uniform vec3 u_viewRight;
uniform vec3 u_viewUp;
out vec2 v_uv;
out vec3 v_worldPos;
out vec3 v_worldNormal;
void main()
{
	int b = a_bone * 3;
	vec4 p = vec4( a_pos, 1.0 );
	vec3 worldPos = vec3( dot( u_bones[b], p ), dot( u_bones[b + 1], p ), dot( u_bones[b + 2], p ));
	vec3 n = vec3( dot( u_bones[b].xyz, a_normal ),
	               dot( u_bones[b + 1].xyz, a_normal ),
	               dot( u_bones[b + 2].xyz, a_normal ));
	v_worldPos = worldPos;
	v_worldNormal = n;
	if( u_chrome != 0 )
	{
		vec3 nn = normalize( n );
		v_uv = vec2( 0.5 + 0.5 * dot( nn, u_viewRight ), 0.5 - 0.5 * dot( nn, u_viewUp ));
	}
	else
	{
		v_uv = a_uv;
	}
	gl_Position = u_viewProj * vec4( worldPos, 1.0 );
}
)GLSL";

static const char kStudioLitFs[] = R"GLSL(#version 330 core
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
// L5R crisp direct profile (csz_flashlight_v3) -- mirrors the world lit FS so studio
// meshes (players, viewmodel-adjacent props) catch the same pool + hotspot. v3=0 -> legacy.
uniform float u_v3;
uniform float u_edgeExp;
uniform float u_hotspotGain;
uniform float u_hotspotSharp;
uniform float u_directGain;
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
	float coneLegacy = clamp(( cosAx - u_cosOuter ) / max( u_cosInner - u_cosOuter, 1e-4 ), 0.0, 1.0 );
	// Crisp pool (sharpened cone band) + central hotspot measured from the axis (see world FS note).
	float edge   = pow( smoothstep( u_cosOuter, u_cosInner, cosAx ), u_edgeExp );
	float axial  = smoothstep( u_cosOuter, 1.0, cosAx );
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

// Studio depth pass (T7 shadow map): skinned position only, empty FS
// (plan 2.4 row 3: locations 0 a_pos + 3 a_bone, the mesh VAO layout keeps
// normals/uv at 1/2 which this program simply does not read).
static const char kStudioDepthVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 3) in int a_bone;
uniform mat4 u_viewProj;
uniform vec4 u_bones[384];
void main()
{
	int b = a_bone * 3;
	vec4 p = vec4( a_pos, 1.0 );
	vec3 worldPos = vec3( dot( u_bones[b], p ), dot( u_bones[b + 1], p ), dot( u_bones[b + 2], p ));
	gl_Position = u_viewProj * vec4( worldPos, 1.0 );
}
)GLSL";

static const char kStudioDepthFs[] = R"GLSL(#version 330 core
void main()
{
}
)GLSL";
