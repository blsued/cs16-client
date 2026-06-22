/*
 * csz_fog_volume_shaders.inl -- CSOZ renderer: flashlight ray-march GLSL (fog M1 Step 3)
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
 * csoz docs/notes/primext-render-mechanisms.md); implemented by an agent
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
// Included ONLY by fog/csz_fog_volume.cpp. The two fragment programs PREPEND the
// shared depth-reconstruct helpers (kFogDepthReconstructGlsl, fog Step 1) at build
// time -- see BuildVolPrograms(): the final FS = "#version 330 core\n" +
// kFogDepthReconstructGlsl + body. Both bodies therefore start straight at their
// own uniform block (the #version + reconstruct uniforms u_zNear/u_zFar/u_invProj/
// u_invViewProj + linViewZ/worldPosFromDepth come from the prepend). GLES3/WebGL2
// intersection only (code-standards section 7).

// Fullscreen-triangle vertex stage (VAO-less, gl_VertexID; same idiom as the HDR
// resolve kComposeVs). Both fog-volume programs share it; uv is reconstructed from
// gl_FragCoord in the FS (consistent with the Step-1 bottom-left-origin convention).
static const char kVolVs[] = R"GLSL(#version 330 core
void main()
{
	vec2 ndc = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0,
	                 ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( ndc, 1.0, 1.0 );
}
)GLSL";

// -----------------------------------------------------------------------------
// Pass 1 -- half-res flashlight ray-march (shadowed single-scatter shafts).
// Output = IN-SCATTER ONLY (radiance to ADD); .a carries linViewZ for the
// depth-aware (bilateral) upsample. The march NEVER attenuates the scene -- the
// analytic base fog (Step 2, folded into the base shaders) is the single owner of
// scene transmittance + the maxOpacity reveal floor (fog M1 spec 4.4, codex pass-2
// single-extinction rule). Tlocal here is march-LOCAL self-shadowing of the shaft
// only and is multiplied into this pass's in-scatter, never back onto the scene.
// -----------------------------------------------------------------------------
static const char kVolMarchFsBody[] = R"GLSL(
uniform sampler2D       u_depthTex;    // scene depth (raw, compare-mode NONE); sky unit
uniform sampler2DShadow u_shadowMap;   // spot shadow (HW PCF, COMPARE_R_TO_TEXTURE); sky unit
uniform vec2  u_targetSize;            // half-res target size in pixels
uniform vec3  u_camPos;                // ray origin (world)
uniform vec3  u_spotOrigin;
uniform vec3  u_spotDir;               // normalized cone forward
uniform vec3  u_spotColor;             // linear, intensity-premultiplied
uniform float u_spotRadius;
uniform float u_cosInner;
uniform float u_cosOuter;
uniform mat4  u_matShadow;             // bias*proj*view (Mat4ShadowBias)
uniform float u_sigmaE;                // extinction (active fog density a); shaft self-shadowing
uniform float u_sigmaS;               // scattering coefficient
uniform float u_hgG;                   // Henyey-Greenstein anisotropy
uniform float u_intensity;             // shaft brightness scale
uniform int   u_steps;                 // march sample count (legacy 4..32; v2 range-derived 6..16)
uniform float u_marchFar;              // hard distance cap (world units); v2 = csz_flashlight_range
uniform int   u_econserve;             // 1 = energy-conserving slice (v2); 0 = pre-L5 linear sum
out vec4 fragColor;

// Interleaved-gradient-noise dither (no blue-noise texture asset for M1): breaks
// the per-step banding into high-frequency noise the eye reads as smooth.
float ignDither( vec2 p )
{
	return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) );
}

// Henyey-Greenstein phase (forward-scatter glow when looking down the beam).
float hgPhase( float c, float g )
{
	float g2 = g * g;
	float denom = 1.0 + g2 - 2.0 * g * c;
	return ( 1.0 - g2 ) / ( 4.0 * 3.14159265 * max( pow( denom, 1.5 ), 1e-4 ) );
}

void main()
{
	vec2 uv = gl_FragCoord.xy / u_targetSize;
	float d = texture( u_depthTex, uv ).r;

	// Camera -> surface ray (world). Sky pixels (d~1) reconstruct to the far
	// plane: a valid direction; the distance cap + cone bound limit the march.
	vec3 surf = worldPosFromDepth( uv, d );
	vec3 ray  = surf - u_camPos;
	float tSurf = length( ray );
	vec3 rd = ray / max( tSurf, 1e-4 );

	float tMax = min( tSurf, u_marchFar );

	// Bound the march to the flashlight influence sphere (analytic ray-sphere):
	// off-cone / behind-camera pixels miss the sphere and early-out (the perf
	// lever that makes a small/edge cone measurably cheaper).
	vec3 oc = u_camPos - u_spotOrigin;
	float b = dot( oc, rd );
	float c = dot( oc, oc ) - u_spotRadius * u_spotRadius;
	float disc = b * b - c;
	if( disc <= 0.0 )
	{
		fragColor = vec4( 0.0, 0.0, 0.0, linViewZ( d ) );
		return;
	}
	float sq = sqrt( disc );
	float t0 = max( -b - sq, u_zNear );
	float t1 = min( -b + sq, tMax );
	if( t1 <= t0 )
	{
		fragColor = vec4( 0.0, 0.0, 0.0, linViewZ( d ) );
		return;
	}

	float segLen = t1 - t0;
	float dt = segLen / float( u_steps );
	float jitter = ignDither( gl_FragCoord.xy );

	// March-local transmittance: Beer-Lambert from the camera to the first
	// sample so far shaft segments self-dim under thick fog. NEVER applied to the
	// scene color (single-extinction rule). u_econserve picks the integration form:
	//   v2  -> Tlocal is the transmittance to the START of each step (Tstart) and
	//          the in-scatter uses the analytic slice (sigmaS/sigmaE)(1-exp(-sigmaE*dt));
	//   v2 0 -> Tlocal decays to the step END before accumulating sigmaS*dt -- the
	//          pre-L5 ordering, reproduced byte-for-byte.
	float Tlocal = exp( -u_sigmaE * t0 );
	float stepTrans = exp( -u_sigmaE * dt );

	vec3 inscatter = vec3( 0.0 );

	for( int i = 0; i < u_steps; i++ )
	{
		float t = t0 + ( float( i ) + jitter ) * dt;
		vec3 P = u_camPos + rd * t;

		float Tstart = Tlocal;        // transmittance to the start of this step
		Tlocal *= stepTrans;          // ...to the end (legacy accumulation point)

		vec3 L = u_spotOrigin - P;
		float dist = length( L );
		L /= max( dist, 1e-4 );

		float atten = clamp( 1.0 - dist / u_spotRadius, 0.0, 1.0 );
		atten *= atten;
		float cone = clamp( ( dot( -L, u_spotDir ) - u_cosOuter ) /
			max( u_cosInner - u_cosOuter, 1e-4 ), 0.0, 1.0 );
		if( cone <= 0.0 || atten <= 0.0 )
			continue;

		// HW-PCF spot shadow: shadowed steps contribute zero in-scatter -> the
		// shaft is cut by occluding geometry (the visible "shadow shafts").
		float shadow = textureProj( u_shadowMap, u_matShadow * vec4( P, 1.0 ) );
		if( shadow <= 0.0 )
			continue;

		float cosTheta = dot( rd, -L );           // forward-scatter angle
		float phase = hgPhase( cosTheta, u_hgG );
		float vis = atten * cone * shadow;

		if( u_econserve != 0 )
		{
			// Energy-conserving slice: the radiance scattered into the eye across
			// this step = (sigmaS/sigmaE)(1 - exp(-sigmaE*dt)), weighted by the
			// transmittance to the step start. Bounded by construction, so thick
			// fog / few steps no longer over-brighten the way the linear sum does.
			float slice = ( u_sigmaS / max( u_sigmaE, 1e-6 ) ) * ( 1.0 - stepTrans );
			inscatter += Tstart * phase * u_spotColor * vis * slice;
		}
		else
		{
			// Legacy linear accumulation (pre-L5; csz_flashlight_v2 0 path).
			inscatter += Tlocal * u_sigmaS * phase * u_spotColor * vis * dt;
		}
	}

	inscatter *= u_intensity;
	fragColor = vec4( inscatter, linViewZ( d ) );
}
)GLSL";

// -----------------------------------------------------------------------------
// Pass 2 -- depth-aware (bilateral) upsample of the half-res in-scatter, ADDED
// into the full-res HDR buffer (composited with kBlendAddPremul: dst.rgb +=
// inscatter.rgb). Additive-only: adding light can only brighten, so it can never
// push a surface below the server maxOpacity reveal floor the base fog set.
// Each half-res texel stored its surface linViewZ in .a; we weight the 2x2 taps
// by their depth agreement with the full-res center to reject light leaking
// across geometry silhouettes (no haloing at edges).
// -----------------------------------------------------------------------------
static const char kVolUpsampleFsBody[] = R"GLSL(
uniform sampler2D u_inscatter;   // half-res march output (rgb = in-scatter, a = linViewZ); sky unit
uniform sampler2D u_depthTex;    // full-res scene depth (raw); sky unit
uniform vec2 u_fullSize;         // full-res target size in pixels
uniform vec2 u_halfSize;         // half-res source size in pixels
out vec4 fragColor;

void main()
{
	vec2 uv = gl_FragCoord.xy / u_fullSize;
	float dC = linViewZ( texture( u_depthTex, uv ).r );

	// 2x2 bilinear footprint in half-res texel space.
	vec2 t = uv * u_halfSize - 0.5;
	vec2 fl = floor( t );
	vec2 fr = t - fl;

	vec3 sum = vec3( 0.0 );
	float wsum = 0.0;
	for( int j = 0; j < 2; j++ )
	{
		for( int i = 0; i < 2; i++ )
		{
			vec2 tap = ( fl + vec2( float( i ), float( j ) ) + 0.5 ) / u_halfSize;
			vec4 s = texture( u_inscatter, tap );
			float bw = ( ( i == 0 ) ? ( 1.0 - fr.x ) : fr.x ) *
			           ( ( j == 0 ) ? ( 1.0 - fr.y ) : fr.y );
			// Depth-agreement weight, RELATIVE to distance (constant world-unit
			// sigma would over-blur near and over-sharpen far). 5% + 1 unit.
			float dw = exp( -abs( dC - s.a ) / ( 0.05 * dC + 1.0 ) );
			float w = bw * dw + 1e-5;
			sum += s.rgb * w;
			wsum += w;
		}
	}

	fragColor = vec4( sum / max( wsum, 1e-4 ), 0.0 );
}
)GLSL";
