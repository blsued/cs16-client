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
static const char kStudioFs[] = R"GLSL(#version 330 core
in vec2 v_uv;
in vec3 v_normal;
uniform sampler2D u_texDiffuse;   // unit 0
uniform float u_alphaTest;        // 0 = off, else discard threshold (0.25)
uniform vec3 u_ambient;
uniform vec3 u_shadeColor;
uniform vec3 u_shadeDir;
out vec4 fragColor;
void main()
{
	vec4 base = texture( u_texDiffuse, v_uv );
	if( u_alphaTest > 0.0 && base.a < u_alphaTest )
		discard;
	vec3 n = normalize( v_normal );
	float ndl = max( dot( n, u_shadeDir ), 0.0 );
	fragColor = vec4( base.rgb * ( u_ambient + u_shadeColor * ndl ), 1.0 );
}
)GLSL";
