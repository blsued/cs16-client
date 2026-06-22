/*
 * csz_sunmoon.h -- CSOZ renderer: sun/moon body public seam (C3) + god-ray source
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
namespace csz
{
struct ViewSetup;   // core/csz_view.h (forward-declared; passed by ref)

// FOG Step 4 god-ray SOURCE contract (FOG-STEP4-GODRAYS-SPEC.md §-BodyContract).
// The single dominant ACTIVE celestial body the screen-space radial god-ray pass
// scatters from. The god-ray pass needs ONE center, but DrawBodies() can draw BOTH
// bodies during the horizon fade, so CszGodraySource() resolves to the brighter one
// deterministically (argmax of the per-body effective visibility; exact tie -> sun).
//   present  -- max(sunEff, moonEff) > 1e-3 (else nothing to scatter; skip the pass).
//   worldDir -- the chosen body's Z-up world dir (the disc DrawBodies() actually
//               draws, incl. the csz_sunmoon_debug capture-aim override).
//   vis      -- the chosen eff = HorizonVis * cvarGate * phaseFactor. The MOON term
//               includes the disc shader's phase-illumination factor, so a NEW MOON
//               gives vis == 0 (no rays). The sun phaseFactor is 1.
//   color    -- the chosen body's RENDERED disc base color * its gain (warm sun /
//               cold moon). NOT amb.moonlightColor (that is the GROUND light).
struct CszGodraySrc
{
	bool  present;
	float worldDir[3];
	float vis;
	float color[3];
};

// Resolve the dominant active body for the god-ray pass. Mirrors DrawBodies()'s
// EXACT visibility/phase/color terms (KEEP IN SYNC -- same pattern as
// MoonBodyOccluder() re-deriving the disc placement). Implemented in csz_sunmoon.cpp.
CszGodraySrc CszGodraySource( const ViewSetup &view );
}
