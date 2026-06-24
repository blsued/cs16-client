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
// FS -- VERY FAINT, HARD-EDGED cone-shape INDICATOR (v3.1). v3 filled the whole near shell
// with a flat faint color -> it read as a TRANSLUCENT FOG VOLUME / debug geometry (a fog
// cloud against the sky; codex v3 FAIL). v3.1 draws ONLY the cone's SILHOUETTE RIM: a thin,
// crisp outline that traces where the shell turns edge-on to the viewer, so the tell reads as
// a sharp cone OUTLINE -- never a filled translucent cloud. Still GL_MAX (caller = kBlendMax)
// so overlapping cones never sum -> overlap brightness <= one cone.
//   * gl_FrontFacing discard => exactly the NEAR shell (one faint layer, no double-cover).
//   * depth occlusion compares this fragment's window-space depth to the stored scene depth
//     (same projection) -> the shell is hidden behind nearer geometry. Sky (depth==1) keeps
//     only the thin RIM (a faint sharp outline over the sky; MAX blend bounds it).
//   * RIM = the shell's geometric normal (face derivatives of the world pos) vs the view dir:
//     edge-on (silhouette) shows, face-on is DISCARDED -> the shell INTERIOR draws nothing,
//     so there is no plume/cloud fill. A narrow smoothstep band makes the rim a CRISP thin
//     line, not a soft volumetric falloff.
//   * NO Step-1 reconstruct helpers needed (raw depth compare only), so this FS is self-
//     contained -- EnsureBuilt() compiles it as-is (no kFogDepthReconstructGlsl prepend).
// -----------------------------------------------------------------------------
static const char kConeFs[] = R"GLSL(#version 330 core
uniform sampler2D u_depthTex;   // FULL-RES scene depth (raw, compare-mode NONE); sky unit
uniform vec2  u_fullSize;       // full-res target size in pixels (gl_FragCoord basis)
uniform vec3  u_color;          // warm-white indicator tint (not premultiplied)
uniform float u_edge;           // indicator brightness (linear HDR); kept VERY faint
uniform vec3  u_camPos;         // v3.1: camera world pos -> view dir for the silhouette rim
in vec3  vWorld;                // world-space cone-surface position (matches kConeVs `out vec3 vWorld`)
out vec4 fragColor;
void main()
{
	if( !gl_FrontFacing )
		discard;                                  // near shell only (single faint layer)

	// Depth occlusion: hide the shell where it is BEHIND opaque scene geometry. Both depths
	// are window-space z from the SAME view-proj, so a raw compare is valid. Sky pixels store
	// depth 1.0 -> the rim survives there (a faint sharp outline over the dark sky).
	vec2  uv = gl_FragCoord.xy / u_fullSize;
	float dscene = texture( u_depthTex, uv ).r;
	if( gl_FragCoord.z > dscene )
		discard;                                  // behind a wall

	// FIX-3: faint HARD-EDGED outline (not a filled translucent shell). The shell's flat-
	// triangle normal (screen-space derivatives of the world pos) vs the camera->fragment
	// view dir gives |facing|: ~1 where the shell faces the viewer (DISCARD -> no fill, no
	// plume/cloud), ~0 at the silhouette (the cone's outline). A narrow smoothstep turns the
	// silhouette into a thin CRISP line. MAX blend bounds overlap to a single cone.
	// NaN guard (acctB code-review nit): cross(dFdx,dFdy) is the ZERO vector on a triangle
	// with no screen-space area (sub-pixel sliver / exactly edge-on). normalize(0) is NaN on
	// some drivers (Intel/AMD/Mesa) and would paint a garbage fragment. Test the geometric
	// normal's length BEFORE normalizing; degenerate -> no facing basis -> treat as face-on
	// (facing 1 -> discarded just below), so a degenerate face never paints.
	vec3  ng      = cross( dFdx( vWorld ), dFdy( vWorld ));
	float ngLen   = length( ng );
	vec3  n       = ( ngLen > 1e-12 ) ? ( ng / ngLen ) : vec3( 0.0 );   // safe normalize (no NaN)
	vec3  viewDir = normalize( u_camPos - vWorld );
	float facing  = ( ngLen > 1e-12 ) ? abs( dot( n, viewDir )) : 1.0;  // 1 = face-on, 0 = edge-on (silhouette)
	// FIX v3.2 (air-halo): TIGHTEN the silhouette band so the rim is a thin HARD outline, not
	// a wide soft glow. The shell is flat-faceted, so `facing` is per-FACE constant (the face
	// normal) -> the rim is the set of near-edge-on FACES; the old smoothstep(0.32,0.10) lit
	// every face with facing<0.32 (~2-3 faces each side), a broad band that over the dark night
	// sky read as airborne HAZE / a fog cloud above the pool (the forbidden air glow, codex #3
	// FAIL). Narrowing to (0.16,0.0) keeps only the SINGLE most edge-on face each side -> a thin
	// crisp cone outline; the air off that one-face line draws NOTHING and returns to the night
	// baseline. u_edge (live cvar csz_flashlight_tp_edge) stays the faintness knob; MAX blend
	// still bounds overlap to a single cone.
	float rim     = smoothstep( 0.16, 0.0, facing );    // thin hard silhouette line only
	if( rim <= 0.0 )
		discard;                                  // off the silhouette: draw nothing (no haze/cloud fill)
	fragColor = vec4( u_color * u_edge * rim, 1.0 );
}
)GLSL";
