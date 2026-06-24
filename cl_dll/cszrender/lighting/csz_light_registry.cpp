/*
 * csz_light_registry.cpp -- CSOZ renderer: dynamic light source registry
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
#include "csz_light_registry.h"
#include "../core/csz_engine.h"
#include "../core/csz_fatal.h"
#include "../core/csz_log.h"

#include <math.h>
#include <string.h>

namespace csz
{

LightRegistry g_lights;

namespace
{

const float kDegToRadHalf = 3.14159265358979323846f / 360.0f;	// degrees -> radians, halved

// L5R direct-profile cvars (registered at init by RegisterLightingCommands). Looked up
// lazily by name and re-fetched until non-NULL so registration order never latches a miss.
cvar_t *s_v3, *s_edge, *s_hot, *s_hotSharp, *s_dgain;

float ReadCvar( cvar_t *cv, float fallback )
{
	return ( cv != NULL ) ? cv->value : fallback;
}

cvar_t *GetCvarCached( cvar_t **slot, const char *name )
{
	if( *slot == NULL )
		*slot = gEngfuncs.pfnGetCvarPointer( name );
	return *slot;
}

// Slot storage lives here so the contract header stays member-free
// (same pattern as WorldRenderer / LightmapAtlas).
ActiveLight s_slots[LightRegistry::kMaxLights];

// Recomputes one slot's derived data (matrices + cull frustum). Called from
// AddOrUpdate immediately (so a light added mid-frame is usable the same
// frame regardless of orchestration order) and from UpdateMatrices.
void UpdateSlot( ActiveLight &light )
{
	const LightDesc &d = light.desc;

	// Spot projection: square cone, near 0.1, far = attenuation radius.
	Mat4Perspective( d.fov, d.fov, 0.1f, d.radius, light.matProj );
	Mat4ViewQuake( d.origin, d.angles, light.matView );
	Mat4ShadowBias( light.matProj, light.matView, light.matShadow );

	Mat4 viewProj;

	Mat4Multiply( light.matProj, light.matView, viewProj );
	// Far plane disabled: attenuation handles range, a hard far clip would
	// pop shadow casters near the cone end (notes-mechanisms e).
	FrustumFromMatrix( viewProj, true, light.frustum );
}

}

int LightRegistry::AddOrUpdate( int key, const LightDesc &desc )
{
	int freeSlot = -1;

	// Same-key slot reuse first; else the first free client slot (< 32;
	// 32..63 are reserved for engine dlight mirroring in M2).
	for( int i = 0; i < kEngineDlightBase; i++ )
	{
		if( s_slots[i].used && s_slots[i].key == key )
		{
			freeSlot = i;
			break;
		}

		if( freeSlot < 0 && !s_slots[i].used )
			freeSlot = i;
	}

	if( freeSlot < 0 )
	{
		// M1 policy: no preemption when full (plan section 9 step 1).
		static float s_nextWarn;
		float now = ClientTime();

		if( now >= s_nextWarn )
		{
			s_nextWarn = now + 1.0f;
			CSZ_LogError( "lighting", "light registry full (%d client slots); dropping key %d",
				kEngineDlightBase, key );
		}

		return -1;
	}

	ActiveLight &light = s_slots[freeSlot];

	light.used = true;
	light.key = key;
	light.desc = desc;
	light.shadowTexSlot = 0;	// shadow pass (T7) re-fills every frame
	UpdateSlot( light );

	return freeSlot;
}

void LightRegistry::Remove( int key )
{
	for( int i = 0; i < kMaxLights; i++ )
	{
		if( s_slots[i].used && s_slots[i].key == key )
			s_slots[i].used = false;
	}
}

void LightRegistry::DecayFrame( float time )
{
	for( int i = 0; i < kMaxLights; i++ )
	{
		if( s_slots[i].used && s_slots[i].desc.die > 0.0f && s_slots[i].desc.die < time )
			s_slots[i].used = false;
	}
}

void LightRegistry::UpdateMatrices()
{
	// Cheap full refresh (<= 64 slots, a handful of trig ops each); also
	// resets per-frame shadow state so a light is never sampled against a
	// stale depth map (T7 re-fills shadowTexSlot after its depth pass).
	for( int i = 0; i < kMaxLights; i++ )
	{
		if( !s_slots[i].used )
			continue;

		s_slots[i].shadowTexSlot = 0;
		UpdateSlot( s_slots[i] );
	}
}

ActiveLight *LightRegistry::Slot( int i )
{
	// Contract: never returns NULL; out-of-range is a programmer error.
	if( i < 0 || i >= kMaxLights )
		CSZ_FatalInit( "lighting", "LightRegistry::Slot index out of range" );

	return &s_slots[i];
}

void LightRegistry::BuildSpotParams( const ActiveLight &light, SpotLightParams &out ) const
{
	const LightDesc &d = light.desc;

	out.origin[0] = d.origin[0];
	out.origin[1] = d.origin[1];
	out.origin[2] = d.origin[2];

	float right[3], up[3];

	AngleVectors( d.angles, out.dir, right, up );

	out.color[0] = d.color[0];
	out.color[1] = d.color[1];
	out.color[2] = d.color[2];
	out.radius = d.radius;

	// Cone falloff band: cutoff at the cone half angle, full intensity
	// inside 70% of it (plan section 9 step 1).
	out.cosOuter = cosf( d.fov * kDegToRadHalf );
	out.cosInner = cosf( d.fov * 0.7f * kDegToRadHalf );

	out.matShadow = light.matShadow;
	out.shadowTexSlot = light.shadowTexSlot;	// T6: always 0 (shadowless)

	// L5R direct profile (csz_flashlight_v3). Read from cvars so the world+studio lit
	// shaders get the crisp analytic pool + central hotspot + direct gain; v3 0 -> the
	// shaders fall back to the legacy linear cone for A/B (defaults match the cvar reg).
	out.v3           = ( ReadCvar( GetCvarCached( &s_v3, "csz_flashlight_v3" ), 1.0f ) >= 0.5f ) ? 1.0f : 0.0f;
	out.edgeExp      = ReadCvar( GetCvarCached( &s_edge,     "csz_flashlight_edge" ),          2.5f );
	out.hotspotGain  = ReadCvar( GetCvarCached( &s_hot,      "csz_flashlight_hotspot" ),       1.4f );
	out.hotspotSharp = ReadCvar( GetCvarCached( &s_hotSharp, "csz_flashlight_hotspot_sharp" ), 8.0f );
	out.directGain   = ReadCvar( GetCvarCached( &s_dgain,    "csz_flashlight_direct_gain" ),   1.8f );
	out.maxBlend     = false;   // FIX-1: default additive (local); RunLightPasses sets it for non-local pools
}

}
