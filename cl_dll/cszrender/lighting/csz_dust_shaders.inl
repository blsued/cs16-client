/*
 * csz_dust_shaders.inl -- CSOZ renderer: gated airborne-dust GLSL (L7)
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
// Included ONLY by lighting/csz_dust.cpp. The FS PREPENDS the shared fog Step-1
// depth-reconstruct helpers (kFogDepthReconstructGlsl: u_zNear/u_zFar + linViewZ)
// at build time -- see EnsureBuilt(): final FS = "#version 330 core\n" + reconstruct + body.
// GLES3/WebGL2 intersection only (code-standards section 7).

// -----------------------------------------------------------------------------
// VS -- a CPU-expanded billboard quad per LIT mote. The CPU does all the gating +
// billboard expansion (gating decides which motes exist at all: the unlit ones are
// never written into the VBO -- the L7 perf contract). Each lit mote is 6 verts
// (2 triangles) with the corner world position already folded in on the CPU using
// the camera right/up basis, so the VS just transforms. vViewZ carries the mote's
// positive view-space distance for the soft-particle depth fade.
// -----------------------------------------------------------------------------
static const char kDustVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_world;   // billboard corner, world space (CPU-expanded)
layout(location = 1) in vec2 a_uv;      // corner in [-1,1] (round mote falloff)
layout(location = 2) in vec3 a_color;   // linear premultiplied radiance (light tint * bright)
uniform mat4 u_matViewProj;
uniform mat4 u_matView;
out vec2  vUv;
out vec3  vColor;
out float vViewZ;                       // positive view-space distance (eye -> mote)
void main()
{
	vUv    = a_uv;
	vColor = a_color;
	vViewZ = -( u_matView * vec4( a_world, 1.0 ) ).z;   // GL eye space: forward is -z
	gl_Position = u_matViewProj * vec4( a_world, 1.0 );
}
)GLSL";

// -----------------------------------------------------------------------------
// FS -- soft, fine, OCCLUSIVE mote. Round radial falloff for the sprite; camera-side
// occlusion + soft intersection via the SAMPLED scene depth (never tested/written
// -- same safe contract as the L6a cone, avoids a depth read/test feedback loop on
// the shared depth attachment). rgb = linear-HDR radiance the mote ADDS (catches the
// light); alpha = an occlusion coverage = u_occlusion * fall * soft that, under
// kBlendPremulOver (ONE, ONE_MINUS_SRC_ALPHA), ATTENUATES the beam behind the mote --
// so fine airborne dust both glints AND eats the flashlight light a touch (the USER
// ask: 明显看出遮挡了一点点点的手电筒光). u_occlusion 0 -> byte-for-byte the old pure-additive mote.
// -----------------------------------------------------------------------------
static const char kDustFsBody[] = R"GLSL(
uniform sampler2D u_depthTex;   // scene depth (raw, compare-mode NONE); sky unit
uniform vec2  u_viewSize;       // full-res scene size in pixels (gl_FragCoord basis)
uniform float u_fade;           // soft depth-fade band (world units)
uniform float u_occlusion;      // per-mote occlusion coverage scale (csz_dust_occlusion)
in vec2  vUv;
in vec3  vColor;
in float vViewZ;
out vec4 fragColor;
void main()
{
	float r2 = dot( vUv, vUv );
	if( r2 > 1.0 )
		discard;                                  // outside the round mote
	float fall = 1.0 - r2;
	fall *= fall;                                 // soft edge

	vec2  uv = gl_FragCoord.xy / u_viewSize;
	float dscene = texture( u_depthTex, uv ).r;
	float sceneZ = ( dscene < 1.0 ) ? linViewZ( dscene ) : 1.0e9;  // 1.0 = sky -> no occluder
	if( vViewZ > sceneZ + u_fade )
		discard;                                  // wholly behind geometry
	float soft = clamp( ( sceneZ - vViewZ ) / max( u_fade, 1.0 ), 0.0, 1.0 );

	float cover = fall * soft;
	float occ   = clamp( u_occlusion * cover, 0.0, 1.0 );  // extinction coverage of THIS mote
	fragColor = vec4( vColor * cover, occ );
}
)GLSL";
