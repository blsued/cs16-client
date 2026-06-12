/*
 * csz_light_pass.cpp -- CSOZ renderer: light pass orchestration
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
#include "csz_light_pass.h"
#include "csz_light_registry.h"
#include "csz_shadowmap.h"
#include "../core/csz_engine.h"
#include "../core/csz_engine_bsp.h"
#include "../core/csz_log.h"
#include "../core/csz_view.h"
#include "../geom/csz_studio.h"
#include "../geom/csz_world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace csz
{

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// Reserved registry keys for the two M1 test lights.
const int kTestSpotKey = -1;	// csz_testspot command (placed at the view)
const int kTestLightKey = -2;	// csz_testlight cvar (demo spot at T spawn)

// Test light tuning (plan section 9 step 2 for the csz_testspot numbers; the
// T-spawn demo light shares them so both showcase the same cone profile).
const float kTestRadius = 700.0f;
const float kTestFov = 50.0f;
const float kTestColor[3] = { 1.0f, 0.95f, 0.85f };	// warm white

// Demo light placement relative to the parsed T spawn: hover above the spawn
// cluster, pitched down ahead of it so the cone paints BOTH a crisp ground
// pool (bots walk through it) and a wall spot within the radius. Pitch 40
// (live-tuned in T6: 30 put most of the energy on a far wall at ~8 percent
// attenuation; 40 lands the bright streak 90..400 units ahead of the spawn).
const float kDemoHeight = 160.0f;
const float kDemoPitchDeg = 40.0f;	// quake +pitch = downward

// Last taken-over main view (csz_testspot places the light here). Latched by
// RunLightPasses each frame; commands run between frames.
float s_viewOrigin[3];
float s_viewAngles[3];
bool s_haveView;

cvar_t *s_cvarTestLight;	// csz_testlight (default 1: M1 demo light on)
cvar_t *s_cvarShadow;		// csz_light_shadow (B-class quality seam, default 1)

// T-spawn parse cache, keyed by map name (re-parsed on map change).
char s_spawnMapName[64];
bool s_spawnValid;
float s_spawnOrigin[3];
float s_spawnYaw;
bool s_demoLightOn;
bool s_parseWarned;

// ---------------------------------------------------------------------------
// Minimal BSP entity-lump scan: finds the first block whose classname is
// "info_player_deathmatch" (the CS T-team spawn) and reads origin + yaw.
// The lump is plain text: { "key" "value" ... } blocks.
// ---------------------------------------------------------------------------
bool ParseTSpawn( const char *ents, float origin[3], float *yaw )
{
	if( ents == NULL )
		return false;

	char key[64];
	char classname[64];
	char originStr[64];
	char angleStr[64];
	bool inBlock = false;

	classname[0] = originStr[0] = angleStr[0] = '\0';

	const char *p = ents;

	while( *p != '\0' )
	{
		if( *p == '{' )
		{
			inBlock = true;
			classname[0] = originStr[0] = angleStr[0] = '\0';
			p++;
			continue;
		}

		if( *p == '}' )
		{
			if( inBlock && strcmp( classname, "info_player_deathmatch" ) == 0 && originStr[0] != '\0' )
			{
				if( sscanf( originStr, "%f %f %f", &origin[0], &origin[1], &origin[2] ) == 3 )
				{
					float dummyPitch = 0.0f, dummyRoll = 0.0f;

					*yaw = 0.0f;

					// Spawn points carry either "angle" (yaw only) or a
					// full "angles" triple; both land in angleStr.
					if( angleStr[0] != '\0' &&
						sscanf( angleStr, "%f %f %f", &dummyPitch, yaw, &dummyRoll ) < 2 )
						*yaw = (float)atof( angleStr );

					return true;
				}
			}

			inBlock = false;
			p++;
			continue;
		}

		if( inBlock && *p == '"' )
		{
			// Quoted key then quoted value.
			const char *start = ++p;

			while( *p != '\0' && *p != '"' ) p++;
			if( *p == '\0' )
				break;

			size_t len = (size_t)( p - start );

			if( len >= sizeof( key ))
				len = sizeof( key ) - 1;
			memcpy( key, start, len );
			key[len] = '\0';
			p++;

			while( *p != '\0' && *p != '"' && *p != '}' && *p != '{' ) p++;
			if( *p != '"' )
				continue;	// malformed pair; resync on next token

			start = ++p;
			while( *p != '\0' && *p != '"' ) p++;
			if( *p == '\0' )
				break;

			len = (size_t)( p - start );

			char *dst = NULL;
			size_t dstSize = 0;

			if( strcmp( key, "classname" ) == 0 )
			{
				dst = classname;
				dstSize = sizeof( classname );
			}
			else if( strcmp( key, "origin" ) == 0 )
			{
				dst = originStr;
				dstSize = sizeof( originStr );
			}
			else if( strcmp( key, "angle" ) == 0 || strcmp( key, "angles" ) == 0 )
			{
				dst = angleStr;
				dstSize = sizeof( angleStr );
			}

			if( dst != NULL )
			{
				if( len >= dstSize )
					len = dstSize - 1;
				memcpy( dst, start, len );
				dst[len] = '\0';
			}

			p++;
			continue;
		}

		p++;
	}

	return false;
}

// Keeps the csz_testlight demo spot in sync with the cvar and the current
// map. Runs once per taken-over frame (cheap: string compare + flag checks).
void SyncDemoLight()
{
	model_t *world = WorldModel();

	if( s_cvarTestLight == NULL || world == NULL )
		return;

	const EngModel *bsp = EngBsp( world );

	// Map change: invalidate the spawn cache (and any stale demo light).
	if( strncmp( s_spawnMapName, bsp->name, sizeof( s_spawnMapName )) != 0 )
	{
		memcpy( s_spawnMapName, bsp->name, sizeof( s_spawnMapName ));
		s_spawnMapName[sizeof( s_spawnMapName ) - 1] = '\0';
		s_spawnValid = ParseTSpawn( bsp->entities, s_spawnOrigin, &s_spawnYaw );
		s_parseWarned = false;

		if( s_demoLightOn )
		{
			g_lights.Remove( kTestLightKey );
			s_demoLightOn = false;
		}
	}

	bool wantOn = ( s_cvarTestLight->value != 0.0f );

	if( wantOn && !s_spawnValid )
	{
		if( !s_parseWarned )
		{
			s_parseWarned = true;
			CSZ_LogWarn( "lighting", "csz_testlight: no info_player_deathmatch in %s; demo light unavailable",
				s_spawnMapName );
		}

		return;
	}

	if( wantOn && !s_demoLightOn )
	{
		LightDesc desc;

		memset( &desc, 0, sizeof( desc ));
		desc.type = kLightSpot;
		desc.origin[0] = s_spawnOrigin[0];
		desc.origin[1] = s_spawnOrigin[1];
		desc.origin[2] = s_spawnOrigin[2] + kDemoHeight;
		desc.angles[0] = kDemoPitchDeg;		// down-forward along the spawn yaw
		desc.angles[1] = s_spawnYaw;
		desc.angles[2] = 0.0f;
		desc.color[0] = kTestColor[0];
		desc.color[1] = kTestColor[1];
		desc.color[2] = kTestColor[2];
		desc.radius = kTestRadius;
		desc.fov = kTestFov;
		desc.die = 0.0f;			// persistent
		desc.castShadow = true;

		int slot = g_lights.AddOrUpdate( kTestLightKey, desc );

		if( slot >= 0 )
		{
			s_demoLightOn = true;
			CSZ_LogInfo( "lighting", "spot light slot=%d key=%d origin=(%.0f %.0f %.0f) fov=%.0f radius=%.0f",
				slot, kTestLightKey, desc.origin[0], desc.origin[1], desc.origin[2], desc.fov, desc.radius );
		}
	}
	else if( !wantOn && s_demoLightOn )
	{
		g_lights.Remove( kTestLightKey );
		s_demoLightOn = false;
		CSZ_LogInfo( "lighting", "demo spot light removed (csz_testlight 0)" );
	}
}

// Conservative world-space bounds of the spot cone: apex + the four corners
// of the (square) far plane. Used for light-vs-view visibility only.
void SpotConeBounds( const LightDesc &d, float mins[3], float maxs[3] )
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

void TestSpotCommand()
{
	// "csz_testspot off" removes; bare "csz_testspot" places/moves the spot
	// to the current view (plan section 9 step 2).
	if( gEngfuncs.Cmd_Argc() >= 2 && strcmp( gEngfuncs.Cmd_Argv( 1 ), "off" ) == 0 )
	{
		g_lights.Remove( kTestSpotKey );
		CSZ_LogInfo( "lighting", "test spot light removed" );
		return;
	}

	if( !s_haveView )
	{
		CSZ_LogWarn( "lighting", "csz_testspot: no taken-over frame yet (enter a map with csz_renderer 1)" );
		return;
	}

	LightDesc desc;

	memset( &desc, 0, sizeof( desc ));
	desc.type = kLightSpot;
	desc.origin[0] = s_viewOrigin[0];
	desc.origin[1] = s_viewOrigin[1];
	desc.origin[2] = s_viewOrigin[2];
	desc.angles[0] = s_viewAngles[0];
	desc.angles[1] = s_viewAngles[1];
	desc.angles[2] = s_viewAngles[2];
	desc.color[0] = kTestColor[0];
	desc.color[1] = kTestColor[1];
	desc.color[2] = kTestColor[2];
	desc.radius = kTestRadius;
	desc.fov = kTestFov;
	desc.die = 0.0f;			// persistent until "csz_testspot off"
	desc.castShadow = true;

	int slot = g_lights.AddOrUpdate( kTestSpotKey, desc );

	if( slot >= 0 )
	{
		CSZ_LogInfo( "lighting", "spot light slot=%d key=%d origin=(%.0f %.0f %.0f) fov=%.0f radius=%.0f",
			slot, kTestSpotKey, desc.origin[0], desc.origin[1], desc.origin[2], desc.fov, desc.radius );
	}
}

}

void RenderShadowMaps( const ViewSetup &mainView, cl_entity_s *const *studioEnts, int studioCount )
{
	// B-class quality seam (plan section 10 step 2): csz_light_shadow 0 keeps
	// every light shadowless this frame -- UpdateMatrices already reset all
	// shadowTexSlot to 0, so returning here lands exactly on T6 behavior.
	if( s_cvarShadow == NULL || s_cvarShadow->value == 0.0f )
		return;

	float now = ClientTime();
	int shadowed = 0;
	int wanted = 0;

	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );

		if( !light->used )
			continue;

		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;	// expires this frame (RunLightPasses skips it too)

		if( light->desc.type != kLightSpot || !light->desc.castShadow )
			continue;

		// Lights that cannot affect the main view get no depth pass (same
		// conservative cone-bbox test RunLightPasses uses for drawing).
		float mins[3], maxs[3];

		SpotConeBounds( light->desc, mins, maxs );

		if( mainView.frustum.CullBox( mins, maxs ))
			continue;

		wanted++;

		// M1 budget: exactly one physical depth map (g_spotShadow). The first
		// visible shadow-casting light claims it; the rest stay shadowless.
		if( shadowed >= 1 )
			continue;

		g_spotShadow.RenderDepth( *light, mainView, studioEnts, studioCount );

		if( light->shadowTexSlot != 0 )
			shadowed++;
	}

	if( wanted > 1 )
	{
		// Per-frame condition at Dev level with 1s self-throttle (R8).
		static float s_nextWarnTime;

		if( now >= s_nextWarnTime )
		{
			s_nextWarnTime = now + 1.0f;
			CSZ_LogDev( "lighting", "%d shadow casters visible; single M1 map serves the first only", wanted );
		}
	}
}

void RunLightPasses( const ViewSetup &mainView, cl_entity_s *const *studioEnts, int studioCount )
{
	// Latch the view for the csz_testspot command (runs between frames).
	s_viewOrigin[0] = mainView.origin[0];
	s_viewOrigin[1] = mainView.origin[1];
	s_viewOrigin[2] = mainView.origin[2];
	s_viewAngles[0] = mainView.angles[0];
	s_viewAngles[1] = mainView.angles[1];
	s_viewAngles[2] = mainView.angles[2];
	s_haveView = true;

	SyncDemoLight();

	float now = ClientTime();
	int active = 0;
	int drawn = 0;

	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );

		if( !light->used )
			continue;

		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;	// expires this frame; DecayFrame reaps it next ClearScene

		active++;

		if( light->desc.type != kLightSpot )
			continue;	// M1 implements spot only (plan 2.2 LightType note)

		// Light-vs-view visibility: cone corner bbox against the main view
		// frustum (plan section 9 step 2).
		float mins[3], maxs[3];

		SpotConeBounds( light->desc, mins, maxs );

		if( mainView.frustum.CullBox( mins, maxs ))
			continue;

		SpotLightParams params;

		g_lights.BuildSpotParams( *light, params );
		g_world.DrawLitAdditive( mainView, params );
		g_studio.DrawLitAdditive( mainView, params, studioEnts, studioCount );
		drawn++;
	}

	// Per-frame stats at Dev level with 1s self-throttle (R8).
	static float s_nextStatsTime;

	if( active > 0 && now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "lighting", "light passes: %d drawn / %d active", drawn, active );
	}
}

void RegisterLightingCommands()
{
	gEngfuncs.pfnAddCommand( "csz_testspot", TestSpotCommand );

	if( s_cvarTestLight == NULL )
		s_cvarTestLight = gEngfuncs.pfnRegisterVariable( "csz_testlight", "1", FCVAR_CLIENTDLL );

	if( s_cvarShadow == NULL )
		s_cvarShadow = gEngfuncs.pfnRegisterVariable( "csz_light_shadow", "1", FCVAR_CLIENTDLL );
}

}
