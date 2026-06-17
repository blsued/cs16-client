/*
 * csz_sky.h -- CSOZ renderer: procedural day/night sky + celestial bodies
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
#include "../core/csz_view.h"
#include "../core/csz_ambience_types.h"
namespace csz
{
// Procedural sky render base (default tier): a phase-driven gradient dome with sun/moon
// discs, a hash star field, a blood-moon variant and a dawn warm gradient,
// drawn at slot 10.5 BEFORE the world (depth test/write off, world overwrites).
// Also the publish point for the dominant celestial directional light that the
// world + studio base passes consume through AmbienceParams.
class SkyRenderer
{
public:
	void EnsureBuilt();                          // lazy GL init keyed on GpuGeneration()
	void DrawSky( const ViewSetup &view );       // slot 10.5 background pass
	float ComputePhase();                        // live off ClientTime(); csz_sky_phase >= 0 freezes
	void PublishLighting( AmbienceParams &amb, float phase );  // overwrite tint + dominant light dir/color
	void RegisterDevCvars();                     // csz_sky_phase + csz_devsun (CSZ_DEV_TOOLS only)
};
extern SkyRenderer g_sky;
}
