/*
 * test_sky_math.cpp -- standalone unit test for csz_sky_math.h
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
// Pure unit test: csz_sky_math.h has no engine deps, so this builds and runs
// standalone (cl /EHsc test_sky_math.cpp  OR  g++ test_sky_math.cpp). NOT added
// to the CMake/client.dll build. Asserts the core contracts: sun/moon are exact
// antipodes, they ride ONE fixed great circle (the property the old azimuth-
// wandering model broke), they share the same rise/set nodes (95/275 deg, 180
// apart, a true center-mirror), and the round->phase mapping is a CONTINUOUS,
// freeze-free ease (sunset/dawn fast, the dark night slowest & longest) with
// the right anchors (0 at start, 0.5 at mid-round, 1 at round end).
#include "../csz_sky_math.h"
#include <stdio.h>
#include <math.h>

using namespace csz::skymath;

static int g_fail = 0;

static void Check( bool cond, const char *name )
{
	if( cond )
		printf( "PASS: %s\n", name );
	else
	{
		printf( "FAIL: %s\n", name );
		g_fail++;
	}
}

static float Vlen( const float v[3] )
{
	return sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
}

// azimuth = atan2(y,x) in degrees, normalized to [0,360); elevation = asin(z).
static float AzimuthDeg( const float v[3] )
{
	float a = atan2f( v[1], v[0] ) / kDegToRad;
	if( a < 0.0f )
		a += 360.0f;
	return a;
}

static float ElevationDeg( const float v[3] )
{
	return asinf( v[2] ) / kDegToRad;
}

// The two horizon-crossing azimuths must be the pair {95,275} (in either order).
static bool NodePairOk( const float az[2], int n )
{
	if( n != 2 )
		return false;
	bool a = fabsf( az[0] - 95.0f ) <= 1.0f && fabsf( az[1] - 275.0f ) <= 1.0f;
	bool b = fabsf( az[0] - 275.0f ) <= 1.0f && fabsf( az[1] - 95.0f ) <= 1.0f;
	return a || b;
}

int main()
{
	// --- 1. Antipodal contract: SunDir + MoonDir == 0 for every phase. ---
	{
		float maxMag = 0.0f;
		for( float ph = 0.0f; ph <= 1.0f + 1e-6f; ph += 0.005f )
		{
			float s[3], m[3];
			SunDir( ph, s );
			MoonDir( ph, m );
			float sum[3] = { s[0] + m[0], s[1] + m[1], s[2] + m[2] };
			float mag = Vlen( sum );
			if( mag > maxMag )
				maxMag = mag;
		}
		printf( "  antipodal: max |sun+moon| over ph 0..1 = %.3e\n", maxMag );
		Check( maxMag < 1e-4f, "antipodal sun+moon ~ 0 over ph 0..1 step 0.005" );
	}

	// --- 2. Fixed great circle: every SunDir lies in ONE plane. THIS is the
	// property the old azimuth-wandering model broke (sun & moon traced
	// different arcs). N = normal of the plane through SunDir(0.1),SunDir(0.6). ---
	{
		float a[3], b[3];
		SunDir( 0.1f, a );
		SunDir( 0.6f, b );
		float N[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
		float nlen = Vlen( N );
		N[0] /= nlen; N[1] /= nlen; N[2] /= nlen;
		float maxDot = 0.0f;
		for( float ph = 0.0f; ph <= 1.0f + 1e-6f; ph += 0.01f )
		{
			float s[3];
			SunDir( ph, s );
			float d = fabsf( s[0] * N[0] + s[1] * N[1] + s[2] * N[2] );
			if( d > maxDot )
				maxDot = d;
		}
		printf( "  great-circle: max |SunDir . N| over ph 0..1 = %.3e\n", maxDot );
		Check( maxDot < 1e-4f, "SunDir stays in one fixed plane (fixed great circle)" );
	}

	// --- 3. Shared nodes: the sun crosses the horizon (z=0) at exactly two
	// azimuths -- 95 and 275 deg, 180 apart -- and the moon crosses at the SAME
	// two azimuths (moon rises where the sun sets, sets where the sun rises). ---
	{
		float sunAz[2], moonAz[2];
		int sunN = 0, moonN = 0;
		float sPrev[3], mPrev[3];
		SunDir( 0.0f, sPrev );
		MoonDir( 0.0f, mPrev );
		for( float ph = 0.0005f; ph <= 1.0f + 1e-6f; ph += 0.0005f )
		{
			float s[3], m[3];
			SunDir( ph, s );
			MoonDir( ph, m );
			if( ( sPrev[2] < 0.0f ) != ( s[2] < 0.0f ) && sunN < 2 )
				sunAz[sunN++] = AzimuthDeg( s );
			if( ( mPrev[2] < 0.0f ) != ( m[2] < 0.0f ) && moonN < 2 )
				moonAz[moonN++] = AzimuthDeg( m );
			sPrev[0] = s[0]; sPrev[1] = s[1]; sPrev[2] = s[2];
			mPrev[0] = m[0]; mPrev[1] = m[1]; mPrev[2] = m[2];
		}
		float sep = ( sunN == 2 ) ? fabsf( sunAz[0] - sunAz[1] ) : 0.0f;
		printf( "  nodes: sun crossings n=%d az=[%.2f,%.2f] sep=%.2f  moon n=%d az=[%.2f,%.2f]\n",
			sunN, sunN > 0 ? sunAz[0] : 0.0f, sunN > 1 ? sunAz[1] : 0.0f, sep,
			moonN, moonN > 0 ? moonAz[0] : 0.0f, moonN > 1 ? moonAz[1] : 0.0f );
		Check( NodePairOk( sunAz, sunN ), "sun crosses horizon at exactly two nodes {95,275}" );
		Check( sunN == 2 && fabsf( sep - 180.0f ) <= 1.0f, "the two sun nodes are 180 deg apart" );
		Check( NodePairOk( moonAz, moonN ), "moon crosses horizon at the SAME two nodes {95,275}" );
	}

	// --- 4. Sunset west, moonrise east at ph0 (true center-mirror). ---
	{
		float s[3], m[3];
		SunDir( 0.0f, s );
		MoonDir( 0.0f, m );
		float sAz = AzimuthDeg( s ), sEl = ElevationDeg( s );
		float mAz = AzimuthDeg( m ), mEl = ElevationDeg( m );
		printf( "  ph0: SunDir az=%.2f el=%.2f  MoonDir az=%.2f el=%.2f\n", sAz, sEl, mAz, mEl );
		// 35deg arc apex (kArcMaxElevDeg=35, was 62): the lower arc puts the ph0
		// bodies slightly closer to the nodes/horizon. Guards pinned to the new
		// value (a regression back to the 62deg apex would fail these).
		Check( sAz >= 269.0f && sAz <= 271.0f, "SunDir(0) azimuth in [269,271] (sunset, west; 35deg apex)" );
		Check( sEl >= 3.0f && sEl <= 4.0f,     "SunDir(0) elevation in [3,4] (just above horizon; 35deg apex)" );
		Check( mAz >= 89.0f && mAz <= 91.0f,   "MoonDir(0) azimuth in [89,91] (moonrise, east; 35deg apex)" );
		Check( mEl >= -4.0f && mEl <= -3.0f,   "MoonDir(0) elevation in [-4,-3] (just below, rising; 35deg apex)" );
	}

	// --- 5. Midnight at ph0.5: the moon reaches its apex (highest) at ph~0.5,
	// where the sun is deep below. (Was the old ~0.44 nadir.) ---
	{
		float maxZ = -2.0f, atPh = 0.0f;
		for( float ph = 0.0f; ph <= 1.0f + 1e-6f; ph += 0.001f )
		{
			float m[3];
			MoonDir( ph, m );
			if( m[2] > maxZ )
			{
				maxZ = m[2];
				atPh = ph;
			}
		}
		float mMid[3], sMid[3];
		MoonDir( 0.5f, mMid );
		SunDir( 0.5f, sMid );
		printf( "  midnight: max MoonDir.z=%.4f at ph=%.3f; MoonDir(0.5).z=%.4f SunDir(0.5).z=%.4f\n",
			maxZ, atPh, mMid[2], sMid[2] );
		Check( fabsf( atPh - 0.5f ) < 0.02f, "max MoonDir.z occurs at ph ~ 0.5 (midnight)" );
		// 35deg arc apex (kArcMaxElevDeg=35, was 62): the moon tops out at sin(35)
		// ~ 0.574 at midnight, not sin(62) ~ 0.883. Guards pinned to the new apex.
		Check( mMid[2] > 0.55f && mMid[2] < 0.60f,   "MoonDir(0.5).z ~ 0.574 (moon at 35deg apex at midnight)" );
		Check( sMid[2] < -0.55f && sMid[2] > -0.60f, "SunDir(0.5).z ~ -0.574 (sun deep below at midnight)" );
	}

	// --- 6. Sunrise at ph~0.86: SunElevDeg crosses 0 inside the window (0.84, 0.88). ---
	{
		bool crossed = false;
		float prev = SunElevDeg( 0.84f );
		float at = 0.0f;
		for( float ph = 0.84f; ph <= 0.88f + 1e-6f; ph += 0.0005f )
		{
			float cur = SunElevDeg( ph );
			if( ( prev < 0.0f && cur >= 0.0f ) || ( prev > 0.0f && cur <= 0.0f ) )
			{
				crossed = true;
				at = ph;
				break;
			}
			prev = cur;
		}
		printf( "  sunrise: SunElevDeg sign change in (0.84,0.88) at ph=%.4f (found=%d)\n", at, (int)crossed );
		Check( crossed, "SunElevDeg crosses 0 (sunrise) within ph (0.84,0.88)" );
	}

	// --- 7. RoundPhase endpoints: 0 at round start (sunset), 0.5 at mid-round
	// (midnight), 1 at round end (full daylight). The continuous ease puts the
	// sin term to zero at tau=0,0.5,1 so the anchors land exactly. ---
	{
		float D = 300.0f;
		float p0   = RoundPhase( 0.0f, D );
		float p150 = RoundPhase( 150.0f, D );	// mid round -> midnight
		float p300 = RoundPhase( 300.0f, D );	// full daylight
		printf( "  endpoints D=300: rp(0)=%.6f rp(150)=%.6f rp(300)=%.6f\n", p0, p150, p300 );
		Check( p0 == 0.0f,                    "RoundPhase(0,300) == 0 (round start = sunset)" );
		Check( fabsf( p150 - 0.5f ) < 1e-4f,  "RoundPhase(150,300) == 0.5 (midnight at mid-round)" );
		Check( p300 == 1.0f,                  "RoundPhase(300,300) == 1.0 (full daylight at round end)" );
	}

	// --- 8. Strictly increasing -- the freeze is GONE. Over 600 samples of
	// elapsed in (0,300) every step's phase is STRICTLY greater than the last
	// (min positive delta > 0); the old held-midnight gave zero deltas here. ---
	{
		float D = 300.0f;
		bool strict = true;
		float minDelta = 1e30f;
		float prev = RoundPhase( 0.0f, D );
		for( int i = 1; i <= 600; i++ )
		{
			float e = ( D * (float)i ) / 600.0f;
			float p = RoundPhase( e, D );
			float delta = p - prev;
			if( delta < minDelta )
				minDelta = delta;
			if( delta <= 0.0f )
				strict = false;
			prev = p;
		}
		printf( "  strict-increase D=300: 600 steps, min delta = %.6e (strict=%d)\n", minDelta, (int)strict );
		Check( strict && minDelta > 0.0f, "RoundPhase strictly increasing over 600 samples (NO freeze)" );
	}

	// --- 9. Night is slowest: the local phase rate near midnight is far smaller
	// than near dusk (slope = 1-A at tau=0.5 vs 1+A at the ends). ---
	{
		float D = 300.0f;
		float nightStep = RoundPhase( 151.5f, D ) - RoundPhase( 148.5f, D );	// across midnight
		float duskStep  = RoundPhase( 1.5f, D )   - RoundPhase( 0.0f, D );	// at sunset
		printf( "  rates D=300: nightStep(148.5->151.5)=%.6f duskStep(0->1.5)=%.6f\n", nightStep, duskStep );
		Check( nightStep < 0.5f * duskStep, "night phase-rate << dusk phase-rate (night is slowest)" );
	}

	// --- 10. Night is longest: the wall-clock fraction of the round spent with
	// phase in the dark band [0.3,0.7] is more than half the round. ---
	{
		float D = 300.0f;
		int total = 0, inBand = 0;
		for( int i = 0; i <= 10000; i++ )
		{
			float tau = (float)i / 10000.0f;
			float p = RoundPhase( tau * D, D );
			total++;
			if( p >= 0.3f && p <= 0.7f )
				inBand++;
		}
		float frac = (float)inBand / (float)total;
		printf( "  night-band D=300: fraction with phase in [0.3,0.7] = %.4f\n", frac );
		Check( frac > 0.50f, "wall-clock fraction with phase in [0.3,0.7] > 0.50 (night is longest)" );
	}

	// --- 11. Short-round fallback: a tiny round uses the same continuous curve,
	// stays in [0,1], monotonic non-decreasing, with the same 0/1 anchors. ---
	{
		float D = 30.0f;
		bool ok = true, nondec = true;
		float prev = -1.0f;
		for( int i = 0; i <= 100; i++ )
		{
			float e = ( D * (float)i ) / 100.0f;
			float p = RoundPhase( e, D );
			if( p < 0.0f || p > 1.0f )
				ok = false;
			if( p < prev - 1e-6f )
				nondec = false;
			prev = p;
		}
		float ps0  = RoundPhase( 0.0f, D );
		float ps30 = RoundPhase( 30.0f, D );
		printf( "  short-round D=30: in_range=%d monotone=%d rp(0)=%.6f rp(30)=%.6f\n", (int)ok, (int)nondec, ps0, ps30 );
		Check( ok,             "RoundPhase(e,30) in [0,1] (short-round)" );
		Check( nondec,         "RoundPhase(e,30) monotonic non-decreasing (short-round)" );
		Check( ps0 == 0.0f,    "RoundPhase(0,30) == 0 (short-round start)" );
		Check( ps30 == 1.0f,   "RoundPhase(30,30) == 1.0 (short-round end)" );
	}

	if( g_fail == 0 )
	{
		printf( "ALL PASS\n" );
		return 0;
	}
	printf( "FAIL: %d check(s) failed\n", g_fail );
	return 1;
}
