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
//   attribute 1 : vec2 a_corner -- per-corner UV in [0,1] (streak/flake falloff)
//   attribute 2 : vec4 a_color  -- rgb (already fog/night modulated) + a (alpha)
// Depth/fog/intensity modulation -- AND the structural "don't blanket the sky"
// and near/far parallax weighting -- are computed CPU-side (full world state:
// camera origin + forward, per-particle world pos/height) and baked into
// a_color (rgb + alpha). The fragment stages only do per-fragment SHAPING that
// cannot be done per-vertex: streak head/tail taper, soft flake falloff, ring.
// This keeps the shaders branch-free, texture-free, GL3.3/GLES3 safe and avoids
// relying on fragile derived-from-clip-space heuristics.
//
// Uniforms:
//   u_viewProj  (all)  : view.matViewProj.m
// All GL3.3 / GLES3 safe; texture-free.

// Rain vertex stage: pass corner (streak param) + color through.
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

// Rain fragment stage: cool grey-blue motion-blur streak. The CPU bakes per-streak
// alpha (depth/fog/intensity/sky-fade) into v_color.a. Here we shape the streak so
// it reads as falling water, not a painted bar:
//   - asymmetric head/tail taper: a tight bright HEAD (bottom, corner.y~0) and a
//     long thin TAIL (top, corner.y~1) -> a real motion streak, brighter at the
//     leading drop, dissolving along the trail.
//   - soft horizontal core: thin bright center, feathered sides (no hard rect).
static const char kRainFs[] = R"GLSL(#version 330 core
in vec2 v_corner;
in vec4 v_color;
out vec4 fragColor;
void main()
{
	// Streak param along its length: corner.y 0 = leading HEAD (bottom of the
	// falling drop), 1 = trailing TAIL (top). A drop is bright/condensed at the
	// head and dissolves up the trail.
	float t = v_corner.y;
	// Head: quick ramp-in over the first 12% so the leading edge is crisp.
	float head = smoothstep( 0.0, 0.12, t );
	// Tail: long fade from ~35% to the very top -> elongated motion-blur trail.
	float tail = 1.0 - smoothstep( 0.35, 1.0, t );
	// Brightness boost at the head (leading drop catches light).
	float headGlow = 1.0 + 0.6 * ( 1.0 - smoothstep( 0.0, 0.22, t ) );
	float vGrad = head * tail;
	// Horizontal feather across the streak width (corner.x 0..1, center 0.5):
	// thin bright core, soft sides -> a fine rain line, not a UI bar.
	float hx = 1.0 - smoothstep( 0.05, 0.5, abs( v_corner.x - 0.5 ) );
	float a = v_color.a * vGrad * hx;
	if( a <= 0.003 )
		discard;
	fragColor = vec4( v_color.rgb * headGlow, a );
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

// Snow fragment stage: soft round flake with a faint bright core so near flakes
// read as fluffy volume rather than flat discs. Procedural radial alpha from the
// corner (no texture). Color is the night-tinted cool white baked on the CPU.
static const char kSnowFs[] = R"GLSL(#version 330 core
in vec2 v_corner;
in vec4 v_color;
out vec4 fragColor;
void main()
{
	// corner*2-1 maps the [0,1] quad to [-1,1]; fade alpha to 0 at the rim.
	vec2 d = v_corner * 2.0 - 1.0;
	float r = length( d );
	// Soft round falloff + a gentle inner core lift so the flake has a fluffy
	// bright middle (reads as a snowflake catching light, not a flat dot).
	float edge = smoothstep( 1.0, 0.0, r );
	float core = 0.35 * ( 1.0 - smoothstep( 0.0, 0.55, r ) );
	float a = v_color.a * min( 1.0, edge + core );
	if( a <= 0.003 )
		discard;
	fragColor = vec4( v_color.rgb, a );
}
)GLSL";

// Splash fragment stage (rain impact rings). Reuses the snow VS (corner pass).
// Draws a thin expanding RING: alpha peaks at a radius that the CPU advances over
// the splash lifetime by scaling the quad size, and the ring thickness tapers.
// corner -> [-1,1]; ring = narrow band near r==1 (the quad rim), so as the CPU
// grows the quad the lit band reads as an outward-expanding ripple on the ground.
static const char kSplashFs[] = R"GLSL(#version 330 core
in vec2 v_corner;
in vec4 v_color;
out vec4 fragColor;
void main()
{
	vec2 d = v_corner * 2.0 - 1.0;
	float r = length( d );
	// Ring centered near r=0.78 with soft inner/outer falloff -> a thin annulus.
	float ring = smoothstep( 0.45, 0.78, r ) * ( 1.0 - smoothstep( 0.78, 1.0, r ) );
	float a = v_color.a * ring;
	if( a <= 0.003 )
		discard;
	fragColor = vec4( v_color.rgb, a );
}
)GLSL";
