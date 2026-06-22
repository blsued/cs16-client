/*
 * csz_fog_godrays_shaders.inl -- CSOZ renderer: sun/moon god-ray GLSL (fog M1 Step 4)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original work written for
 * CSOZ; no code in this file is copied or translated from PrimeXT, Paranoia,
 * Trinity, retail/leaked sources, or any other license-tainted source
 * (see csoz docs/provenance.md, section 6).
 * Clean-room implementation. The screen-space radial-scatter method is the
 * textbook GPU Gems 3 Ch.13 light-shaft loop, re-expressed from the published
 * algorithm; no code/shader is copied from any license-tainted renderer.
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
// Included ONLY by fog/csz_fog_godrays.cpp. Three GL3.3-core fragment programs +
// one shared fullscreen-triangle vertex stage (VAO-less gl_VertexID idiom, same as
// kVolVs). All work is in vpUV (the [0,1] position over the rendered viewport); the
// half-res stages read the full-extent depth/occlusion via the §5 remap, the
// composite reads back via the §6.5 inverse. GLES3/WebGL2 intersection only.

// Fullscreen-triangle vertex stage. uv reconstructed from gl_FragCoord in each FS
// (bottom-left origin, no y-flip -- matches csz_fog_shaders.inl:53).
static const char kGrVs[] = R"GLSL(#version 330 core
void main()
{
	vec2 ndc = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0,
	                 ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( ndc, 1.0, 1.0 );
}
)GLSL";

// -----------------------------------------------------------------------------
// Stage 1 -- occlusion/emitter (half-res FBO #1, RGBA16F). Spec §5. A broad smooth
// glow centered on the body's screen position, GATED to the open SKY (raw-depth
// isSky): geometry silhouettes near the bright source region carve the dark gaps the
// radial blur stretches into shafts. Indexed in vpUV; the full-extent depth read is
// remapped through the viewport uniforms (nonzero-origin safe).
// -----------------------------------------------------------------------------
static const char kGrOcclFsBody[] = R"GLSL(#version 330 core
uniform sampler2D u_depthTex;       // SkyComposeDepthTex: raw D24, NEAREST, compare off; sky unit
uniform vec2  u_halfSize;           // half-res target dims (px)
uniform vec2  u_viewOrigin;         // scene viewport origin (px)
uniform vec2  u_viewSize;           // scene viewport size (px)
uniform vec2  u_fullSize;           // full HDR/depth FBO extent (px) = the depth tex dims
uniform vec2  u_lightVpUV;          // body screen pos in vpUV [0,1]
uniform float u_aspect;             // u_viewSize.x / u_viewSize.y
uniform float u_skyDepthEps;        // 0.999999: depth >= eps -> sky
uniform vec3  u_bodyColor;          // chosen body disc color * gain (linear)
uniform float u_sourceIntensity;    // emitter scale (clamped [0..4])
uniform float u_glowFalloff;        // broad-glow decay (clamped [2..20])
out vec4 fragColor;

void main()
{
	vec2 vpUV    = gl_FragCoord.xy / u_halfSize;                  // [0,1] over viewport, half-res
	vec2 sceneUV = ( u_viewOrigin + vpUV * u_viewSize ) / u_fullSize;   // -> full-extent depth uv
	float depth  = textureLod( u_depthTex, sceneUV, 0.0 ).r;
	bool  isSky  = ( depth >= u_skyDepthEps );                    // cleared far 1.0 = sky; geom < 1 occludes
	float d      = length( ( vpUV - u_lightVpUV ) * vec2( u_aspect, 1.0 ) );
	float glow   = exp( -d * u_glowFalloff );                     // broad symmetric glow
	vec3  emit   = u_bodyColor * ( u_sourceIntensity * glow );
	fragColor    = vec4( isSky ? emit : vec3( 0.0 ), 1.0 );
}
)GLSL";

// -----------------------------------------------------------------------------
// Stage 2 -- radial scatter (half-res FBO #2, RGBA16F). Spec §6. GPU Gems 3 Ch.13
// radial-blur loop marching from the fragment toward the light, accumulating the
// decayed/weighted occlusion. NUM_SAMPLES+1 = 49 taps, ALL at half-res. OOB taps
// (outside the [0,1] occlusion buffer) are REJECTED to 0, never CLAMP_TO_EDGE
// smeared. No exposure / bodyVis / composite here -- that is Stage 3.
// -----------------------------------------------------------------------------
static const char kGrScatterFsBody[] = R"GLSL(#version 330 core
uniform sampler2D u_occlusionTex;   // Stage 1 output (vpUV space); sky unit, LINEAR
uniform vec2  u_halfSize;           // half-res dims (px)
uniform vec2  u_lightVpUV;          // body screen pos in vpUV [0,1]
uniform float u_density;            // step length scale (clamped [0.1..1.5])
uniform float u_decay;              // per-step decay (clamped [0.80..0.99])
uniform float u_weight;             // per-step weight (clamped [0..2])
out vec4 fragColor;

const int NUM_SAMPLES = 48;         // +1 initial tap = 49 total (GL3.3 constant loop)

void main()
{
	vec2  vpUV  = gl_FragCoord.xy / u_halfSize;
	vec2  delta = ( vpUV - u_lightVpUV ) * ( 1.0 / float( NUM_SAMPLES ) ) * u_density;
	vec2  t     = vpUV;
	vec3  color = texture( u_occlusionTex, vpUV ).rgb;           // sample 0
	float decayAcc = u_decay;
	for( int i = 0; i < NUM_SAMPLES; ++i )
	{
		t -= delta;
		bool inUnit = all( greaterThanEqual( t, vec2( 0.0 ) ) ) &&
		              all( lessThanEqual( t, vec2( 1.0 ) ) );
		vec3 s = inUnit ? texture( u_occlusionTex, t ).rgb : vec3( 0.0 );   // reject OOB
		color   += s * ( decayAcc * u_weight );
		decayAcc *= u_decay;
	}
	fragColor = vec4( color, 1.0 );
}
)GLSL";

// -----------------------------------------------------------------------------
// Stage 3 -- composite (full-res, ADDITIVE into the HDR FBO). Spec §6.5. One cheap
// bilinear upsample of the half-res scatter, scaled by u_exposure * u_bodyVis (the
// smooth horizon/cvar/phase fade), written with glBlendFunc(GL_ONE,GL_ONE) and
// fragColor.a = 0.0 (never accumulates HDR alpha; never re-attenuates the scene --
// the Step-2 base fog owns extinction). NO clamp, NO tonemap (downstream resolve).
// -----------------------------------------------------------------------------
static const char kGrCompositeFsBody[] = R"GLSL(#version 330 core
uniform sampler2D u_scatterTex;     // Stage 2 output (vpUV space); sky unit, LINEAR
uniform vec2  u_viewOrigin;         // scene viewport origin (px)
uniform vec2  u_viewSize;           // scene viewport size (px)
uniform float u_exposure;           // 0.30 base * csz_fog_godrays_intensity (clamped [0..3])
uniform float u_bodyVis;            // smooth horizon/cvar/phase fade (= src.vis)
out vec4 fragColor;

void main()
{
	vec2 vpUV = ( gl_FragCoord.xy - u_viewOrigin ) / u_viewSize;  // full-res -> vpUV
	vec3 s    = texture( u_scatterTex, vpUV ).rgb;               // bilinear upsample
	fragColor = vec4( s * ( u_exposure * u_bodyVis ), 0.0 );     // additive; a=0
}
)GLSL";
