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
void main()
{
	int b = a_bone * 3;
	vec4 p = vec4( a_pos, 1.0 );
	vec3 worldPos = vec3( dot( u_bones[b], p ), dot( u_bones[b + 1], p ), dot( u_bones[b + 2], p ));
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
uniform sampler2D u_texDiffuse;   // unit 0
uniform float u_alphaTest;        // 0 = off, else discard threshold (0.25)
uniform vec3 u_ambient;
uniform vec3 u_shadeColor;
uniform vec3 u_shadeDir;
uniform vec4 u_fog;               // rgb = fog color (linear), w = density; w<=0 -> off
uniform vec3 u_ambTint;           // night tint; (1,1,1) neutral
uniform vec3 u_sunDir;            // surface -> dominant body, normalized; base pass only
uniform vec3 u_sunColor;          // intensity-premultiplied light color; (0,0,0) = off
out vec4 fragColor;
void main()
{
	vec4 base = texture( u_texDiffuse, v_uv );
	if( u_alphaTest > 0.0 && base.a < u_alphaTest )
		discard;
	vec3 n = normalize( v_normal );
	float ndl = max( dot( n, u_shadeDir ), 0.0 );
	vec3 col = base.rgb * ( u_ambient + u_shadeColor * ndl );
	col *= u_ambTint;
	// Shadowless directional sun/moon (Option A, base pass only, pitfall 23):
	// add N.L on top of the model's own lambert before the fog mix. v_normal is
	// bone-transformed to world space (kStudioVs), same space as u_sunDir.
	// u_sunColor is 0 when the body light is off, so the term vanishes.
	col += base.rgb * u_sunColor * max( dot( n, u_sunDir ), 0.0 );
	float fogDepth = gl_FragCoord.z / gl_FragCoord.w;      // cheap view depth (clean-room f)
	float fogF = ( u_fog.w > 0.0 ) ? clamp( exp2( -u_fog.w * fogDepth ), 0.0, 1.0 ) : 1.0;
	fragColor = vec4( mix( u_fog.rgb, col, fogF ), 1.0 );
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
