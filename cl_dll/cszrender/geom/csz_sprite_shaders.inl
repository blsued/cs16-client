/*
 * csz_sprite_shaders.inl -- CSOZ renderer: sprite GLSL sources
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
// Included ONLY by csz_sprite.cpp (plan section 2.1). Attribute locations
// and uniform names follow the plan 2.4 sprite contract. The quad corners
// are computed on the CPU (world-space billboard), so the vertex stage is a
// plain transform. GLES3 equivalence assumption: GL3.3 core / GLES3 /
// WebGL2 intersection only (code-standards section 7).
static const char kSpriteVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
uniform mat4 u_viewProj;
out vec2 v_uv;
out vec4 v_color;
void main()
{
	v_uv = a_uv;
	v_color = a_color;
	gl_Position = u_viewProj * vec4( a_pos, 1.0 );
}
)GLSL";

// Plain modulate: blending (additive / alpha) is fixed-function state chosen
// per rendermode on the CPU side (engine-parity, see csz_sprite.cpp).
// Fog block (plan 2.6 contract, M2a A1): sprites are emitters, so NO
// u_ambTint here; u_fogAdditive selects between the alpha-blended mix and the
// additive fade-to-black form (never add fog color into an additive draw).
static const char kSpriteFs[] = R"GLSL(#version 330 core
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_texDiffuse;   // unit 0
uniform vec4 u_fog;               // rgb = fog color (linear), w = density; w<=0 -> off
uniform int u_fogAdditive;        // 1 on additive blend modes (CPU-selected)
out vec4 fragColor;
void main()
{
	vec4 col = texture( u_texDiffuse, v_uv ) * v_color;
	float fogDepth = gl_FragCoord.z / gl_FragCoord.w;      // cheap view depth (clean-room f)
	float fogF = ( u_fog.w > 0.0 ) ? clamp( exp2( -u_fog.w * fogDepth ), 0.0, 1.0 ) : 1.0;
	if( u_fogAdditive != 0 )
		col.rgb *= fogF;                                   // fade to black, never add fog color
	else
		col.rgb = mix( u_fog.rgb, col.rgb, fogF );
	fragColor = col;
}
)GLSL";
