/*
 * csz_sky_shaders.inl -- CSOZ renderer: procedural day/night sky GLSL sources
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
 * that has not read that source. Disc/star/gradient approaches studied from
 * docs/references/sky-moon.md (named with licenses there) and reimplemented
 * from the public-domain math; nothing transcribed.
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
// Included ONLY by csz_sky.cpp. Uniform names are this program's private
// contract (the only CROSS-FILE channel is AmbienceParams, written CPU-side in
// PublishLighting, never a sky uniform).
// GLES3 equivalence assumption: nothing here beyond the GL3.3 core / GLES3 /
// WebGL2 intersection (code-standards section 7); fwidth() is core in both.

// Vertex stage: VAO-less fullscreen triangle via gl_VertexID (3 verts cover
// the NDC viewport, the third pushed to (3,-1)/(-1,3) so the clip triangle
// fully contains [-1,1]^2). The per-pixel world-space view ray is rebuilt from
// the camera basis (u_camFwd/Right/Up, Quake world space, Z up). u_camRight and
// u_camUp are PRE-SCALED on the CPU by tan(fovX/2)/tan(fovY/2), so the ray =
// fwd + right*ndc.x + up*ndc.y needs no fov uniform (and the wrapper set only
// exposes vec3 uploads). Working in a world direction keeps the fragment stage
// rotation-stable -- stars do not shimmer when the camera turns. No depth
// output beyond the fixed clip z (depth test/write are OFF for this pass; the
// world overwrites the sky wherever it draws).
static const char kSkyVs[] = R"GLSL(#version 330 core
uniform vec3 u_camFwd;
uniform vec3 u_camRight;     // already scaled by tan(fovX/2)
uniform vec3 u_camUp;        // already scaled by tan(fovY/2)
out vec3 v_dir;              // world-space view direction (un-normalized)
void main()
{
	// (0,0)->(-1,-1), (1,0)->(3,-1), (0,1)->(-1,3): clip-space fullscreen tri.
	vec2 ndc = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0,
	                 ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	v_dir = u_camFwd + u_camRight * ndc.x + u_camUp * ndc.y;
	gl_Position = vec4( ndc, 1.0, 1.0 );
}
)GLSL";

// Fragment stage: procedural gradient dome + sun/moon disc + hash star field +
// blood-moon variant + dawn warm gradient + 1/4 fog fusion, all driven by the
// scalar phase and the body-direction uniforms. Body directions are computed
// on the CPU from phase (so PublishLighting and the disc agree) and uploaded.
static const char kSkyFs[] = R"GLSL(#version 330 core
in vec3 v_dir;
uniform vec3 u_sunDir;      // world dir toward the sun (normalized)
uniform vec3 u_moonDir;     // world dir toward the moon (normalized)
uniform vec3 u_sunColor;    // disc/halo tint for the sun
uniform vec3 u_moonColor;   // disc/halo tint for the moon
uniform float u_phase;      // 0=nightfall .. 0.5=midnight .. 1=daylight
uniform float u_starAmount; // 0..1 star field brightness gate (1 at midnight)
uniform float u_bloodMoon;  // 0..1 blood-moon push (1 at midnight when armed)
uniform float u_sunCosR;    // cos(sun angular radius)
uniform float u_moonCosR;   // cos(moon angular radius)
uniform float u_moonHalo;   // moon halo intensity 0..1
uniform vec4 u_fog;         // rgb = fog color (linear), w = density; w<=0 -> off
out vec4 fragColor;

// Hash a direction-on-the-sphere into a pseudo-random scalar (public-domain
// integer-hash style; world-direction input => rotation stable).
float hash13( vec3 p )
{
	p = fract( p * 0.1031 );
	p += dot( p, p.yzx + 19.19 );
	return fract( ( p.x + p.y ) * p.z );
}

// Three keyframe sky color sets (zenith, horizon), lerped by phase. Linear
// space; the renderer is not gamma-managed past this point (matches world FS).
void skyColors( float ph, out vec3 zenith, out vec3 horizon )
{
	// Nightfall (deep blue, dusk band low on the horizon).
	vec3 nfZen = vec3( 0.020, 0.035, 0.075 );
	vec3 nfHor = vec3( 0.060, 0.065, 0.110 );
	// Midnight (darkest, cold).
	vec3 mnZen = vec3( 0.006, 0.010, 0.028 );
	vec3 mnHor = vec3( 0.018, 0.024, 0.050 );
	// Dawn / daylight (warm gold horizon, brightening zenith blue).
	vec3 dwZen = vec3( 0.230, 0.330, 0.520 );
	vec3 dwHor = vec3( 0.900, 0.560, 0.300 );

	if( ph < 0.5 )
	{
		float t = smoothstep( 0.0, 0.5, ph );   // nightfall -> midnight
		zenith  = mix( nfZen, mnZen, t );
		horizon = mix( nfHor, mnHor, t );
	}
	else
	{
		float t = smoothstep( 0.5, 1.0, ph );   // midnight -> dawn/day
		zenith  = mix( mnZen, dwZen, t );
		horizon = mix( mnHor, dwHor, t );
	}
}

void main()
{
	vec3 dir = normalize( v_dir );

	// --- Gradient dome: horizon->zenith ramp by elevation (dir.z, Z up). ---
	vec3 zenith, horizon;
	skyColors( u_phase, zenith, horizon );
	float up = clamp( dir.z, 0.0, 1.0 );
	float grad = pow( up, 0.55 );               // pull the ramp toward the horizon
	vec3 col = mix( horizon, zenith, grad );

	// --- Dawn warm horizon glow concentrated around the sun azimuth. ---
	float dawn = smoothstep( 0.78, 1.0, u_phase );
	if( dawn > 0.0 )
	{
		float toSun = max( dot( dir, u_sunDir ), 0.0 );
		float lowBand = 1.0 - smoothstep( 0.0, 0.32, up );  // strongest near horizon
		vec3 warm = vec3( 1.00, 0.62, 0.28 );
		col += warm * dawn * lowBand * ( 0.35 + 0.65 * pow( toSun, 4.0 ));
	}

	// --- Hash star field: world-direction cells, twinkle-free, faded by dawn. ---
	if( u_starAmount > 0.001 && up > 0.02 )
	{
		vec3 cell = floor( dir * 260.0 );
		float h = hash13( cell );
		float star = smoothstep( 0.9965, 1.0, h );          // sparse bright points
		float horizonFade = smoothstep( 0.02, 0.30, up );   // keep them off the rim
		col += vec3( star ) * u_starAmount * horizonFade * ( 0.7 + 0.3 * hash13( cell + 7.0 ));
	}

	// --- Moon disc + halo (visible through the night arc). ---
	float cm = dot( dir, u_moonDir );
	float aaM = fwidth( cm ) + 1e-5;
	float moonDisc = smoothstep( u_moonCosR - aaM, u_moonCosR + aaM, cm );
	float moonHalo = pow( max( cm, 0.0 ), 256.0 ) * u_moonHalo;
	// Blood-moon: shift the moon body + its halo toward red, keep it legible
	// (mix, never flat-replace), and warm the surrounding sky a touch.
	vec3 moonBody = mix( u_moonColor, vec3( 0.75, 0.06, 0.04 ), u_bloodMoon );
	vec3 moonGlow = mix( u_moonColor, vec3( 0.55, 0.05, 0.03 ), u_bloodMoon );
	col = mix( col, moonBody, clamp( moonDisc, 0.0, 1.0 ));
	col += moonGlow * moonHalo;
	if( u_bloodMoon > 0.0 )
		col += vec3( 0.05, 0.0, 0.0 ) * u_bloodMoon * pow( max( cm, 0.0 ), 8.0 );

	// --- Sun disc + halo (rises near dawn; only contributes above the horizon). ---
	float cs = dot( dir, u_sunDir );
	float sunVis = clamp( ( u_sunDir.z + 0.10 ) * 4.0, 0.0, 1.0 );  // fade in as it clears the horizon
	if( sunVis > 0.0 )
	{
		float aaS = fwidth( cs ) + 1e-5;
		float sunDisc = smoothstep( u_sunCosR - aaS, u_sunCosR + aaS, cs );
		float sunHalo = pow( max( cs, 0.0 ), 256.0 );
		col = mix( col, u_sunColor, clamp( sunDisc, 0.0, 1.0 ) * sunVis );
		col += u_sunColor * sunHalo * 0.6 * sunVis;
	}

	// --- 1/4 fog fusion: sky depth -> exp2(-density * 0.25 * dist). Underwater
	// passes density 0 (no fog). Mix toward fog color at the horizon, where the
	// view distance is largest. Use a fixed sky reference distance so the blend
	// reads as a horizon band, not a per-pixel depth (sky has no real depth). ---
	if( u_fog.w > 0.0 )
	{
		// Distance grows toward the horizon (up -> 0) and stays small at zenith.
		float dist = 1.0 / max( up * up + 0.02, 0.02 );
		float fogF = clamp( exp2( -u_fog.w * 0.25 * dist ), 0.0, 1.0 );
		col = mix( u_fog.rgb, col, fogF );
	}

	fragColor = vec4( col, 1.0 );
}
)GLSL";
