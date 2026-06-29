/*
 * csz_triapi_shaders.inl -- CSOZ renderer: TriAPI emulation GLSL sources
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
// Included ONLY by csz_triapi.cpp. Vertex layout: pos3 (world, straight from
// Vertex3fv) + uv2 + color4. GL3.3 core / GLES3 / WebGL2 intersection only.
static const char kTriVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
uniform mat4 u_viewProj;
out vec2 v_uv;
out vec4 v_color;
out vec3 v_worldPos;
void main()
{
	v_uv = a_uv;
	v_color = a_color;
	v_worldPos = a_pos;	// world-space vertex: analytic fog ray origin
	gl_Position = u_viewProj * vec4( a_pos, 1.0 );
}
)GLSL";

// APPLY_FOG_EXP variant: the same analytic transmittance closed form the world /
// studio / sprite passes use (csz_fog_shaders.inl conventions; the inl ships the
// depth-reconstruct helpers, the apply form is replicated here as in csz_sprite
// since the inl exposes no apply function). u_textured 0 -> color-only (untextured
// TriAPI draw). u_fogAdditive selects the additive fade-to-black form (never add
// fog color into an additive draw) so particleman/beam content darkens with fog
// instead of staying brighter than the foggy world (G-P7 fix).
static const char kTriFs[] = R"GLSL(#version 330 core
in vec2 v_uv;
in vec4 v_color;
in vec3 v_worldPos;
uniform sampler2D u_texDiffuse;   // unit 0
uniform int  u_textured;          // 1 = modulate by the bound texture
uniform vec4 u_fog;               // rgb = fog color (linear), w = extinction a (1/units); w<=0 -> off
uniform vec4 u_fogParams;         // x = height falloff b, z = maxOpacity
uniform vec3 u_camPos;            // camera world position (ray origin)
uniform int  u_fogAdditive;       // 1 on additive blend modes (CPU-selected)
out vec4 fragColor;
float cszFogT( vec3 worldPos, vec3 camPos, float a, float b, float maxOpacity )
{
	if( a <= 0.0 )
		return 1.0;
	vec3 d = worldPos - camPos;
	float t = length( d );
	float rdz = ( t > 1e-4 ) ? d.z / t : 0.0;
	float F;
	if( abs( b ) < 1e-4 )
		F = a * t;
	else if( abs( rdz ) < 1e-4 )
		F = a * exp( -b * camPos.z ) * t;
	else
		F = ( a / b ) * exp( -b * camPos.z ) * ( 1.0 - exp( -b * t * rdz )) / rdz;
	float T = exp( -max( F, 0.0 ));
	return max( T, 1.0 - maxOpacity );
}
void main()
{
	vec4 col = v_color;
	if( u_textured != 0 )
		col *= texture( u_texDiffuse, v_uv );
	float T = cszFogT( v_worldPos, u_camPos, u_fog.w, u_fogParams.x, u_fogParams.z );
	if( u_fogAdditive != 0 )
		col.rgb *= T;			// fade to black, never add fog color
	else
		col.rgb = mix( u_fog.rgb, col.rgb, T );
	fragColor = col;
}
)GLSL";
