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
uniform vec3  u_boxMin;        // hero AABB min corner (world)
uniform vec3  u_boxMax;        // hero AABB max corner (world)
uniform float u_time;          // bounded client time (s) for slow wind scroll
uniform float u_frame;         // per-frame jitter lattice offset (animated IGN)
uniform float u_density;       // density multiplier
uniform float u_coverage;      // 0..1 coverage gate (more => fuller box)
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
uniform float u_lightReach;    // TOTAL cone light-march reach toward the lit body (world units)
uniform float u_marchFar;      // hard distance cap (world units)
uniform vec2  u_targetSize;    // quarter-res target size in pixels
uniform int   u_steps;         // view march steps
uniform int   u_lightSteps;    // cone light march steps

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
const int MAX_STEPS = 96;
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
// ORGANIC MACRO SHAPE (the anti-CUBE re-architecture). The AABB is now ONLY an
// invisible march bound -- the visible silhouette comes from a smooth-union of
// ellipsoid lobes: a wide flat MOTHER BODY + several cauliflower TOWERS at varied
// heights + a rounded CAP, perturbed by low-frequency noise and cut by a ragged,
// broken BASE. This is a believable cumulus/cumulonimbus, not a box. Density
// reaches 0 within the outer u_falloff fraction of every AABB face (a SAFETY fade,
// NEVER the cloud shape). Built in box-normalized coords q in [-1,1]^3 so the lobe
// layout is independent of the hero box's world size (Quake Z-up = vertical).
// =============================================================================
float sdEllipsoid( vec3 p, vec3 r )
{
	vec3 q = p / r;
	return ( length( q ) - 1.0 ) * min( min( r.x, r.y ), r.z );
}

// Polynomial smooth-minimum (metaball union of two SDFs over blend radius k).
float smin( float a, float b, float k )
{
	float t = clamp( 0.5 + 0.5 * ( b - a ) / k, 0.0, 1.0 );
	return mix( b, a, t ) - k * t * ( 1.0 - t );
}

float CloudShapeEnvelope( vec3 p, out float h )
{
	vec3 bc = 0.5 * ( u_boxMin + u_boxMax );
	vec3 bh = max( 0.5 * ( u_boxMax - u_boxMin ), vec3( 1.0 ) );
	vec3 q  = ( p - bc ) / bh;                          // box-normalized [-1,1]^3

	float bound = max( max( abs( q.x ), abs( q.y ) ), abs( q.z ) );
	h = clamp( ( q.z + 0.55 ) / 1.25, 0.0, 1.0 );       // 0 at ragged base .. 1 at crown
	if( bound > 0.999 )
		return 0.0;

	// low-frequency Perlin-Worley (R) sampled at a SHAPE scale, decoupled from the
	// body-noise frequency, used to warp the lobe shell into an organic silhouette.
	float low = texture( u_base3d, ( p + vec3( u_time * 0.25, 0.0, 0.0 ) ) * ( 1.0 / 8500.0 ) ).r;

	// mother body: wide, flat, sitting low in the box.
	float d = sdEllipsoid( q - vec3(  0.00,  0.00, -0.34 ), vec3( 0.74, 0.60, 0.26 ) );
	// stacked cauliflower towers at varied positions / heights / sizes (the billowing crown).
	d = smin( d, sdEllipsoid( q - vec3( -0.34,  0.06, -0.04 ), vec3( 0.32, 0.27, 0.40 ) ), 0.20 );
	d = smin( d, sdEllipsoid( q - vec3(  0.06, -0.12,  0.16 ), vec3( 0.40, 0.32, 0.58 ) ), 0.22 );
	d = smin( d, sdEllipsoid( q - vec3(  0.38,  0.10,  0.06 ), vec3( 0.31, 0.26, 0.46 ) ), 0.20 );
	d = smin( d, sdEllipsoid( q - vec3( -0.10,  0.30,  0.30 ), vec3( 0.30, 0.24, 0.34 ) ), 0.18 );
	d = smin( d, sdEllipsoid( q - vec3(  0.20, -0.26,  0.22 ), vec3( 0.27, 0.23, 0.36 ) ), 0.18 );
	// rounded cap riding the central tower.
	d = smin( d, sdEllipsoid( q - vec3( -0.06,  0.10,  0.50 ), vec3( 0.26, 0.22, 0.30 ) ), 0.16 );

	// perturb the implicit surface with low-freq noise -> ragged, organic, non-symmetric edge.
	d += ( 0.5 - low ) * u_envWarp;

	// SDF -> soft 0..1 mask (deep inside -> 1, outside the shell -> 0).
	float env = smoothstep( 0.12, -0.10, d );

	// ragged, noise-broken BASE (no flat plane): density fades below a perturbed base height.
	float baseZ = -0.52 + ( low - 0.5 ) * 0.12;
	env *= smoothstep( baseZ, baseZ + 0.12, q.z );

	// SAFETY fade ONLY: guarantee density -> 0 before the box faces over the outer u_falloff band.
	env *= 1.0 - smoothstep( 1.0 - u_falloff, 1.0, bound );

	return env;
}

// SampleCloudDensity -- the SINGLE density function used by BOTH the view march AND the cone
// light / sky-visibility marches (no hidden "delete density / keep lighting" coupling). The
// organic macro envelope sets the silhouette; the baked noise only FILLS and ERODES WITHIN it:
//   * low-freq Perlin-Worley (R) dilated by the Worley FBM = the SOFT billowy body (anti-popcorn,
//     never raw Worley), coverage slides the existence threshold;
//   * high-freq Worley = EDGE-ONLY erosion (the (1-smoothstep) edge weight keeps dense interiors
//     smooth -- the anti-popcorn rule). detail=0 skips ONLY the fine erosion (cheap shadow taps).
float SampleCloudDensity( vec3 p, int detail )
{
	float h;
	float macro = CloudShapeEnvelope( p, h );
	if( macro <= 0.001 )
		return 0.0;

	vec3 wind = vec3( u_time * 0.6, u_time * 0.25, 0.0 );   // SLOW wind (do not swamp parallax)
	vec4 b    = texture( u_base3d, ( p + wind ) * u_baseFreq );
	// Worley billow FBM from the increasing-frequency G/B/A octaves: dilates the Perlin base.
	float wfbm = dot( b.gba, vec3( 0.625, 0.25, 0.125 ) );

	// Nubis-style: coherent Perlin-Worley base (R), coverage slides the existence threshold,
	// the Worley FBM DILATES it into round soft billows (NOT raw Worley = popcorn).
	float base = remap( b.r, 1.0 - u_coverage, 1.0, 0.0, 1.0 );
	base = mix( base, remap( base, wfbm * 0.35, 1.0, 0.0, 1.0 ), 0.35 );

	float cloud = macro * base;

	if( detail == 1 && cloud > 0.01 && u_detailAmt > 0.001 )
	{
		// high-freq Worley detail, EDGE-WEIGHTED so dense interiors are untouched (the anti-popcorn
		// rule): the (1 - smoothstep) edge mask -> 0 inside thick cloud, 1 only near the thin edge.
		// u_erodeOct blends 1..3 detail octaves; u_erodeDepth scales how far it bites; wispy at the
		// base -> firmer toward the crown via h.
		vec3 dt = texture( u_detail3d, ( p + wind * 1.7 ) * u_detailFreq ).rgb;
		float dfbm = dt.r;
		float norm = 1.0, amp = 0.5;
		if( u_erodeOct >= 2 ) { dfbm += dt.g * amp; norm += amp; amp *= 0.5; }
		if( u_erodeOct >= 3 ) { dfbm += dt.b * amp; norm += amp; }
		dfbm /= norm;
		float edge     = 1.0 - smoothstep( 0.35, 0.85, cloud );
		float erodeAmt = u_detailAmt * u_erodeDepth * edge * mix( 0.55, 1.0, h );
		cloud = remap( cloud, dfbm * erodeAmt, 1.0, 0.0, 1.0 );
	}

	return clamp( cloud, 0.0, 1.0 ) * u_density;
}

// Cheap UPWARD density trace (Quake Z-up) for sky-visibility ambient occlusion: accumulates
// optical-depth toward the sky so deep/under-cloud samples receive LESS sky ambient (AO).
float TraceDensityUp( vec3 p, int steps, float reach )
{
	float stepLen = reach / float( max( steps, 1 ) );
	float acc = 0.0;
	for( int i = 0; i < 4; i++ )
	{
		if( i >= steps ) break;
		vec3 sp = p + vec3( 0.0, 0.0, 1.0 ) * ( stepLen * ( float( i ) + 0.5 ) );
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

	int steps = u_steps;
	float marchLen = t1 - t0;
	float stepLen = marchLen / float( steps );
	float jit = ign( gl_FragCoord.xy, u_frame );
	float t = t0 + stepLen * jit;

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
		vec3 p = ro + rd * t;
		float dens = SampleCloudDensity( p, 1 );
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
			float skyVis = exp( -0.5 * u_sigmaT * TraceDensityUp( p, 3, 900.0 ) );
			vec3  ambient = u_ambSky * skyVis * mix( 0.35, 1.0, hf )
			              + u_ambGround * 0.35 * ( 1.0 - hf );

			// energy-conserving in-scatter slice (Beer-Lambert)
			float stepT = exp( -u_sigmaT * dens * stepLen );
			vec3 S = u_lightColor * ( scatter * powderTerm + rim ) + ambient;
			L += Tview * ( 1.0 - stepT ) * S;
			Tview *= stepT;
			if( Tview < 0.01 )
				break;
		}
		t += stepLen;
	}

	float alpha = 1.0 - Tview;
	fragColor = vec4( L, alpha );   // premultiplied (L weighted by coverage along the march)
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
void main()
{
	vec2 uv = gl_FragCoord.xy / u_fullSize;
	fragColor = texture( u_cloudTex, uv );   // bilinear upsample, premultiplied
}
)GLSL";
