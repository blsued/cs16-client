/*
 * csz_atmos_math.h -- CSOZ renderer: physically-based atmosphere constants +
 *                     a compact CPU scattering evaluator (C2, PRIVATE to csz_atmos)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). CLEAN-ROOM implementation of the
 * Bruneton (2008, "Precomputed Atmospheric Scattering") and Hillaire (2020, "A
 * Scalable and Production Ready Sky and Atmosphere Rendering Technique") MODELS,
 * written from the PAPERS' mathematics only. No code, shader source, or data is
 * copied or translated from their MIT/BSD reference implementations, from
 * Unreal's EULA tree, or from any other license-tainted source (see csoz
 * docs/provenance.md). The numeric atmosphere coefficients below are physical
 * constants (Rayleigh/Mie/ozone cross-sections, scale heights, Earth radii),
 * not authored code.
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
// PRIVATE header, included ONLY by csz_atmos.cpp. The CPU evaluator here exists
// because the loaded GL function table (csz_glfuncs.h) has no glReadPixels /
// glGetTexImage -- the GPU LUTs cannot be read back -- so the CPU mirror below
// computes the two coupling values the rest of the engine needs on the CPU:
//   * horizonColor    -- low-frequency horizon CHROMA target for the C3 fog
//                        coupling (a color target, NOT a fog-density term).
//   * sunTransmittance -- ground->sun atmospheric transmittance for the C3 disc.
// It is a faithful but LOW-sample-count single-scattering + isotropic-ambient
// march (the SAME physics and SAME constants as csz_atmos_shaders.inl, which
// MUST be kept numerically in sync). Run once per phase change, never per frame
// in the hot path; no heap allocation.
#include <math.h>

namespace csz
{
namespace atmosmath
{

// --- physical constants (units: kilometres, per-kilometre cross-sections) -----
// These MUST match the GLSL block in csz_atmos_shaders.inl.
const float kRg = 6360.0f;          // ground (sea-level) radius
const float kRt = 6420.0f;          // atmosphere top radius (60 km shell)

const float kBetaR[3] = { 5.802e-3f, 13.558e-3f, 33.100e-3f }; // Rayleigh scattering (= extinction)
const float kHR       = 8.0f;       // Rayleigh density scale height

const float kBetaMs   = 3.996e-3f;  // Mie scattering
const float kBetaMe   = 4.440e-3f;  // Mie extinction (scattering + absorption)
const float kHM       = 1.2f;       // Mie density scale height
const float kMieG     = 0.80f;      // Mie phase asymmetry

const float kBetaO[3] = { 0.650e-3f, 1.881e-3f, 0.085e-3f };   // ozone absorption (no scattering)
const float kOzoneCenter = 25.0f;   // ozone tent centre (km)
const float kOzoneWidth  = 15.0f;   // ozone tent half-width (km)

const float kPi = 3.14159265358979323846f;

// --- tiny vec3 helpers (CPU only) ---------------------------------------------
inline float Dot3( const float a[3], const float b[3] ) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

// densities at geometric height h (km above ground)
inline float DensityR( float h ) { return expf( -h / kHR ); }
inline float DensityM( float h ) { return expf( -h / kHM ); }
inline float DensityO( float h )
{
	float d = 1.0f - fabsf( h - kOzoneCenter ) / kOzoneWidth;
	return ( d > 0.0f ) ? d : 0.0f;
}

// Nearest positive intersection distance of ray origin+t*dir with a sphere of
// the given radius centred at the planet centre (origin in planet-centric
// coords). Returns -1 if no positive hit. |dir| must be 1.
inline float RaySphere( const float ro[3], const float rd[3], float radius )
{
	float b = Dot3( ro, rd );
	float c = Dot3( ro, ro ) - radius * radius;
	float disc = b * b - c;
	if( disc < 0.0f )
		return -1.0f;
	float s = sqrtf( disc );
	float t0 = -b - s;
	float t1 = -b + s;
	if( t0 >= 0.0f ) return t0;
	if( t1 >= 0.0f ) return t1;
	return -1.0f;
}

// Distance from a point at radius r (with view-zenith cosine mu) to the top of
// the atmosphere, assuming the ray stays inside the shell (Bruneton geometry).
inline float DistanceToTop( float r, float mu )
{
	float disc = r * r * ( mu * mu - 1.0f ) + kRt * kRt;
	if( disc < 0.0f ) disc = 0.0f;
	float d = -r * mu + sqrtf( disc );
	return ( d > 0.0f ) ? d : 0.0f;
}

// Optical depth (per channel: Rayleigh+Mie+ozone) along a ray from planet-centric
// position `pos` in unit direction `dir`, integrated over `dist` km with N steps.
inline void OpticalDepth( const float pos[3], const float dir[3], float dist, int steps, float outTau[3] )
{
	outTau[0] = outTau[1] = outTau[2] = 0.0f;
	if( steps < 1 ) steps = 1;
	float ds = dist / (float)steps;
	for( int i = 0; i < steps; i++ )
	{
		float t = ( i + 0.5f ) * ds;
		float p[3] = { pos[0] + dir[0] * t, pos[1] + dir[1] * t, pos[2] + dir[2] * t };
		float r = sqrtf( Dot3( p, p ) );
		float h = r - kRg;
		float dR = DensityR( h );
		float dM = DensityM( h );
		float dO = DensityO( h );
		outTau[0] += ( kBetaR[0] * dR + kBetaMe * dM + kBetaO[0] * dO ) * ds;
		outTau[1] += ( kBetaR[1] * dR + kBetaMe * dM + kBetaO[1] * dO ) * ds;
		outTau[2] += ( kBetaR[2] * dR + kBetaMe * dM + kBetaO[2] * dO ) * ds;
	}
}

// Transmittance from `pos` toward `dir` to the atmosphere boundary. If the ray
// hits the ground first it is integrated only to the ground hit (so a downward /
// below-horizon sun returns a heavily extinct, reddened transmittance).
inline void Transmittance( const float pos[3], const float dir[3], float out[3] )
{
	float r = sqrtf( Dot3( pos, pos ) );
	float mu = Dot3( pos, dir ) / ( r > 1e-5f ? r : 1e-5f );
	float dGround = RaySphere( pos, dir, kRg );
	float dist = DistanceToTop( r, mu );
	if( dGround > 0.0f && dGround < dist )
		dist = dGround;
	float tau[3];
	OpticalDepth( pos, dir, dist, 24, tau );
	out[0] = expf( -tau[0] );
	out[1] = expf( -tau[1] );
	out[2] = expf( -tau[2] );
}

inline float RayleighPhase( float c ) { return ( 3.0f / ( 16.0f * kPi ) ) * ( 1.0f + c * c ); }

inline float MiePhase( float c )
{
	float g = kMieG;
	float g2 = g * g;
	float denom = 1.0f + g2 - 2.0f * g * c;
	if( denom < 1e-4f ) denom = 1e-4f;
	return ( 3.0f / ( 8.0f * kPi ) ) * ( ( 1.0f - g2 ) * ( 1.0f + c * c ) ) /
	       ( ( 2.0f + g2 ) * powf( denom, 1.5f ) );
}

}	// namespace atmosmath
}	// namespace csz
