/*
 * csz_beam_shaders.inl -- CSOZ renderer: self-drawn beam GLSL (C-BEAM)
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
// Included ONLY by geom/csz_beam.cpp. GL3.3 core / GLES3 / WebGL2 intersection
// only (code-standards section 7). The ribbon is CPU-expanded (world-space verts
// with the camera-facing lateral offset already folded in), so the VS is a plain
// transform that forwards the world position for the analytic fog term.

// -----------------------------------------------------------------------------
// VS -- CPU-expanded ribbon vertex. a_uv.x = arc-length param (scrolled on the
// CPU), a_uv.y = across-width in [0,1]. a_color = linear premultiplied radiance
// (beam tint * brightness * lifetime fade). v_worldPos feeds the fog integral.
// -----------------------------------------------------------------------------
static const char kBeamVs[] = R"GLSL(#version 330 core
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
	v_worldPos = a_pos;
	gl_Position = u_viewProj * vec4( a_pos, 1.0 );
}
)GLSL";

// -----------------------------------------------------------------------------
// FS -- additive beam. When u_textured != 0 the beam sprite (xbeam1/smoke strip)
// is sampled (V across width, U scrolling along the arc); otherwise a procedural
// soft core is used: a triangular falloff across the width (bright spine, soft
// edges). Either way the result is MULTIPLIED by the analytic base-fog
// transmittance (fade-to-black, same closed form as the world/studio/sprite
// passes -- M2c §1 point 4: a self-drawn beam must NOT glow brighter than the
// fogged world behind it). Additive blend is selected on the CPU side.
// -----------------------------------------------------------------------------
static const char kBeamFsBody[] = R"GLSL(
uniform sampler2D u_texDiffuse;   // unit 0 (beam sprite strip)
uniform int   u_textured;         // 1 = sample the sprite, 0 = procedural core
uniform vec4  u_fog;              // rgb = fog color (linear), w = extinction a (1/units); w<=0 -> off
uniform vec4  u_fogParams;        // x = height falloff b, z = maxOpacity
uniform vec3  u_camPos;           // camera world position (fog ray origin)
in vec2 v_uv;
in vec4 v_color;
in vec3 v_worldPos;
out vec4 fragColor;
// Analytic base-fog transmittance (fog M1 spec 4.3); identical closed form to the
// sprite/world/studio passes so black-fog density/maxOpacity thicken beam fog
// consistently (height + distance, |b|<eps and |rd.z|<eps guards).
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
	{
		col *= texture( u_texDiffuse, v_uv );
	}
	else
	{
		float across = abs( v_uv.y * 2.0 - 1.0 );   // 0 at the spine, 1 at the edge
		float core = 1.0 - across;
		core *= core;                                // soft-edged bright core
		col.rgb *= core;
	}
	float T = cszFogT( v_worldPos, u_camPos, u_fog.w, u_fogParams.x, u_fogParams.z );
	col.rgb *= T;                                    // additive emitter: fade to black, never add fog color
	fragColor = vec4( col.rgb * col.a, col.a );
}
)GLSL";
