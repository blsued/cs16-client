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

// Sun angle along the circle vs phase, two-segment so the geometry's key looks
// land on clean phase values:
// theta: 174deg (sunset, just above WEST horizon) -> 270 (nadir/anti-apex =
// MIDNIGHT, moon at apex/highest) at ph0.5
//        -> 360 (sunrise, EAST horizon) at ph0.86 -> ~395 (sun up east, daylight) at ph1.
inline float SunThetaDeg( float ph )
{
	ph = clampf01( ph );
	if( ph < 0.5f ) return 174.0f + ( 270.0f - 174.0f ) * ( ph / 0.5f );		// sunset -> midnight
	return 270.0f + ( 395.0f - 270.0f ) * ( ( ph - 0.5f ) / 0.5f );			// midnight -> daylight
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

// --- Round-driven phase mapping on a CONTINUOUS easing curve (5-min round
// target): the sky NEVER freezes -- the sun & moon ride the whole round, the
// moon slowly arcing across the night. A single smooth curve makes the sunset
// and dawn move fast while the dark NIGHT is the slowest, longest stretch.
// Phase 0 = sunset at round start, phase 0.5 = midnight at mid-round, phase 1 =
// full daylight at the round's end. Short rounds use the same curve (the sin
// term still vanishes at both ends and the slope stays > 0). ---
inline float RoundPhase( float elapsed, float duration )
{
	if( duration <= 1.0f ) return 0.0f;
	if( elapsed < 0.0f ) elapsed = 0.0f;
	if( elapsed > duration ) elapsed = duration;
	float tau = elapsed / duration;                 // 0..1 normalized round time
	// Continuous ease: phase = tau + (A/2pi) sin(2pi tau).
	// Slope = 1 + A*cos(2pi tau): HIGH at the ends (sunset/dawn move fast),
	// LOW (=1-A) at tau=0.5 (the dark night moves slowest and lasts longest).
	// A in (0,1) keeps slope > 0 everywhere => STRICTLY increasing, never frozen.
	const float kNightDwell = 0.85f;                // A: night-dwell strength (higher = longer/slower night). tunable.
	const float kTwoPi = 6.2831853071795864f;
	float ph = tau + ( kNightDwell / kTwoPi ) * sinf( kTwoPi * tau );
	return clampf01( ph );
}

}	// namespace skymath
}	// namespace csz
