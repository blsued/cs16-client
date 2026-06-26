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
 *   - hash13() and hash33() are Dave Hoskins' "Hash without Sine"
 *     (https://www.shadertoy.com/view/4djSRW), MIT-licensed; the 0.1031 / 0.1030 /
 *     0.0973 constants are his. hash13 is already used in csz_sky_shaders.inl; hash33
 *     feeds the compact Worley (cellular) edge-erosion field added in this tuning pass.
 *     The Worley F1-distance construction itself is standard, non-proprietary math.
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
uniform float u_silver;        // silver-lining strength (forward-HG lobe boost)
uniform float u_tint;          // brooding storm tint amount (desaturate + cold teal-grey)
uniform float u_detail;        // high-frequency Worley edge-erosion amount
uniform float u_sigmaT;        // extinction coefficient (1/world-units along the march)
uniform float u_slabBase;      // slab bottom altitude above the camera (world units)
uniform float u_slabThick;     // slab thickness (world units)
uniform float u_lightReach;    // TOTAL cone light-march reach toward the lit body (world units)
uniform float u_coverScale;    // coverage-field frequency vs body (smaller => bigger masses + gaps)
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
const int MAX_STEPS = 96;
const int MAX_LIGHT = 8;
const int MAX_OCT   = 6;
const int MAX_MSOCT = 4;

// --- generated integer-bit-mix hashes (formulas, not tables; clean-room) ----------
// NOTE on symmetry: the original mixer fract((p.x+p.y)*p.z) is INVARIANT under an x<->y
// swap of the input lattice cell, so the value-noise field it feeds is mirror-correlated
// across the x=y diagonal. Combined with the billow fold and the (previously) axis-aligned
// octave advance, that read on screen as a fake left-right mirror. The mixer below uses an
// asymmetric, non-commutative chain of the three components (each axis weighted by a distinct
// irrational and folded in a different order) so no input swap leaves the output unchanged.
float hash13( vec3 p )
{
	p = fract( p * vec3( 0.1031, 0.11369, 0.13787 ) );
	p += dot( p, p.yzx + 19.19 );
	return fract( ( p.x + 2.07 * p.y ) * ( p.z + 1.39 * p.x ) + p.y * 0.71 );
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

// Per-octave decorrelating rotations. A 3D rotation cannot be expressed as an axis swap,
// so rotating the domain between octaves guarantees the billow ridges of successive octaves
// are NOT axis-aligned and cannot stack into a single mirrored macro shape. Two fixed,
// mutually-prime rotation matrices (about tilted axes) are applied alternately.
const mat3 kRotA = mat3(
	 0.00,  0.80, -0.60,
	-0.80,  0.36,  0.48,
	 0.60,  0.48,  0.64 );
const mat3 kRotB = mat3(
	 0.36, -0.48,  0.80,
	 0.48,  0.80,  0.36,
	-0.80,  0.36,  0.48 );

// Billow variant: |2n-1| inverted -> puffy lumps. Octave-summed fBm with per-octave
// ROTATION + NONUNIFORM SCALE + large irrational OFFSET (the anti-mirror trio). The
// lacunarity is nonuniform per axis so the lattice never re-aligns with itself, and the
// alternating rotation breaks the axis-aligned ridge stacking that exposed the mirror.
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
		// nonuniform anisotropic lacunarity + rotate the domain + irrational offset.
		mat3 R = ( ( o & 1 ) == 0 ) ? kRotA : kRotB;
		p    = R * ( p * vec3( 2.13, 1.97, 2.41 ) ) + vec3( 17.3, 9.1, 23.7 );
		amp  *= 0.5;
	}
	return ( norm > 0.0 ) ? ( s / norm ) : 0.0;
}

// Cheap 2-octave vector warp field for the ASYMMETRIC domain warp. Returns a world-space
// displacement that is NOT centered on the camera/screen and NOT mirror-symmetric (each
// component samples a DIFFERENT, offset, rotated slice of the noise), so it bends the cloud
// domain into asymmetric flowing shapes and destroys any residual repetition.
vec3 domainWarp( vec3 q )
{
	vec3 a = kRotA * q + vec3(  41.7,  7.3, 13.1 );
	vec3 b = kRotB * q + vec3( -19.4, 31.8, 27.2 );
	return vec3(
		vnoise3( a )            - 0.5,
		vnoise3( b * 1.27 )     - 0.5,
		vnoise3( a * 0.83 + b ) - 0.5 );
}
)GLSL"
// ---- MSVC string-literal split: the march FS exceeds the ~16KB single-literal cap, so
// it is authored as two ADJACENT raw string literals. C/C++ concatenates adjacent string
// literals at translation, so the GLSL the driver sees is byte-identical to one source. ----
R"GLSL(
float remap( float v, float a, float b, float c, float d )
{
	return c + ( clamp( ( v - a ) / max( b - a, 1e-4 ), 0.0, 1.0 ) ) * ( d - c );
}

// Dave Hoskins "Hash without Sine" vec3->vec3 (MIT). Feeds the Worley feature points.
vec3 hash33( vec3 p )
{
	p = fract( p * vec3( 0.1031, 0.1030, 0.0973 ) );
	p += dot( p, p.yxz + 33.33 );
	return fract( ( p.xxy + p.yxx ) * p.zyx );
}

// Compact cellular (Worley) noise -> BILLOW form ( 1 - F1 ). One 3x3x3 neighborhood, one
// jittered feature point per cell. High near feature points => puffy cauliflower lumps;
// used to erode crisp 3D wisps out of the cloud edges (gated to the VIEW march only).
float worleyBillow( vec3 p )
{
	vec3 ip = floor( p );
	vec3 fp = fract( p );
	float f1 = 1.0;
	for( int x = -1; x <= 1; x++ )
	for( int y = -1; y <= 1; y++ )
	for( int z = -1; z <= 1; z++ )
	{
		vec3 g   = vec3( float( x ), float( y ), float( z ) );
		vec3 fpt = g + hash33( ip + g );      // feature point in neighbor cell
		vec3 r   = fpt - fp;
		f1 = min( f1, dot( r, r ) );          // squared F1 distance
	}
	return 1.0 - clamp( sqrt( f1 ), 0.0, 1.0 );
}

// Height fraction across the slab [0 = base, 1 = top].
float heightFrac( vec3 p )
{
	return clamp( ( p.z - ( u_camPos.z + u_slabBase ) ) / max( u_slabThick, 1.0 ), 0.0, 1.0 );
}

// Vertical density profile: a SHARP defined base, a tall built-up storm-tower body that
// carries density high into the slab, and a lightly eroded anvil at the crown -- so the
// masses read as massive vertical cumulonimbus towers, not a thin flat layer.
float heightProfile( float h )
{
	float base  = smoothstep( 0.0, 0.08, h );               // crisp cloud base
	float tower = smoothstep( 1.0, 0.30, h );               // density built up high (towering)
	float anvil = 1.0 - 0.30 * smoothstep( 0.80, 1.0, h );  // slight anvil thinning at the crown
	return base * tower * anvil;
}

// Cloud density at world point p (0..~1, scaled by u_density). The DENSITY BACKEND
// is the swappable cost lever for the measurement cost-ladder.
// detailLod: 1 = VIEW march (pay for high-freq Worley erosion), 0 = cone LIGHT march
// (skip erosion -- the shadow lookup does not need edge detail, buying back the budget
// the Worley spends on the primary rays).
float cloudDensity( vec3 p, int oct, int detailLod )
{
	float h    = heightFrac( p );
	float prof = heightProfile( h );
	if( prof <= 0.0 )
		return 0.0;

	// WORLD-SPACE sampling: wind-scrolled world position, no camera/screen centering, no
	// abs()/radial term -> the noise lattice is continuous and never folded about an axis.
	vec3 wind = vec3( u_time * 0.8, u_time * 0.35, 0.0 );  // SLOW wind scroll (do not swamp parallax)
	vec3 q    = ( p + wind ) * u_noiseFreq;
	q.z *= 0.65;                                           // vertical stretch -> taller towers

	// const-slab backend short-circuits BEFORE any noise.
	if( u_backend == 2 )           // const-slab: no noise, profile only (cheapest)
		return u_density * prof;

	// detailLod 1 = VIEW march: pays for the full anti-mirror domain warp + decoupled
	// coverage + Worley. detailLod 0 = cone LIGHT (shadow) march: it runs up to u_lightSteps
	// times PER active view step, so it takes the CHEAP path (no warp, one shared fbm field
	// for coverage+body). The shadow lookup only needs approximate occlusion, not edge detail
	// or anti-mirror decorrelation, so this buys back nearly all the budget those cost.
	bool viewMarch = ( detailLod >= 1 );

	// ---- ASYMMETRIC DOMAIN WARP (VIEW march only) -------------------------------------
	// Bend the sample domain by a non-centered, non-mirror vector field BEFORE looking up
	// the shape noise. This is the single strongest anti-repetition move: the macro shape
	// flows asymmetrically instead of tiling, so neither the coverage nor the density field
	// can read as a left-right mirror. Gated to the full procedural backend on the view march.
	vec3 qd = q;
	if( u_backend == 0 && viewMarch )
		qd = q + ( 0.55 + 0.9 * u_detail ) * domainWarp( q * 0.85 + vec3( 3.1, 8.7, 1.9 ) );

	// ---- SEPARATE non-centered COVERAGE field -----------------------------------------
	// Coverage (WHERE clouds are) uses its OWN low-frequency, independently-offset field --
	// NOT the same octave as the density body (reusing one low-freq octave for both was a
	// prime cause of the macro repetition). On the cheap LIGHT march the body field is reused
	// for coverage (one fbm) -- the shadow term does not need the decorrelation.
	float shape, coverNoise;
	if( u_backend == 3 )      // 3D-texture fetch (same march; microbench backend)
	{
		shape      = texture( u_noise3d, q  * 0.25 ).r;
		coverNoise = texture( u_noise3d, q * 0.11 + vec3( 0.37 ) ).r;
	}
	else if( u_backend == 1 )      // cheap-hash: single value-noise octave
	{
		shape      = vnoise3( q );
		coverNoise = viewMarch ? vnoise3( q * 0.42 + vec3( 11.7, 4.3, 2.9 ) ) : shape;
	}
	else                           // procedural full fBM (primary path)
	{
		shape = fbm( qd, oct );                                            // mid/high billow BODY
		coverNoise = viewMarch
			? fbm( q * u_coverScale + vec3( 51.3, 13.9, 7.1 ), min( oct, 2 ) ) // low-freq COVERAGE (view)
			: shape;                                                       // light march: reuse body
	}

	// LOW-FREQUENCY COVERAGE: gate where clouds exist. Higher u_cover -> more sky filled.
	float coverage = remap( coverNoise, 1.0 - u_cover, 1.0, 0.0, 1.0 );
	if( coverage <= 0.0 )
		return 0.0;

	// MID-FREQUENCY BILLOW BODY remapped INTO the coverage gate: the body density only
	// exists where coverage allows, and the coverage value erodes the base of the body so
	// edges thin out naturally (defined base, built-up body, ragged silhouette).
	float d = remap( shape, 1.0 - 0.85 * coverage, 1.0, 0.0, 1.0 );
	d *= prof;

	// ---- HIGH-FREQUENCY WORLEY EROSION at the silhouette (VIEW march only) -------------
	// Two octaves of cellular billow carve crisp cauliflower wisps out of the EDGES (where
	// d is small) far more than the dense cores (where d is large), so silhouettes become
	// torn/lumpy and thin wisps separate from solid dark cores. We have the perf headroom.
	if( u_backend == 0 && detailLod >= 1 && u_detail > 0.001 && d > 0.0 && d < 0.97 )
	{
		float w1 = worleyBillow( qd * 2.7 + vec3( 19.1,  3.7, 11.3 ) );
		float w2 = worleyBillow( qd * 6.3 + vec3(  4.9, 27.1,  8.2 ) );
		float w  = 0.65 * w1 + 0.35 * w2;
		float erodeAmt = u_detail * ( 1.0 - 0.55 * d );   // erode thin edges hardest
		d = remap( d, w * erodeAmt, 1.0, 0.0, 1.0 );
	}

	// Wider density DYNAMIC RANGE: a steeper-than-smoothstep contrast curve pushes mids
	// apart so dense dark cores stand clearly distinct from thin eroded wisps (not a flat
	// uniform stain). Two smoothstep passes ~= a gentle gamma toward the extremes.
	d = clamp( d, 0.0, 1.0 );
	d = d * d * ( 3.0 - 2.0 * d );
	d = d * d * ( 3.0 - 2.0 * d );

	return d * u_density;
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
	// SILVER LINING: u_silver sharpens the forward HG lobe AND heavies its weight, so the
	// sun/moon BACKLIGHTS the cloud edges into bright blazing rims against dark dense cores
	// -- the single biggest drama cue for storm clouds. A gentle back lobe keeps fill.
	float sil   = clamp( u_silver, 0.0, 1.5 );
	float gFwd  = 0.74 + 0.16 * clamp( sil, 0.0, 1.0 );             // forward anisotropy 0.74..0.90
	float phase = ( 0.80 + 0.95 * sil ) * hg( cosT, gFwd ) + 0.16 * hg( cosT, -0.18 );
	// Backscatter gate (0 when looking toward the light, 1 looking away): the silver RIM is
	// only allowed to blaze where the light is genuinely BEHIND the cloud edge (cosT<0), so
	// it appears as a thin bright lining on the backlit silhouette, not an all-over wash.
	float backlit = smoothstep( -0.05, -0.55, cosT );

	// Cone light-march reach is sized to the FEATURE scale (passed as u_lightReach), NOT the
	// slab thickness, so dense cores self-shadow over SEVERAL feature-diameters toward the sun
	// regardless of slab geometry. Falls back to a slab-relative reach if the uniform is 0.
	float reachTotal   = ( u_lightReach > 1.0 ) ? u_lightReach : ( u_slabThick * 0.6 );
	float lightStepLen = reachTotal / float( max( u_lightSteps, 1 ) );

	vec3  L = vec3( 0.0 );
	float Tview = 1.0;

	for( int i = 0; i < MAX_STEPS; i++ )
	{
		if( i >= steps ) break;
		vec3 p = ro + rd * t;
		float dens = cloudDensity( p, u_oct, 1 );
		if( dens > 0.0015 )
		{
			// --- cone light march toward the lit body (BASE density only) ---------
			float lt = 0.0;
			for( int j = 0; j < MAX_LIGHT; j++ )
			{
				if( j >= u_lightSteps ) break;
				vec3 lp = p + u_lightDir * ( lightStepLen * ( float( j ) + 0.5 ) );
				lt += cloudDensity( lp, min( u_oct, 2 ), 0 ) * lightStepLen;
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

			// --- SILVER-LINING RIM: a thin, intense bright lining on backlit edges.
			// Tl is HIGH on thin edges (the light reaches through) and LOW in dense cores
			// (self-shadowed). (1-Tl)*Tl peaks at the THIN transition zone -> the rim sits
			// exactly on the silhouette. Gated by the backlit factor + u_silver, so it only
			// blazes on edges with the light behind them. Strong forward HG sharpens it.
			float rim = u_silver * backlit * Tl * ( 1.0 - Tl ) * hg( cosT, 0.92 ) * 5.0;

			// --- POWDER / dark-edge term: deepens the SELF-SHADOWED cores. The lit-facing
			// near surface of a dense lump scatters little single-light back toward the eye
			// (light has to travel through the lump first) -> a characteristic dark "powder"
			// edge that, with the bright rim, reads the masses as 3D volumes not a flat sheet.
			float powder = 1.0 - exp( -2.0 * dens * stepLen * 40.0 );
			float powderShade = mix( 1.0, 0.45 + 0.55 * Tl, 0.6 ) * ( 1.0 - 0.35 * powder );

			// --- height-aware ambient skylight (undersides not black) -------------
			float hf = heightFrac( p );
			vec3 ambient = mix( u_ambGround, u_ambSky, hf );

			// --- energy-conserving in-scatter slice (Beer-Lambert) ----------------
			float stepT = exp( -u_sigmaT * dens * stepLen );
			vec3 S = u_lightColor * ( ms * phase * powderShade + rim ) + ambient;
			L += Tview * ( 1.0 - stepT ) * S;
			Tview *= stepT;

			if( u_earlyout == 1 && Tview < 0.01 )
				break;
		}
		t += stepLen;
	}

	float alpha = 1.0 - Tview;

	// Brooding STORM TINT: desaturate toward luminance and push a cold grey-teal cast for
	// the survival-horror mood. Luminance-preserving, so the bright silver rims stay bright
	// and readable -- "dramatic dark", not a flat dark smear. (L is premultiplied; scaling
	// the colour without touching alpha keeps the composite physically correct.)
	float tnt  = clamp( u_tint, 0.0, 1.0 );
	float lum  = dot( L, vec3( 0.2126, 0.7152, 0.0722 ) );
	vec3  storm = vec3( lum ) * vec3( 0.78, 0.93, 0.90 );   // cold desaturated teal-grey
	L = mix( L, storm, tnt );

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
