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
uniform vec3 u_sunDir;      // world dir toward the sun (normalized): warm horizon glow
uniform vec3 u_moonDir;     // world dir toward the moon (normalized): moonlit-cloud lighting
uniform vec3 u_moonColor;   // moonlit-cloud tint
uniform float u_phase;      // 0=nightfall .. 0.5=midnight .. 1=daylight
uniform float u_starAmount; // 0..1 star field brightness gate (1 at midnight)
uniform vec4 u_fog;         // rgb = fog color (linear), w = extinction a (1/units); w<=0 -> off
uniform vec4 u_fogParams;   // x = height falloff b (unused for the sky band), y = sun glow, z = maxOpacity
// Legacy disc uniforms (u_sunColor / u_bloodMoon / u_sunCosR / u_moonCosR /
// u_moonHalo) were RETIRED with the legacy moon/sun disc (DEAD-1, infra pass 2
// 2026-06-18): the physically-based bodies are C3's, so this fallback FS draws
// no disc and never referenced them. Removed from the shader AND their CPU
// plumbing (csz_sky.cpp). KEPT u_sunDir/u_moonDir/u_moonColor (still used above).
out vec4 fragColor;

// Hash a direction-on-the-sphere into a pseudo-random scalar (public-domain
// integer-hash style; world-direction input => rotation stable).
float hash13( vec3 p )
{
	p = fract( p * 0.1031 );
	p += dot( p, p.yzx + 19.19 );
	return fract( ( p.x + p.y ) * p.z );
}

// Cheap trilinear value noise on the world-direction lattice (public-domain
// technique; world-direction input => rotation stable, like the star field).
float vnoise( vec3 p )
{
	vec3 i = floor( p );
	vec3 f = fract( p );
	f = f * f * ( 3.0 - 2.0 * f );                       // Hermite smoothing
	float n000 = hash13( i + vec3( 0.0, 0.0, 0.0 ) );
	float n100 = hash13( i + vec3( 1.0, 0.0, 0.0 ) );
	float n010 = hash13( i + vec3( 0.0, 1.0, 0.0 ) );
	float n110 = hash13( i + vec3( 1.0, 1.0, 0.0 ) );
	float n001 = hash13( i + vec3( 0.0, 0.0, 1.0 ) );
	float n101 = hash13( i + vec3( 1.0, 0.0, 1.0 ) );
	float n011 = hash13( i + vec3( 0.0, 1.0, 1.0 ) );
	float n111 = hash13( i + vec3( 1.0, 1.0, 1.0 ) );
	float nx00 = mix( n000, n100, f.x );
	float nx10 = mix( n010, n110, f.x );
	float nx01 = mix( n001, n101, f.x );
	float nx11 = mix( n011, n111, f.x );
	return mix( mix( nx00, nx10, f.y ), mix( nx01, nx11, f.y ), f.z );
}

// 3-octave fBm (kept cheap: octaves low, math is plain mul/add). Output ~0..0.94.
float fbm3( vec3 p )
{
	float s = 0.0;
	float a = 0.5;
	for( int o = 0; o < 3; o++ )
	{
		s += a * vnoise( p );
		p *= 2.02;
		a *= 0.5;
	}
	return s;
}

// Three keyframe sky color sets (zenith, horizon), lerped by phase. Linear
// space; the renderer is not gamma-managed past this point (matches world FS).
void skyColors( float ph, out vec3 zenith, out vec3 horizon )
{
	// Sunset / round start: COOL twilight base. The warm sunset is a DIRECTIONAL
	// sun-side glow added in main(), NOT a 360-degree horizon ring.
	vec3 ssZen = vec3( 0.060, 0.075, 0.175 );
	vec3 ssHor = vec3( 0.120, 0.115, 0.195 );
	// Night (cool blue moonlit): the warm sunset is gone by ~phase 0.18, so the
	// night reads blue/moonlit, not a lingering orange dusk. Deeper blue at the
	// TOP, a brighter+less-saturated haze band near the HORIZON.
	vec3 ntZen = vec3( 0.009, 0.018, 0.056 );
	vec3 ntHor = vec3( 0.088, 0.110, 0.190 );
	// Midnight (darkest): deep cold-blue zenith, thin cool horizon haze glow. The
	// horizon band is kept >= 0.05 luma brighter than the zenith (gradient depth),
	// both bands cool (B >= R).
	// RECAL 2026-06-21 (MATCH-REFERENCE): the reference sky is deep NAVY, not black; the
	// shipped zenith (0.004,0.007,0.020) measured near-black (darkest-decile B-R ~ +1.8,
	// FAILS the navy test B>R+3). Lift the ZENITH toward navy: B 0.020->0.055 (B clearly
	// dominant) with a touch of R/G so it reads deep-blue, NOT grey. Still dark enough that
	// the dense star field + Milky Way pop above it. This is the legitimate navy mechanism
	// (the uniform DOME base color); it is NOT the band's smooth underglow, so it does not
	// violate the anti-fog red line. mnHor is LEFT UNCHANGED (already bright enough per the
	// red-team caveat). Only the deep-night path (phase~0.5) sees mnZen; day/dusk/dawn/gold
	// use ssZen/ntZen/dwZen and are untouched.
	vec3 mnZen = vec3( 0.006, 0.012, 0.055 );
	vec3 mnHor = vec3( 0.100, 0.124, 0.200 );
	// Dawn / daylight: COOL brightening blue base. The warm sunrise is the same
	// directional sun-side glow in main(), NOT a 360-degree gold ring.
	vec3 dwZen = vec3( 0.230, 0.330, 0.520 );
	vec3 dwHor = vec3( 0.380, 0.470, 0.640 );

	// Thresholds realigned to the absolute-time schedule's phase anchors
	// (midnight 0.5, dawn 0.86, day 1.0). ph<0.5 darkens sunset->night->midnight;
	// 0.5..0.86 lightens the darkest to a still-DIM cool pre-dawn (ntZen, not the
	// bright dwZen); 0.86..1.0 ramps that dim pre-dawn up to full daylight (dwZen).
	// During the held night the phase sits at 0.5, so the dome stays at mnZen.
	if( ph < 0.25 )                              // sunset -> cool night (fast handoff)
	{
		float t = smoothstep( 0.0, 0.25, ph );
		zenith  = mix( ssZen, ntZen, t );
		horizon = mix( ssHor, ntHor, t );
	}
	else if( ph < 0.5 )                          // night -> darkest midnight
	{
		float t = smoothstep( 0.25, 0.5, ph );
		zenith  = mix( ntZen, mnZen, t );
		horizon = mix( ntHor, mnHor, t );
	}
	else if( ph < 0.86 )                         // hold dark, then lighten toward a dim pre-dawn
	{
		float t = smoothstep( 0.5, 0.86, ph );
		zenith  = mix( mnZen, ntZen, t );
		horizon = mix( mnHor, ntHor, t );
	}
	else                                         // dim pre-dawn -> full daylight (final ramp)
	{
		float t = smoothstep( 0.86, 1.0, ph );
		zenith  = mix( ntZen, dwZen, t );
		horizon = mix( ntHor, dwHor, t );
	}
}

void main()
{
	vec3 dir = normalize( v_dir );

	// --- Gradient dome: horizon->zenith ramp by elevation (dir.z, Z up). ---
	vec3 zenith, horizon;
	skyColors( u_phase, zenith, horizon );
	float up = clamp( dir.z, 0.0, 1.0 );
	float grad = pow( up, 0.40 );               // pull the ramp toward the horizon (steepened: brighter horizon band, deeper zenith => more gradient depth)
	vec3 col = mix( horizon, zenith, grad );

	// --- Warm horizon glow that FOLLOWS THE SUN (sunset in the west, sunrise in the
	// east), active ONLY while the sun is near the horizon. Concentrated toward the
	// sun azimuth (pow(toSun,3)) and hugging the horizon, so the anti-sun sky stays
	// cool/dark -- a directional sunset/dawn with a clear sunset|night boundary as
	// the moon rises opposite, NOT a 360-degree warm ring. ---
	float sunLow = 1.0 - smoothstep( 0.04, 0.34, abs( u_sunDir.z ) );  // 1 on horizon, 0 high/deep
	if( sunLow > 0.0 )
	{
		float toSun = max( dot( dir, u_sunDir ), 0.0 );
		float lowBand = 1.0 - smoothstep( 0.0, 0.40, up );  // hug the horizon
		// Sunset (evening) is a deeper red-orange; dawn (morning) is cooler and
		// PINKER -- real atmospheric difference (dusty dusk air vs clean dawn air;
		// sunrise-sunset.org / ScienceDaily). Distinct glow color at each end.
		vec3 sunsetGlow = vec3( 1.00, 0.45, 0.18 );   // evening: deep red-orange
		vec3 dawnGlow   = vec3( 1.00, 0.62, 0.52 );   // morning: cooler pink/peach
		vec3 warm = ( u_phase < 0.5 ) ? sunsetGlow : dawnGlow;
		col += warm * sunLow * lowBand * pow( toSun, 3.0 ) * 1.1;
	}

	// --- Hash star field: world-direction cells, twinkle-free, faded by dawn.
	// Fine & subtle: finer cells (smaller points), a high threshold (sparse, ~0.1-
	// 0.3% coverage) and a capped brightness so no star rivals the moon (<= ~0.6
	// luma). The dawn/horizon fade ramp (u_starAmount, csz_sky.cpp) is unchanged. ---
	if( u_starAmount > 0.001 && up > 0.02 )
	{
		vec3 cell = floor( dir * 300.0 );                   // finer cells => smaller (<=2px) points
		float h = hash13( cell );
		float star = smoothstep( 0.9950, 1.0, h );          // sparser bright points
		float horizonFade = smoothstep( 0.02, 0.30, up );   // keep them off the rim
		float starBright = 0.45 + 0.22 * hash13( cell + 7.0 );  // cap < 0.7 luma (must not rival the moon)
		col += vec3( star ) * u_starAmount * horizonFade * starBright;
	}

	// Moon visibility + angular position, computed here so the cloud layer below
	// can light its wisps by the moon before the disc is drawn on top.
	float cm = dot( dir, u_moonDir );
	float moonVis = clamp( ( u_moonDir.z + 0.10 ) * 4.0, 0.0, 1.0 );  // fade in as the moon rises above the horizon

	// --- Moonlit wispy clouds: cheap 3-octave fBm over the view direction, upper
	// sky only, low coverage (wispy, not overcast), tinted by the moonlight color.
	// SILVER where lit -- near the moon (proximity) and at the cloud's leading edge
	// (rim) -- DARK/subtle far from it. Drawn UNDER the moon disc/halo so it never
	// competes with the moon as the focal point. Night-only (gated by moonVis), so
	// the fBm cost is paid only when the moon is up. Single default path (no tier). ---
	if( up > 0.05 && moonVis > 0.01 )
	{
		float n = fbm3( dir * 3.0 );                                   // low freq => large soft wisps
		float cloud = smoothstep( 0.46, 0.82, n );                     // lowered threshold => wider contiguous wisps (still not overcast)
		cloud *= smoothstep( 0.05, 0.45, up );                         // live in the upper sky
		float nearMoon = pow( max( cm, 0.0 ), 6.0 );                   // lit toward the moon, dark away
		float rim = smoothstep( 0.56, 0.66, n ) * ( 1.0 - smoothstep( 0.74, 0.92, n ) );  // bright leading edge
		float lit = 0.18 + 0.55 * nearMoon + 0.32 * rim * nearMoon;    // silver edges near the moon (subtle; clearly dimmer than the moon disc)
		vec3 cloudCol = u_moonColor * lit;
		col = mix( col, cloudCol, clamp( cloud, 0.0, 1.0 ) * moonVis * 0.9 );
	}

	// --- Legacy moon/sun disc + halo RETIRED (Chunk A, FIX-PLAN 2026-06-18). ---
	// The physically-based bodies are now owned by C3 (geom/csz_sunmoon.cpp, real
	// phases + NASA surface + atmospheric extinction + soft-knee) and the stars by
	// C4. This legacy fallback FS keeps only the gradient dome + warm horizon glow +
	// hash stars + moonlit clouds + fog as the C1-identity baseline; drawing a SECOND
	// disc/halo set here (different radii) risked compositing a real annulus and a
	// double-body when the dev fullscreen overlay was armed. The moon-direction
	// fields (cm / moonVis above) are kept because the moonlit-cloud lighting uses
	// them. The legacy disc uniforms (u_sunColor / u_bloodMoon / u_sunCosR /
	// u_moonCosR / u_moonHalo) and their CPU plumbing were fully removed (DEAD-1).

	// --- 1/4 fog fusion (analytic base fog, fog M1 Step 2): the horizon band now
	// uses the SAME natural-exp extinction as world/studio (e^(-a*0.25*dist)), so
	// it owns the horizon with no double/seam against the depth-driven world fog.
	// u_fog.w is the converted extinction a, so e^(-a*0.25*dist) == the old
	// exp2(-density*0.25*dist) -- look preserved. Underwater passes a=0 (no fog).
	// Mix toward fog color at the horizon, where the view distance is largest. The
	// server maxOpacity floor clamps the band so a blackout reaches the sky too. ---
	if( u_fog.w > 0.0 )
	{
		// Distance grows toward the horizon (up -> 0) and stays small at zenith.
		float dist = 1.0 / max( up * up + 0.02, 0.02 );
		float fogF = clamp( exp( -u_fog.w * 0.25 * dist ), 0.0, 1.0 );
		fogF = max( fogF, 1.0 - u_fogParams.z );           // server reveal floor (maxOpacity)
		col = mix( u_fog.rgb, col, fogF );
	}

	// Ordered dither: break up 8-bit quantization banding on the very dark sky
	// gradient (only ~24-46 levels/channel at midnight). Triangular-PDF (+/-1 LSB)
	// from two decorrelated interleaved-gradient-noise samples; screen-space via
	// gl_FragCoord, no uniform needed. (Jimenez IGN base; Gjol/Playdead TPDF.)
	float ign1 = fract( 52.9829189 * fract( dot( gl_FragCoord.xy,        vec2( 0.06711056, 0.00583715 ) ) ) );
	float ign2 = fract( 52.9829189 * fract( dot( gl_FragCoord.xy + 17.0, vec2( 0.06711056, 0.00583715 ) ) ) );
	col += ( ign1 + ign2 - 1.0 ) / 255.0;   // TPDF noise in [-1/255, +1/255]
	fragColor = vec4( col, 1.0 );
}
)GLSL";
