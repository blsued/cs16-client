/*
 * csz_particle_shaders.inl -- CSOZ renderer: soft-particle/tracer GLSL (C-PAR)
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
// Included ONLY by geom/csz_particle.cpp. The FS PREPENDS the shared fog Step-1
// depth-reconstruct helpers (kFogDepthReconstructGlsl: linViewZ) at build time --
// final FS = "#version 330 core\n" + reconstruct + body. GL3.3 core / GLES3 /
// WebGL2 intersection only. CPU-billboarded quads (round motes) and CPU-expanded
// tracer ribbons share this program: round motes carry a_uv in [-1,1]; tracer
// ribbon verts carry a_uv = (lateral in [-1,1], 0) so the same dot()-falloff
// soft-edges them across the width while staying bright along the length.

static const char kParticleVs[] = R"GLSL(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
uniform mat4 u_matViewProj;
uniform mat4 u_matView;
out vec2  vUv;
out vec4  vColor;
out float vViewZ;     // positive view-space distance (eye -> vertex), soft fade
out vec3  vWorld;     // world-space position, analytic fog ray endpoint
void main()
{
	vUv    = a_uv;
	vColor = a_color;
	vWorld = a_pos;
	vViewZ = -( u_matView * vec4( a_pos, 1.0 ) ).z;   // GL eye space: forward is -z
	gl_Position = u_matViewProj * vec4( a_pos, 1.0 );
}
)GLSL";

static const char kParticleFsBody[] = R"GLSL(
uniform sampler2D u_depthTex;   // scene depth (raw, compare-mode NONE); sky unit
uniform vec2  u_viewSize;       // full-res scene size (gl_FragCoord basis)
uniform float u_fade;           // soft depth-fade band (world units)
uniform int   u_softFade;       // 1 = HDR path (depth tex valid): soft fade; 0 = hard depth test
uniform vec4  u_fog;            // rgb = fog color (linear), w = extinction a (1/units); w<=0 -> off
uniform vec4  u_fogParams;      // x = height falloff b, z = maxOpacity
uniform vec3  u_camPos;         // camera world position (fog ray origin)
in vec2  vUv;
in vec4  vColor;
in float vViewZ;
in vec3  vWorld;
out vec4 fragColor;
// Analytic base-fog transmittance (fog M1 spec 4.3); identical closed form to the
// sprite/world/studio passes so black-fog darkens particles consistently.
float cszFogT( vec3 worldPos, vec3 camPos, float a, float b, float maxOpacity )
{
	if( a <= 0.0 )
		return 1.0;
	vec3 d = worldPos - camPos;
	float t = length( d );
	float rdz = ( t > 1e-4 ) ? d.z / t : 0.0;
	float F;
	if( abs( b ) < 1e-4 )
		F = a * t;
	else if( abs( rdz ) < 1e-4 )
		F = a * exp( -b * camPos.z ) * t;
	else
		F = ( a / b ) * exp( -b * camPos.z ) * ( 1.0 - exp( -b * t * rdz )) / rdz;
	float T = exp( -max( F, 0.0 ));
	return max( T, 1.0 - maxOpacity );
}
void main()
{
	float r2 = dot( vUv, vUv );
	if( r2 > 1.0 )
		discard;                              // outside the round mote / ribbon edge
	float fall = 1.0 - r2;
	fall *= fall;                             // soft edge

	float soft = 1.0;
	if( u_softFade != 0 )
	{
		// depth test/write OFF on the HDR path: do soft intersection in-shader
		// from the SAMPLED scene depth (dust precedent: avoids a read/test
		// feedback loop on the shared depth attachment).
		vec2  uv = gl_FragCoord.xy / u_viewSize;
		float dscene = texture( u_depthTex, uv ).r;
		float sceneZ = ( dscene < 1.0 ) ? linViewZ( dscene ) : 1.0e9;   // sky = no occluder
		if( vViewZ > sceneZ + u_fade )
			discard;                          // wholly behind geometry
		soft = clamp( ( sceneZ - vViewZ ) / max( u_fade, 1.0 ), 0.0, 1.0 );
	}

	float cover = fall * soft;
	float T = cszFogT( vWorld, u_camPos, u_fog.w, u_fogParams.x, u_fogParams.z );
	// additive sparks: alpha ignored by ONE,ONE blend; alpha smoke: alpha drives
	// SRC_ALPHA,ONE_MINUS_SRC_ALPHA. Fog T fades rgb to black for both forms.
	fragColor = vec4( vColor.rgb * cover * T, vColor.a * cover );
}
)GLSL";
