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
out vec3 v_worldPos;
void main()
{
	v_uv = a_uv;
	v_color = a_color;
	v_worldPos = a_pos;	// world-space billboard corner: analytic fog ray origin (fog M1 Step 2)
	gl_Position = u_viewProj * vec4( a_pos, 1.0 );
}
)GLSL";

// Plain modulate: blending (additive / alpha) is fixed-function state chosen
// per rendermode on the CPU side (engine-parity, see csz_sprite.cpp).
// Fog block (plan 2.6 contract; analytic base fog fog M1 Step 2): sprites are
// emitters, so NO u_ambTint and NO sun-glow in-scatter here; u_fogAdditive
// selects between the alpha-blended mix and the additive fade-to-black form
// (never add fog color into an additive draw). Sprites draw AFTER the kTmVolume
// seam and write no readable depth, so their fog stays in-shader (spec 3.2);
// they consume the SAME analytic transmittance as world/studio so black-fog
// density/maxOpacity thicken sprite fog consistently.
static const char kSpriteFs[] = R"GLSL(#version 330 core
in vec2 v_uv;
in vec4 v_color;
in vec3 v_worldPos;
uniform sampler2D u_texDiffuse;   // unit 0
uniform vec4 u_fog;               // rgb = fog color (linear), w = extinction a (1/units); w<=0 -> off
uniform vec4 u_fogParams;         // x = height falloff b, y = sun glow (unused for emitters), z = maxOpacity
uniform vec3 u_camPos;            // camera world position (ray origin)
uniform int u_fogAdditive;        // 1 on additive blend modes (CPU-selected)
uniform float u_alphaTest;        // >0 -> hard cutout discard at this coverage (kRenderNormal .spr); <=0 off
out vec4 fragColor;
// Analytic base-fog transmittance (fog M1 spec 4.3) -- same closed form as the
// world/studio base passes (height+distance, |b|<eps and |rd.z|<eps guards).
float cszFogT( vec3 worldPos, vec3 camPos, float a, float b, float maxOpacity )
{
	if( a <= 0.0 )
		return 1.0;
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
	return max( T, 1.0 - maxOpacity );
}
void main()
{
	vec4 col = texture( u_texDiffuse, v_uv ) * v_color;
	// Hard alpha-test cutout for kRenderNormal .spr (GL3 core has no fixed-function
	// alpha test): discard sub-threshold coverage so edges are crisp -- no soft halo,
	// no black box. v_color.a is 1 for normal mode, so col.a is the texture coverage.
	if( u_alphaTest > 0.0 && col.a < u_alphaTest )
		discard;
	float T = cszFogT( v_worldPos, u_camPos, u_fog.w, u_fogParams.x, u_fogParams.z );
	if( u_fogAdditive != 0 )
		col.rgb *= T;                                      // fade to black, never add fog color
	else
		col.rgb = mix( u_fog.rgb, col.rgb, T );
	fragColor = col;
}
)GLSL";
