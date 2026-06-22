/*
 * csz_light_budget.cpp -- CSOZ renderer: multi-flashlight hard-cap budgeter (L6b)
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
#include "csz_light_budget.h"
#include "csz_light_registry.h"
#include "../core/csz_engine.h"
#include "../core/csz_log.h"
#include "../core/csz_math.h"
#include "../core/csz_view.h"

#include <math.h>

namespace csz
{

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// Cheap-tier cone march steps (full tier uses the cone's native kConeSteps=16).
// Halving the per-pixel loop is the bulk of the cheap saving; 8 still anti-bands.
const int kCheapConeSteps = 8;

cvar_t *s_cvarMaxFull;     // csz_flashlight_max_full   default "4"  (cone+direct+shadow)
cvar_t *s_cvarMaxCheap;    // csz_flashlight_max_cheap  default "8"  (cone reduced + direct, shadowless)

int s_countFull;
int s_countCheap;
int s_countCull;

float ReadCvar( cvar_t *cv, float fallback )
{
	return ( cv != NULL ) ? cv->value : fallback;
}

// Conservative world-space bounds of the spot cone (apex + four far-plane corners),
// for the light-vs-view visibility test. Self-contained copy of the same math
// RunLightPasses uses (SpotConeBounds) so the budgeter has no cross-TU dependency.
void ConeBounds( const LightDesc &d, float mins[3], float maxs[3] )
{
	float fwd[3], right[3], up[3];

	AngleVectors( d.angles, fwd, right, up );

	float halfTan = tanf( d.fov * 0.5f * kDegToRad );

	for( int j = 0; j < 3; j++ )
	{
		mins[j] = d.origin[j];
		maxs[j] = d.origin[j];
	}

	for( int sx = -1; sx <= 1; sx += 2 )
	{
		for( int sy = -1; sy <= 1; sy += 2 )
		{
			for( int j = 0; j < 3; j++ )
			{
				float v = d.origin[j] + ( fwd[j] + right[j] * (float)sx * halfTan +
					up[j] * (float)sy * halfTan ) * d.radius;

				if( v < mins[j] ) mins[j] = v;
				if( v > maxs[j] ) maxs[j] = v;
			}
		}
	}
}

}  // anonymous namespace

void LightBudgetRegisterCvars()
{
	if( s_cvarMaxFull == NULL )
		// Full beams: cone-mesh (16 steps) + spot direct + the single shadow map.
		// 4 matches the research budget (L4D-style: a handful of close beams full).
		s_cvarMaxFull = gEngfuncs.pfnRegisterVariable( "csz_flashlight_max_full", "4", FCVAR_CLIENTDLL );
	if( s_cvarMaxCheap == NULL )
		// Cheap beams: cone-mesh at reduced steps + shadowless spot direct. 8 keeps
		// a dozen total beams visible before the rest cull. Set both huge to A/B-off
		// the cap (every visible beam becomes full).
		s_cvarMaxCheap = gEngfuncs.pfnRegisterVariable( "csz_flashlight_max_cheap", "8", FCVAR_CLIENTDLL );

	CSZ_LogDev( "lightbudget", "cvars registered (csz_flashlight_max_full/_max_cheap)" );
}

void LightBudgetCompute( const ViewSetup &view )
{
	int maxFull  = (int)( ReadCvar( s_cvarMaxFull, 4.0f ) + 0.5f );
	int maxCheap = (int)( ReadCvar( s_cvarMaxCheap, 8.0f ) + 0.5f );

	if( maxFull < 0 )  maxFull = 0;
	if( maxCheap < 0 ) maxCheap = 0;

	// Gather visible spot candidates with a priority score (lower = more
	// prominent -> higher tier). Score = distance to the camera, with the local
	// player's own beam forced to the front (isLocal). Screen-occupancy is folded
	// in as a mild bias so a wide near cone outranks a thin far one at equal range.
	int   cand[LightRegistry::kMaxLights];
	float score[LightRegistry::kMaxLights];
	int   numCand = 0;

	float now = ClientTime();

	s_countFull = s_countCheap = s_countCull = 0;

	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );

		if( !light->used || light->desc.type != kLightSpot )
			continue;
		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;

		// Off-screen beams cull for free and never spend cap budget.
		float mins[3], maxs[3];

		ConeBounds( light->desc, mins, maxs );

		if( view.frustum.CullBox( mins, maxs ))
		{
			light->budgetTier = kBudgetCull;
			s_countCull++;
			continue;
		}

		float dx = light->desc.origin[0] - view.origin[0];
		float dy = light->desc.origin[1] - view.origin[1];
		float dz = light->desc.origin[2] - view.origin[2];
		float dist = sqrtf( dx * dx + dy * dy + dz * dz );

		// Wider cones occupy more screen at the same range -> slightly higher
		// priority. tan(halfFov) in [~0.1, ~1]; the bias shrinks effective distance
		// by up to ~30% for a very wide beam, never reorders across large ranges.
		float widthBias = 1.0f - 0.3f * tanf( light->desc.fov * 0.5f * kDegToRad );

		if( widthBias < 0.5f ) widthBias = 0.5f;

		float s = dist * widthBias;

		if( light->desc.isLocal )
			s = -1.0f;	// the viewer's own beam is always top priority

		cand[numCand] = i;
		score[numCand] = s;
		numCand++;
	}

	// Selection sort by ascending score (numCand <= 64; a handful in practice).
	for( int a = 0; a < numCand; a++ )
	{
		int best = a;

		for( int b = a + 1; b < numCand; b++ )
			if( score[b] < score[best] )
				best = b;

		if( best != a )
		{
			float ts = score[a]; score[a] = score[best]; score[best] = ts;
			int ti = cand[a]; cand[a] = cand[best]; cand[best] = ti;
		}
	}

	// Assign tiers in priority order: first maxFull full, next maxCheap cheap,
	// the remainder culled. This is the hard cap -- at most maxFull+maxCheap beams
	// ever render, regardless of how many players light up.
	for( int k = 0; k < numCand; k++ )
	{
		ActiveLight *light = g_lights.Slot( cand[k] );

		if( k < maxFull )
		{
			light->budgetTier = kBudgetFull;
			s_countFull++;
		}
		else if( k < maxFull + maxCheap )
		{
			light->budgetTier = kBudgetCheap;
			s_countCheap++;
		}
		else
		{
			light->budgetTier = kBudgetCull;
			s_countCull++;
		}
	}

	// Cap evidence at Dev level, 1s self-throttle (R8). Only chatters when more
	// beams exist than the full tier -- i.e. when the cap is actually doing work.
	static float s_nextStats;

	if( ( s_countFull + s_countCheap + s_countCull ) > s_countFull && now >= s_nextStats )
	{
		s_nextStats = now + 1.0f;
		CSZ_LogDev( "lightbudget", "[csz_lightbudget] spots=%d visible=%d full=%d cheap=%d cull=%d (cap %d+%d)",
			s_countFull + s_countCheap + s_countCull, s_countFull + s_countCheap,
			s_countFull, s_countCheap, s_countCull, maxFull, maxCheap );
	}
}

int LightBudgetCount( LightBudgetTier tier )
{
	switch( tier )
	{
	case kBudgetFull:  return s_countFull;
	case kBudgetCheap: return s_countCheap;
	case kBudgetCull:  return s_countCull;
	}

	return 0;
}

int LightBudgetVisible()
{
	return s_countFull + s_countCheap;
}

int LightBudgetCheapSteps()
{
	return kCheapConeSteps;
}

}
