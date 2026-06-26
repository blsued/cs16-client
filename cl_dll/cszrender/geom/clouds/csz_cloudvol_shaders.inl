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
uniform float u_detailAmt;     // high-frequency Worley edge-erosion amount
uniform float u_falloff;       // X/Y face-falloff window fraction (density->0 BEFORE the box faces)
uniform float u_hBase;         // height-gradient: base taper fraction (flat-ish feathered base)
uniform float u_hTop;          // height-gradient: where the rounded top begins
uniform float u_powder;        // powder dark-edge strength
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

// Cumulus vertical profile across the hero box [0 = base, 1 = top]: a flat-ish defined
// base that ramps up quickly, full density through the body, rounding off into a fat
// rounded top -- reads as a towering cumulus, not a thin flat layer.
float heightGradient( float h )
{
	float base = clamp( remap( h, 0.0, u_hBase, 0.0, 1.0 ), 0.0, 1.0 );
	float top  = clamp( remap( h, u_hTop, 1.0, 1.0, 0.0 ), 0.0, 1.0 );
	return base * top;
}

// SampleCloudDensity -- the SINGLE density function used by BOTH the view march AND the
// cone light march (codex #8: no hidden "delete density / keep lighting" coupling).
// detail=1 pays for the high-freq Worley edge erosion (view march); detail=0 skips it
// (the shadow lookup only needs approximate occlusion), buying back the cone-march cost.
float SampleCloudDensity( vec3 p, int detail )
{
	float h = clamp( ( p.z - u_boxMin.z ) / max( u_boxMax.z - u_boxMin.z, 1.0 ), 0.0, 1.0 );
	float grad = heightGradient( h );
	if( grad <= 0.0 )
		return 0.0;

	vec3 wind = vec3( u_time * 0.6, u_time * 0.25, 0.0 );   // SLOW wind (do not swamp parallax)
	vec3 uvw  = ( p + wind ) * u_baseFreq;
	vec4 b    = texture( u_base3d, uvw );
	// Worley FBM from the increasing-frequency G/B/A octaves -> billow dilation field.
	float wfbm = b.g * 0.625 + b.b * 0.25 + b.a * 0.125;
	// Dilate the low-freq Perlin-Worley (R) by the Worley FBM: billowy cauliflower lumps
	// emerge instead of a smooth blob -- the move that kills the "flat" look at the root.
	float cloud = remap( b.r, wfbm - 1.0, 1.0, 0.0, 1.0 );
	cloud *= grad;
	// Coverage gate (WHERE cloud exists). Higher coverage -> a fuller, more solid tower.
	cloud = remap( cloud, 1.0 - u_coverage, 1.0, 0.0, 1.0 );

	if( detail == 1 && cloud > 0.0 && u_detailAmt > 0.001 )
	{
		vec3 duvw = ( p + wind * 2.0 ) * u_detailFreq;
		vec3 dt   = texture( u_detail3d, duvw ).rgb;
		float dfbm = dt.r * 0.625 + dt.g * 0.25 + dt.b * 0.125;
		// Erode the silhouette: wispy curls at the base, firmer toward the crown.
		float erode = mix( dfbm, 1.0 - dfbm, clamp( h * 2.0, 0.0, 1.0 ) );
		cloud = remap( cloud, erode * u_detailAmt, 1.0, 0.0, 1.0 );
	}

	// X/Y FACE FALLOFF -- the box-silhouette killer. Feather density smoothly to ZERO over the
	// outer u_falloff fraction of each horizontal half-extent so the cloud NEVER reaches the AABB
	// side faces: open sky reads through near the box boundary, and the remaining mass is an
	// organic blob inside the box, not a filled cube. (The vertical Z faces are already feathered
	// by heightGradient's base taper + rounded top.)
	vec3  bc    = 0.5 * ( u_boxMin + u_boxMax );
	vec2  bhalf = max( 0.5 * ( u_boxMax.xy - u_boxMin.xy ), vec2( 1.0 ) );
	vec2  dn    = abs( p.xy - bc.xy ) / bhalf;          // 0 at center -> 1 at the X/Y face
	float win   = smoothstep( 1.0, 1.0 - u_falloff, dn.x )
	            * smoothstep( 1.0, 1.0 - u_falloff, dn.y );
	cloud *= win;

	return clamp( cloud, 0.0, 1.0 ) * u_density;
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
	float gFwd   = 0.72;                     // forward in-scatter lobe (peaks looking toward the light)
	float phase  = 0.9 * hg( cosT, gFwd ) + 0.12 * hg( cosT, -0.2 );
	// Toward-light gate for the silver lining: rises as the view turns INTO the light (the real
	// golden-hour gameplay case = looking toward the low bright sun THROUGH the cloud). Broad
	// onset (-0.15..0.55) so the rim is a WIDE glowing band, not a razor sliver. This REPLACES the
	// prior INVERTED gate smoothstep(-0.05,-0.6,cosT), which only fired when looking AWAY from the
	// sun -- so the silver lining never appeared head-on toward a low golden-hour sun.
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
			// --- cone light march toward the lit body (BASE density only) ---
			float lt = 0.0;
			for( int j = 0; j < MAX_LIGHT; j++ )
			{
				if( j >= u_lightSteps ) break;
				vec3 lp = p + u_lightDir * ( lightStepLen * ( float( j ) + 0.5 ) );
				lt += SampleCloudDensity( lp, 0 ) * lightStepLen;
			}
			float Tl = exp( -u_sigmaT * lt );
			// cheap multiscatter octave reuse (no re-march)
			float ms = Tl + 0.5 * pow( Tl, 0.5 );
			// --- forward-scatter SILVER LINING: the brilliant gold/bright rim of a BACKLIT cloud.
			// Fires when looking toward the lit body (towardLight) and where sunlight still penetrates
			// the cloud (Tl high => thin rim/edge). pow(Tl,u_silverWidth) with a SMALL width broadens
			// the glow INWARD from the razor edge; the hg core gives the directional blaze and the
			// +0.4 floor keeps the whole lit rim glowing (not just the peak). Colored by u_lightColor
			// downstream (warm sun => gold lining; cool moon => cool lining).
			float pene = pow( clamp( Tl, 0.0, 1.0 ), u_silverWidth );
			float rim  = u_silver * towardLight * pene * ( hg( cosT, 0.6 ) + 0.4 );
			// powder dark-edge: deepens the self-shadowed near faces of dense lumps
			float powder = 1.0 - exp( -2.0 * dens * stepLen * 40.0 );
			float powderShade = mix( 1.0, 0.45 + 0.55 * Tl, 0.6 ) * max( 0.05, 1.0 - 0.35 * u_powder * powder );
			// height-aware ambient skylight (undersides not black)
			float hf = clamp( ( p.z - u_boxMin.z ) / max( u_boxMax.z - u_boxMin.z, 1.0 ), 0.0, 1.0 );
			vec3 ambient = mix( u_ambGround, u_ambSky, hf );
			// energy-conserving in-scatter slice (Beer-Lambert)
			float stepT = exp( -u_sigmaT * dens * stepLen );
			vec3 S = u_lightColor * ( ms * phase * powderShade + rim ) + ambient;
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
