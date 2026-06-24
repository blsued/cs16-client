/*
 * csz_light_cone_shaders.inl -- CSOZ renderer: world-space beam-volume GLSL (L6a / slot 13.4)
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
// Included ONLY by lighting/csz_light_cone.cpp.
//
// === SPEC v3 (USER 2026-06-24): clear-fog cone + VERY FAINT, SHARP cone indicator ===
// MECHANISM PIVOT away from v1/v2. v1/v2 rendered the third-person cone as a VOLUMETRIC
// in-scatter glow (a lit air shaft). The USER rejected that twice on the real 31-bot
// machine: a hazy glowing volume that, in dense overlap, washed into a detail-less bright
// blob and dimmed the sky. v3 removes the glow entirely:
//   (1) CLEAR FOG -- the non-local cone LOWERS the base fog extinction inside its volume so
//       you see THROUGH to lit surfaces + enemies. That lives in the WORLD + STUDIO base
//       fragment shaders (an extension of the first-person csz_flashlight_defog), NOT here.
//   (2) This file now draws ONLY a VERY FAINT, SHARP cone-shape INDICATOR -- a flat, hard-
//       edged cone shell at low brightness, so a third-person observer sees "someone is
//       shining a flashlight there" without any hazy volumetric god-ray.
//   (3) NO-BRIGHTEN OVERLAP -- the indicator is drawn with glBlendEquation(GL_MAX) (caller
//       sets kBlendMax), so N overlapping cones composite to max(...) = a SINGLE cone's
//       brightness. Structural, not tuned.
// There is no half-res buffer, no march, no bilateral upsample, no soft-knee anymore: the
// indicator is a single direct full-res cone-mesh pass into HDR. The procedural cone VS is
// the only piece carried over from the v2 pipeline (cone-mesh proxy). The LOCAL first-person
// beam is never drawn here (slot 13.5 owns it; the local defog is the world FS view-ray term).

// -----------------------------------------------------------------------------
// VS -- procedural unit cone (no VBO; gl_VertexID), apex at u_apex, opening along
// u_axis (= dir * length). Each of u_segments wedges is one apex-rim-rim triangle
// (3*segments verts), forming the cone's LATERAL shell (no base cap). The CPU folds
// the half-angle + length into the basis vectors (u_axis/u_right/u_up) so the SAME
// procedural mesh maps to any spot. Only the world position is interpolated.
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
// FS -- VERY FAINT, SHARP cone-shape INDICATOR (v3). Outputs a flat low-brightness warm
// color over the cone's lateral shell, depth-occluded by the scene. NO volumetric integral,
// NO soft falloff: the crisp edge is the cone mesh silhouette itself. Drawn with GL_MAX
// (caller = kBlendMax) so overlapping cones never sum -> overlap brightness <= one cone.
//   * gl_FrontFacing discard => exactly the NEAR shell (one faint layer, no double-cover).
//   * depth occlusion compares this fragment's window-space depth to the stored scene depth
//     (same projection) -> the shell is hidden behind nearer geometry. Sky (depth==1) keeps
//     the faint shell, but MAX blend means it can only ever read as a faint tell, never a wash.
//   * NO Step-1 reconstruct helpers needed (raw depth compare only), so this FS is self-
//     contained -- EnsureBuilt() compiles it as-is (no kFogDepthReconstructGlsl prepend).
// -----------------------------------------------------------------------------
static const char kConeFs[] = R"GLSL(#version 330 core
uniform sampler2D u_depthTex;   // FULL-RES scene depth (raw, compare-mode NONE); sky unit
uniform vec2  u_fullSize;       // full-res target size in pixels (gl_FragCoord basis)
uniform vec3  u_color;          // warm-white indicator tint (not premultiplied)
uniform float u_edge;           // indicator brightness (linear HDR); kept VERY faint
in vec3  vWorld;                // world-space cone-surface position (matches kConeVs `out vec3 vWorld`)
out vec4 fragColor;
void main()
{
	if( !gl_FrontFacing )
		discard;                                  // near shell only (single faint layer)

	// Depth occlusion: hide the shell where it is BEHIND opaque scene geometry. Both depths
	// are window-space z from the SAME view-proj, so a raw compare is valid. Sky pixels store
	// depth 1.0 -> the shell survives there (a faint cone over the dark sky; MAX blend keeps
	// it from ever washing the sky brighter than this faint level).
	vec2  uv = gl_FragCoord.xy / u_fullSize;
	float dscene = texture( u_depthTex, uv ).r;
	if( gl_FragCoord.z > dscene )
		discard;                                  // behind a wall

	// Flat, hard-edged, faint cone shell. The sharp edge IS the cone mesh boundary; there is
	// NO soft volumetric falloff (the v2 in-scatter is gone). MAX blend bounds overlap.
	fragColor = vec4( u_color * u_edge, 1.0 );
}
)GLSL";
