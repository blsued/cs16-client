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
uniform mat4 u_viewProj;
uniform mat4 u_model;
out vec2 v_uv;
out vec2 v_lmuv;
out vec3 v_normal;
out vec3 v_worldPos;
void main()
{
	v_uv = a_uv;
	v_lmuv = a_lmuv;
	// World-space normal forwarded raw (BSP face plane normal, baked world-space
	// at build time, csz_world.cpp:330). u_model is not applied: the lit VS
	// (kWorldLitVs) likewise forwards a_normal unrotated, so the base directional
	// term matches the lit pass for moving brush submodels (parity choice).
	v_normal = a_normal;
	// World-space fragment position for the weather wet/snow splice (view vector,
	// fresnel). u_model IS applied here so brush submodels get correct positions.
	v_worldPos = ( u_model * vec4( a_pos, 1.0 )).xyz;
	gl_Position = u_viewProj * ( u_model * vec4( a_pos, 1.0 ));
}
)GLSL";

// Fog + night-tint block (plan 2.6 contract, M2a A1): per-pixel exp2 fog on
// cheap view depth, tint multiplier before the fog mix. Base pass ONLY: the
// lit-additive and depth programs below stay fog-free (clean-room pitfall 23,
// whole-pipeline ruling).
static const char kWorldFs[] = R"GLSL(#version 330 core
in vec2 v_uv;
in vec2 v_lmuv;
in vec3 v_normal;
in vec3 v_worldPos;
uniform sampler2D u_texDiffuse;   // unit 0
uniform sampler2D u_texLightmap;  // unit 1
uniform float u_alphaTest;        // 0 = off, else discard threshold (0.25)
uniform vec4 u_fog;               // rgb = fog color (linear), w = density; w<=0 -> off
uniform vec3 u_ambTint;           // night tint; (1,1,1) neutral
uniform vec3 u_sunDir;            // surface -> dominant body, normalized; base pass only
uniform vec3 u_sunColor;          // intensity-premultiplied light color; (0,0,0) = off
uniform float u_brushAlpha;       // per-entity translucency (curstate.renderamt/255); 1.0 = opaque/world
// Weather surface contract (csz_weather.h WeatherSurfaceState). SAFETY: every
// weather term is gated on u_wetness / u_snowAmount, so when both are 0 the
// shader output is byte-identical to the night-atmosphere baseline.
uniform vec3 u_camPos;            // world-space view origin (view.origin)
uniform float u_wetness;          // 0..1 rain ground wetness (>0 only in rain)
uniform float u_snowAmount;       // 0..1 snow coverage (>0 only in snow)
uniform vec3 u_snowColor;         // linear RGB, already night-cooled & not overexposed
out vec4 fragColor;
void main()
{
	vec4 base = texture( u_texDiffuse, v_uv );
	if( u_alphaTest > 0.0 && base.a < u_alphaTest )
		discard;
	vec3 lm = texture( u_texLightmap, v_lmuv ).rgb;
	vec3 col = base.rgb * lm * ( 2.0 * 128.0 / 192.0 );
	col *= u_ambTint;
	// Shadowless directional sun/moon (Option A, base pass only, pitfall 23):
	// add N.L on top of the baked lightmap before the fog mix. u_sunColor is 0
	// when the publisher hasn't enabled the light, so the term vanishes.
	col += base.rgb * u_sunColor * max( dot( normalize( v_normal ), u_sunDir ), 0.0 );
	// Day-for-night grade: at night the ambient tint is cool (B>R); push warm
	// baked-lightmap/sandstone surfaces toward a cool blue-grey so the WORLD visibly
	// tracks the day/night timeline (not just the sky). Phase-correct & contract-safe:
	// derived purely from u_ambTint's cool-bias, so it is IDENTITY at day (tint white,
	// b-r=0) and at sunset (tint warm, b-r<0 -> 0), and only engages at night/dawn (b>r).
	float csz_night = smoothstep( 0.0, 0.10, u_ambTint.b - u_ambTint.r );
	float csz_l = dot( col, vec3( 0.2126, 0.7152, 0.0722 ));
	vec3  csz_cool = vec3( csz_l ) * vec3( 0.75, 0.92, 1.25 );   // luminance pushed cool-blue
	col = mix( col, csz_cool, csz_night * 0.70 );                // 0.70 = grade strength (tunable)
	// ---- Weather wet-ground / snow-cover splice (after cool grade, before fog).
	// SAFETY CONTRACT: each term is multiplied by u_wetness or u_snowAmount, so
	// with both at 0 every line below is an identity no-op (col unchanged).
	vec3  csz_N = normalize( v_normal );
	vec3  csz_V = normalize( u_camPos - v_worldPos );
	float csz_up = clamp( csz_N.z, 0.0, 1.0 );                   // floors/puddles are up-facing
	float csz_lmLum = dot( lm, vec3( 0.2126, 0.7152, 0.0722 )); // only lit areas get highlights
	if( u_wetness > 0.0 )
	{
		// Wet pooling mostly on near-horizontal surfaces ("积水" on floors/tops).
		float ground = smoothstep( 0.5, 0.9, csz_up );
		float wet = u_wetness * ground;
		// Water absorbs -> darken albedo where wet (reflective dark bands).
		col *= mix( 1.0, 0.65, wet );
		// Grazing fresnel reflection of the sky/fog color, only at low view angles.
		float fres = pow( 1.0 - max( dot( csz_N, csz_V ), 0.0 ), 5.0 );
		vec3  reflCol = u_fog.rgb * 1.1 + u_sunColor * 0.6;
		col = mix( col, reflCol, clamp( fres * wet * 0.6, 0.0, 0.6 ) * ( 0.4 + 0.6 * csz_lmLum ));
		// Tight specular glint from the directional light on wet floor, gated lit.
		vec3  H = normalize( u_sunDir + csz_V );
		float spec = pow( max( dot( csz_N, H ), 0.0 ), 80.0 );
		col += u_sunColor * spec * wet * ( 0.3 + csz_lmLum );
	}
	if( u_snowAmount > 0.0 )
	{
		// Accumulate on up-facing surfaces: box tops, steps, ground get more;
		// walls get ~none. u_snowColor is pre-cooled & not overexposed (the
		// "暗处不过曝" requirement is handled upstream in WeatherRenderer).
		float topMask = smoothstep( 0.35, 0.85, csz_up );
		float snow = u_snowAmount * topMask;
		// Near brighter than far: like the wetland branch, modulate the snow
		// albedo by the baked lightmap (lit ground reads brighter than shaded)
		// AND by a cheap distance falloff so the snow sheet recedes into the
		// fog instead of staying a flat bright slab. Both terms are <= 1 so
		// snow never over-brightens past u_snowColor.
		float csz_snowDist = clamp( 1.0 - ( gl_FragCoord.z / gl_FragCoord.w ) / 1400.0, 0.45, 1.0 );
		float csz_snowLit = ( 0.55 + 0.45 * csz_lmLum ) * csz_snowDist;
		col = mix( col, u_snowColor * csz_snowLit, clamp( snow, 0.0, 0.9 ));
	}
	float fogDepth = gl_FragCoord.z / gl_FragCoord.w;      // cheap view depth (clean-room f)
	float fogF = ( u_fog.w > 0.0 ) ? clamp( exp2( -u_fog.w * fogDepth ), 0.0, 1.0 ) : 1.0;
	fragColor = vec4( mix( u_fog.rgb, col, fogF ), base.a * u_brushAlpha );
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
	float cone = clamp(( dot( -L, u_lightDir ) - u_cosOuter ) / max( u_cosInner - u_cosOuter, 1e-4 ), 0.0, 1.0 );
	float ndotl = max( dot( normalize( v_worldNormal ), L ), 0.0 );
	float shadow = 1.0;
	if( u_hasShadow != 0 )
		shadow = textureProj( u_shadowMap, u_matShadow * vec4( v_worldPos, 1.0 ));
	fragColor = vec4( base.rgb * u_lightColor * ( atten * cone * ndotl * shadow ), 1.0 );
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
