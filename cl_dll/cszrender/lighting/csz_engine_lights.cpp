/*
 * csz_engine_lights.cpp -- CSOZ renderer: engine dynamic/entity light mirror (M2)
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
#include "csz_engine_lights.h"
#include "csz_light_registry.h"
#include "../core/csz_engine.h"
#include "../core/csz_log.h"
#include "../core/csz_view.h"

#include <math.h>
#include <string.h>

namespace csz
{

namespace
{

// Xash3D-FWGS engine pools (cl_efx). render_api GetDynamicLight/GetEntityLight
// clamp out-of-range indices to slot 0, so over-polling is harmless but yields a
// duplicate slot-0 pointer -- the per-pointer dedup below drops those.
const int kMaxEngineDlights = 64;
const int kMaxEngineElights = 64;

cvar_t *s_cvarDlight;       // csz_dlight            master enable (1)
cvar_t *s_cvarElight;       // csz_elight            master enable (1)
cvar_t *s_cvarDlightMax;    // csz_dlight_max        nearest-N dlights drawn (band capped)
cvar_t *s_cvarElightMax;    // csz_elight_max        nearest-N elights drawn (band capped)
cvar_t *s_cvarDlightInt;    // csz_dlight_intensity  premultiplied color gain
cvar_t *s_cvarElightInt;    // csz_elight_intensity  premultiplied color gain

struct Cand
{
	const dlight_t *dl;
	float dist;
};

// Active = positive radius, not expired, not a dark (subtractive) light. The
// engine reuses pool slots and leaves stale entries with radius 0 / die in the
// past; both filter out here so only live lights mirror.
bool IsActive( const dlight_t *dl, float now )
{
	return dl != NULL && dl->radius > 0.0f && dl->die >= now && !dl->dark;
}

// Already collected this exact pool element? (clamp-duplicate or, defensively,
// a repeated pointer). Linear scan over a handful of candidates.
bool AlreadyHave( const Cand *cand, int n, const dlight_t *dl )
{
	for( int i = 0; i < n; i++ )
		if( cand[i].dl == dl )
			return true;
	return false;
}

// color24 (0..255) -> linear 0..1, intensity premultiplied. A 0,0,0 engine
// dlight means "white" (the muzzle/explosion path leaves color unset), so an
// all-zero color maps to white before the gain (matches the engine's own add).
void MakeColor( const color24 &c, float gain, float out[3] )
{
	float r = (float)c.r;
	float g = (float)c.g;
	float b = (float)c.b;

	if( r == 0.0f && g == 0.0f && b == 0.0f )
		r = g = b = 255.0f;

	out[0] = ( r / 255.0f ) * gain;
	out[1] = ( g / 255.0f ) * gain;
	out[2] = ( b / 255.0f ) * gain;
}

// Gather active engine lights from one pool, dedup, then partial-select the
// nearest `want` by camera distance straight into the registry band.
int MirrorPool( dlight_t *( *getter )( int ), int poolSize, const ViewSetup &view,
	int base, int count, int want, float gain )
{
	if( getter == NULL )
		return 0;

	if( want > count ) want = count;
	if( want <= 0 )
		return 0;

	Cand cand[kMaxEngineDlights > kMaxEngineElights ? kMaxEngineDlights : kMaxEngineElights];
	int numCand = 0;

	float now = ClientTime();

	for( int i = 0; i < poolSize; i++ )
	{
		dlight_t *dl = getter( i );

		if( !IsActive( dl, now ) || AlreadyHave( cand, numCand, dl ))
			continue;

		float dx = dl->origin[0] - view.origin[0];
		float dy = dl->origin[1] - view.origin[1];
		float dz = dl->origin[2] - view.origin[2];

		cand[numCand].dl = dl;
		cand[numCand].dist = sqrtf( dx * dx + dy * dy + dz * dz );
		numCand++;
	}

	// Selection sort by ascending distance for the first `want` (numCand small).
	int placed = 0;

	for( int a = 0; a < numCand && placed < want; a++ )
	{
		int best = a;

		for( int b = a + 1; b < numCand; b++ )
			if( cand[b].dist < cand[best].dist )
				best = b;

		if( best != a )
		{
			Cand t = cand[a]; cand[a] = cand[best]; cand[best] = t;
		}

		const dlight_t *dl = cand[a].dl;

		LightDesc desc;
		memset( &desc, 0, sizeof( desc ));
		desc.type = kLightPoint;
		desc.origin[0] = dl->origin[0];
		desc.origin[1] = dl->origin[1];
		desc.origin[2] = dl->origin[2];
		desc.radius = dl->radius;
		desc.die = dl->die;		// belt-and-suspenders: DecayFrame/RunLightPasses also gate on this
		desc.castShadow = false;	// omni, shadowless in M2
		MakeColor( dl->color, gain, desc.color );

		if( g_lights.PutEngineLight( base, count, desc ) >= 0 )
			placed++;
	}

	return placed;
}

}  // anonymous namespace

void CollectEngineLights( const ViewSetup &mainView )
{
	// Always reset the bands first so a frame that disables a class (or has no
	// live lights) leaves no stale point lights behind.
	g_lights.ResetEngineBand( LightRegistry::kEngineDlightBase, LightRegistry::kEngineDlightCount );
	g_lights.ResetEngineBand( LightRegistry::kElightBase, LightRegistry::kElightCount );

	int dlights = 0;
	int elights = 0;

	bool wantD = ( s_cvarDlight == NULL ) ? true : ( s_cvarDlight->value >= 0.5f );
	bool wantE = ( s_cvarElight == NULL ) ? true : ( s_cvarElight->value >= 0.5f );

	if( wantD )
	{
		int maxD = (int)( ReadCvar( s_cvarDlightMax, (float)LightRegistry::kEngineDlightCount ) + 0.5f );
		float gain = ReadCvar( s_cvarDlightInt, 2.5f );
		dlights = MirrorPool( gRenderAPI.GetDynamicLight, kMaxEngineDlights, mainView,
			LightRegistry::kEngineDlightBase, LightRegistry::kEngineDlightCount, maxD, gain );
	}

	if( wantE )
	{
		int maxE = (int)( ReadCvar( s_cvarElightMax, (float)LightRegistry::kElightCount ) + 0.5f );
		float gain = ReadCvar( s_cvarElightInt, 2.5f );
		elights = MirrorPool( gRenderAPI.GetEntityLight, kMaxEngineElights, mainView,
			LightRegistry::kElightBase, LightRegistry::kElightCount, maxE, gain );
	}

	// Per-frame stat at Dev level, 1s self-throttle (R8). Only chatters while at
	// least one engine light is live, so an idle scene stays quiet.
	static float s_nextStats;
	float now = ClientTime();

	if( ( dlights + elights ) > 0 && now >= s_nextStats )
	{
		s_nextStats = now + 1.0f;
		CSZ_LogDev( "lighting", "[CSZ:light] dlights=%d elights=%d", dlights, elights );
	}
}

void RegisterEngineLightCvars()
{
	if( s_cvarDlight == NULL )
		s_cvarDlight = gEngfuncs.pfnRegisterVariable( "csz_dlight", "1", FCVAR_CLIENTDLL );
	if( s_cvarElight == NULL )
		s_cvarElight = gEngfuncs.pfnRegisterVariable( "csz_elight", "1", FCVAR_CLIENTDLL );

	// Nearest-N caps (bounds the per-light world+studio additive draws under a
	// 32-bot muzzle storm). Default = full band; lower to trade reach for frame.
	if( s_cvarDlightMax == NULL )
		s_cvarDlightMax = gEngfuncs.pfnRegisterVariable( "csz_dlight_max", "16", FCVAR_CLIENTDLL );
	if( s_cvarElightMax == NULL )
		s_cvarElightMax = gEngfuncs.pfnRegisterVariable( "csz_elight_max", "16", FCVAR_CLIENTDLL );

	// Premultiplied color gain (brightness of the omni pool). 2.5 makes a white
	// muzzle dlight clearly brighten nearby walls + player models without blowing.
	if( s_cvarDlightInt == NULL )
		s_cvarDlightInt = gEngfuncs.pfnRegisterVariable( "csz_dlight_intensity", "2.5", FCVAR_CLIENTDLL );
	if( s_cvarElightInt == NULL )
		s_cvarElightInt = gEngfuncs.pfnRegisterVariable( "csz_elight_intensity", "2.5", FCVAR_CLIENTDLL );

	CSZ_LogDev( "lighting", "engine-light cvars registered (csz_dlight/csz_elight + _max/_intensity)" );
}

}
