/*
 * csz_sunmoon_math.h -- CSOZ renderer: pure CPU geometry for the sun/moon bodies (C3)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original work written for
 * CSOZ; no code in this file is copied or translated from PrimeXT, Paranoia,
 * Trinity, retail/leaked sources, or any other license-tainted source
 * (see csoz docs/provenance.md, section 6). Clean-room implementation: the
 * disc/halo/phase geometry below is derived from first-principles spherical
 * trigonometry, not from any external renderer source.
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
// Header-only pure math for C3 sun/moon body placement. No engine/GL deps
// (only <math.h>), so the contract is testable exactly as shipped and never
// drags GL state into a math unit. All directions are world-space (Quake Z-up)
// unit vectors FROM the viewer TOWARD the body, matching skymath::SunDir /
// MoonDir and the AmbienceParams 2.6 angle convention.
#include <math.h>

namespace csz
{
namespace sunmoon
{

inline float Clampf( float v, float lo, float hi )
{
	return ( v < lo ) ? lo : ( v > hi ? hi : v );
}

inline void Normalize3( float v[3] )
{
	float len = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
	if( len > 1e-8f )
	{
		float inv = 1.0f / len;
		v[0] *= inv; v[1] *= inv; v[2] *= inv;
	}
}

// Tangent basis for a body looking direction `dir` (unit, viewer->body). `right`
// is horizontal (world-up cross dir) so the body's "up" stays aligned with world
// up -- the lunar near-side keeps the same face upright as it arcs across the sky
// instead of spinning. Degenerate near the zenith (dir ~ world up); we fall back
// to a fixed east axis there so the basis stays finite (the day-arc apex is only
// ~35deg, so this guard only matters for the dev capture-aim override).
inline void BodyBasis( const float dir[3], float right[3], float up[3] )
{
	const float worldUp[3] = { 0.0f, 0.0f, 1.0f };
	// right = normalize( worldUp x dir )
	right[0] = worldUp[1] * dir[2] - worldUp[2] * dir[1];
	right[1] = worldUp[2] * dir[0] - worldUp[0] * dir[2];
	right[2] = worldUp[0] * dir[1] - worldUp[1] * dir[0];
	float rl = sqrtf( right[0] * right[0] + right[1] * right[1] + right[2] * right[2] );
	if( rl < 1e-4f )
	{
		// dir nearly parallel to world up: use world east as the right axis.
		right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
	}
	else
	{
		float inv = 1.0f / rl;
		right[0] *= inv; right[1] *= inv; right[2] *= inv;
	}
	// up = dir x right (completes a right-handed frame; unit because dir,right unit & perp)
	up[0] = dir[1] * right[2] - dir[2] * right[1];
	up[1] = dir[2] * right[0] - dir[0] * right[2];
	up[2] = dir[0] * right[1] - dir[1] * right[0];
	Normalize3( up );
}

// Elevation (deg) of a world dir (Z up).
inline float ElevDeg( const float dir[3] )
{
	return asinf( Clampf( dir[2], -1.0f, 1.0f ) ) * ( 180.0f / 3.14159265358979323846f );
}

// Smooth 0..1 visibility as a body crosses the horizon: 0 when >~1deg below,
// 1 once a couple degrees up. Keeps a setting body from popping on/off.
inline float HorizonVis( const float dir[3] )
{
	float e = ElevDeg( dir );
	return Clampf( ( e + 1.0f ) / 3.0f, 0.0f, 1.0f );
}

}	// namespace sunmoon
}	// namespace csz
