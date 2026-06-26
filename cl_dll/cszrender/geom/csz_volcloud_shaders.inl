/*
 * csz_volcloud_shaders.inl -- CSOZ renderer: volumetric raymarch cloud GLSL
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). PROVENANCE / LICENSE:
 *   - The raymarch loop, the density model (height profile + coverage remap + edge
 *     erosion + wind scroll) and the lighting (Beer-Lambert transmittance, cone
 *     light-march, multi-scatter octave reuse, ground/sky ambient tint) are original
 *     work written for CSOZ from first principles. The Beer-Lambert extinction, the
 *     normalized Henyey-Greenstein phase and fractional-Brownian-motion noise are
 *     standard, non-proprietary math used directly from their published descriptions.
 *   - hash13() is Dave Hoskins' "Hash without Sine" (https://www.shadertoy.com/view/4djSRW),
 *     MIT-licensed; the 0.1031 constant is his. The same hash is already used in
 *     csz_sky_shaders.inl.
 *   - ign() is Jorge Jimenez's Interleaved Gradient Noise ("Next Generation Post
 *     Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014); its magic
 *     constants (52.9829189, 0.06711056, 0.00583715) are used verbatim.
 *   No code, permutation table or gradient table is copied or translated from
 *   Unreal/Unity/Frostbite/Hillaire sample code, GPU-Gems snippets, PrimeXT,
 *   Paranoia, Trinity, or any retail/leaked source.
 *
 * This program is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * In addition, as a special exception, the author gives permission to link the code
 * of this program with the Half-Life Game Engine ("HL Engine") and Modified Game
 * Libraries ("MODs") developed by Valve, L.L.C ("Valve"). You must obey the GNU
 * General Public License in all respects for all of the code used other than the HL
 * Engine and MODs from Valve. If you modify this file, you may extend this exception
 * to your version of the file, but you are not obligated to do so. If you do not wish
 * to do so, delete this exception statement from your version.
 */
// Included ONLY by geom/csz_volcloud.cpp. GL3.3 core / GLES3 / WebGL2 intersection.
// Uniform names are this program's private contract (the only CROSS-FILE channel is
// the per-frame uniforms set CPU-side in VolCloudRenderer::Contribute).

// =============================================================================
// Vertex stage: VAO-less fullscreen triangle (gl_VertexID). The per-pixel world
// view ray is rebuilt from the camera basis (u_camFwd/Right/Up, Quake Z-up).
// u_camRight/u_camUp are PRE-SCALED CPU-side by tan(fovX/2)/tan(fovY/2), so the
// ray = fwd + right*ndc.x + up*ndc.y -- IDENTICAL convention to the sky / stars /
// panorama passes, so the clouds register exactly with the sky behind them. Shared
// by BOTH the march program and the upsample program (the upsample needs v_dir for
// the horizon fade).
// =============================================================================
static const char kVolCloudVs[] = R"GLSL(#version 330 core
uniform vec3 u_camFwd;
uniform vec3 u_camRight;   // already scaled by tan(fovX/2)
uniform vec3 u_camUp;      // already scaled by tan(fovY/2)
out vec3 v_dir;            // world-space view direction (un-normalized)
void main()
{
	vec2 ndc = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0,
	                 ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	v_dir = u_camFwd + u_camRight * ndc.x + u_camUp * ndc.y;
	gl_Position = vec4( ndc, 1.0, 1.0 );
}
)GLSL";

// =============================================================================
// Pass 1 -- QUARTER-RES volumetric ray-march into an RGBA16F offscreen target.
// Output = (premultiplied in-scatter radiance .rgb, coverage alpha = 1 - Tview).
// Composited later with kBlendPremulOver: dst = src.rgb + dst*(1-src.a), the
// physically-correct premultiplied "over". LINEAR HDR (pre-tonemap); do NOT sRGB.
//
// These are BACKGROUND sky-dome clouds: they live in a world-space altitude slab
// [u_slabBase, u_slabBase+u_slabThick] above the camera, only along up-going view
// rays (rd.z > 0). World geometry renders AFTER and overwrites them, so there is no
// scene-depth occlusion to respect here (the upsample is therefore plain bilinear +
// horizon-aware, NOT a scene-depth bilateral).
// =============================================================================
static const char kVolCloudMarchFs[] = R"GLSL(#version 330 core
in vec3 v_dir;
out vec4 fragColor;

uniform vec3  u_camPos;        // ray origin (world, Quake Z-up)
uniform vec3  u_lightDir;      // world dir toward the LIT body (sun by day / moon by night)
uniform vec3  u_lightColor;    // linear HDR radiance of the lit body (already phase-independent)
uniform vec3  u_ambGround;     // ambient skylight toward the cloud UNDERSIDE (darker)
uniform vec3  u_ambSky;        // ambient skylight toward the cloud TOP (sky/zenith)
uniform float u_time;          // bounded client time (s) for slow wind scroll
uniform float u_jitterFrame;   // per-frame jitter lattice offset (animated IGN)
uniform float u_cover;         // 0..1 coverage threshold
uniform float u_density;       // density multiplier
uniform float u_sigmaT;        // extinction coefficient (1/world-units along the march)
uniform float u_slabBase;      // slab bottom altitude above the camera (world units)
uniform float u_slabThick;     // slab thickness (world units)
uniform float u_noiseFreq;     // base noise frequency (1/world-units)
uniform int   u_steps;         // view march steps
uniform int   u_lightSteps;    // cone light march steps
uniform int   u_oct;           // density fBm octaves
uniform int   u_msOct;         // multiscatter octaves (reuse light transmittance)
uniform int   u_backend;       // 0 procedural-fBM, 1 cheap-hash, 2 const-slab, 3 3D-texture
uniform int   u_earlyout;      // 1 = transmittance early-out enabled
uniform sampler3D u_noise3d;   // small procedurally-filled 3D noise (backend 3 / microbench)

const float PI = 3.14159265358979323846;
// Compile-time caps so the driver can bound the (uniform-controlled) loops; the
// live uniforms clamp BELOW these on the CPU side.
const int MAX_STEPS = 64;
const int MAX_LIGHT = 8;
const int MAX_OCT   = 6;
const int MAX_MSOCT = 4;

// --- generated integer-bit-mix hashes (formulas, not tables; clean-room) ----------
float hash13( vec3 p )
{
	p = fract( p * 0.1031 );
	p += dot( p, p.zyx + 31.3216 );
	return fract( ( p.x + p.y ) * p.z );
}

// Trilinear value noise on the integer lattice (smoothstep-interpolated).
float vnoise3( vec3 x )
{
	vec3 i = floor( x );
	vec3 f = fract( x );
	f = f * f * ( 3.0 - 2.0 * f );
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

// Billow variant: |2n-1| inverted -> puffy lumps. Octave-summed fBm.
float fbm( vec3 p, int oct )
{
	float s = 0.0, amp = 0.5, norm = 0.0;
	for( int o = 0; o < MAX_OCT; o++ )
	{
		if( o >= oct ) break;
		float n = vnoise3( p );
		n = 1.0 - abs( 2.0 * n - 1.0 );     // billow
		s    += amp * n;
		norm += amp;
		p    = p * 2.02 + vec3( 7.3, 1.7, 3.9 );
		amp  *= 0.5;
	}
	return ( norm > 0.0 ) ? ( s / norm ) : 0.0;
}

float remap( float v, float a, float b, float c, float d )
{
	return c + ( clamp( ( v - a ) / max( b - a, 1e-4 ), 0.0, 1.0 ) ) * ( d - c );
}

// Height fraction across the slab [0 = base, 1 = top].
float heightFrac( vec3 p )
{
	return clamp( ( p.z - ( u_camPos.z + u_slabBase ) ) / max( u_slabThick, 1.0 ), 0.0, 1.0 );
}

// Vertical density profile: rounded bottom, eroded anvil top (denser low-mid).
float heightProfile( float h )
{
	float bottom = smoothstep( 0.0, 0.15, h );
	float top    = smoothstep( 1.0, 0.55, h );
	return bottom * top;
}

// Cloud density at world point p (0..~1, scaled by u_density). The DENSITY BACKEND
// is the swappable cost lever for the measurement cost-ladder.
float cloudDensity( vec3 p, int oct )
{
	float h    = heightFrac( p );
	float prof = heightProfile( h );
	if( prof <= 0.0 )
		return 0.0;

	vec3 wind = vec3( u_time * 3.0, u_time * 1.3, 0.0 );   // slow wind scroll
	vec3 q    = ( p + wind ) * u_noiseFreq;

	float shape;
	if( u_backend == 2 )           // const-slab: no noise, profile only (cheapest)
	{
		return u_density * prof;
	}
	else if( u_backend == 3 )      // 3D-texture fetch (same march; microbench backend)
	{
		// tri-LINEAR filtered fetch of the small repeating 3D noise; .r is the base.
		shape = texture( u_noise3d, q * 0.25 ).r;
	}
	else if( u_backend == 1 )      // cheap-hash: single value-noise octave
	{
		shape = vnoise3( q );
	}
	else                           // procedural full fBM (primary path)
	{
		shape = fbm( q, oct );
	}

	// Coverage threshold: higher u_cover -> lower threshold -> more cloud.
	float d = remap( shape, 1.0 - u_cover, 1.0, 0.0, 1.0 );
	d *= prof;

	// Edge erosion (full path only): carve detail out of the wispy edges.
	if( u_backend == 0 && d > 0.0 && d < 0.7 )
	{
		float ero = fbm( q * 3.1 + vec3( 19.1, 3.7, 11.3 ), min( oct, 2 ) );
		d = remap( d, ero * 0.5, 1.0, 0.0, 1.0 );
	}
	return clamp( d, 0.0, 1.0 ) * u_density;
}

// Normalized Henyey-Greenstein phase.
float hg( float c, float g )
{
	float g2 = g * g;
	return ( 1.0 - g2 ) / ( 4.0 * PI * pow( max( 1.0 + g2 - 2.0 * g * c, 1e-4 ), 1.5 ) );
}

// Interleaved-gradient-noise hash for the LOW-amplitude start jitter (no texture).
float ign( vec2 px, float frame )
{
	px += frame * 5.588238;
	return fract( 52.9829189 * fract( dot( px, vec2( 0.06711056, 0.00583715 ) ) ) );
}

void main()
{
	vec3 ro = u_camPos;
	vec3 rd = normalize( v_dir );

	// Sky-dome clouds live only ABOVE: down-going / near-horizon rays see no cloud.
	if( rd.z <= 0.02 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	// Intersect the view ray with the altitude slab (planes z = camZ + base / +base+thick).
	float zBase = ro.z + u_slabBase;
	float zTop  = ro.z + u_slabBase + u_slabThick;
	float t0 = ( zBase - ro.z ) / rd.z;
	float t1 = ( zTop  - ro.z ) / rd.z;
	t0 = max( t0, 0.0 );
	float marchLen = t1 - t0;
	if( marchLen <= 0.0 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	int steps = u_steps;
	float stepLen = marchLen / float( steps );

	// LOW-amplitude (<= 1 step) generated jitter to break slab banding.
	float jit = ign( gl_FragCoord.xy, u_jitterFrame );
	float t = t0 + stepLen * ( 0.5 + ( jit - 0.5 ) * 0.9 );

	float cosT = dot( rd, u_lightDir );
	// Dual-lobe HG: strong forward silver-lining + a gentle back lobe.
	float phase = 0.85 * hg( cosT, 0.80 ) + 0.15 * hg( cosT, -0.20 );

	float lightStepLen = u_slabThick / float( max( u_lightSteps, 1 ) ) * 0.6;

	vec3  L = vec3( 0.0 );
	float Tview = 1.0;

	for( int i = 0; i < MAX_STEPS; i++ )
	{
		if( i >= steps ) break;
		vec3 p = ro + rd * t;
		float dens = cloudDensity( p, u_oct );
		if( dens > 0.0015 )
		{
			// --- cone light march toward the lit body (BASE density only) ---------
			float lt = 0.0;
			for( int j = 0; j < MAX_LIGHT; j++ )
			{
				if( j >= u_lightSteps ) break;
				vec3 lp = p + u_lightDir * ( lightStepLen * ( float( j ) + 0.5 ) );
				lt += cloudDensity( lp, min( u_oct, 2 ) ) * lightStepLen;
			}
			float Tl = exp( -u_sigmaT * lt );

			// --- multiscatter approximation: REUSE Tl with halved coeffs/octave ---
			float ms = 0.0, a = 1.0, b = 1.0;
			for( int o = 0; o < MAX_MSOCT; o++ )
			{
				if( o >= u_msOct ) break;
				ms += a * pow( Tl, b );    // pow(Tl,b) == exp(-sigmaT*b*lt), no re-march
				a  *= 0.5;
				b  *= 0.5;
			}
			if( u_msOct <= 0 ) ms = Tl;

			// --- height-aware ambient skylight (undersides not black) -------------
			float hf = heightFrac( p );
			vec3 ambient = mix( u_ambGround, u_ambSky, hf );

			// --- energy-conserving in-scatter slice (Beer-Lambert) ----------------
			float stepT = exp( -u_sigmaT * dens * stepLen );
			vec3 S = u_lightColor * ( ms * phase ) + ambient;
			L += Tview * ( 1.0 - stepT ) * S;
			Tview *= stepT;

			if( u_earlyout == 1 && Tview < 0.01 )
				break;
		}
		t += stepLen;
	}

	float alpha = 1.0 - Tview;
	// Premultiplied: L is already weighted by coverage along the march.
	fragColor = vec4( L, alpha );
}
)GLSL";

// =============================================================================
// Pass 2 -- horizon-aware bilinear upsample + composite. Samples the LINEAR-filtered
// quarter-res target (texture() gives the bilinear tap for free) and fades the cloud
// alpha out toward the horizon so the slab's hard lower edge never shows as a seam.
// Output is premultiplied (rgb, alpha); the caller composites with kBlendPremulOver.
// =============================================================================
static const char kVolCloudUpsampleFs[] = R"GLSL(#version 330 core
in vec3 v_dir;
out vec4 fragColor;
uniform sampler2D u_cloudTex;   // quarter-res march result (LINEAR, CLAMP)
uniform vec2 u_fullSize;        // full viewport size (px)
void main()
{
	vec2 uv = gl_FragCoord.xy / u_fullSize;
	vec4 c = texture( u_cloudTex, uv );      // bilinear upsample

	// Horizon-aware fade: rays near / below the horizon carry no cloud; soften the
	// transition so the slab's lower silhouette edge is feathered, not stair-stepped.
	vec3 rd = normalize( v_dir );
	float horizon = smoothstep( 0.02, 0.16, rd.z );
	c *= horizon;

	fragColor = c;   // premultiplied (rgb, alpha) -> kBlendPremulOver
}
)GLSL";
