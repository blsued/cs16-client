/*
 * csz_decal_shaders.inl -- CSOZ renderer: decal GLSL sources
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
// Included ONLY by csz_decal.cpp. The vertex stream is the exact float[7] layout
// gRenderAPI.R_DecalSetupVerts hands us (pos.xyz + base-uv + lightmap-uv), copied
// verbatim into the per-frame VBO -- so the vertex stage is a plain transform and
// the two UV sets pass straight through. GL3.3 core / GLES3 / WebGL2 intersection
// only (code-standards section 7).
static const char kDecalVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;     // world-space clipped decal vertex
layout(location = 1) in vec2 a_uv;      // decal diffuse uv (unit quad)
layout(location = 2) in vec2 a_lmuv;    // engine lightmap atlas uv (3rd pair)
uniform mat4 u_viewProj;
out vec2 v_uv;
out vec2 v_lmuv;
out vec3 v_worldPos;
void main()
{
	v_uv = a_uv;
	v_lmuv = a_lmuv;
	v_worldPos = a_pos;	// analytic fog ray origin (fog M1 Step 2), same as world/sprite
	gl_Position = u_viewProj * vec4( a_pos, 1.0 );
}
)GLSL";

// Diffuse * face-lightmap, fogged consistently with the world (same analytic
// closed form as csz_sprite / csz_world). Two classes (CPU-selected blend state):
//   u_modulate == 0 (ALPHA class, blood/scorch): SRC_ALPHA blend, drawn pre-light.
//     Fog mixes toward u_fog.rgb; the texture alpha drives the blend.
//   u_modulate != 0 (MODULATE class, classic bullet holes): DST_COLOR x SRC_COLOR
//     blend (the engine's 2x decal blend) drawn post-light. The identity (no
//     visible effect) of that blend is src == 0.5 (2*0.5*dst == dst), so fog
//     fades the decal toward vec3(0.5), NOT toward black/white -- a fully fogged
//     bullet hole leaves the wall untouched. Masked texels (alpha-cutout) are
//     discarded so they stay identity.
// Lightmap modulation is applied to BOTH classes (坑22): the engine's own decal
// path does decal*lightmap and relies on the 2x modulate blend to compensate, so
// matching it keeps the same-session A/B (csz_renderer 0<->1) consistent.
static const char kDecalFs[] = R"GLSL(#version 330 core
in vec2 v_uv;
in vec2 v_lmuv;
in vec3 v_worldPos;
uniform sampler2D u_texDiffuse;   // unit 0: decal diffuse
uniform sampler2D u_texLightmap;  // unit 1: engine lightmap page (when u_hasLightmap)
uniform int  u_hasLightmap;       // 1 = modulate by the face lightmap
uniform int  u_modulate;          // 1 = DST_COLOR x SRC_COLOR class, 0 = SRC_ALPHA class
uniform vec4 u_fog;               // rgb = fog color (linear), w = extinction a (1/units); w<=0 -> off
uniform vec4 u_fogParams;         // x = height falloff b, z = maxOpacity
uniform vec3 u_camPos;            // camera world position (ray origin)
out vec4 fragColor;
// Analytic base-fog transmittance -- the same closed form as csz_sprite_shaders.inl
// and the world/studio base passes (height + distance, |b|<eps and |rd.z|<eps guards).
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
	vec4 d = texture( u_texDiffuse, v_uv );
	vec3 lm = ( u_hasLightmap != 0 ) ? texture( u_texLightmap, v_lmuv ).rgb : vec3( 1.0 );
	vec3 lit = d.rgb * lm;
	float T = cszFogT( v_worldPos, u_camPos, u_fog.w, u_fogParams.x, u_fogParams.z );
	if( u_modulate != 0 )
	{
		if( d.a < 0.5 )
			discard;	// masked decal cutout -> identity (leave the wall untouched)
		vec3 c = mix( vec3( 0.5 ), lit, T );	// fog fades toward the 2x-modulate identity (0.5)
		fragColor = vec4( c, 1.0 );
	}
	else
	{
		vec3 c = mix( u_fog.rgb, lit, T );
		fragColor = vec4( c, d.a );
	}
}
)GLSL";
