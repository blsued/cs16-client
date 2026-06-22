/*
 * csz_clouds.h -- CSOZ renderer: drifting local night cloud dome (sky-base D L3a)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original work written for
 * CSOZ; no code in this file is copied or translated from PrimeXT, Paranoia,
 * Trinity, retail/leaked sources, or any other license-tainted source
 * (see csoz docs/provenance.md, section 6). The cloud FBM, Beer-Lambert
 * transmittance and Henyey-Greenstein phase are published physical/empirical
 * FORMULAS (facts, not copyrightable); re-typed clean-room. Clean-room
 * implementation; implemented by an agent that has not read any license-tainted
 * source.
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
// Drifting LOCAL night clouds (sky-base D, layer L3a). A vertexless fullscreen
// triangle projects the view ray onto an analytic high cloud shell and samples a
// CPU pre-baked 2D tiling noise texture as scrolling multi-octave FBM. The pass
// renders the clouds (alpha-over the HDR FBO, AFTER the panorama + stars and
// BEFORE the moon disc, so clouds occlude the Milky Way / stars) AND computes
// three OWNED scalar outputs into AmbienceParams. L3a does NOT apply any
// darkening to the map/world/moon -- it only stores the scalars for L3b/L4.
// csz_clouds 0 early-outs so the sky is byte-for-behavior identical to current.
class CloudRenderer
{
public:
	void EnsureBuilt();                                   // lazy GL init keyed on GpuGeneration()
	void Contribute( const ViewSetup &view );            // slot 10.5+: cloud pass (after stars, before moon)
	void RegisterCvars();                                // csz_clouds + csz_cloud_cover (always)
	void UpdateScalars( AmbienceParams &amb, float phase ); // compute the 3 owned scalars (NOT applied here)
	void Shutdown();                                      // generation-safe GL teardown
};
extern CloudRenderer g_clouds;
}
