/*
 * csz_light_cone_shaders.inl -- CSOZ renderer: world-space beam-volume GLSL (L6a)
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
// Included ONLY by lighting/csz_light_cone.cpp. The FS PREPENDS the shared
// fog Step-1 depth-reconstruct helpers (kFogDepthReconstructGlsl: u_zNear/
// u_zFar/u_invProj/u_invViewProj + linViewZ/worldPosFromDepth) at build time --
// see BuildConeProgram(): final FS = "#version 330 core\n" + reconstruct + body.
// GLES3/WebGL2 intersection only (code-standards section 7).

// -----------------------------------------------------------------------------
// VS -- procedural unit cone (no VBO; gl_VertexID), apex at u_apex, opening along
// u_axis (= dir * length). Each of u_segments wedges is one apex-rim-rim triangle
// (3*segments verts). The CPU folds the half-angle + length into the basis vectors
// (u_axis/u_right/u_up) so the SAME procedural mesh maps to any spot. Only the
// world position is interpolated; the fragment shader does the volume integral.
// -----------------------------------------------------------------------------
static const char kConeVs[] = R"GLSL(#version 330 core
uniform mat4  u_matViewProj;
uniform vec3  u_apex;      // cone tip (spot origin), world
uniform vec3  u_axis;      // dir * length      (tip -> base centre)
uniform vec3  u_right;     // right * (tanHalf*length)
uniform vec3  u_up;        // up    * (tanHalf*length)
uniform float u_segments;  // radial wedge count (matches the CPU draw call)
out vec3 vWorld;
void main()
{
	int tri    = gl_VertexID / 3;
	int corner = gl_VertexID - tri * 3;
	vec3 world;
	if( corner == 0 )
	{
		world = u_apex;                                   // shared tip
	}
	else
	{
		int idx = tri + ( corner - 1 );                   // corner1 -> i, corner2 -> i+1
		float ang = 6.28318530718 * float( idx ) / u_segments;
		world = u_apex + u_axis + cos( ang ) * u_right + sin( ang ) * u_up;
	}
	vWorld = world;
	gl_Position = u_matViewProj * vec4( world, 1.0 );
}
)GLSL";

// -----------------------------------------------------------------------------
// FS -- world-space single-scatter beam glow. The cone mesh is purely the screen
// COVERAGE proxy (it decides which pixels run this shader, in world space, with
// correct perspective -> visible from ANY angle); the actual brightness is a short
// bounded march of the view ray through the finite cone:
//   * gl_FrontFacing discard => exactly ONE fragment per covered pixel (winding-
//     independent: front+back of the convex mesh cover the same silhouette, we
//     keep one). No double-add, no rim seam.
//   * march extent = view-ray ray-sphere with the cone's bounding sphere, clamped
//     near (u_zNear) and FAR to the reconstructed scene depth -> camera-side
//     occlusion (walls / players hide the beam) with a SOFT fade band (no hard
//     punch-through, no hard intersection line). Depth is SAMPLED, never written.
//   * per sample: exact point-in-cone test (axial s in [0,len] AND angle <= half),
//     soft angular rim (cosOuter..cosInner), axial distance falloff (tip bright),
//     Henyey-Greenstein forward scatter (brighter looking into the beam) over a
//     base so side views stay clearly lit. brightness ~ chord length => bright
//     core / dim rim "triangular beam" from the side, a disc head-on.
// Output is linear-HDR radiance ADDED (kBlendAddPremul) into the scene; never
// re-attenuates anything (additive can only brighten).
// -----------------------------------------------------------------------------
static const char kConeFsBody[] = R"GLSL(
uniform sampler2D u_depthTex;   // scene depth (raw, compare-mode NONE); sky unit
uniform vec2  u_viewSize;       // full-res scene size in pixels (gl_FragCoord basis)
uniform vec3  u_camPos;         // view ray origin (world)
uniform vec3  u_apex;           // cone tip (spot origin)
uniform vec3  u_axisDir;        // normalized cone forward
uniform float u_len;            // beam length (world units; = min(radius, range))
uniform float u_cosInner;       // soft rim start (cos half-angle, inner)
uniform float u_cosOuter;       // cone cutoff   (cos half-angle, outer)
uniform vec3  u_color;          // linear, intensity-premultiplied spot tint
uniform float u_intensity;      // beam brightness scale (csz_flashlight_tp_intensity)
uniform float u_hgG;            // Henyey-Greenstein anisotropy
uniform int   u_steps;          // bounded march sample count
uniform float u_surfFade;       // 0 = local (tight band), 1 = non-local (wide surface fade)
in vec3 vWorld;
out vec4 fragColor;

float ignDither( vec2 p )
{
	return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) );
}

float hgPhase( float c, float g )
{
	float g2 = g * g;
	float denom = 1.0 + g2 - 2.0 * g * c;
	return ( 1.0 - g2 ) / ( 4.0 * 3.14159265 * max( pow( denom, 1.5 ), 1e-4 ) );
}

void main()
{
	if( !gl_FrontFacing )
		discard;                                  // single coverage (see header note)

	vec3 O = u_camPos;
	vec3 V = normalize( vWorld - O );

	// Bounding sphere of the finite cone (centre = apex, radius = slant length to
	// the rim = len/cosOuter) -> tight ray bounds without an exact cone solve.
	float sphR = u_len / max( u_cosOuter, 1e-3 );
	vec3  oc = O - u_apex;
	float b = dot( oc, V );
	float c = dot( oc, oc ) - sphR * sphR;
	float disc = b * b - c;
	if( disc <= 0.0 )
		discard;
	float sq = sqrt( disc );
	float tNear = max( -b - sq, u_zNear );
	float tFar  = -b + sq;
	if( tFar <= tNear )
		discard;

	// Camera-side occlusion: clamp the far bound to the scene surface along V.
	vec2  uv = gl_FragCoord.xy / u_viewSize;
	float dscene = texture( u_depthTex, uv ).r;
	float tScene = -1.0;
	if( dscene < 1.0 )                            // 1.0 = no geometry (sky) -> no clamp
	{
		vec3 sw = worldPosFromDepth( uv, dscene );
		tScene = dot( sw - O, V );
		if( tScene > 0.0 )
			tFar = min( tFar, tScene );
	}
	if( tFar <= tNear )
		discard;                                  // wholly behind a wall

	// Camera-side occlusion bands (world units). LOCAL first-person (u_surfFade 0)
	// keeps the tight legacy band: its crisp direct lit pool owns the near-surface
	// look, so the air cone is unchanged there. NON-LOCAL (u_surfFade > 0, carries the
	// csz_flashlight_nl_surffade scale) uses a MUCH wider band + a smoothstep ramp so
	// the in-scatter reaches ZERO well before a sample touches the surface -- another
	// player's beam therefore deposits NO lit patch (圈) on floors/walls, leaving only
	// the airborne 光柱. The smoothstep keeps the taper soft (no hard clip / detached
	// look): the shaft thins out smoothly as it nears geometry, just far earlier and
	// far more completely than the local band, so the beam fades to nothing before the
	// wall instead of landing a (dimmer) circle on it.
	float softBand = 0.06 * u_len + 8.0;
	float nlBand   = max( ( 0.9 * u_len + 256.0 ) * u_surfFade, 1.0 );
	bool  nonLocal = ( u_surfFade > 0.0 );
	float dt = ( tFar - tNear ) / float( u_steps );
	float jitter = ignDither( gl_FragCoord.xy );

	vec3 acc = vec3( 0.0 );
	for( int i = 0; i < u_steps; i++ )
	{
		float t = tNear + ( float( i ) + jitter ) * dt;
		vec3 P = O + V * t;

		vec3  ap = P - u_apex;
		float s = dot( ap, u_axisDir );           // axial distance from the tip
		if( s <= 0.0 || s > u_len )
			continue;
		float r = length( ap );
		float cosAx = s / max( r, 1e-4 );         // cos( angle off axis )
		float coneFall = smoothstep( u_cosOuter, u_cosInner, cosAx );
		if( coneFall <= 0.0 )
			continue;

		float atten = clamp( 1.0 - s / u_len, 0.0, 1.0 );  // tip bright, far rim dim
		atten *= atten;

		// Soft camera-side occlusion: samples close to the scene surface fade out.
		// Non-local uses the wide smoothstep band -> the shaft reaches ZERO well before
		// the surface (no deposited patch); local keeps the tight linear legacy band.
		float occ;
		if( tScene <= 0.0 )
			occ = 1.0;                            // sky behind this sample -> no clamp
		else if( nonLocal )
			occ = smoothstep( 0.0, nlBand, tScene - t );
		else
			occ = clamp( ( tScene - t ) / softBand, 0.0, 1.0 );

		// Forward scatter: brighter looking into the beam, with a base term so the
		// side view (the L6a acceptance shot) stays clearly visible.
		float cosTheta = dot( V, normalize( u_apex - P ) );
		float scatter = 0.55 + 1.4 * hgPhase( cosTheta, u_hgG );

		acc += u_color * ( coneFall * atten * occ * scatter );
	}

	// Integral ~ (chord length / beam length): scale-invariant, intensity tunes it.
	acc *= u_intensity * dt / max( u_len, 1.0 );
	fragColor = vec4( acc, 1.0 );
}
)GLSL";
