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
#include "csz_light_budget.h"
#include "csz_flashlight_state.h"
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

// Reserved registry keys for the M1 test lights.
const int kTestSpotKey = -1;	// csz_testspot command (placed at the view)
const int kTestLightKey = -2;	// csz_testlight cvar (demo spot at T spawn)
const int kTestBeamKey = -3;	// csz_testbeam command (L6a: spot OFFSET from the view so the
                            	// world-space beam VOLUME is seen externally from the fixed camera)

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

cvar_t *s_cvarTestLight;	// csz_testlight (default 0: opt-in M1/M2 test fixture, off by default)
cvar_t *s_cvarShadow;		// csz_light_shadow (B-class quality seam, default 1)
cvar_t *s_cvarFlashlightReal;	// csz_flashlight_real (default 1: feed the table from live player flashlights)

// L6c dev-override latch. The csz_flashlight_test fixture and the real per-player
// feed both write the SAME state table with owner keys that overlap (test uses
// owners 1..N, real uses player entity indices 1..maxclients), so they must be
// mutually exclusive. While a test fixture is placed, CollectRealFlashlights is a
// no-op (test overrides); "csz_flashlight_test off" clears the table and the latch,
// and the real feed resumes the next frame.
bool s_flashlightTestActive;

// Eye-height offset for OTHER players' beams: their entity origin is at the feet,
// the muzzle/flashlight rides at roughly standing eye height. Approximate (M1):
// the local player uses the exact view eye instead.
const float kOtherPlayerEyeHeight = 24.0f;

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

// (L6a SpotConeBounds removed in L6b: the light-vs-view cone-bbox cull now lives
// once in the budgeter -- csz_light_budget.cpp ConeBounds -- which feeds budgetTier
// to both the shadow and direct passes here. Same math, single owner.)

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

// L6a third-person test fixture (csz_testbeam). csz_testspot puts the spot AT the
// eye (first-person: the camera sits at the cone apex and cannot see the volume from
// outside); csz_testlight needs a working devcam aim to frame the T-spawn light, which
// this devcam path does not provide. csz_testbeam instead places the spot OFFSET from
// the current view (a little up + to the side) and pitched further DOWN, so the beam
// shoots ahead into the scene while the camera stays OUTSIDE the cone -- the fixed
// forward view then sees the world-space beam VOLUME obliquely (the L6a acceptance shot).
void TestBeamCommand()
{
	if( gEngfuncs.Cmd_Argc() >= 2 && strcmp( gEngfuncs.Cmd_Argv( 1 ), "off" ) == 0 )
	{
		g_lights.Remove( kTestBeamKey );
		CSZ_LogInfo( "lighting", "test beam light removed" );
		return;
	}

	if( !s_haveView )
	{
		CSZ_LogWarn( "lighting", "csz_testbeam: no taken-over frame yet (enter a map with csz_renderer 1)" );
		return;
	}

	float fwd[3], right[3], up[3];

	AngleVectors( s_viewAngles, fwd, right, up );

	LightDesc desc;

	memset( &desc, 0, sizeof( desc ));
	desc.type = kLightSpot;
	// Apex up + to the right of the eye so the camera is clearly outside the cone.
	for( int j = 0; j < 3; j++ )
		desc.origin[j] = s_viewOrigin[j] + up[j] * 60.0f + right[j] * 50.0f + fwd[j] * 24.0f;
	// Aim along the view yaw but pitched well DOWN: the beam descends into the floor
	// ahead, fully inside the forward frustum, seen side-on from the upper-left.
	desc.angles[0] = s_viewAngles[0] + 30.0f;	// quake +pitch = downward
	desc.angles[1] = s_viewAngles[1];
	desc.angles[2] = 0.0f;
	desc.color[0] = kTestColor[0];
	desc.color[1] = kTestColor[1];
	desc.color[2] = kTestColor[2];
	desc.radius = kTestRadius;
	desc.fov = kTestFov;
	desc.die = 0.0f;			// persistent until "csz_testbeam off"
	desc.castShadow = true;

	int slot = g_lights.AddOrUpdate( kTestBeamKey, desc );

	if( slot >= 0 )
	{
		CSZ_LogInfo( "lighting", "beam light slot=%d key=%d origin=(%.0f %.0f %.0f) ang=(%.0f %.0f) fov=%.0f radius=%.0f",
			slot, kTestBeamKey, desc.origin[0], desc.origin[1], desc.origin[2],
			desc.angles[0], desc.angles[1], desc.fov, desc.radius );
	}
}

// L6b multi-flashlight test fixture. Exercises the DECOUPLED state table
// (csz_flashlight_state.h -- the server-authoritative seam) AND the hard-cap
// budgeter end to end: "csz_flashlight_test N" populates N beams (owner #1 is the
// local player's beam at the eye, isLocal -> always top priority; the rest fan out
// ahead at staggered ranges so the budgeter has a clear priority order to cap on).
// The renderer then mirrors the table into g_lights (FlashlightPublishToRegistry)
// and LightBudgetCompute tiers them. "csz_flashlight_test off" clears the table.
void FlashlightTestCommand()
{
	if( gEngfuncs.Cmd_Argc() >= 2 && strcmp( gEngfuncs.Cmd_Argv( 1 ), "off" ) == 0 )
	{
		FlashlightClearAll();
		s_flashlightTestActive = false;	// release the dev override -> real feed resumes
		CSZ_LogInfo( "flashlight", "test flashlights cleared" );
		return;
	}

	if( !s_haveView )
	{
		CSZ_LogWarn( "flashlight", "csz_flashlight_test: no taken-over frame yet (enter a map with csz_renderer 1)" );
		return;
	}

	int count = ( gEngfuncs.Cmd_Argc() >= 2 ) ? atoi( gEngfuncs.Cmd_Argv( 1 ) ) : 12;

	if( count < 1 )  count = 1;
	if( count > 24 ) count = 24;	// state table holds 32; cap the fixture below that

	FlashlightClearAll();

	float fwd[3], right[3], up[3];

	AngleVectors( s_viewAngles, fwd, right, up );

	for( int i = 0; i < count; i++ )
	{
		FlashlightState st;

		memset( &st, 0, sizeof( st ));
		st.enabled = true;
		st.owner = i + 1;

		if( i == 0 )
		{
			// The viewer's own beam: at the eye, aimed along the view, isLocal so the
			// budgeter pins it to the full tier no matter how crowded it gets.
			st.isLocal = true;
			for( int j = 0; j < 3; j++ )
				st.origin[j] = s_viewOrigin[j] + up[j] * 8.0f + fwd[j] * 8.0f;
			st.angles[0] = s_viewAngles[0];
			st.angles[1] = s_viewAngles[1];
			st.angles[2] = 0.0f;
		}
		else
		{
			// Fan the rest around the view ahead, on a ring, at staggered forward
			// distances so each has a distinct camera distance -> a deterministic
			// budget priority order (closest survive full, mid go cheap, far cull).
			float az = ( (float)i / (float)count ) * 2.0f * 3.14159265f;
			float fwdDist = 120.0f + (float)( i % 6 ) * 110.0f;	// 120..670
			float ringR = 140.0f;

			for( int j = 0; j < 3; j++ )
				st.origin[j] = s_viewOrigin[j] + fwd[j] * fwdDist
					+ right[j] * cosf( az ) * ringR
					+ up[j] * ( 50.0f + sinf( az ) * 40.0f );

			st.angles[0] = s_viewAngles[0] + 35.0f;	// pitch down into the floor ahead
			st.angles[1] = s_viewAngles[1] + cosf( az ) * 25.0f;
			st.angles[2] = 0.0f;
		}

		st.range = 0.0f;	// module defaults (warm-white, 700u, 50deg)
		st.fov = 0.0f;

		FlashlightSet( st );
	}

	s_flashlightTestActive = true;	// dev override engaged: CollectRealFlashlights no-ops until "off"

	CSZ_LogInfo( "flashlight", "placed %d test flashlights (owner #1 = local, isLocal); state table count=%d",
		count, FlashlightCount() );
}

}  // anonymous namespace

// L6c: feed the decoupled per-flashlight state table (csz_flashlight_state.h) from
// LIVE player state each frame, so the real F-key flashlight drives the same crisp
// cone the csz_flashlight_test fixture exercises -- without any dev command.
//   - local player: the viewer's own beam at the EXACT view eye, aimed along the
//     view (isLocal -> budget top priority), gated on the local entity's EF_DIMLIGHT.
//   - other players: one beam per player whose curstate.effects has EF_DIMLIGHT,
//     at the entity origin + eye height, aimed along its angles (owner = entity index).
//   - players who turned the flashlight off / left are FlashlightClear'd.
// EF_DIMLIGHT is the same server-set effect (CBasePlayer::FlashlightTurnOn sets
// pev->effects |= EF_DIMLIGHT) the engine reads to draw the legacy flashlight dlight,
// so it is present in curstate for every player including the local one. The L6b
// budgeter (4 full + 8 cheap) tiers the result; no renderer change needed.
void CollectRealFlashlights( const ViewSetup &mainView )
{
	// Default ON (csz_flashlight_real 1). 0 disables the real feed entirely (e.g. to
	// isolate the dev fixture or A/B the legacy path).
	if( s_cvarFlashlightReal != NULL && s_cvarFlashlightReal->value == 0.0f )
		return;

	// Dev override: while a csz_flashlight_test fixture is placed, leave the table to
	// it (its owner keys overlap real player indices, so the two cannot coexist).
	if( s_flashlightTestActive )
		return;

	cl_entity_t *local = gEngfuncs.GetLocalPlayer();
	int localIdx = ( local != NULL ) ? local->index : -1;
	int maxClients = gEngfuncs.GetMaxClients();

	if( maxClients < 1 )
		maxClients = 1;
	if( maxClients > 32 )
		maxClients = 32;	// state table cap; never overflow the per-owner table

	float fwd[3], right[3], up[3];

	AngleVectors( mainView.angles, fwd, right, up );

	for( int i = 1; i <= maxClients; i++ )
	{
		cl_entity_t *ent = gEngfuncs.GetEntityByIndex( i );
		bool lit = ( ent != NULL && ( ent->curstate.effects & EF_DIMLIGHT ) != 0 );

		if( !lit )
		{
			FlashlightClear( i );	// off this frame / disconnected -> drop the beam
			continue;
		}

		FlashlightState st;

		memset( &st, 0, sizeof( st ));
		st.enabled = true;
		st.owner = i;

		if( i == localIdx )
		{
			// The viewer's own beam: exact view eye, aimed along the view. isLocal
			// pins it to the full tier no matter how crowded the scene gets. A small
			// forward nudge keeps the cone apex just ahead of the camera (mirrors the
			// csz_flashlight_test owner #1 placement intent: camera outside the cone).
			st.isLocal = true;
			for( int j = 0; j < 3; j++ )
				st.origin[j] = mainView.origin[j] + fwd[j] * 8.0f;
			st.angles[0] = mainView.angles[0];
			st.angles[1] = mainView.angles[1];
			st.angles[2] = 0.0f;
		}
		else
		{
			// Other players: feet origin + eye height, aimed along the transmitted
			// player angles. Pitch fidelity for remote players is approximate (M1).
			st.origin[0] = ent->curstate.origin[0];
			st.origin[1] = ent->curstate.origin[1];
			st.origin[2] = ent->curstate.origin[2] + kOtherPlayerEyeHeight;
			st.angles[0] = ent->curstate.angles[0];
			st.angles[1] = ent->curstate.angles[1];
			st.angles[2] = 0.0f;
		}

		st.range = 0.0f;	// module defaults (warm-white, 700u, 50deg)
		st.fov = 0.0f;

		FlashlightSet( st );
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

		// L6b: only a full-tier beam may claim the single shadow map. The budgeter
		// already culled off-screen + over-budget lights (kBudgetCull) and demoted
		// the lower-priority ones to kBudgetCheap (shadowless by contract), so the
		// map always lands on a top-priority visible beam.
		if( light->budgetTier != kBudgetFull )
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

		// L6b hard cap (LightBudgetCompute, run earlier this frame): skip culled
		// beams -- both off-screen (the budgeter ran the same cone-bbox vs main
		// frustum test) and the lowest-priority ones beyond the full+cheap cap.
		// This bounds the per-light world+studio direct add the same way the cone
		// volume is bounded. Full + cheap both light the holder + struck surfaces;
		// cheap is shadowless automatically (the shadow pass skipped it, so
		// shadowTexSlot stays 0 and BuildSpotParams yields a shadowless light).
		if( light->budgetTier == kBudgetCull )
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
	gEngfuncs.pfnAddCommand( "csz_testbeam", TestBeamCommand );	// L6a third-person beam fixture
	gEngfuncs.pfnAddCommand( "csz_flashlight_test", FlashlightTestCommand );	// L6b multi-beam + cap fixture

	LightBudgetRegisterCvars();	// L6b: csz_flashlight_max_full / _max_cheap

	// L5R first-person "crisp redo" tunables. csz_flashlight_v3 is the master A/B switch
	// (1 = new crisp analytic direct profile + compressed volume + dome fix; 0 = pre-L5R).
	// Registered here (the direct lit-pass owner) so it exists at init before any frame;
	// the registry (BuildSpotParams) + the volume modules read it by name. The direct
	// profile knobs feed kWorldLitFs/kStudioLitFs via SpotLightParams.
	gEngfuncs.pfnRegisterVariable( "csz_flashlight_v3", "1", FCVAR_CLIENTDLL );
	gEngfuncs.pfnRegisterVariable( "csz_flashlight_edge", "2.5", FCVAR_CLIENTDLL );           // cone-edge exponent
	gEngfuncs.pfnRegisterVariable( "csz_flashlight_hotspot", "1.4", FCVAR_CLIENTDLL );        // central hotspot gain
	gEngfuncs.pfnRegisterVariable( "csz_flashlight_hotspot_sharp", "8.0", FCVAR_CLIENTDLL );  // hotspot tightness
	gEngfuncs.pfnRegisterVariable( "csz_flashlight_direct_gain", "1.8", FCVAR_CLIENTDLL );    // direct-pool brightness

	if( s_cvarTestLight == NULL )
		s_cvarTestLight = gEngfuncs.pfnRegisterVariable( "csz_testlight", "0", FCVAR_CLIENTDLL );

	if( s_cvarShadow == NULL )
		s_cvarShadow = gEngfuncs.pfnRegisterVariable( "csz_light_shadow", "1", FCVAR_CLIENTDLL );

	// L6c real per-player flashlight feed (default 1). When 1, CollectRealFlashlights
	// drives the state table from live player EF_DIMLIGHT each frame; 0 disables it
	// (dev: isolate the csz_flashlight_test fixture or the legacy engine flashlight).
	if( s_cvarFlashlightReal == NULL )
		s_cvarFlashlightReal = gEngfuncs.pfnRegisterVariable( "csz_flashlight_real", "1", FCVAR_CLIENTDLL );
}

}
