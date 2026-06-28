/*
 * csz_cloudvol_shaders.inl -- CSOZ renderer: volumetric cloud REBUILD v2 GLSL
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). PROVENANCE / LICENSE:
 *   - The ray-box march, the density model (Perlin-Worley base dilated by a Worley
 *     FBM, a cumulus height gradient, coverage carve, high-freq Worley edge erosion --
 *     the "Nubis"-class remap chain) and the lighting (Beer-Lambert transmittance,
 *     cone light-march, multi-scatter octave reuse, powder dark-edge, silver-lining
 *     rim, ground/sky ambient) are ORIGINAL work written for CSOZ from first
 *     principles and the PUBLISHED algorithm descriptions. Beer-Lambert extinction,
 *     the Henyey-Greenstein phase, Perlin/Worley noise, the standard remap(), the
 *     empty-space-skip + step-back march, the joint depth/alpha bilateral upscale and
 *     the AMD-CAS-style contrast-adaptive sharpen are non-proprietary techniques used
 *     directly from their published descriptions. The depth-linearize / world-position
 *     reconstruct helpers are the standard GL inverse-viewproj math.
 *   - ign() is Jorge Jimenez's Interleaved Gradient Noise (SIGGRAPH 2014), magic
 *     constants verbatim. No code, permutation table or gradient table is copied or
 *     translated from Unreal/Unity/Frostbite/Hillaire sample code, GPU-Gems/GPU-Pro
 *     snippets, PrimeXT, Paranoia, Trinity, or any retail/leaked source.
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
// Included ONLY by geom/clouds/csz_cloudvol.cpp. GL3.3 core. Uniform names are this
// program's private contract (set CPU-side in CloudVolRenderer::Contribute). REBUILD v2
// scope: weather 0 (scattered cumulus) ONLY -- the rain/snow overcast branches were
// deleted from this path. GLSL compiles only at RUNTIME: every uniform/varying/function
// referenced below is declared here AND looked up + set in csz_cloudvol.cpp.

// =============================================================================
// Vertex stage: VAO-less fullscreen triangle (gl_VertexID). The per-pixel world view
// ray is rebuilt from the camera basis (u_camFwd/Right/Up, Quake Z-up); u_camRight/
// u_camUp are PRE-SCALED CPU-side by tan(fovX/2)/tan(fovY/2), so ray = fwd +
// right*ndc.x + up*ndc.y -- IDENTICAL convention to the sky/fog passes, so the clouds
// register exactly with the depth buffer behind them.
// =============================================================================
static const char kCloudVs[] = R"GLSL(#version 330 core
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
// Pass 1 -- HALF-RES (csz_clouds_res=2), WORLD-SPACE, DEPTH-BOUNDED ray-box march into an
// RGBA16F offscreen target. Output = (premultiplied in-scatter radiance .rgb, coverage
// alpha = 1 - Tview). Composited later by the joint-bilateral upscale (Pass 2) with
// kBlendPremulOver. LINEAR HDR (pre-tonemap).
//
// REBUILD v2 march: empty-space-skip (coarse stride) -> step-back on first hit -> fine
// in-cloud steps (dtFine = dtCoarse/u_fineDiv) -> resume coarse on exit. This pins the
// silhouette first-hit AND halves per-ray cost to afford the extra half-res rays. The
// adaptive COARSE count (from u_stepLenMax) + frame-stable per-step dither + low-freq
// domain warp + Tview<0.01 early-out are kept. Density comes from the baked 3D textures.
// =============================================================================
static const char kCloudMarchFs[] = R"GLSL(#version 330 core
in vec3 v_dir;
out vec4 fragColor;

uniform vec3  u_camPos;        // ray origin (world, Quake Z-up)
uniform vec3  u_lightDir;      // world dir toward the LIT body (sun by day / moon by night)
uniform vec3  u_lightColor;    // linear HDR radiance/chroma of the lit body (DAY magnitude even at night; u_nightLum dims)
uniform vec3  u_ambGround;     // ambient skylight toward the cloud UNDERSIDE (darker)
uniform vec3  u_ambSky;        // ambient skylight toward the cloud TOP (sky/zenith)
uniform vec3  u_boxMin;        // cloud-slab AABB min corner (world)
uniform vec3  u_boxMax;        // cloud-slab AABB max corner (world)
uniform float u_time;          // bounded client time (s) for slow wind scroll / evolve
uniform float u_frame;         // per-frame jitter lattice offset (animated IGN start jitter)
uniform vec3  u_windVec;       // world-u/sec horizontal wind drift (dir*speed); drift = u_windVec*u_time
uniform float u_evolveRate;    // slow volume-EVOLVE (morph) rate: advances the noise sample THROUGH the volume
uniform float u_density;       // density multiplier
uniform float u_coverage;      // 0..1 coverage LEVEL/mean (the macro field spreads it spatially)
uniform float u_hBase;         // height gradient: feathered flat-ish base ramp-in fraction [0,u_hBase]
uniform float u_hTop;          // height gradient: rounded-dome fade-out start fraction [u_hTop,1]
uniform float u_silver;        // silver-lining (forward-scatter) rim strength -- the ONLY route to bright edges
uniform float u_silverWidth;   // rim band width: LOW=broad glow inward, HIGH=razor edge only
uniform float u_sigmaT;        // extinction coefficient (1/world-units along the march)
uniform float u_baseFreq;      // base 3D-noise frequency (1/world-units)
uniform float u_detailFreq;    // detail 3D-noise frequency (1/world-units)
uniform float u_detailAmt;     // high-frequency Worley EDGE-erosion strength (interiors stay smooth)
uniform float u_powder;        // powder dark-edge strength
uniform float u_erodeDepth;    // high-freq edge-erosion DEPTH (how far the detail bites the silhouette)
uniform float u_selfShadow;    // cone-march self-shadow / multi-scatter extinction weight (internal pockets)
uniform float u_capLight;      // BROAD sun-facing CAP light (density-gradient normal); 0 = off (skips the gradient taps)
uniform float u_capEps;        // world-space epsilon for the density-gradient cap normal (~ cloud-feature scale)
uniform float u_sunForward;    // DIRECT-sun forward-scatter strength (sun-facing caps); CLAMPED phase (see directBeam)
uniform float u_sunG;          // direct-sun forward HG anisotropy g (0.78-0.85)
uniform float u_lightReach;    // TOTAL cone light-march reach toward the lit body (world units)
uniform float u_marchFar;      // hard distance cap (world units)
uniform float u_stepLenMax;    // target MAX world-space COARSE step length (= dtCoarse; the coarse count derives from this)
uniform float u_fineDiv;       // ESS in-cloud fine-step divisor: dtFine = dtCoarse / u_fineDiv
uniform float u_nightLum;      // SINGLE NIGHTNESS LUMINANCE AUTHORITY: mix(1, ~0.06, nightness); scales TOTAL radiance before the knee
uniform vec2  u_targetSize;    // half-res target size in pixels
uniform int   u_steps;         // MIN coarse march steps (quality floor for steep/short rays)
uniform int   u_lightSteps;    // cone light march steps (exponentially spaced)
uniform float u_domainWarp;    // low-freq world-space domain-warp amplitude (decorrelate grazing rays / step planes); 0 = off
uniform float u_horizonFadeLo; // screen-elevation fade LOW threshold (sin elev; fully faded at/below)
uniform float u_horizonFadeHi; // screen-elevation fade HIGH threshold (sin elev; fully present at/above)
uniform int   u_dbgMode;       // R5 debug viz (0 off / 1 density / 2 transmittance / 3 stepcount / 4 first-hit / 5 scatter / 6 raw-nearest-upsample)
// PATH A macro distribution (KEEP verbatim): a LOW-FREQUENCY world-XY coverage field + an
// INCOMMENSURATE second base tap so the sky has real large-scale structure (cloudy regions vs
// clear sky) with NO visible tiling, and the within-region shape never repeats either.
uniform float u_covFreq;       // macro coverage-field frequency (1/world-period); period >> footprint => no visible repeat
uniform float u_covContrast;   // coverage-field spread around the LEVEL (HIGH=scattered gaps => scattered cumulus)
uniform float u_covDrift;      // weather-system world drift fraction of u_windVec (0 = world-static mask)
uniform float u_detile;        // blend weight of the incommensurate (0.73x) second base tap (0 = single tap)

// scene depth + reconstruct (standard inverse-viewproj; re-derived, no fog include)
uniform sampler2D u_depthTex;  // raw window-space scene depth (compare-mode NONE)
uniform float u_zNear;
uniform float u_zFar;
uniform mat4  u_invViewProj;   // inverse(proj*view): clip -> world

// baked structured density (in-process bake: 128^3 base + 32^3 detail, RGBA8 GL_REPEAT LINEAR)
uniform sampler3D u_base3d;    // 128^3 RGBA8: R = Perlin-Worley, GBA = Worley octaves (cells 6/12/24)
uniform sampler3D u_detail3d;  // 32^3 RGBA8: high-freq Worley detail (RGB octaves, cells 4/8/16)

const float PI = 3.14159265358979323846;
// Compile-time caps so the driver can bound the uniform-controlled loops; the CPU clamps below them.
const int MAX_COARSE = 128;   // adaptive COARSE-count ceiling (worst-case grazing cost bound)
const int MAX_ITER   = 176;   // PERF: TOTAL march iterations (coarse + fine) hard cap. 256->176 caps the GRAZING
                              // worst-case (acctB flagged the 128->256 doubling as the main worst-case driver);
                              // paired with fineDiv 4->2 the worst LEGIT ray (vertical full-slab ~96 fine, grazing
                              // ~156) sits well under 176 so the dense scattered-cumulus deck is fully marched (no
                              // flat back-cutoff) while pathological long rays stay bounded. (Tview/horizon fade end sooner.)
const int MAX_LIGHT  = 8;     // cone light tap compile cap (u_lightSteps clamps below)

// Reconstruct world-space position from screen uv + raw depth d.
vec3 worldPosFromDepth( vec2 uv, float d )
{
	vec4 clip = vec4( uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0 );
	vec4 w = u_invViewProj * clip;
	return w.xyz / w.w;
}

// remap(v,a,b,c,d) = clamp((v-a)/(b-a),0,1) rescaled to [c,d]; a clamp-defined density edge.
float remap( float v, float a, float b, float c, float d )
{
	return c + ( clamp( ( v - a ) / max( b - a, 1e-4 ), 0.0, 1.0 ) ) * ( d - c );
}

// Cumulus height-density gradient across the slab thickness (REBUILD form): a feathered
// flat-ish base (linear ramp-in over [0,u_hBase]) and a smoothstep-ROUNDED dome toward the
// top (over [u_hTop,1]), ->0 at both slab faces so neither Z face is a visible edge. hf in [0,1].
float HeightGradient( float hf )
{
	float hb = clamp( hf / max( u_hBase, 1e-3 ), 0.0, 1.0 );             // feathered flat base
	float ht = clamp( ( 1.0 - hf ) / max( 1.0 - u_hTop, 1e-3 ), 0.0, 1.0 );
	ht = ht * ht * ( 3.0 - 2.0 * ht );                                  // smoothstep -> ROUNDED dome top
	return clamp( hb * ht, 0.0, 1.0 );
}
)GLSL"
// MSVC C2026: a single raw string literal caps ~16 KB -- split into adjacent literals (the
// compiler concatenates them into one contiguous GLSL source; identical text, no semantic change).
R"GLSL(
// MACRO SPATIAL COVERAGE FIELD over world horizontal XY (Quake Z-up). LOW-FREQUENCY (period
// 1/u_covFreq ~42000u >> marchFar) so ONE "weather cell" spans the reachable sky => clouds-here /
// clear-there with NO visible repeat; two INCOMMENSURATE taps (ratio 0.73) push the field's own
// period to their LCM. World-anchored (u_covDrift default 0); the cloud texture drifts THROUGH it.
float CoverageField( vec2 wxy )
{
	vec2 q  = wxy + u_windVec.xy * ( u_time * u_covDrift );
	float f1 = texture( u_base3d, vec3( q * u_covFreq, 0.317 ) ).r;
	float f2 = texture( u_base3d, vec3( q * ( u_covFreq * 0.73 ) + vec2( 0.41, 0.19 ), 0.622 ) ).r;
	return clamp( 0.6 * f1 + 0.4 * f2, 0.0, 1.0 );
}

// SampleCloudDensity -- the SINGLE density function shared by the view march, the cone light
// march AND the AO trace. REBUILD v2 chain (order is load-bearing):
//   animate(drift+evolve) -> domain-warp + incommensurate de-tile taps
//   STAGE 1 DILATE  : base = remap(perlinWorley.r, worleyFBM-1, 1, 0,1)        (round billows)
//                     x HeightGradient (flat feathered base, rounded dome, ->0 at both faces)
//   STAGE 2 CARVE   : cloud = remap(base, 1-cov, 1, 0,1) * cov                 (CRISP existence edge, NO pow-lift)
//   STAGE 3 ERODE   : edge-ONLY high-freq Worley, height-varying (wispy base / cauliflower top)
// The pow(cloud,0.6) body-lift and the outer-shell `mid` cauliflower pass are DELETED (they
// flattened the 0->1 silhouette = soft halo / popcorn). Fullness is recovered optically via
// u_density/u_sigmaT, NEVER by re-lifting low densities. detail=0 skips ONLY the fine erosion.
float SampleCloudDensity( vec3 p, int detail )
{
	// in-slab height fraction (Quake Z-up): 0 at the cloud base, 1 at the deck top.
	float hf = ( p.z - u_boxMin.z ) / max( u_boxMax.z - u_boxMin.z, 1e-3 );
	if( hf <= 0.0 || hf >= 1.0 )
		return 0.0;

	// --- ANIMATE (KEEP verbatim): horizontal wind DRIFT + slow volume EVOLVE/morph. evolve advances
	//     the noise sample THROUGH the volume so clouds form/dissipate, not just translate. ---
	vec3 drift  = u_windVec * u_time;
	vec3 evolve = vec3( 0.18, -0.13, 1.0 ) * ( u_evolveRate * u_time );
	vec3 ps     = p + drift + evolve;

	// --- low-freq world-space DOMAIN WARP (KEEP): one shared offset to break the radial fan /
	//     corduroy of near-parallel grazing rays. World-stable (no screen term) => no shimmer. ---
	vec3 warpOff = ( texture( u_base3d, ps * ( u_baseFreq * 0.37 ) ).gba * 2.0 - 1.0 ) * u_domainWarp;
	ps += warpOff;

	// --- incommensurate DE-TILE base taps (KEEP): a second 0.73x non-harmonic tap kills the macro
	//     base repeat without a new asset. u_detile=0 => single tap. ---
	vec4 b1   = texture( u_base3d, ps * u_baseFreq );
	vec4 b2   = texture( u_base3d, ps * ( u_baseFreq * 0.73 ) + vec3( 0.19, 0.41, 0.27 ) );
	vec4 b    = mix( b1, b2, u_detile );
	float wfbm = dot( b.gba, vec3( 0.625, 0.25, 0.125 ) );             // Worley billow FBM

	// STAGE 1 DILATE: Perlin-Worley base (R) grown by the Worley billow FBM => round connected billows.
	float base = remap( b.r, wfbm - 1.0, 1.0, 0.0, 1.0 );
	// x HEIGHT GRADIENT applied BEFORE the carve so the coverage threshold cuts a crisp edge through
	// the vertical faces too (flat feathered base, rounded dome).
	base *= HeightGradient( hf );

	// PATH A per-column coverage: u_coverage = field LEVEL, CoverageField spreads it +/- contrast.
	// weather-0 = LOW level + HIGH contrast => many columns clear (blue sky), the rest puffy cumulus.
	float covMean = clamp( u_coverage, 0.0, 1.0 );
	float cov     = clamp( covMean + ( CoverageField( p.xy ) - 0.5 ) * u_covContrast, 0.0, 1.0 );

	// STAGE 2 CARVE: the CRISP existence edge (Schneider coverage carve). NO pow-lift, NO mid pass.
	float cloud = remap( base, 1.0 - cov, 1.0, 0.0, 1.0 ) * cov;

	// STAGE 3 EDGE-ONLY high-freq erosion (anti-popcorn): the (1-smoothstep) edge mask is ~0 inside
	// thick cloud and 1 only on the thin outer shell, so detail TEARS the silhouette without bumping
	// dense interiors. Height-varying chroma: wispy base -> billowy cauliflower top. Detail evolves
	// FASTER so edges continuously fray/morph.
	if( detail == 1 && cloud > 0.0 && u_detailAmt > 0.001 )
	{
		vec3  dt   = texture( u_detail3d, ( p + drift * 1.5 + evolve * 2.3 + warpOff ) * u_detailFreq ).rgb;
		float dfbm = dot( dt, vec3( 0.625, 0.25, 0.125 ) );
		float chr  = mix( 1.0 - dfbm, dfbm, clamp( hf * 1.5, 0.0, 1.0 ) );   // wispy base -> cauliflower top
		float edge = 1.0 - smoothstep( 0.0, 0.55, cloud );                  // erosion confined to the outer shell
		cloud = remap( cloud, chr * ( u_detailAmt * u_erodeDepth ) * edge, 1.0, 0.0, 1.0 );
	}

	return clamp( cloud, 0.0, 1.0 ) * u_density;
}

// Cheap UPWARD density trace (Quake Z-up) for sky-visibility ambient occlusion. `tilt` decorrelates
// a near-vertical VIEW ray from a collinear AO column (the zenith-knot fix); 0 for oblique rays.
float TraceDensityUp( vec3 p, int steps, float reach, vec2 tilt )
{
	float stepLen = reach / float( max( steps, 1 ) );
	float acc = 0.0;
	for( int i = 0; i < 4; i++ )
	{
		if( i >= steps ) break;
		float h  = stepLen * ( float( i ) + 0.5 );
		vec3  sp = p + vec3( tilt * h, h );
		acc += SampleCloudDensity( sp, 0 ) * stepLen;
	}
	return acc;
}

// Normalized Henyey-Greenstein phase.
float hg( float c, float g )
{
	float g2 = g * g;
	return ( 1.0 - g2 ) / ( 4.0 * PI * pow( max( 1.0 + g2 - 2.0 * g * c, 1e-4 ), 1.5 ) );
}

// Interleaved-gradient-noise hash for the low-amplitude start jitter (no texture asset).
float ign( vec2 px, float frame )
{
	px += frame * 5.588238;
	return fract( 52.9829189 * fract( dot( px, vec2( 0.06711056, 0.00583715 ) ) ) );
}

// Ray-AABB intersection (slab method). Returns near/far hit parameters.
bool intersectBox( vec3 ro, vec3 rd, vec3 bmin, vec3 bmax, out float t0, out float t1 )
{
	vec3 inv  = 1.0 / rd;
	vec3 ta   = ( bmin - ro ) * inv;
	vec3 tb   = ( bmax - ro ) * inv;
	vec3 tmin = min( ta, tb );
	vec3 tmax = max( ta, tb );
	t0 = max( max( tmin.x, tmin.y ), tmin.z );
	t1 = min( min( tmax.x, tmax.y ), tmax.z );
	return t1 > max( t0, 0.0 );
}
)GLSL"
// MSVC C2026: split before main() into another adjacent raw literal (concatenated contiguously).
R"GLSL(
void main()
{
	vec2 uv = gl_FragCoord.xy / u_targetSize;
	vec3 ro = u_camPos;
	vec3 rd = normalize( v_dir );

	// --- scene-depth bound: terrain occludes the cloud (tMax = min(tSurf, marchFar)) ---
	float d = texture( u_depthTex, uv ).r;
	vec3 surf = worldPosFromDepth( uv, d );
	float tSurf = dot( surf - ro, rd );
	bool isSky = ( d >= 0.99999 );           // far plane = no occluder (open sky)
	if( isSky || tSurf <= 0.0 )
		tSurf = u_marchFar;
	float tMax = min( tSurf, u_marchFar );

	// --- bound the march to the cloud-slab AABB ---
	float t0, t1;
	if( !intersectBox( ro, rd, u_boxMin, u_boxMax, t0, t1 ) )
	{
		fragColor = vec4( 0.0 );
		return;
	}
	t0 = max( t0, 0.0 );
	t1 = min( t1, tMax );
	if( t1 <= t0 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	// Adaptive COARSE step count from the target step LENGTH (KEEP mechanism): floor u_steps (steep
	// rays keep quality), ceil MAX_COARSE (worst-case grazing cost bound). dtFine refines in-cloud.
	float marchLen   = t1 - t0;
	int   coarseN    = int( ceil( marchLen / max( u_stepLenMax, 1.0 ) ) );
	coarseN          = clamp( coarseN, u_steps, MAX_COARSE );
	float dtCoarse   = marchLen / float( coarseN );
	float dtFine     = dtCoarse / max( u_fineDiv, 1.0 );

	float jit      = ign( gl_FragCoord.xy, u_frame );   // animated start jitter (breaks first-plane banding)
	float pixPhase = ign( gl_FragCoord.xy, 0.0 );       // FRAME-STABLE per-pixel blue-noise-ish phase (no shimmer)
	float t        = t0 + dtCoarse * jit;

	// R5 debug accumulators (near-free; consumed only when u_dbgMode>0).
	float dbgDens = 0.0; int dbgSteps = 0; float dbgFirstHitT = -1.0; vec3 dbgScatter = vec3( 0.0 );

	float cosT        = dot( rd, u_lightDir );                       // +1 => view looks TOWARD the lit body (backlit)
	float towardLight = smoothstep( -0.15, 0.55, cosT );             // silver-lining gate (looking into the light)

	// cone light EXPONENTIAL spacing: dense near the sample, sparse far. segments 2^j sum to u_lightReach.
	float coneDenom = exp2( float( u_lightSteps ) ) - 1.0;
	float coneUnit  = u_lightReach / max( coneDenom, 1.0 );

	vec3  L      = vec3( 0.0 );
	float Tview  = 1.0;
	bool  inCloud = false;
	int   emptyRun = 0;                 // consecutive empty FINE samples (revert to coarse after a full cell)
	const float EPS = 0.0015;

	// EMPTY-SPACE-SKIP (coarse) -> first-hit STEP-BACK -> FINE in-cloud steps -> resume coarse on exit.
	for( int i = 0; i < MAX_ITER; i++ )
	{
		if( t >= t1 ) break;
		float dtCur = inCloud ? dtFine : dtCoarse;
		// frame-stable per-step dither of the SAMPLE position (LOWER amplitude at half-res: +/-0.25 cell).
		// Only the sample position is dithered; the t advance (Beer-Lambert slab thickness) is unchanged.
		float stepJit = ( fract( pixPhase + float( i ) * 0.61803398875 ) - 0.5 ) * 0.5;
		vec3  p = ro + rd * ( t + dtCur * stepJit );
		float dens = SampleCloudDensity( p, 1 );
		// far-tail fade: ramp the coarsely-sampled far tail to nothing over the last ~30% of the march.
		dens *= 1.0 - smoothstep( u_marchFar * 0.70, u_marchFar, t );
		dbgSteps++; dbgDens += dens * dtCur;
		if( dens > EPS && dbgFirstHitT < 0.0 ) dbgFirstHitT = t;

		if( !inCloud )
		{
			if( dens > EPS )
			{
				// FIRST HIT: step back one coarse cell, switch to fine (pins the leading silhouette).
				t = max( t - dtCoarse, t0 );
				inCloud  = true;
				emptyRun = 0;
				continue;
			}
			t += dtCoarse;               // empty-space skip
			continue;
		}

		// --- in cloud ---
		if( dens > EPS )
		{
			emptyRun = 0;
			float hf = clamp( ( p.z - u_boxMin.z ) / max( u_boxMax.z - u_boxMin.z, 1e-3 ), 0.0, 1.0 );

			// cone light march toward the lit body (BASE density only) -> cone optical depth (6 exp taps).
			float lt = 0.0, lpos = 0.0;
			for( int j = 0; j < MAX_LIGHT; j++ )
			{
				if( j >= u_lightSteps ) break;
				float seg = coneUnit * exp2( float( j ) );
				vec3  lp  = p + u_lightDir * ( lpos + seg * 0.5 );
				lt   += SampleCloudDensity( lp, 0 ) * seg;
				lpos += seg;
			}
			float tauL = u_sigmaT * lt;

			// HILLAIRE-STYLE MULTI-SCATTER: 3 octaves halving extinction / phase anisotropy / weight,
			// octave 0 carrying the dual-lobe HG (0.8 fwd / 0.2 back). No extra marching.
			float scatter = 0.0;
			float weight = 1.0, ext = 1.0, g = 0.80;
			for( int o = 0; o < 3; o++ )
			{
				float Tr = exp( -tauL * ext * u_selfShadow );
				float ph = 0.8 * hg( cosT, g ) + 0.2 * hg( cosT, -0.15 * ext );
				scatter += weight * Tr * ph;
				weight *= 0.45;
				ext    *= 0.55;
				g      *= 0.55;
			}

			// powder dark-edge sugar (edge-masked so interiors stay full).
			float powderD    = 1.0 - exp( -2.0 * dens * dtFine * 40.0 );
			float powderTerm = mix( 1.0, powderD, u_powder * 0.5 );

			// SILVER LINING: ONLY on thin / backlit / translucent edges (the ONLY route to bright edges).
			float thin = smoothstep( 0.02, 0.14, dens ) * ( 1.0 - smoothstep( 0.25, 0.55, dens ) );
			float pene = smoothstep( 0.95 - 0.10 * u_silverWidth, 0.95, exp( -tauL ) );
			float rim  = u_silver * towardLight * thin * pene * hg( cosT, 0.75 );

			// SKY-VISIBILITY AMBIENT (cheap upward AO), decorrelated near zenith; desaturate deep cores.
			float vertAO = smoothstep( 0.72, 0.92, abs( rd.z ) );
			float aoAng  = pixPhase * 6.2831853;
			vec2  aoTilt = vec2( cos( aoAng ), sin( aoAng ) ) * ( vertAO * 0.40 );
			float skyVis = exp( -0.5 * u_sigmaT * TraceDensityUp( p, 3, 900.0, aoTilt ) );
			vec3  ambient = u_ambSky * skyVis * mix( 0.35, 1.0, hf )
			              + u_ambGround * 0.35 * ( 1.0 - hf );
			float ambGrey = dot( ambient, vec3( 0.3333 ) );
			ambient = mix( ambient, vec3( ambGrey ), 0.40 * smoothstep( 0.10, 0.55, dens ) );

			// energy-conserving in-scatter slice (Beer-Lambert) over the FINE step.
			float stepT      = exp( -u_sigmaT * dens * dtFine );
			float sunVis     = exp( -tauL * u_selfShadow );
			// DIRECT-sun forward beam, CLAMPED phase (min(HG,1.5)) so day caps do not clip white;
			// octave-0 of multiscatter already carries the forward lobe so u_sunForward stays low (~0.6).
			float directBeam = u_sunForward * sunVis * min( hg( cosT, u_sunG ), 1.5 );

			// BROAD SUNLIT CAP from the density-gradient normal (6 taps), gated by u_capLight / sunVis / height.
			float capLight = 0.0;
			if( u_capLight > 0.001 )
			{
				float e  = u_capEps;
				float gx = SampleCloudDensity( p + vec3( e, 0.0, 0.0 ), 0 ) - SampleCloudDensity( p - vec3( e, 0.0, 0.0 ), 0 );
				float gy = SampleCloudDensity( p + vec3( 0.0, e, 0.0 ), 0 ) - SampleCloudDensity( p - vec3( 0.0, e, 0.0 ), 0 );
				float gz = SampleCloudDensity( p + vec3( 0.0, 0.0, e ), 0 ) - SampleCloudDensity( p - vec3( 0.0, 0.0, e ), 0 );
				vec3  gN = vec3( gx, gy, gz );
				float gl = length( gN );
				vec3  N  = ( gl > 1e-6 ) ? ( -gN / gl ) : vec3( 0.0, 0.0, 1.0 );   // outward (toward thinner cloud)
				float wrap = clamp( dot( N, u_lightDir ) * 0.5 + 0.5, 0.0, 1.0 );
				capLight = u_capLight * pow( wrap, 1.3 ) * sunVis * mix( 0.25, 1.0, hf );
			}

			// neutral-warm cloud ALBEDO on the DIRECT-lit response; cool sky ambient stays separate.
			// capLight is added OUTSIDE the powder term so broad faces are not edge-darkened.
			vec3 capAlbedo = vec3( 1.0, 0.94, 0.84 );
			vec3 S = u_lightColor * capAlbedo * ( ( scatter + directBeam ) * powderTerm + capLight + rim ) + ambient;
			dbgScatter += Tview * ( 1.0 - stepT ) * u_lightColor * ( scatter + directBeam );   // R5 mode 5
			L     += Tview * ( 1.0 - stepT ) * S;
			Tview *= stepT;
			if( Tview < 0.01 ) break;
			t += dtFine;
		}
		else
		{
			// empty FINE sample: keep fine-stepping; only resume coarse after a full coarse cell of gap
			// (so we never oscillate against the step-back at the leading edge).
			emptyRun++;
			t += dtFine;
			if( float( emptyRun ) > u_fineDiv + 2.0 )
			{
				inCloud  = false;
				emptyRun = 0;
			}
		}
	}

	// ===================================================================================
	// REBUILD v2 NIGHT AUTHORITY (§3.E): dim TOTAL marched radiance (direct + ambient + rim)
	// by u_nightLum, AFTER the march, BEFORE the day soft-knee. alpha = (1-Tview) is UNTOUCHED
	// so the cloud still occludes the night sky/stars correctly; night L stays far below the
	// knee so it never whitens. Day (nightLum=1) is identity.
	// ===================================================================================
	L          *= u_nightLum;
	dbgScatter *= u_nightLum;

	// R5: DEBUG VIZ (modes 1-5), RAW signal (after nightLum, before the knee/elevation fade). Mode 6
	// is handled by the upsample pass (nearest). u_dbgMode==0 => zero cost (single uniform compare).
	if( u_dbgMode > 0 && u_dbgMode <= 5 )
	{
		vec3 dbg = vec3( 0.0 );
		if( u_dbgMode == 1 )      dbg = vec3( clamp( dbgDens * 0.002, 0.0, 1.0 ) );                                              // density (accumulated)
		else if( u_dbgMode == 2 ) dbg = vec3( Tview );                                                                          // transmittance
		else if( u_dbgMode == 3 ) dbg = vec3( float( dbgSteps ) / float( MAX_ITER ) );                                          // step-count heatmap
		else if( u_dbgMode == 4 ) dbg = vec3( clamp( ( dbgFirstHitT < 0.0 ? u_marchFar : dbgFirstHitT ) / u_marchFar, 0.0, 1.0 ) ); // first-hit depth
		else                      dbg = dbgScatter;                                                                             // 5 scatter-only
		fragColor = vec4( dbg, 1.0 );
		return;
	}

	// DAY soft-knee: tightened (kneeK 1.0->1.2, kneeW 6.0->2.2) so the brightest sun-lit caps roll
	// toward the scene white point instead of a 6x plateau (the daytime over-exposure fix, §3.F).
	// Luminance-preserving (hue kept); identity below the knee. Because nightLum was applied first,
	// day and night controls are orthogonal (night L is below the knee => identity => no clip).
	{
		float lum = max( dot( L, vec3( 0.2126, 0.7152, 0.0722 ) ), 1e-4 );
		const float kneeK = 1.2;   // below this luminance: linear, no change
		const float kneeW = 2.2;   // shoulder asymptote (max output luminance)
		if( lum > kneeK )
		{
			float x      = lum - kneeK;
			float mapped = kneeK + ( kneeW - kneeK ) * ( x / ( x + ( kneeW - kneeK ) ) );
			L *= mapped / lum;
		}
	}

	// HORIZON FADE by screen-elevation (rd.z = sin elev). Relaxed to fade_hi 0.03 (half-res shrank the
	// grazing fan) to recover the far deck. Applied to in-scatter L AND alpha together (premultiplied).
	float elevFade = smoothstep( u_horizonFadeLo, u_horizonFadeHi, rd.z );
	float alpha = ( 1.0 - Tview ) * elevFade;
	vec3  Lout  = L * elevFade;
	fragColor = vec4( Lout, alpha );   // premultiplied (L weighted by coverage along the march)
}
)GLSL";

// =============================================================================
// Pass 2 -- JOINT BILATERAL upscale (depth AND cloud-alpha edge-stops) + clamped CAS sharpen +
// premultiplied composite into the HDR scene FBO. CRITICAL: clouds do NOT write depth, so a
// depth-only bilateral fixes cloud-vs-terrain edges but NOT the dominant cloud-vs-sky silhouette;
// the cloud-ALPHA edge-stop is what sharpens that. CAS (AMD FidelityFX model) sharpens the
// silhouette while leaving the dithered interior smooth (no ring/overshoot). dbgMode 6 = raw nearest.
// =============================================================================
static const char kCloudUpsampleFs[] = R"GLSL(#version 330 core
out vec4 fragColor;
uniform sampler2D u_cloudTex;   // half-res march result (LINEAR, CLAMP): .rgb premult radiance, .a coverage
uniform sampler2D u_depthTex;   // FULL-res scene depth (compare-mode NONE)
uniform vec2  u_fullSize;       // full viewport size (px)
uniform float u_zNear;
uniform float u_zFar;
uniform float u_depthSigma;     // DEPTH edge-stop falloff (1/linear-world-u) -> crisp vs terrain
uniform float u_alphaSigma;     // cloud-ALPHA edge-stop falloff -> crisp cloud-vs-sky silhouette
uniform float u_casAmount;      // CAS sharpen amount (0 = bilateral only)
uniform int   u_dbgMode;        // R5: 6 = RAW nearest-neighbour upsample (no bilinear), else bilateral+CAS

float linDepth( float draw )
{
	float z = draw * 2.0 - 1.0;   // NDC
	return ( 2.0 * u_zNear * u_zFar ) / max( u_zFar + u_zNear - z * ( u_zFar - u_zNear ), 1e-4 );
}

void main()
{
	vec2 uv = gl_FragCoord.xy / u_fullSize;

	// R5 mode 6: nearest-neighbour fetch so the capture can separate the RAW half-res ray-phase fan
	// from magnification (texelFetch snaps to the source texel, no interpolation/bilateral/sharpen).
	if( u_dbgMode == 6 )
	{
		ivec2 qs = textureSize( u_cloudTex, 0 );
		fragColor = texelFetch( u_cloudTex, ivec2( clamp( uv, 0.0, 0.999999 ) * vec2( qs ) ), 0 );
		return;
	}

	vec2  lowSize = vec2( textureSize( u_cloudTex, 0 ) );
	vec2  texel   = 1.0 / lowSize;
	float refD    = linDepth( texture( u_depthTex, uv ).r );          // full-res scene depth at this pixel
	float centerA = texture( u_cloudTex, uv ).a;                      // bilinear center alpha estimate

	// JOINT BILATERAL over the 2x2 low-res tap neighbourhood: bilinear weight x depth edge-stop x
	// alpha edge-stop. Taps across a scene-depth discontinuity (terrain) OR a cloud-alpha edge (sky)
	// are rejected => the silhouette stays crisp instead of a 2x bilinear smear.
	vec2 f    = uv * lowSize - 0.5;
	vec2 fr   = fract( f );
	vec2 base = ( floor( f ) + 0.5 ) * texel;
	vec4 acc  = vec4( 0.0 );
	float wsum = 0.0;
	for( int j = 0; j < 2; j++ )
	for( int k = 0; k < 2; k++ )
	{
		vec2  tuv = base + vec2( float( k ), float( j ) ) * texel;
		vec4  cl  = texture( u_cloudTex, tuv );
		float bw  = ( ( k == 0 ) ? ( 1.0 - fr.x ) : fr.x ) * ( ( j == 0 ) ? ( 1.0 - fr.y ) : fr.y );
		float td  = linDepth( texture( u_depthTex, tuv ).r );
		float wD  = exp( -abs( td - refD ) * u_depthSigma );
		float wA  = exp( -abs( cl.a - centerA ) * u_alphaSigma );
		float w   = bw * wD * wA + 1e-6;
		acc  += cl * w;
		wsum += w;
	}
	vec4 cloud = acc / max( wsum, 1e-4 );

	// CLAMPED CAS-style contrast-adaptive sharpen (AMD FidelityFX model): sharpen low-contrast,
	// back off on high-contrast/HDR-bright => crisps the silhouette without ringing the dithered
	// interior or banding. Neighbours sampled at the SOURCE (low-res) grid so the sharpen is meaningful.
	if( u_casAmount > 0.001 )
	{
		vec4 nN = texture( u_cloudTex, uv + vec2( 0.0, -texel.y ) );
		vec4 nS = texture( u_cloudTex, uv + vec2( 0.0,  texel.y ) );
		vec4 nE = texture( u_cloudTex, uv + vec2(  texel.x, 0.0 ) );
		vec4 nW = texture( u_cloudTex, uv + vec2( -texel.x, 0.0 ) );
		vec4 mn = min( cloud, min( min( nN, nS ), min( nE, nW ) ) );
		vec4 mx = max( cloud, max( max( nN, nS ), max( nE, nW ) ) );
		vec4 amp  = sqrt( clamp( min( mn, 1.0 - mx ) / max( mx, 1e-4 ), 0.0, 1.0 ) );
		float peak = -1.0 / mix( 8.0, 5.0, clamp( u_casAmount, 0.0, 1.0 ) );   // [-0.125,-0.20]
		vec4 w4   = amp * peak;                                                // [-0.20,0] per channel
		vec4 sharp = ( cloud + ( nN + nS + nE + nW ) * w4 ) / ( 1.0 + 4.0 * w4 );   // denom in [0.2,1] (safe)
		cloud = mix( cloud, sharp, clamp( u_casAmount, 0.0, 1.0 ) );
	}

	cloud.rgb = max( cloud.rgb, vec3( 0.0 ) );
	cloud.a   = clamp( cloud.a, 0.0, 1.0 );
	fragColor = cloud;   // premultiplied; caller composites with kBlendPremulOver
}
)GLSL";
