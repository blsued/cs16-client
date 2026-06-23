/*
 * csz_flashlight_state.cpp -- CSOZ renderer: decoupled per-flashlight state table (L6b)
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
#include "csz_flashlight_state.h"
#include "csz_light_registry.h"
#include "../core/csz_engine.h"
#include "../core/csz_log.h"

#include <string.h>

namespace csz
{

namespace
{

// Per-owner state table. 32 is the engine player cap; a flashlight is one beam
// per holder so this never overflows in normal play (full -> oldest dropped, logged).
const int kMaxFlashlights = 32;

// Registry key namespace for published flashlights. The M1 test fixtures own
// keys -1..-3 (csz_light_pass.cpp); flashlight owners map to kKeyBase - owner so
// the two namespaces never collide.
const int kKeyBase = -100;

// Module defaults when a state leaves a field at 0 (same warm-white spot profile
// as the M1 test lights so a single published beam looks identical to csz_testbeam).
// Playtest r1 (operator ask 手电筒距离翻倍): the real F-key flashlight publishes
// st.range == 0 (CollectRealFlashlights), so this default IS the spotlight reach
// that drives the direct lit pool, the fog march far, and the world-cone length.
// Doubled 700 -> 1400 so the throw reaches ~2x farther; falloff shape unchanged
// (every consumer normalizes by the radius). Pairs with csz_flashlight_range 1600.
const float kDefaultRange = 1400.0f;
const float kDefaultFov   = 50.0f;
const float kDefaultColor[3] = { 1.0f, 0.95f, 0.85f };

FlashlightState s_table[kMaxFlashlights];

int KeyForOwner( int owner )
{
	return kKeyBase - owner;
}

int FindOwner( int owner )
{
	for( int i = 0; i < kMaxFlashlights; i++ )
		if( s_table[i].enabled && s_table[i].owner == owner )
			return i;
	return -1;
}

}  // anonymous namespace

void FlashlightSet( const FlashlightState &st )
{
	if( !st.enabled )
	{
		FlashlightClear( st.owner );
		return;
	}

	int slot = FindOwner( st.owner );

	if( slot < 0 )
	{
		for( int i = 0; i < kMaxFlashlights; i++ )
		{
			if( !s_table[i].enabled )
			{
				slot = i;
				break;
			}
		}
	}

	if( slot < 0 )
	{
		static float s_nextWarn;
		float now = ClientTime();

		if( now >= s_nextWarn )
		{
			s_nextWarn = now + 1.0f;
			CSZ_LogError( "flashlight", "state table full (%d owners); dropping owner %d",
				kMaxFlashlights, st.owner );
		}

		return;
	}

	s_table[slot] = st;
	s_table[slot].enabled = true;
}

void FlashlightClear( int owner )
{
	int slot = FindOwner( owner );

	if( slot >= 0 )
	{
		s_table[slot].enabled = false;
		g_lights.Remove( KeyForOwner( owner ) );	// drop the mirrored registry light now
	}
}

void FlashlightClearAll()
{
	for( int i = 0; i < kMaxFlashlights; i++ )
	{
		if( s_table[i].enabled )
		{
			g_lights.Remove( KeyForOwner( s_table[i].owner ) );
			s_table[i].enabled = false;
		}
	}
}

int FlashlightCount()
{
	int n = 0;

	for( int i = 0; i < kMaxFlashlights; i++ )
		if( s_table[i].enabled )
			n++;

	return n;
}

void FlashlightPublishToRegistry()
{
	// One writer per frame: mirror every enabled state into the spot registry.
	// Disabled owners were already Remove()'d at FlashlightClear time, so this
	// only upserts -- AddOrUpdate reuses the same-key slot, no churn.
	for( int i = 0; i < kMaxFlashlights; i++ )
	{
		const FlashlightState &st = s_table[i];

		if( !st.enabled )
			continue;

		LightDesc desc;

		memset( &desc, 0, sizeof( desc ));
		desc.type = kLightSpot;
		desc.origin[0] = st.origin[0];
		desc.origin[1] = st.origin[1];
		desc.origin[2] = st.origin[2];
		desc.angles[0] = st.angles[0];
		desc.angles[1] = st.angles[1];
		desc.angles[2] = st.angles[2];

		bool noColor = ( st.color[0] == 0.0f && st.color[1] == 0.0f && st.color[2] == 0.0f );

		desc.color[0] = noColor ? kDefaultColor[0] : st.color[0];
		desc.color[1] = noColor ? kDefaultColor[1] : st.color[1];
		desc.color[2] = noColor ? kDefaultColor[2] : st.color[2];
		desc.radius = ( st.range > 0.0f ) ? st.range : kDefaultRange;
		desc.fov    = ( st.fov   > 0.0f ) ? st.fov   : kDefaultFov;
		desc.die = 0.0f;			// lifetime owned by the state table, not a timer
		desc.castShadow = true;		// the budgeter decides who actually gets the 1 shadow map
		desc.isLocal = st.isLocal;	// budget top-priority hook for the viewer's own beam

		g_lights.AddOrUpdate( KeyForOwner( st.owner ), desc );
	}
}

}
