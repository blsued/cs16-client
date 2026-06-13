/*
 * csz_ambience_types.h -- CSOZ renderer: server-authoritative ambience POD
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
 * csoz docs/notes/primext-render-mechanisms-m2.md); implemented by an agent
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
namespace csz
{
// Server-authoritative ambience snapshot (spec 3.2). Produced by fog/
// (envelope decode), consumed by geom + lighting through ViewSetup.ambience.
// Lives in core so fog/ never includes (and is never included by) lighting
// or geom (spec 4.6 one-way rule). All colors linear 0..1, premultiplied by
// their intensity; directions are normalized world-space unit vectors
// pointing FROM the scene TOWARD the sky object (see 2.6 angle convention).
struct AmbienceParams
{
	float fogColor[3];
	float fogDensity;       // exp2 fog, 1/units; <= 0 disables fog entirely
	float tint[3];          // night tint multiplier; (1,1,1) = neutral
	bool  moonEnabled;
	float moonDir[3];
	float moonCosRadius;    // cos(angular radius); disc test threshold
	float moonColor[3];
	float moonHalo;         // halo intensity 0..1
	bool  moonlightEnabled;
	float moonlightDir[3];  // surface -> moon (shader L vector, constant)
	float moonlightColor[3];
};
// (0,0,0,0)/(1,1,1)/disabled everything -- the vanilla daylight look.
inline AmbienceParams AmbienceNeutral()
{
	AmbienceParams p = AmbienceParams();	// value-initialized: every float 0, bools false

	p.tint[0] = 1.0f;
	p.tint[1] = 1.0f;
	p.tint[2] = 1.0f;
	return p;
}
}
