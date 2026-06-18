/*
 * csz_weather_shaders.inl -- CSOZ renderer: weather particle shader sources
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
 * csoz docs/notes/primext-render-mechanisms-m2.md); implemented by an agent
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
// This file is #included from csz_weather.cpp (single translation unit); it is
// intentionally NOT listed in CMakeLists.
//
// ===Precipitation shaders===
// Two tiny programs (rain, snow). Both share the same vertex layout built on the
// CPU each frame into a dynamic VBO:
//   attribute 0 : vec3 a_pos    -- world-space billboard corner
//   attribute 1 : vec2 a_corner -- per-corner UV in [0,1] (round-flake falloff)
//   attribute 2 : vec4 a_color  -- rgb (already fog/night modulated) + a (alpha)
// All depth/fog/intensity modulation is done CPU-side and baked into a_color, so
// the fragment stages stay branch-free and texture-free (cheap, GL3.3/GLES3 safe).
// The only uniform is u_viewProj (view.matViewProj.m), matching csz_sprite.

// Rain vertex stage: pass corner (for the vertical streak gradient) + color.
static const char kRainVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_corner;
layout(location = 2) in vec4 a_color;
uniform mat4 u_viewProj;
out vec2 v_corner;
out vec4 v_color;
void main()
{
	v_corner = a_corner;
	v_color = a_color;
	gl_Position = u_viewProj * vec4( a_pos, 1.0 );
}
)GLSL";

// Rain fragment stage: cool grey-blue streak. The CPU bakes the per-streak alpha
// (depth/fog/intensity); here we add a soft vertical gradient (brighter mid,
// fading at the head/tail) and a gentle horizontal feather so the thin quad does
// not read as a hard-edged rectangle. Alpha-blended.
static const char kRainFs[] = R"GLSL(#version 330 core
in vec2 v_corner;
in vec4 v_color;
out vec4 fragColor;
void main()
{
	// Vertical gradient along the streak (corner.y 0..1): fade both ends a touch
	// so streaks taper instead of ending abruptly.
	float vy = v_corner.y;
	float vGrad = smoothstep( 0.0, 0.15, vy ) * smoothstep( 1.0, 0.72, vy );
	// Horizontal feather across the streak width (corner.x 0..1, center 0.5): start
	// fading from the center outward (0.05) so the streak has a thin soft core and
	// no hard rectangular edge -> reads as a fine rain line, not a UI bar.
	float hx = 1.0 - smoothstep( 0.05, 0.5, abs( v_corner.x - 0.5 ) );
	float a = v_color.a * vGrad * hx;
	if( a <= 0.003 )
		discard;
	fragColor = vec4( v_color.rgb, a );
}
)GLSL";

// Snow vertex stage: identical transform; corner drives the round-flake falloff.
static const char kSnowVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_corner;
layout(location = 2) in vec4 a_color;
uniform mat4 u_viewProj;
out vec2 v_corner;
out vec4 v_color;
void main()
{
	v_corner = a_corner;
	v_color = a_color;
	gl_Position = u_viewProj * vec4( a_pos, 1.0 );
}
)GLSL";

// Snow fragment stage: soft round flake. Procedural radial alpha from the corner
// (no texture). Color is the night-tinted cool white baked on the CPU.
static const char kSnowFs[] = R"GLSL(#version 330 core
in vec2 v_corner;
in vec4 v_color;
out vec4 fragColor;
void main()
{
	// corner*2-1 maps the [0,1] quad to [-1,1]; fade alpha to 0 at the rim.
	vec2 d = v_corner * 2.0 - 1.0;
	float r = length( d );
	float a = v_color.a * smoothstep( 1.0, 0.0, r );
	if( a <= 0.003 )
		discard;
	fragColor = vec4( v_color.rgb, a );
}
)GLSL";
