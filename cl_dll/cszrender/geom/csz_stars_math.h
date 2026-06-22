/*
 * csz_stars_math.h -- CSOZ renderer: pure star-field math (C4)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original work written for
 * CSOZ; no code in this file is copied or translated from PrimeXT, Paranoia,
 * Trinity, retail/leaked sources, or any other license-tainted source
 * (see csoz docs/provenance.md, section 6). The star colour (Ballesteros B-V
 * -> temperature, blackbody -> RGB) and magnitude->flux relations are published
 * physical/empirical FORMULAS (facts, not copyrightable); re-typed clean-room.
 * Clean-room implementation; implemented by an agent that has not read any
 * license-tainted source.
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
// Single source of truth for the star field's pure CPU-side math: the
// night-visibility curve (sun elevation -> 0..1 star intensity). NO engine
// dependencies (only <math.h>). The star colour model (B-V -> Teff -> Planckian
// locus) and the magnitude -> total-flux relation now live entirely in the GLSL
// (csz_stars_shaders.inl) -- the catalogue stores B-V, not a baked RGB -- so the
// old client-side BVtoRGB / BVtoTemp / TempToRGB / MagToFlux helpers are gone
// (they were dead runtime code; the bake tool keeps its own Python copies).
#include <math.h>

namespace csz
{
namespace starsmath
{

inline float Clampf01( float v )
{
	return ( v < 0.0f ) ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

inline float Smooth01( float edge0, float edge1, float x )
{
	float t = Clampf01( ( x - edge0 ) / ( edge1 - edge0 ) );
	return t * t * ( 3.0f - 2.0f * t );
}

// Night-visibility factor from the sun's elevation (degrees). Stars are fully
// out while the sun is well up, fade in quickly as the sun nears/crosses the
// horizon, and are fully present once the sun is a few degrees below it.
// Continuous and monotone so it tracks csz_sky_phase smoothly (phase 0 = sunset
// -> faint, phase 0.5 = midnight sun at ~-35 deg -> 1, out again by dawn).
//
// FIX (no-daylight-stars, 2026-06-18): the previous window (-5..+5) let the night
// factor reach ~0.22 at sun elevation +2 deg -- combined with the 5x brightness
// fix and StarsContribute's 0.01 cutoff, stars drew while the sun was still up to
// ~+4 deg ABOVE the horizon (visible daylight stars). The factor must be exactly 0
// while the sun is at or above the horizon, then fade 0..1 through twilight as the
// sun sinks, and be full at night. With Smooth01(edge0,edge1,x) returning 0 at
// edge0 and 1 at edge1, NightFactor = 1 - Smooth01(end,start,x): it is 0 at start
// (=0 deg, horizon) and 1 at end (=-7 deg). So no stars in daylight, they begin
// appearing just below the horizon, and reach full visibility by ~-7 deg. The
// brightness (5x) fix is untouched, so full-night stars stay clearly visible.
const float kTwilightEndDeg   = -7.0f;   // sun this far below horizon -> full night (stars at full)
const float kTwilightStartDeg =  0.0f;   // sun at/above horizon -> night factor 0 (no daylight stars)
inline float NightFactorFromSunElev( float sunElevDeg )
{
	return 1.0f - Smooth01( kTwilightEndDeg, kTwilightStartDeg, sunElevDeg );
}

// TWO-CHANNEL APPEARANCE TIMING (2026-06-21, STAR-MW-TIMING-RESEARCH.md).
// Physical fact that drives the art: bright point sources (stars/planets) become
// visible LONG BEFORE the diffuse low-surface-brightness Milky Way / faint dense
// field, which need ~2-3x deeper sun depression. Our two independently-gated render
// channels each get their OWN smoothstep on sun elevation `e` (deg, negative below
// horizon). Because alpha is a PURE FUNCTION of e, dusk and dawn are automatically
// symmetric -- no separate dawn code (the sun rising back through the same elevations
// fades each channel out in the correct reverse order: MW fades first, bright stars
// linger). Smooth01(edge0,edge1,x) returns 0 at edge0, 1 at edge1; passing the deeper
// (more negative) angle as edge1 makes alpha ramp 0->1 as the sun DESCENDS.
//
// Channel 1 -- live BRIGHT stars (csz_stars point-sprites): fade in -2 -> -8 deg
// (civil/nautical twilight). Bright stars appear FIRST. Was 0 above the horizon so
// still no daylight stars; full by early nautical twilight.
const float kBrightStarFadeStartDeg = -2.0f;   // first bright stars just below horizon
const float kBrightStarFadeFullDeg  = -8.0f;   // bright stars fully in by early nautical
inline float BrightStarNightFactor( float sunElevDeg )
{
	return Smooth01( kBrightStarFadeStartDeg, kBrightStarFadeFullDeg, sunElevDeg );
}

// Channel 2 -- baked PANORAMA backdrop (faint dense field + Milky Way band under ONE
// texture alpha): fade in LATER, -9 -> -16 deg (astronomical twilight -> night). The
// research's ideal is 3 layers (field -7->-15, MW -12->-18); since the field+MW share
// one texture alpha here, -9->-16 is the combined compromise: the dense field + Milky
// Way only emerge once the sky is genuinely dark, well after the bright stars.
const float kPanoramaFadeStartDeg = -9.0f;    // dense field + MW band start once near-dark
const float kPanoramaFadeFullDeg  = -16.0f;   // fully in approaching full night
inline float PanoramaNightFactor( float sunElevDeg )
{
	return Smooth01( kPanoramaFadeStartDeg, kPanoramaFadeFullDeg, sunElevDeg );
}

}	// namespace starsmath
}	// namespace csz
