/*
 * csz_cloudvol_shaders.inl -- CSOZ renderer: volumetric cloud REBUILD v2 GLSL (Phase 0)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). PROVENANCE / LICENSE:
 *   - The ray-box march, the density model (Perlin-Worley base dilated by a Worley
 *     FBM, a cumulus height gradient, coverage gate, high-freq Worley edge erosion --
 *     the "Nubis"-class remap chain) and the lighting (Beer-Lambert transmittance,
 *     cone light-march, multi-scatter octave reuse, powder dark-edge, silver-lining
 *     rim, ground/sky ambient) are ORIGINAL work written for CSOZ from first
 *     principles and the PUBLISHED algorithm descriptions. Beer-Lambert extinction,
 *     the Henyey-Greenstein phase, Perlin/Worley noise and the standard remap() are
 *     non-proprietary math used directly from their published descriptions. The
 *     depth-linearize / world-position-reconstruct helpers are the standard GL
 *     inverse-viewproj math (same as fog/csz_fog_shaders.inl, re-derived here so this
 *     module never cross-includes fog).
 *   - hash33() is Dave Hoskins' "Hash without Sine"
 *     (https://www.shadertoy.com/view/4djSRW), MIT-licensed (constants are his); only
 *     used in the optional in-shader fallback path. ign() is Jorge Jimenez's
 *     Interleaved Gradient Noise (SIGGRAPH 2014), magic constants verbatim.
 *   No code, permutation table or gradient table is copied or translated from
 *   Unreal/Unity/Frostbite/Hillaire sample code, GPU-Gems/GPU-Pro snippets, PrimeXT,
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
// Included ONLY by geom/clouds/csz_cloudvol.cpp. GL3.3 core / GLES3 / WebGL2
// intersection. Uniform names are this program's private contract (set CPU-side in
// CloudVolRenderer::Contribute).

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
// Pass 1 -- QUARTER-RES, WORLD-SPACE, DEPTH-BOUNDED ray-box march into an RGBA16F
// offscreen target. Output = (premultiplied in-scatter radiance .rgb, coverage alpha
// = 1 - Tview). Composited later with kBlendPremulOver. LINEAR HDR (pre-tonemap).
//
// Unlike the rejected csz_volcloud (camera-relative sky slab, composited BEFORE the
// world), this marches a bounded WORLD-SPACE AABB and clamps the march to the scene
// depth surface (tMax = min(tSurf, marchFar)) so TERRAIN OCCLUDES the cloud and the
// cloud SIDES are visible. Density comes from the baked 3D Perlin-Worley textures.
// =============================================================================
static const char kCloudMarchFs[] = R"GLSL(#version 330 core
in vec3 v_dir;
out vec4 fragColor;

uniform vec3  u_camPos;        // ray origin (world, Quake Z-up)
uniform vec3  u_lightDir;      // world dir toward the LIT body (sun by day / moon by night)
uniform vec3  u_lightColor;    // linear HDR radiance of the lit body
uniform vec3  u_ambGround;     // ambient skylight toward the cloud UNDERSIDE (darker)
uniform vec3  u_ambSky;        // ambient skylight toward the cloud TOP (sky/zenith)
uniform float u_skyVisFloor;   // P3: overcast skylight floor for skyVis (weather-gated; 0 = clear NORMAL, no floor)
uniform vec3  u_boxMin;        // hero AABB min corner (world)
uniform vec3  u_boxMax;        // hero AABB max corner (world)
uniform float u_time;          // bounded client time (s) for slow wind scroll
uniform float u_frame;         // per-frame jitter lattice offset (animated IGN)
uniform vec3  u_windVec;       // world-u/sec horizontal wind drift (dir*speed); drift = u_windVec*u_time
uniform float u_evolveRate;    // slow volume-EVOLVE (morph) rate: advances the noise sample THROUGH the volume
uniform float u_density;       // density multiplier
uniform float u_coverage;      // 0..1 coverage gate (more => fuller box)
uniform float u_hBase;         // cumulus height gradient: flat-ish base ramp-in fraction [0,u_hBase]
uniform float u_hTop;          // cumulus height gradient: rounded top fade-out start fraction [u_hTop,1]
uniform float u_silver;        // silver-lining (forward-scatter) rim strength
uniform float u_silverWidth;   // rim band width: LOW=broad glow reaching inward, HIGH=razor edge only
uniform float u_sigmaT;        // extinction coefficient (1/world-units along the march)
uniform float u_baseFreq;      // base 3D-noise frequency (1/world-units)
uniform float u_detailFreq;    // detail 3D-noise frequency (1/world-units)
uniform float u_detailAmt;     // high-frequency Worley EDGE-erosion strength (interiors stay smooth)
uniform float u_falloff;       // SAFETY-fade band: density->0 within this outer fraction of each AABB face
uniform float u_powder;        // powder dark-edge strength
uniform float u_erodeDepth;    // high-freq edge-erosion DEPTH (how far the detail bites the envelope edge)
uniform int   u_erodeOct;      // detail erosion octave count 1..3
uniform float u_selfShadow;    // cone-march self-shadow / multi-scatter extinction weight (internal pockets)
uniform float u_envWarp;       // organic silhouette perturbation amount (low-freq noise warp of the lobe shell)
uniform float u_mid;           // MID-frequency cauliflower strength (rounded packed bumps on the OUTER half)
uniform float u_midFreq;       // mid-frequency 3D-noise frequency (1/world-units)
uniform float u_virga;         // faint rain/virga shaft hanging under the darkest core (0 = off)
uniform float u_capLight;      // BROAD sun-facing CAP light (density-gradient normal); 0 = off (skips the gradient taps)
uniform float u_shelf;         // storm BASE flatten/shelf strength (scales the underside clip-band width)
uniform float u_mammatus;      // small downward MAMMATUS lobes hanging under the base (0 = off)
uniform float u_capEps;        // world-space epsilon for the density-gradient cap normal (set CPU-side ~ box feature scale)
uniform float u_sunForward;    // DIRECT-sun forward-scatter strength (blows sun-facing upper lobes to near-white)
uniform float u_sunG;          // direct-sun forward HG anisotropy g (0.78-0.85)
uniform float u_lightReach;    // TOTAL cone light-march reach toward the lit body (world units)
uniform float u_marchFar;      // hard distance cap (world units)
uniform float u_stepLenMax;    // P0: target MAX world-space step length; the view step COUNT is derived from this (grazing-ray under-sampling cap)
uniform vec2  u_targetSize;    // quarter-res target size in pixels
uniform int   u_steps;         // view march steps
uniform int   u_lightSteps;    // cone light march steps
uniform float u_domainWarp;    // R1/R3: world-space low-freq domain-warp amplitude (decorrelate grazing rays / step planes); 0 = off
uniform float u_horizonFadeLo; // R2: screen-elevation fade LOW threshold (sin elev; fully faded at/below)
uniform float u_horizonFadeHi; // R2: screen-elevation fade HIGH threshold (sin elev; fully present at/above)
uniform int   u_dbgMode;       // R5: debug viz (0 off / 1 density / 2 transmittance / 3 stepcount / 4 first-hit / 5 scatter / 6 raw-nearest-upsample)
uniform int   u_weatherKind;   // iter5: EXPLICIT weather hard-gate (0 = clear NORMAL => old path byte-identical; 1 = rain; 2 = snow). Gates ALL overcast-structure behavior (NOT coverage inference).
uniform float u_overcastVar;   // iter5 P1: low-freq billow injected into the coverage field (overcast only; 0 = off)
uniform float u_lowHaze;       // iter5 P2: cold low-band horizon-haze amplitude (overcast only; 0 = off)
// PATH A (macro cloud distribution): a LOW-FREQUENCY world-XY coverage field replaces the global
// coverage scalar so the sky has REAL large-scale structure (cloudy regions vs clear/open sky)
// for ALL weather states, and an INCOMMENSURATE second base tap de-tiles the within-region shape.
uniform float u_covFreq;       // PATH A: macro coverage-field frequency (1/world-period); period >> marched footprint => no visible repeat
uniform float u_covContrast;   // PATH A: coverage-field spread around the preset level (HIGH=scattered gaps, LOW=mild overcast variation)
uniform float u_covDrift;      // PATH A: weather-system world drift fraction of u_windVec (0 = world-static field; existing cloud drift/evolve unchanged)
uniform float u_detile;        // CHANGE 2: blend weight of the incommensurate (0.73x) second base tap (0 = old single tap; de-repeats within-region shape)

// scene depth + reconstruct (standard inverse-viewproj; re-derived, no fog include)
uniform sampler2D u_depthTex;  // raw window-space scene depth (compare-mode NONE)
uniform float u_zNear;
uniform float u_zFar;
uniform mat4  u_invViewProj;   // inverse(proj*view): clip -> world

// baked structured density (Phase 0 in-process bake; Phase 1 = offline baker + assets)
uniform sampler3D u_base3d;    // 128^3 RGBA8: R = Perlin-Worley, GBA = Worley octaves
uniform sampler3D u_detail3d;  // 32^3 RGBA8: high-freq Worley detail (RGB octaves)

const float PI = 3.14159265358979323846;
// Compile-time caps so the driver can bound the uniform-controlled loops (codex #2/#10);
// the live uniforms clamp BELOW these on the CPU side.
const int MAX_STEPS = 128;   // R3 iter4: 80->128 (finer grazing march; CPU clamp still governs cost)
const int MAX_LIGHT = 8;

// Reconstruct world-space position from screen uv + raw depth d.
vec3 worldPosFromDepth( vec2 uv, float d )
{
	vec4 clip = vec4( uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0 );
	vec4 w = u_invViewProj * clip;
	return w.xyz / w.w;
}

float remap( float v, float a, float b, float c, float d )
{
	return c + ( clamp( ( v - a ) / max( b - a, 1e-4 ), 0.0, 1.0 ) ) * ( d - c );
}

// =============================================================================
// WORLD-SPACE CLOUD LAYER (the anti-CUBE re-architecture, v5 PIVOT). The AABB is now a
// thin HORIZONTAL SLAB spanning the whole sky (huge XY, thin Z = the cloud deck). The
// AABB is ONLY an invisible march bound: density -> 0 at the slab top & bottom via the
// height gradient (the Z faces are never a visible edge) and the XY faces sit far beyond
// u_marchFar (never reached), so NO box silhouette is possible. The silhouette comes
// ENTIRELY from the coverage-thresholded Perlin-Worley field + the cumulus height
// gradient (Nubis/Schneider): LOW coverage => isolated puffy cumulus dotting blue sky,
// HIGH coverage => connected overcast. Animated by a horizontal wind DRIFT + a slow
// volume EVOLVE (the noise sample advances THROUGH the volume, so clouds form/dissipate,
// not just translate). Quake Z-up = vertical, so the slab is thin in Z.
// =============================================================================

// Cumulus height-density gradient across the slab thickness: feathered flat-ish base
// (ramp in over [0,u_hBase]) rounded off toward the top (fade out over [u_hTop,1]), so
// individual clouds get a puffy vertical form instead of a flat sheet. hf in [0,1].
float HeightGradient( float hf )
{
	float baseR = smoothstep( 0.0, max( u_hBase, 0.02 ), hf );
	float topR  = 1.0 - smoothstep( clamp( u_hTop, u_hBase + 0.05, 0.98 ), 1.0, hf );
	return clamp( baseR * topR, 0.0, 1.0 );
}
)GLSL"
// MSVC C2026 (iter5): the per-step structure additions grew SampleCloudDensity past the ~16 KB
// single-literal cap, so split the first march-FS literal here too (the compiler concatenates the
// adjacent raw literals into one contiguous GLSL source -- identical text, no semantic change).
R"GLSL(
// SampleCloudDensity -- the SINGLE density function used by BOTH the view march AND the cone
// light / sky-visibility marches (no hidden "delete density / keep lighting" coupling). The
// coverage-remap Perlin-Worley chain sets the silhouette; the high-freq Worley only ERODES
// edges WITHIN it (anti-popcorn):
//   * Perlin-Worley base (R) DILATED by the Worley FBM (G/B/A) = the SOFT billowy body
//     (NEVER raw Worley = popcorn); the coverage gate slides the existence threshold;
//   * the cumulus height gradient gives vertical puffy form (flat base, rounded top);
//   * high-freq Worley = EDGE-ONLY erosion (the (1-smoothstep) edge weight keeps dense
//     interiors smooth). detail=0 skips ONLY the fine erosion (cheap shadow taps).
// PATH A -- MACRO SPATIAL COVERAGE FIELD over world horizontal XY (Quake Z-up = vertical, so
// horizontal = X,Y). A LOW-FREQUENCY field whose world period (1/u_covFreq, ~40000u) EXCEEDS the
// marched footprint (marchFar ~20000 => ~40000u sky diameter) so it does NOT visibly repeat in
// view: ONE "weather cell" spans the reachable sky => some regions cloudy, others clear/open. Two
// INCOMMENSURATE taps (ratio 0.73, NON-harmonic) push the apparent period to their LCM so even the
// field itself shows no tile seam. WORLD-ANCHORED: u_covDrift defaults 0 (the weather mask is
// world-static) and the cloud TEXTURE still drifts THROUGH it via ps below, so existing wind drift +
// volume evolve animation is UNCHANGED -- clouds simply form on entering a cloudy region and
// dissipate leaving it. Returns 0..1 (the R channel = the smooth Perlin-Worley base). Sampled on
// RAW world p.xy (not the drifted ps) so the distribution is camera-parallax-correct and stable.
float CoverageField( vec2 wxy )
{
	vec2 q  = wxy + u_windVec.xy * ( u_time * u_covDrift );
	float f1 = texture( u_base3d, vec3( q * u_covFreq, 0.317 ) ).r;
	float f2 = texture( u_base3d, vec3( q * ( u_covFreq * 0.73 ) + vec2( 0.41, 0.19 ), 0.622 ) ).r;
	return clamp( 0.6 * f1 + 0.4 * f2, 0.0, 1.0 );
}

float SampleCloudDensity( vec3 p, int detail )
{
	// in-slab height fraction (Quake Z-up): 0 at the cloud base, 1 at the deck top.
	float hf = ( p.z - u_boxMin.z ) / max( u_boxMax.z - u_boxMin.z, 1.0 );
	if( hf <= 0.0 || hf >= 1.0 )
		return 0.0;

	// --- ANIMATION (first-class): horizontal wind DRIFT + slow volume EVOLVE/morph. ---
	// drift scrolls the whole field along the wind; evolve advances the noise sample THROUGH
	// the volume (dominant Z march in noise-space, decoupled from the fixed height gradient) so
	// at a fixed world XY the coverage pattern slowly forms & dissipates -- clouds change shape,
	// not just translate. Detail & mid scroll at DIFFERENT rates so the silhouette frays/morphs
	// rather than sliding as one rigid texture.
	vec3 drift  = u_windVec * u_time;
	vec3 evolve = vec3( 0.18, -0.13, 1.0 ) * ( u_evolveRate * u_time );
	vec3 ps     = p + drift + evolve;

	// R1/R3 (iter4): LOW-FREQ WORLD-SPACE DOMAIN WARP. Two near-parallel grazing rays otherwise
	// march the SAME coarse noise lane -> radial fan; coherent step planes -> corduroy. A world-
	// stable low-freq offset field makes the noise PHASE at a given world point diverge after a
	// short distance, breaking both. Applied CONSISTENTLY as ONE shared offset to base + mid +
	// detail (NOT the rejected ps-only partial). u_domainWarp=0 disables it (capture A/B isolates
	// it vs the per-step dither). World-stable (no screen-space term) => no shimmer.
	vec3 warpOff = ( texture( u_base3d, ps * ( u_baseFreq * 0.37 ) ).gba * 2.0 - 1.0 ) * u_domainWarp;
	ps += warpOff;

	// CHANGE 2 (de-tile): the 128^3 base is GL_REPEAT-tiled at world period 1/u_baseFreq (~3000u),
	// so a single tap repeats the SAME lump 6-13x across the visible sky. Composite a SECOND tap at
	// an INCOMMENSURATE (non-harmonic) frequency u_baseFreq*0.73 with a phase offset: the apparent
	// period becomes the LCM of the two (effectively never inside the footprint), killing the macro
	// repeat WITHOUT a new asset. u_detile=0 => the old single tap exactly. mid/detail taps below stay
	// coherent because they erode the resulting `cloud`, not the raw base.
	vec4 b1    = texture( u_base3d, ps * u_baseFreq );
	vec4 b2    = texture( u_base3d, ps * ( u_baseFreq * 0.73 ) + vec3( 0.19, 0.41, 0.27 ) );
	vec4 b     = mix( b1, b2, u_detile );
	float wfbm = dot( b.gba, vec3( 0.625, 0.25, 0.125 ) );

	// Perlin-Worley base (R) DILATED by the Worley billow FBM => round connected billows.
	float baseCloud = remap( b.r, wfbm - 1.0, 1.0, 0.0, 1.0 );

	// cumulus vertical form (flat base, rounded top).
	baseCloud *= HeightGradient( hf );

	// COVERAGE GATE: slide the existence threshold (low => isolated puffs, high => overcast),
	// then re-scale by coverage to anchor the round billow look (Schneider).
	// PATH A (load-bearing): cov is no longer a single GLOBAL scalar applied identically everywhere
	// (which made "cloudy here / clear there" impossible by construction). It is now a per-column
	// value driven by the MACRO spatial field: u_coverage is the field LEVEL/MEAN (set by the 3
	// weather presets) and CoverageField(p.xy) spreads it spatially by +/- u_covContrast around 0.5.
	//   * weather 0 (scattered): LOW level + HIGH contrast => many columns fall below the gate =>
	//     CLEAR blue sky, the rest = puffy cumulus (the headline distribution).
	//   * weather 1/2 (overcast): HIGH level + MILD contrast => mostly above the gate (full deck) but
	//     not a dead-flat ceiling.
	// Every downstream consumer (body curve, mid mask, edge mask, the light-march / AO re-invocations
	// of SampleCloudDensity at neighbour points) reads THIS same local cov, so they inherit the
	// spatial field for free.
	float covMean = clamp( u_coverage, 0.0, 1.0 );
	float cov     = clamp( covMean + ( CoverageField( p.xy ) - 0.5 ) * u_covContrast, 0.0, 1.0 );
	// OVERCAST meso-break (HARD-GATED to weather != 0), layered ON TOP of the macro field for stratiform
	// thick/thin texture WITHIN the overcast regions. At high cov the gate window [1-cov,1] spans almost
	// the whole base range so the base field stops carving patches => featureless deck; this coarse,
	// slowly-drifting base3d tap dips cov so the window narrows and the base carves again. World-stable
	// scale => morphs with the volume, no shimmer. weather==0 skips it (the macro field already varies cov).
	if( u_weatherKind != 0 && u_overcastVar > 0.001 )
	{
		float lowf = texture( u_base3d, ( ps + evolve * 0.5 ) * ( u_baseFreq * 0.45 ) ).r;
		cov = clamp( cov - u_overcastVar * ( 1.0 - lowf ), 0.0, 1.0 );
	}
	float cloud = remap( baseCloud, 1.0 - cov, 1.0, 0.0, 1.0 ) * cov;
	// P2 FIX (popcorn/flat): FATTEN mid densities so the body fills out instead of being
	// treated as thin "edge" material. The coverage gate caps cloud at <=cov (0.40 for NORMAL)
	// while the erosion masks below use ABSOLUTE thresholds -- so without this most of the cloud
	// read as edge => wispy flakes. pow(.,0.6) lifts the body into the masks' interior range
	// (effectively making the absolute thresholds relative to coverage) and leaves clear sky
	// (cloud==0) untouched. NOT smoothstep -- that LOWERS density for cloud<=0.5 (backwards).
	// P1 iter5: coverage-AWARE body curve, HARD-GATED to overcast (weather != 0). pow(0.6) fattens
	// SPARSE cumulus bodies (good at low cov) but is concave => it CRUSHES contrast at high cov (the
	// 0.5..0.95 band the overcast deck lives in collapses to a uniform plateau). Relax the exponent
	// toward ~0.95 as coverage rises so overcast keeps its tonal range. weather==0 => exp 0.60 exactly
	// => pow(.,0.6) byte-identical to the old clear-NORMAL path.
	float bodyExp = ( u_weatherKind != 0 ) ? mix( 0.60, 0.95, smoothstep( 0.55, 0.92, cov ) ) : 0.60;
	cloud = pow( max( cloud, 0.0 ), bodyExp );

	// MID-FREQUENCY cauliflower bumps on the OUTER half only (the (1-smoothstep) outer mask keeps
	// thick interiors smooth = the anti-popcorn rule); scrolls at a different rate so the lumps
	// live/breathe with the morph.
	if( u_mid > 0.001 && cloud > 0.01 )
	{
		vec3  mb    = texture( u_base3d, ( p + drift * 1.3 + evolve * 1.7 + warpOff ) * u_midFreq ).gba;
		float mid   = dot( mb, vec3( 0.6, 0.3, 0.1 ) );          // round billow (NOT raw cells)
		// P1 iter5: keep the MID cauliflower alive in the OVERCAST interior (HARD-GATED). The absolute
		// [0.25,0.70] mask zeroes for cloud>0.70, but the overcast deck is ALL cloud>0.70 => no mid =>
		// smooth slate. Ride the band up with coverage and FLOOR it with max() (BLOCKER 1: the spec's
		// mix(0.25,expr,1.0) is a no-op that returns expr -- a real floor must use max) so mid never
		// fully vanishes in the dense interior. weather==0 => the exact old [0.25,0.70] mask.
		float outer;
		if( u_weatherKind != 0 )
		{
			float covT  = smoothstep( 0.55, 0.92, cov );
			float midLo = mix( 0.25, 0.55, covT );
			float midHi = mix( 0.70, 1.20, covT );
			outer = max( 1.0 - smoothstep( midLo, midHi, cloud ), 0.25 * covT );
		}
		else
		{
			outer = 1.0 - smoothstep( 0.25, 0.70, cloud );   // clear NORMAL: byte-identical
		}
		cloud = remap( cloud, mid * u_mid * outer, 1.0, 0.0, 1.0 );
	}

	// HIGH-FREQUENCY Worley EDGE-ONLY erosion (anti-popcorn): the (1-smoothstep) edge mask is ~0
	// inside thick cloud and 1 only on the thin outer shell, so detail TEARS the silhouette edge
	// without bumping dense interiors. Detail evolves FASTER so edges continuously fray/morph.
	if( detail == 1 && cloud > 0.01 && u_detailAmt > 0.001 )
	{
		vec3 dt = texture( u_detail3d, ( p + drift * 1.5 + evolve * 2.3 + warpOff ) * u_detailFreq ).rgb;
		float dfbm = dt.r;
		float norm = 1.0, amp = 0.5;
		if( u_erodeOct >= 2 ) { dfbm += dt.g * amp; norm += amp; amp *= 0.5; }
		if( u_erodeOct >= 3 ) { dfbm += dt.b * amp; norm += amp; }
		dfbm /= norm;
		// P1 iter5: coverage-relative high-freq erosion (HARD-GATED) so the overcast deck gets a
		// torn, turbulent underside instead of a glassy shell; a small floor keeps fine erosion even
		// in dense cores (rain undersides are RAGGED). weather==0 => the exact old [0.15,0.45] mask.
		float edge;
		if( u_weatherKind != 0 )
		{
			float covT = smoothstep( 0.55, 0.92, cov );
			edge = max( 1.0 - smoothstep( mix( 0.15, 0.45, covT ), mix( 0.45, 1.15, covT ), cloud ), 0.12 * covT );
		}
		else
		{
			edge = 1.0 - smoothstep( 0.15, 0.45, cloud );   // clear NORMAL: byte-identical
		}
		float erodeAmt = u_detailAmt * u_erodeDepth * edge * mix( 0.55, 1.0, hf );
		cloud = remap( cloud, dfbm * erodeAmt, 1.0, 0.0, 1.0 );
	}

	return clamp( cloud, 0.0, 1.0 ) * u_density;
}

// Cheap UPWARD density trace (Quake Z-up) for sky-visibility ambient occlusion: accumulates
// optical-depth toward the sky so deep/under-cloud samples receive LESS sky ambient (AO).
float TraceDensityUp( vec3 p, int steps, float reach, vec2 tilt )
{
	float stepLen = reach / float( max( steps, 1 ) );
	float acc = 0.0;
	for( int i = 0; i < 4; i++ )
	{
		if( i >= steps ) break;
		// P3 iter6 (ZENITH-KNOT DECORRELATE): tilt the upward trace horizontally by a small per-pixel,
		// per-step (i) amount. A near-vertical VIEW ray marches ONE noise column; a PURE (0,0,1) AO trace
		// is COLLINEAR with it and re-darkens that SAME dense column => a persistent zenith "knot". The
		// (i+0.5) growth spreads the 3 taps across adjacent lanes so the AO stops doubling the view column.
		// `tilt` is ZERO for oblique/grazing rays (gated by verticality at the call site) => byte-identical
		// away from zenith, no fan (the anti-fan dither lives on grazing rays). The trace stays physically
		// "up" (z keeps the original advance); we only nudge XY. Deterministic (no frame/random term) =>
		// no shimmer. This DECORRELATES (does NOT fade) the AO, so overhead clouds stay legitimately dark.
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
// MSVC C2026: a single string literal caps at ~16 KB -- split the march FS into two adjacent
// raw literals here (the compiler concatenates them into one contiguous GLSL source).
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

	// --- bound the march to the hero AABB ---
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

	// P0 FIX (radial horizon streak): adaptive step COUNT from a target step LENGTH (codex-
	// corrected; NOT the inverted fix-spec `max(marchLen/steps, stepLenMax)`). A grazing ray
	// stays inside the thin slab for a huge horizontal distance, so a FIXED count gave ~940u
	// steps at the horizon = radial spoke aliasing. Derive the count from u_stepLenMax; clamp
	// the FLOOR to u_steps (steep rays keep the old quality) and the CEILING to MAX_STEPS (the
	// worst-case grazing cost is bounded). stepLen is then the exact even division.
	float marchLen = t1 - t0;
	int   steps    = int( ceil( marchLen / max( u_stepLenMax, 1.0 ) ) );
	steps          = clamp( steps, u_steps, MAX_STEPS );
	float stepLen  = marchLen / float( steps );
	float jit = ign( gl_FragCoord.xy, u_frame );
	float t = t0 + stepLen * jit;
	// R1 (iter4) PRIMARY FAN/RIB KILLER: per-step stratified dither of the SAMPLE position.
	// pixPhase is a FRAME-STABLE per-pixel blue-noise-ish phase (ign WITHOUT u_frame => identical
	// every frame => NO shimmer, NO temporal history). In the loop each step adds a golden-ratio
	// increment of pixPhase, so the step cadence is decorrelated per-pixel AND per-step -> adjacent
	// grazing rays no longer share a coarse noise lane (kills the radial fan) and the slab planes
	// stop aliasing into corduroy ribs. Applied to the sample position only; t += stepLen cadence
	// (and thus the Beer-Lambert slab thickness) is unchanged, so transmittance stays energy-exact.
	float pixPhase = ign( gl_FragCoord.xy, 0.0 );

	// R5 debug accumulators (near-free; consumed only when u_dbgMode>0).
	float dbgDens = 0.0; int dbgSteps = 0; float dbgFirstHitT = -1.0; vec3 dbgScatter = vec3( 0.0 );

	float cosT   = dot( rd, u_lightDir );   // +1 => view looks TOWARD the lit body (forward scatter / BACKLIT cloud)
	// Toward-light gate for the silver lining: rises as the view turns INTO the light (the real
	// golden-hour gameplay case = looking toward the low bright sun THROUGH the cloud).
	float towardLight = smoothstep( -0.15, 0.55, cosT );

	float lightStepLen = u_lightReach / float( max( u_lightSteps, 1 ) );

	vec3  L = vec3( 0.0 );
	float Tview = 1.0;

	for( int i = 0; i < MAX_STEPS; i++ )
	{
		if( i >= steps ) break;
		// R1: per-step blue-noise-ish dither of the SAMPLE position (within +/-0.5 step cell).
		float stepJit = fract( pixPhase + float( i ) * 0.61803398875 ) - 0.5;
		vec3 p = ro + rd * ( t + stepLen * stepJit );
		float dens = SampleCloudDensity( p, 1 );
		// P0 HORIZON FADE: ramp the coarsely-sampled far tail to nothing over the last ~30% of
		// the march so any residual grazing-ray step aliasing near u_marchFar is invisible (the
		// thin deck is sub-pixel out there anyway). Fades opacity AND in-scatter together.
		dens *= 1.0 - smoothstep( u_marchFar * 0.70, u_marchFar, t );
		dbgSteps++; dbgDens += dens * stepLen;
		if( dens > 0.0015 && dbgFirstHitT < 0.0 ) dbgFirstHitT = t;
		if( dens > 0.0015 )
		{
			float hf = clamp( ( p.z - u_boxMin.z ) / max( u_boxMax.z - u_boxMin.z, 1.0 ), 0.0, 1.0 );

			// --- cone light march toward the lit body (BASE density only) -> cone optical depth ---
			float lt = 0.0;
			for( int j = 0; j < MAX_LIGHT; j++ )
			{
				if( j >= u_lightSteps ) break;
				vec3 lp = p + u_lightDir * ( lightStepLen * ( float( j ) + 0.5 ) );
				lt += SampleCloudDensity( lp, 0 ) * lightStepLen;
			}
			float tauL = u_sigmaT * lt;

			// --- HILLAIRE-STYLE MULTI-SCATTER: 3 octaves, each HALVING extinction / phase
			// anisotropy / contribution weight -> the soft, deep, layered in-scatter glow of a real
			// cumulus, NO extra marching. octave 0 carries the dual-lobe HG (0.8 forward / -0.15 back),
			// deeper octaves broaden into the diffuse multi-scatter floor. u_selfShadow weights the
			// cone extinction so lit lobes still pop against shadowed valleys/undersides. ---
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

			// powder dark-edge sugar: darkens the THIN lit edges (the dark-sugar look), interiors full.
			float powderD    = 1.0 - exp( -2.0 * dens * stepLen * 40.0 );
			float powderTerm = mix( 1.0, powderD, u_powder * 0.5 );

			// SILVER LINING: ONLY on THIN / backlit / translucent edges (the codex `thin` mask), NOT
			// sqrt(Tl) frosting the whole top. Fires looking toward the lit body where light still
			// penetrates (exp(-tauL) high); u_silverWidth broadens the penetration band inward.
			float thin = smoothstep( 0.02, 0.14, dens ) * ( 1.0 - smoothstep( 0.25, 0.55, dens ) );
			float pene = smoothstep( 0.95 - 0.10 * u_silverWidth, 0.95, exp( -tauL ) );
			float rim  = u_silver * towardLight * thin * pene * hg( cosT, 0.75 );

			// SKY-VISIBILITY AMBIENT: cheap upward density trace -> ambient occlusion; sky term scaled
			// by sky-vis * height gradient, plus a dim ground-bounce term (undersides not black).
			// P3 iter6: decorrelate the upward AO trace from a near-vertical VIEW ray (which integrates ONE
			// noise column) by tilting the trace XY a small, deterministic per-pixel amount. vertAO gates it
			// to near-vertical rays only (abs(rd.z) in [0.85,0.99]) => oblique/grazing views get tilt=0 =>
			// BYTE-IDENTICAL there + no fan risk. pixPhase (frame-stable per-pixel, see inl:415) picks a
			// temporally-stable direction (no shimmer); magnitude max ~0.40 (= ~22deg off vertical) so the
			// trace stays "up" but samples fresh lanes => the knot dissolves WITHOUT fading AO.
			// P2 iter7 FIX-1: the iter6 gate smoothstep(0.85,0.99) NEVER FIRED at the knot's elevation
			// (knot sits near top-center at abs(rd.z)~0.82-0.88, BELOW the 0.85 onset => vertAO~0 =>
			// tilt~0 => w0 byte-identical, knot persisted). Lower the onset to 0.72 (saturate 0.92) so
			// the EXISTING AO-direction tilt engages over the knot WITHOUT grabbing the broad upper sky
			// (codex: 0.55 = ~33deg = upper third = over-reach; do NOT use 0.55). abs(rd.z)<0.72
			// (oblique/grazing, the gameplay norm) still gets vertAO=0 => byte-identical there + NO fan
			// (the anti-fan dither lives on grazing rays, untouched). Intentionally touches near-zenith
			// w0 pixels only (the zenith fix). FIX-2 (primary-ray XY jitter) DEFERRED this pass.
			float vertAO = smoothstep( 0.72, 0.92, abs( rd.z ) );
			float aoAng  = pixPhase * 6.2831853;
			vec2  aoTilt = vec2( cos( aoAng ), sin( aoAng ) ) * ( vertAO * 0.40 );
			float skyVis = exp( -0.5 * u_sigmaT * TraceDensityUp( p, 3, 900.0, aoTilt ) );
				// P3: WEATHER-GATED overcast skylight floor. In a dense overcast deck skyVis collapses
				// toward 0 => ambient -> black ceiling (rejected rain/snow look). Floor it ONLY for
				// overcast (u_skyVisFloor>0 set CPU-side for rain/snow); clear NORMAL keeps floor 0 so
				// its lit/shadow contrast is NOT flattened (codex: no GLOBAL skyVis clamp).
					// P1-FIX iter6 (AO VARIANCE RESTORE -- the key rain lever): the hard max(skyVis,floor)
				// clamped EVERY overcast AO sample flat to 0.40 => zero spatial variation => rain read as a
				// smooth dark dome (skyLumaSD~4). Replace with a variance-PRESERVING LIFT into [F,1]: deep
				// cores stay near F, thin breaks rise toward 1 => the shelf-banding returns. F=0.18 (= the
				// 0.40 floor * 0.45) keeps the floor LOW so the deck MEAN does NOT rise (codex: lifting at
				// 0.40 raised mean ~ skyVis 0.2 -> 0.52). weather==0 => u_skyVisFloor=0 => aoFloor=0 =>
				// skyVis unchanged (BYTE-IDENTICAL clear NORMAL). u_skyVisFloor stays 0.40 for the inl:505
				// ambient term (untouched); only THIS AO lift uses the lower 0.18 floor.
				// P1 iter7 (2): drop the AO-lift pedestal 0.45->0.25 (F 0.18->0.10). The 0.18 pedestal
				// added a constant floor to every overcast skyVis, RAISING the ambient base and diluting
				// the AO swing's relative contrast => deep cores could not go dark enough (SD stuck ~7).
				// 0.10 lets cores sit lower (wider AO range = more variance) and can only LOWER the mean,
				// never raise it. weather==0 => u_skyVisFloor=0 => aoFloor=0 => skyVis unchanged (BYTE-IDENTICAL).
				float aoFloor = u_skyVisFloor * 0.25;
				skyVis = aoFloor + ( 1.0 - aoFloor ) * skyVis;
			vec3  ambient = u_ambSky * skyVis * mix( 0.35, 1.0, hf )
			              + u_ambGround * 0.35 * ( 1.0 - hf );
				// R4 (iter4): OVERCAST UNDERSIDE LIFT. The mix(0.35,1.0,hf) height scale crushes the
				// underside even with the weather-gated skyVis floor => RAIN/SNOW read black at a low
				// upward vantage. Add a skylight floor GATED TOWARD THE UNDERSIDE (1-smoothstep over hf)
				// so the dark base lifts to a readable blue-grey WITHOUT washing out the lit tops / cap
				// contrast. Weather-gated: u_skyVisFloor=0 for clear NORMAL => term vanishes (untouched).
				// P1 iter5: DENSITY-MODULATE the underside lift so dark turbulent cores stay darker than
				// the thin breaks (rain reads layered, not a flat panel) instead of a spatially-constant
				// glow that re-fills the contrast P1-FIX-1..3 just restored. u_skyVisFloor=0 for clear
				// NORMAL => the whole term is 0 => byte-identical. dens already includes u_density.
				float floorMod = 1.0 - 0.75 * smoothstep( 0.05, 0.55, dens );   // P1-FIX iter6: deepen overcast underside swing 0.55->0.75 => dark turbulent cores read distinctly darker than thin breaks (more band contrast; holds/lowers mean). weather-gated via u_skyVisFloor below (=0 for NORMAL => term vanishes, byte-identical).
				ambient += u_ambSky * u_skyVisFloor * 0.5 * ( 1.0 - smoothstep( 0.0, 0.45, hf ) ) * floorMod;
			// DESATURATE the cool sky-ambient inside dense / shadowed cloud (codex #6): shadows must
			// read NEUTRAL GREY, not blue. Blend the ambient toward its own luminance where density is
			// high (deep cores) so we lose the "blue-grey smoke" cast without flattening the lit caps.
			float ambGrey = dot( ambient, vec3( 0.3333 ) );
			ambient = mix( ambient, vec3( ambGrey ), 0.40 * smoothstep( 0.10, 0.55, dens ) );
)GLSL"
// (split raw-string literal: MSVC C2026 caps a single string literal ~16KB; adjacent literals concat)
R"GLSL(

			// energy-conserving in-scatter slice (Beer-Lambert)
			float stepT = exp( -u_sigmaT * dens * stepLen );
			// DIRECT-SUN FORWARD SCATTER (codex #3): a punchy forward lobe gated by SUN EXPOSURE
				// (low cone optical depth => sun-facing). Blows sun-lit upper turrets toward near-WHITE
				// while shadowed cores (high tauL) stay dark grey/near-black -- the HARD storm tonal
				// range (do NOT chase this by lowering ambient, which would re-flatten the cloud).
				float sunVis     = exp( -tauL * u_selfShadow );
				float directBeam = u_sunForward * sunVis * hg( cosT, u_sunG );

				// BROAD SUNLIT CAP (codex compare2 lighting #1): the bright areas were only thin EDGES
				// (silver rim) -- not broad sun-facing FACES. Estimate a surface NORMAL from the density
				// gradient (central differences on SampleCloudDensity) and add a broad wrap-light on the
				// SUN-FACING cap/turret faces. Gated by u_capLight (0 => skip the 6 gradient taps), by sun
				// visibility (shadowed cores stay dark) and by height (the base shelf stays a dark slab).
				float capLight = 0.0;
				if( u_capLight > 0.001 )
				{
					float e  = u_capEps;
					float gx = SampleCloudDensity( p + vec3( e, 0.0, 0.0 ), 0 ) - SampleCloudDensity( p - vec3( e, 0.0, 0.0 ), 0 );
					float gy = SampleCloudDensity( p + vec3( 0.0, e, 0.0 ), 0 ) - SampleCloudDensity( p - vec3( 0.0, e, 0.0 ), 0 );
					float gz = SampleCloudDensity( p + vec3( 0.0, 0.0, e ), 0 ) - SampleCloudDensity( p - vec3( 0.0, 0.0, e ), 0 );
					vec3  g  = vec3( gx, gy, gz );
					float gl = length( g );
					vec3  N  = ( gl > 1e-6 ) ? ( -g / gl ) : vec3( 0.0, 0.0, 1.0 );   // outward (toward thinner cloud)
					float wrap = clamp( dot( N, u_lightDir ) * 0.5 + 0.5, 0.0, 1.0 );
					capLight = u_capLight * pow( wrap, 1.3 ) * sunVis * mix( 0.25, 1.0, hf );   // P1: pow 2.0->1.3 = broader sun-facing FACE (not just a thin edge lobe)
				}

				// neutral-warm cloud ALBEDO on the DIRECT-lit response (codex #6): sun-lit caps read
				// warm-white; the cool sky AMBIENT below stays separate so shadows go grey, not blue.
				// capLight is added OUTSIDE the powder term so the broad faces are not edge-darkened.
				vec3 capAlbedo = vec3( 1.0, 0.94, 0.84 );
				vec3 S = u_lightColor * capAlbedo * ( ( scatter + directBeam ) * powderTerm + capLight + rim ) + ambient;
			dbgScatter += Tview * ( 1.0 - stepT ) * u_lightColor * ( scatter + directBeam );   // R5 mode 5
			L += Tview * ( 1.0 - stepT ) * S;
			Tview *= stepT;
			if( Tview < 0.01 )
				break;
		}
		t += stepLen;
	}

	// R5: DEBUG VIZ (modes 1-5). Branch AFTER the early-outs (empty pixels stayed black) and
	// BEFORE the final premultiplied write, so the diagnostic is the RAW quarter-res signal (no
	// elevation fade). Mode 6 falls through to the normal cloud output and is handled by the
	// upsample pass (nearest, no bilinear). u_dbgMode==0 => zero cost (single uniform compare).
	if( u_dbgMode > 0 && u_dbgMode <= 5 )
	{
		vec3 dbg = vec3( 0.0 );
		if( u_dbgMode == 1 )      dbg = vec3( clamp( dbgDens * 0.002, 0.0, 1.0 ) );                                              // density (accumulated)
		else if( u_dbgMode == 2 ) dbg = vec3( Tview );                                                                          // transmittance
		else if( u_dbgMode == 3 ) dbg = vec3( float( dbgSteps ) / float( MAX_STEPS ) );                                         // step-count heatmap
		else if( u_dbgMode == 4 ) dbg = vec3( clamp( ( dbgFirstHitT < 0.0 ? u_marchFar : dbgFirstHitT ) / u_marchFar, 0.0, 1.0 ) ); // first-hit depth
		else                      dbg = dbgScatter;                                                                             // 5 scatter-only
		fragColor = vec4( dbg, 1.0 );
		return;
	}

	// R2 (iter4): HORIZON FADE BY SCREEN-ELEVATION (not world distance). rd is normalized so
	// rd.z = sin(elevation). Fade the cloud to nothing across the thin near-horizon band where the
	// grazing-ray fan/ribs live, independent of march distance. Applied to in-scatter L AND alpha
	// together (stay premultiplied). The existing distance fade (above) is kept as far-tail insurance.
	// P4 iter5: LUMINANCE-PRESERVING soft-knee tonemap on the cloud in-scatter. Identity BELOW the
	// knee (mids/shadows byte-stable, incl. clear NORMAL non-clipping deck) -- only the blown sun-lit
	// caps above the knee roll off toward W instead of clipping flat. Scaled by luminance (NOT
	// per-channel) so hue/saturation are preserved (codex: per-channel desaturates/flattens). This
	// bounds only the cloud's OWN highlight energy; the HDR resolve still owns final exposure.
	{
		float lum = max( dot( L, vec3( 0.2126, 0.7152, 0.0722 ) ), 1e-4 );
		const float kneeK = 1.0;   // below this luminance: linear, no change
		const float kneeW = 6.0;   // shoulder asymptote (max output luminance)
		if( lum > kneeK )
		{
			float x      = lum - kneeK;
			float mapped  = kneeK + ( kneeW - kneeK ) * ( x / ( x + ( kneeW - kneeK ) ) );
			L *= mapped / lum;
		}
	}

	// P1 iter7 (3) — codex's DIRECT SD lever: weather-gated CONTRAST S-curve on the final cloud luma.
	// The rain deck's integrated luma L clusters in a narrow LOW band (~0.10-0.40) => low skyLumaSD
	// (~7) even after the AO/selfShadow levers. Expand contrast around a LOW midpoint so dark turbulent
	// structure spreads (variance UP) while the pivot stays dark (mean NOT washed out): cores below
	// midR darken, breaks above it lift, symmetric about a dark pivot. max(0) clamps so empty/black sky
	// stays black. Gated u_weatherKind!=0 => clear NORMAL (w0) skips this branch entirely => BYTE-
	// IDENTICAL. Tuning dial (capture-gated, per directive): if w1 skyLumaSD<15 raise gain toward 1.5;
	// if w1 mean creeps >~70 lower midR toward 0.15. selfShadow 1.10->0.95 is the next staged lever
	// (NOT pre-applied here).
	if( u_weatherKind != 0 )
	{
		const float scGain = 1.35;   // contrast expansion factor
		const float scMidR = 0.18;   // low pivot (inside the rain luma band) => spreads dark structure
		L = max( vec3( 0.0 ), scMidR + ( L - scMidR ) * scGain );
	}

	// R2 (iter4): horizon fade by screen-elevation. iter5 P2: for OVERCAST the CPU lowers
	// u_horizonFadeHi (->0.03) so the storm deck stays opaque lower toward the horizon and COVERS the
	// warm sky band; clear NORMAL keeps the cvar value unchanged.
	float elevFade = smoothstep( u_horizonFadeLo, u_horizonFadeHi, rd.z );
	float alpha = ( 1.0 - Tview ) * elevFade;
	vec3  Lout  = L * elevFade;

	// P2 iter5 (OVERCAST only): cold low-band haze so any residual sky bleed at the horizon reads as
	// cold rain/snow haze, not a warm sunset gap. Codex BLOCKER-4 form: hazeCov fills ONLY the
	// UNcovered fraction (hazeA*(1-alpha)) -- no double-apply of uncovered coverage. weather==0 skips.
	if( u_weatherKind != 0 && u_lowHaze > 0.0 )
	{
		float lowBand  = 1.0 - smoothstep( 0.02, 0.16, rd.z );    // strongest at the horizon, gone by ~9 deg
		vec3  coldHaze = u_ambSky * 0.6;                          // cold grey from the (already cool) sky ambient
		float hazeA    = clamp( u_lowHaze * lowBand, 0.0, 1.0 );
		float hazeCov  = hazeA * ( 1.0 - alpha );
		Lout  += coldHaze * hazeCov;
		alpha += hazeCov;
	}
	fragColor = vec4( Lout, alpha );   // premultiplied (L weighted by coverage along the march)
}
)GLSL";

// =============================================================================
// Pass 2 -- bilinear upsample + premultiplied composite into the HDR scene FBO.
// Phase 0 is a plain bilinear tap (the LINEAR-filtered quarter-res target gives it for
// free). A scene-depth BILATERAL upsample is Phase 2 polish (DEFERRED). Output is
// premultiplied (rgb, alpha); the caller composites with kBlendPremulOver.
// =============================================================================
static const char kCloudUpsampleFs[] = R"GLSL(#version 330 core
out vec4 fragColor;
uniform sampler2D u_cloudTex;   // quarter-res march result (LINEAR, CLAMP)
uniform vec2 u_fullSize;        // full viewport size (px)
uniform int  u_dbgMode;         // R5: 6 = RAW nearest-neighbour upsample (no bilinear), else bilinear
void main()
{
	vec2 uv = gl_FragCoord.xy / u_fullSize;
	// R5 mode 6: nearest-neighbour fetch so the capture can separate the RAW quarter-res ray-phase
	// fan from bilinear magnification (texelFetch snaps to the source texel, no interpolation).
	if( u_dbgMode == 6 )
	{
		ivec2 qs = textureSize( u_cloudTex, 0 );
		fragColor = texelFetch( u_cloudTex, ivec2( clamp( uv, 0.0, 0.999999 ) * vec2( qs ) ), 0 );
		return;
	}
	fragColor = texture( u_cloudTex, uv );   // bilinear upsample, premultiplied
}
)GLSL";
