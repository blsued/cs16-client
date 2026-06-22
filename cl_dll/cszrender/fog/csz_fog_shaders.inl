/*
 * csz_fog_shaders.inl -- CSOZ renderer: fog GLSL source fragments
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
// =============================================================================
// Fog Step 1 (prerequisite): GLSL depth-linearize / position-reconstruct helpers
// for the sampleable scene depth texture (geom/csz_sky_compose.cpp now backs the
// HDR FBO GL_DEPTH_ATTACHMENT with a GL_DEPTH_COMPONENT24 *texture*, compare-mode
// GL_NONE -> raw window-space z in [0,1]).
//
// This fragment is NOT YET compiled into any program. It is the math foundation
// the fog analytic term (Step 2), the half-res flashlight march (Step 3) and the
// soft-particle depth fade (Step 5) prepend to their fragment shaders. Keeping it
// here, header-only and unused, is what makes Step 1 a pure visual no-op while
// still landing the helpers the spec (FOG-MILESTONE1 §4.2) requires.
//
// Conventions (verified against core/csz_view.cpp:73-78, core/csz_math.cpp):
//   * Perspective projection, GL column-major, clip z in [-1, 1] (GL default).
//   * zNear = 4.0, zFar = 16384.0 (csz_view). Pushed as uniforms u_zNear/u_zFar.
//   * u_invProj     = inverse(proj)      -> view-space reconstruction.
//   * u_invViewProj = inverse(proj*view) -> world-space reconstruction.
//     (both built CPU-side with csz::Mat4Inverse -- core/csz_math.h.)
//   * uv = gl_FragCoord.xy / u_viewportSize. The depth texture and the HDR color
//     share the FBO, so both use GL's bottom-left origin consistently; a screen-
//     space god-ray pass (Step 4) MUST reuse this same uv convention.
//
// linViewZ returns POSITIVE view-space distance (eye in front is -z in GL eye
// space; we return its magnitude so it reads as "metres from camera"). At
// zNear=4/zFar=16384 the 24-bit non-linear depth quantises hard at the far plane
// (documented-acceptable: fog is low-frequency there); near-camera precision,
// where the soft-particle fade lives, is ample.
// -----------------------------------------------------------------------------
static const char kFogDepthReconstructGlsl[] = R"GLSL(
uniform float u_zNear;
uniform float u_zFar;
uniform mat4  u_invProj;       // inverse(proj):     clip -> view
uniform mat4  u_invViewProj;   // inverse(proj*view): clip -> world

// Raw window-space depth d in [0,1] -> positive view-space distance from eye.
float linViewZ( float d )
{
    float ndc = d * 2.0 - 1.0;
    return ( 2.0 * u_zNear * u_zFar ) /
           ( u_zFar + u_zNear - ndc * ( u_zFar - u_zNear ) );
}

// Reconstruct view-space position from screen uv + raw depth d.
vec3 viewPosFromDepth( vec2 uv, float d )
{
    vec4 clip = vec4( uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0 );
    vec4 v = u_invProj * clip;
    return v.xyz / v.w;
}

// Reconstruct world-space position from screen uv + raw depth d.
vec3 worldPosFromDepth( vec2 uv, float d )
{
    vec4 clip = vec4( uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0 );
    vec4 w = u_invViewProj * clip;
    return w.xyz / w.w;
}
)GLSL";
