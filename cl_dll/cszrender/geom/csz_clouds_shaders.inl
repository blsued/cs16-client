/*
 * csz_clouds_shaders.inl -- CSOZ renderer: drifting night cloud dome GLSL (L3a)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original work written for
 * CSOZ; no code here is copied or translated from PrimeXT, Paranoia, Trinity,
 * retail/leaked sources, or any other license-tainted source (see csoz
 * docs/provenance.md, section 6). The Beer-Lambert transmittance and the
 * Henyey-Greenstein phase function are published physical FORMULAS (facts);
 * re-typed clean-room. Clean-room implementation; implemented by an agent that
 * has not read any license-tainted source.
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
// Included ONLY by csz_clouds.cpp. Uniform names are this program's private
// contract (the only CROSS-FILE channel is AmbienceParams, written CPU-side in
// UpdateScalars, never a cloud uniform).
// GL3.3 core / GLES3 / WebGL2 intersection only (code-standards section 7).

// Vertex stage: VAO-less fullscreen triangle via gl_VertexID. The per-pixel
// world-space view ray is rebuilt from the camera basis (u_camFwd/Right/Up,
// Quake world space, Z up). u_camRight/u_camUp are PRE-SCALED on the CPU by
// tan(fovX/2)/tan(fovY/2), so the ray = fwd + right*ndc.x + up*ndc.y. Working in
// a world direction keeps the clouds rotation-stable -- they do not shimmer when
// the camera turns (identical convention to the sky / panorama / stars passes).
static const char kCloudsVs[] = R"GLSL(#version 330 core
uniform vec3 u_camFwd;
uniform vec3 u_camRight;     // already scaled by tan(fovX/2)
uniform vec3 u_camUp;        // already scaled by tan(fovY/2)
out vec3 v_dir;              // world-space view direction (un-normalized)
void main()
{
	vec2 ndc = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0,
	                 ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	v_dir = u_camFwd + u_camRight * ndc.x + u_camUp * ndc.y;
	gl_Position = vec4( ndc, 1.0, 1.0 );
}
)GLSL";

// Fragment stage: project the view ray onto an analytic high cloud shell
// (dome-planar mapping shellUV = viewRay.xy / viewRay.z), sample the CPU
// pre-baked 2D tiling noise as 3-octave scrolling FBM + a separate slow
// low-frequency coverage gate, convert density -> Beer-Lambert transmittance,
// and emit THREE quantities packed as vec4(cloudRadiance.rgb, visualAlpha):
//   starOcclusion = 1 - T          (how much cloud hides the sky behind)
//   cloudRadiance = moonlit glow   (moonColor * moonLitFrac * HG * (1-T) * gain)
//   visualAlpha   = starOcclusion capped < ~0.9 so the galaxy stays mostly visible
// cloudRadiance is LINEAR HDR, pre-tonemap (the FBO is RGBA16F; do NOT sRGB).
static const char kCloudsFs[] = R"GLSL(#version 330 core
in vec3 v_dir;
uniform sampler2D u_noise;     // CPU pre-baked tiling FBM noise (R = detail fbm seed, G = coverage seed)
uniform vec3  u_moonDir;       // world dir toward the moon (normalized)
uniform vec3  u_moonColor;     // moonlit-cloud tint (linear)
uniform float u_moonLitFrac;   // 0..1 illuminated fraction of the moon disc
uniform float u_night;         // 0..1 night gate (clouds invisible by day)
uniform float u_cover;         // 0..1 cloud amount (csz_cloud_cover)
uniform float u_time;          // engine client time (seconds) -> scroll/drift
out vec4 fragColor;

const float PI = 3.14159265358979323846;

// Cheap hash for the spatial-only dither (screen-space; NO time term).
float hash12( vec2 p )
{
	vec3 p3 = fract( vec3( p.xyx ) * 0.1031 );
	p3 += dot( p3, p3.yzx + 33.33 );
	return fract( ( p3.x + p3.y ) * p3.z );
}

// Henyey-Greenstein phase (normalized): forward-scatter lobe toward the moon.
float hg( float cosT, float g )
{
	float gg = g * g;
	float d = 1.0 + gg - 2.0 * g * cosT;
	return ( 1.0 - gg ) / ( 4.0 * PI * pow( max( d, 1e-4 ), 1.5 ) );
}

// 3-octave detail FBM from the tiling noise R channel, each octave at its OWN
// scale AND its own scroll direction/speed so the inter-layer differential reads
// as visible drift. Weights 0.5/0.25/0.125 normalized to ~[0,1].
float detailFbm( vec2 uv, float t )
{
	float s = 0.0;
	s += 0.500 * texture( u_noise, uv * 1.00 + vec2(  0.011,  0.004 ) * t ).r;
	s += 0.250 * texture( u_noise, uv * 2.07 + vec2( -0.017,  0.013 ) * t ).r;
	s += 0.125 * texture( u_noise, uv * 4.13 + vec2(  0.009, -0.021 ) * t ).r;
	return s / 0.875;   // (0.5+0.25+0.125)
}

void main()
{
	vec3 rd = normalize( v_dir );

	// Below/near the horizon there is no cloud shell to project onto: fade out as
	// the ray approaches the horizon (rd.z -> 0) and skip entirely below it.
	float horizon = smoothstep( 0.02, 0.18, rd.z );
	if( u_night <= 0.001 || horizon <= 0.0 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	// Analytic high cloud shell: dome-planar projection of the ray onto a plane a
	// fixed height above the camera. shellUV = rd.xy / rd.z. The scale sets the
	// cloud cell size on screen: the visible sky must span SEVERAL noise tiles so
	// the FBM reads as distinct cloud cells (too small a scale -> the whole sky
	// samples one near-constant texel region -> invisible). Rotation-stable.
	vec2 shellUV = rd.xy / max( rd.z, 0.05 ) * 1.10;

	// Low-frequency coverage field: large scale, very slow scroll. Gates WHERE
	// clouds exist; threshold by u_cover so higher cover = more/denser clouds.
	float coverNoise = texture( u_noise, shellUV * 0.35 + vec2( 0.003, 0.0017 ) * u_time ).g;

	// Detail FBM (the actual cloud shape) scrolling faster, layered drift.
	float fbm = detailFbm( shellUV, u_time );

	// Coverage gate: map u_cover (0..1) to a density threshold. Higher cover lowers
	// the threshold so more of the sky fills in; lower cover leaves wide gaps for
	// the Milky Way. The coverage noise (slow, large-scale) breaks the sky into
	// cloudy vs clear REGIONS; the detail fbm shapes each cloud.
	float covBias = mix( 0.62, 0.12, clamp( u_cover, 0.0, 1.0 ) );   // high cover -> low cutoff
	float field = fbm * 0.55 + coverNoise * 0.45;                    // blend detail + region coverage
	float density = smoothstep( covBias, covBias + 0.22, field );
	density *= horizon;

	if( density <= 0.0005 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	// Beer-Lambert transmittance through the analytic cloud thickness.
	const float sigmaT = 2.2;                       // extinction (tuned: thin, galaxy shows through)
	float tau = density * sigmaT;
	float T = exp( -tau );                          // transmittance behind the cloud
	float starOcclusion = 1.0 - T;                  // how much sky the cloud hides

	// Moonlit glow: forward-scatter lobe toward the moon (HG), gated by the moon's
	// illuminated fraction. cloudRadiance is LINEAR HDR (pre-tonemap).
	float cosTheta = dot( rd, normalize( u_moonDir ) );
	float phase = hg( cosTheta, 0.80 );
	const float kGain = 2.4;
	// Directional moonlit lobe (bright silver lining toward the moon) PLUS a faint
	// uniform ambient glow so cloud masses read as dim silver-grey everywhere, not
	// pure-black star holes. Both gated by the moon's illuminated fraction.
	float ambient = 0.06;
	// Nit 2 (L3b): do NOT pre-multiply cloudRadiance by starOcclusion. The cloud
	// draw uses standard NON-premultiplied alpha (SetBlend kBlendAlpha ->
	// glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)), so the on-screen source
	// term is cloudRadiance*visualAlpha. visualAlpha already carries starOcclusion
	// (below); folding it in here too made thin clouds read as starOcclusion^2 ->
	// too dark. Occlusion is now carried ONCE (by alpha) -> brighter silver lining.
	vec3 cloudRadiance = u_moonColor * u_moonLitFrac * ( phase * kGain + ambient );

	// visualAlpha: keep even thick local clouds translucent so the Milky Way stays
	// mostly visible (USER: <~90% cover). Cap peak alpha < 0.9.
	float visualAlpha = starOcclusion * 0.85;

	// Apply the night gate to BOTH the emitted light and the occluding alpha so
	// clouds vanish cleanly by day.
	cloudRadiance *= u_night;
	visualAlpha   *= u_night;

	// Spatial-only blue-noise dither (screen-space hash; NO time term, NO frame%N)
	// to break alpha banding. Tiny amplitude, centered.
	float d = ( hash12( gl_FragCoord.xy ) - 0.5 ) * (1.0/255.0);
	visualAlpha = clamp( visualAlpha + d, 0.0, 0.9 );

	fragColor = vec4( cloudRadiance, visualAlpha );
}
)GLSL";
