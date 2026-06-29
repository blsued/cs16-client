/*
 * csz_polyblend_shaders.inl -- CSOZ renderer: polyblend GLSL sources
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
// Included ONLY by csz_polyblend.cpp. Attributeless fullscreen triangle via
// gl_VertexID (no VBO); the fragment stage emits one constant tint color. The
// over-blend (src_alpha, 1-src_alpha) is fixed-function state set on the CPU
// side -- identical math to GoldSrc R_PolyBlend's glColor4f + blend.
static const char kPolyblendVs[] = R"GLSL(#version 330 core
void main()
{
	vec2 ndc = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0,
	                 ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( ndc, 0.0, 1.0 );
}
)GLSL";

static const char kPolyblendFs[] = R"GLSL(#version 330 core
uniform vec4 u_blend;   // rgb = blend color (0..1, display space), a = coverage
out vec4 fragColor;
void main()
{
	fragColor = u_blend;
}
)GLSL";
