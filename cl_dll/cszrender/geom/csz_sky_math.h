/*
 * csz_sky_math.h -- CSOZ renderer: pure day/night sky math (phase + body arcs)
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
// Single source of truth for the procedural sky's celestial geometry and
// round->phase mapping. NO engine dependencies (only <math.h>): the renderer
// (csz_sky.cpp) and the standalone unit test (tests/test_sky_math.cpp) both
// pull these so the contract is tested exactly as shipped. All inline (header-
// only); the const floats have internal linkage (no multiple-definition link).
#include <math.h>

namespace csz
{
namespace skymath
{

inline float clampf01( float v )
{
	return ( v < 0.0f ) ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

inline float Smooth01( float edge0, float edge1, float x )
{
	float t = clampf01( ( x - edge0 ) / ( edge1 - edge0 ) );
	return t * t * ( 3.0f - 2.0f * t );
}

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// Elevation+yaw (degrees) -> unit world direction, FROM the scene TOWARD the
// body. Same convention as fog/csz_fog.cpp ElevYawToDir (Z up): the sky FS and
// PublishLighting must agree with the dominant-light channel, so both call this.
inline void ElevYawDir( float elevDeg, float yawDeg, float out[3] )
{
	float e = elevDeg * kDegToRad;
	float y = yawDeg * kDegToRad;

	out[0] = cosf( e ) * cosf( y );
	out[1] = cosf( e ) * sinf( y );
	out[2] = sinf( e );
}

// --- Fixed day-arc plane: the sun & moon ride ONE great circle, antipodal, so
// they share the same rise node (east) and set node (west) -- the moon rises
// where the sun sets and vice versa, a true mirror about the map center (the
// viewer). Phase 0 = SUNSET (sun just above the WEST horizon), ~0.44 = NADIR/
// MIDNIGHT (sun deep below, moon high overhead), ~0.86 = SUNRISE (sun crosses
// the EAST horizon, inside the dawn window), 1 = risen daylight. The MOON is the
// exact antipode (-sunDir) by construction.
const float kNodeYawDeg    = 95.0f;	// rise node azimuth (east); set node = +180 (west). matches the old dawn azimuth.
const float kArcMaxElevDeg = 35.0f;	// arc apex elevation (the moon's height at midnight). eye-level-viewable (~30-40deg), not near-zenith.

// Plane basis: H = horizontal unit toward the EAST (rise) node; P = apex
// direction (unit, perpendicular to H, in-plane, tilted toward the south).
inline void ArcBasis( float H[3], float P[3] )
{
	float ny = kNodeYawDeg * kDegToRad;
	H[0] = cosf( ny );  H[1] = sinf( ny );  H[2] = 0.0f;		// east node, horizontal
	float ay = ( kNodeYawDeg + 90.0f ) * kDegToRad;			// apex azimuth = south of the rise node
	float me = kArcMaxElevDeg * kDegToRad;
	P[0] = cosf( ay ) * cosf( me );  P[1] = sinf( ay ) * cosf( me );  P[2] = sinf( me );
}

// Sun angle along the circle vs phase, now THREE-segment so the round can FREEZE
// at a warm low GOLDEN-HOUR sun at the end instead of the old blue overhead
// daylight (USER DECISION 2026-06-18, Option A). The key looks still land on clean
// phase anchors:
// theta: 174deg (sunset, just above WEST horizon) -> 270 (nadir/anti-apex =
// MIDNIGHT, moon at apex/highest) at ph0.5 -> 360 (sunrise, EAST horizon) at
// ph0.86 -> kDaylightThetaDeg (~370 = sun ~5.7deg, GOLDEN HOUR) at ph1. The final
// segment CAPS the rise so the round-end sun stays low (the atmosphere reddens/
// warms physically there, Belt-of-Venus); the old curve overshot to ~395deg
// (~19deg elevation) = a flat blue overhead daylight, past the warm window.
const float kSunrisePhase     = 0.86f;	// east-horizon crossing (sunrise); a contract anchor (the unit test pins it).
const float kDaylightThetaDeg = 370.0f;	// round-end "daylight" arc angle: ~5.7deg elevation = warm low golden sun (was 395 = ~19deg blue). Tunable lower (~365 = ~2.9deg) for a warmer/redder held dawn.

inline float SunThetaDeg( float ph )
{
	ph = clampf01( ph );
	if( ph < 0.5f )
		return 174.0f + ( 270.0f - 174.0f ) * ( ph / 0.5f );						// sunset -> midnight
	if( ph < kSunrisePhase )
		return 270.0f + ( 360.0f - 270.0f ) * ( ( ph - 0.5f ) / ( kSunrisePhase - 0.5f ) );	// midnight -> sunrise (EAST horizon, elev 0)
	return 360.0f + ( kDaylightThetaDeg - 360.0f ) * ( ( ph - kSunrisePhase ) / ( 1.0f - kSunrisePhase ) ); // sunrise -> held GOLDEN HOUR (low sun)
}

inline void SunDir( float ph, float out[3] )
{
	float H[3], P[3];
	ArcBasis( H, P );
	float t = SunThetaDeg( ph ) * kDegToRad;
	float c = cosf( t ), s = sinf( t );
	out[0] = c * H[0] + s * P[0];
	out[1] = c * H[1] + s * P[1];
	out[2] = c * H[2] + s * P[2];
}

inline void MoonDir( float ph, float out[3] ){ float sd[3]; SunDir( ph, sd ); out[0] = -sd[0]; out[1] = -sd[1]; out[2] = -sd[2]; } // ANTIPODAL by construction

// Sun elevation in degrees (PublishLighting's light gates still use this).
inline float SunElevDeg( float ph ){ float sd[3]; SunDir( ph, sd ); return asinf( sd[2] ) / kDegToRad; }

// --- Round-driven phase mapping on a CONTINUOUS monotone cubic-Hermite ease
// (5-min round target): the sky NEVER freezes mid-round -- the sun & moon ride the
// whole round. Three anchors (tau,phase) = (0,0) sunset, (0.5,0.5) midnight,
// (1,1) round end, with INDEPENDENT knot slopes: dusk falls FAST, the dark NIGHT
// is the slowest + longest stretch, and the round DECELERATES into a HELD warm
// GOLDEN-HOUR dawn (USER DECISION 2026-06-18, Option A: the round freezes at a low
// warm sun, so the dawn window must DWELL at the end -- the old single-sine curve
// rushed dawn through in a handful of seconds). Short rounds use the same curve. ---
inline float RoundPhase( float elapsed, float duration )
{
	if( duration <= 1.0f ) return 0.0f;
	if( elapsed < 0.0f ) elapsed = 0.0f;
	if( elapsed > duration ) elapsed = duration;
	float tau = elapsed / duration;                 // 0..1 normalized round time

	// Per-anchor phase rate (slope dphase/dtau). All within the Fritsch-Carlson
	// monotonicity bound (each in [0, 3*secant], secant = 1) => phase is STRICTLY
	// increasing (never frozen) and stays in [0,1]; Hermite interpolates its knots
	// exactly, so the (0,0.5,1) anchors land regardless of the slopes.
	const float kDuskRate  = 1.85f;   // tau=0   : sunset drops fast (matches the old end slope; "dusk fast")
	const float kNightRate = 0.12f;   // tau=0.5 : deep night, slowest + longest
	const float kDawnRate  = 0.40f;   // tau=1   : golden-hour dawn DWELLS at round end (slow hold). tunable.

	float p0, m0, m1, t;
	if( tau < 0.5f ) { p0 = 0.0f; m0 = kDuskRate;  m1 = kNightRate; t = tau * 2.0f; }
	else             { p0 = 0.5f; m0 = kNightRate; m1 = kDawnRate;  t = ( tau - 0.5f ) * 2.0f; }

	float t2 = t * t, t3 = t2 * t;
	float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;       // Hermite basis functions
	float h10 = t3 - 2.0f * t2 + t;
	float h01 = -2.0f * t3 + 3.0f * t2;
	float h11 = t3 - t2;
	// phase span per segment = 0.5; tangents scaled by the segment width (0.5).
	float ph = h00 * p0 + h01 * ( p0 + 0.5f ) + ( h10 * m0 + h11 * m1 ) * 0.5f;
	return clampf01( ph );
}

}	// namespace skymath
}	// namespace csz
