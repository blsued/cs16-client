/*
 * csz_light_types.h -- CSOZ renderer: light parameter PODs shared with geom
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
#pragma once
#include "csz_math.h"
namespace csz
{
// POD consumed by geom lit-additive draws; produced by lighting/.
// Lives in core so geom never includes lighting headers (spec 4.6 one-way rule).
struct SpotLightParams
{
	float origin[3];
	float dir[3];           // normalized forward
	float color[3];         // linear 0..1, intensity premultiplied
	float radius;           // attenuation end distance (world units)
	float cosInner;         // cone falloff start (cos of half angle)
	float cosOuter;         // cone cutoff (cos of half angle)
	Mat4 matShadow;         // bias*proj*view; valid only when shadowTexSlot != 0
	int shadowTexSlot;      // engine texture slot for GL_Bind; 0 = shadowless
	// L5R crisp first-person direct profile (csz_flashlight_v3). Filled by
	// BuildSpotParams from the flashlight cvars; v3<0.5 -> legacy linear cone (A/B).
	float v3;               // 1 = analytic crisp profile, 0 = legacy linear cone
	float edgeExp;          // cone-edge sharpening exponent (crisper pool boundary)
	float hotspotGain;      // central hotspot peak gain (axis brightness boost)
	float hotspotSharp;     // hotspot tightness (higher = smaller bright core)
	float directGain;       // direct light-pool brightness multiplier
	// FIX-1 (v3.1): NON-LOCAL ground-pool overlap must not brighten. When true the
	// lit-additive draw composites with glBlendEquation(GL_MAX) (dst = max(src,dst))
	// instead of additive, so N overlapping other-player pools clamp to a SINGLE
	// cone's brightness (structural, not tuned). false (default) = the local first-
	// person pool stays purely additive -> first-person look byte-unchanged.
	bool  maxBlend;
};
}
