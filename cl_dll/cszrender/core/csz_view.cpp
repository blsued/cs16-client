/*
 * csz_view.cpp -- CSOZ renderer: view setup and fat PVS cache
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
#include "csz_view.h"
#include "csz_engine.h"		// gRenderAPI.R_FatPVS, ref_viewpass_t, gEngfuncs
#include "csz_fatal.h"
#include "csz_math.h"		// AngleVectors
#include "../lighting/csz_light_registry.h"	// g_lights, kTestLightKey orbit target

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace csz
{

namespace
{

// Engine MAX_MAP_LEAFS is 131072 (pinned engine common/bspfile.h:76), so the
// engine-side R_FatPVS bridge may write up to 131072/8 = 16384 bytes into the
// caller buffer (it clamps to world.visbytes, cl_render.c:23). Sized for the
// worst case; the post-call assert below is a tripwire, not the protection.
const int kFatPvsBufferSize = 16384;

unsigned char s_fatPvs[kFatPvsBufferSize];
bool s_fatPvsValid;

// ---------------------------------------------------------------------------
// csz_debugcam: deterministic capture camera. Overrides the render view at the
// TOP of BuildViewFromPass (before matrices/frustum/PVS) so PVS + frustum +
// projection all follow the new camera and world geometry renders around it.
//   0 = off (engine view)   1 = explicit pos/ang   2 = orbit the testlight
// ---------------------------------------------------------------------------
const int kTestLightKey = -2;	// csz_testlight demo spot (orbit target in mode 2)

cvar_t *s_cvarDebugCam;		// csz_debugcam
cvar_t *s_cvarDebugCamPos;	// csz_debugcam_pos  "x y z"
cvar_t *s_cvarDebugCamAng;	// csz_debugcam_ang  "pitch yaw roll"
cvar_t *s_cvarDebugCamDist;	// csz_debugcam_dist (orbit: distance back along beam)
cvar_t *s_cvarDebugCamSide;	// csz_debugcam_side (orbit: perpendicular offset)
cvar_t *s_cvarDebugCamHeight;	// csz_debugcam_height (orbit: vertical lift)

// Mode 1: parse the explicit pos/ang cvar strings. Returns false (no override)
// if either string is empty or malformed -- never overrides on garbage.
bool DebugCamExplicit( float origin[3], float angles[3] )
{
	if( s_cvarDebugCamPos == NULL || s_cvarDebugCamAng == NULL )
		return false;

	const char *posStr = s_cvarDebugCamPos->string;
	const char *angStr = s_cvarDebugCamAng->string;

	if( posStr == NULL || posStr[0] == '\0' || angStr == NULL || angStr[0] == '\0' )
		return false;

	if( sscanf( posStr, "%f %f %f", &origin[0], &origin[1], &origin[2] ) != 3 )
		return false;
	if( sscanf( angStr, "%f %f %f", &angles[0], &angles[1], &angles[2] ) != 3 )
		return false;

	return true;
}

// Mode 2: frame the csz_testlight cone SIDE-ON. Reads the testlight (key -2)
// from the registry; falls back to no override if its slot is absent.
bool DebugCamOrbit( float origin[3], float angles[3] )
{
	ActiveLight *target = NULL;

	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );

		if( light->used && light->key == kTestLightKey )
		{
			target = light;
			break;
		}
	}

	if( target == NULL )
		return false;	// testlight not present -> leave the engine view alone

	float dist = ( s_cvarDebugCamDist != NULL ) ? s_cvarDebugCamDist->value : 260.0f;
	float side = ( s_cvarDebugCamSide != NULL ) ? s_cvarDebugCamSide->value : 260.0f;
	float height = ( s_cvarDebugCamHeight != NULL ) ? s_cvarDebugCamHeight->value : 80.0f;

	const float *L = target->desc.origin;

	float fwd[3], right[3], up[3];
	AngleVectors( target->desc.angles, fwd, right, up );

	// Horizontal vector perpendicular to the beam direction (project the light
	// "right" onto the ground plane; fall back to world +X if degenerate).
	float perp[3] = { right[0], right[1], 0.0f };
	float plen = sqrtf( perp[0] * perp[0] + perp[1] * perp[1] );
	if( plen < 1e-3f )
	{
		perp[0] = 1.0f; perp[1] = 0.0f; plen = 1.0f;
	}
	perp[0] /= plen; perp[1] /= plen;

	// Camera: out to the side, back down the beam axis, lifted up.
	origin[0] = L[0] + perp[0] * side - fwd[0] * dist;
	origin[1] = L[1] + perp[1] * side - fwd[1] * dist;
	origin[2] = L[2] - fwd[2] * dist + height;

	// Look toward a point partway down the beam so the whole shaft is framed.
	float look[3] = { L[0] + fwd[0] * 200.0f, L[1] + fwd[1] * 200.0f, L[2] + fwd[2] * 200.0f };
	float dir[3] = { look[0] - origin[0], look[1] - origin[1], look[2] - origin[2] };
	float dlen = sqrtf( dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2] );
	if( dlen < 1e-3f )
		return false;
	dir[0] /= dlen; dir[1] /= dlen; dir[2] /= dlen;

	float yaw = atan2f( dir[1], dir[0] ) * ( 180.0f / 3.14159265358979323846f );
	float pitch = -atan2f( dir[2], sqrtf( dir[0] * dir[0] + dir[1] * dir[1] )) * ( 180.0f / 3.14159265358979323846f );

	angles[0] = pitch;	// quake +pitch = downward
	angles[1] = yaw;
	angles[2] = 0.0f;

	return true;
}

// Returns true and fills origin/angles when an override is active and valid.
bool DebugCamOverride( float origin[3], float angles[3] )
{
	if( s_cvarDebugCam == NULL )
		return false;

	int mode = (int)s_cvarDebugCam->value;

	if( mode == 1 )
		return DebugCamExplicit( origin, angles );
	if( mode == 2 )
		return DebugCamOrbit( origin, angles );

	return false;
}

}

void RegisterViewDevCvars()
{
	if( s_cvarDebugCam == NULL )
		s_cvarDebugCam = gEngfuncs.pfnRegisterVariable( "csz_debugcam", "0", FCVAR_CLIENTDLL );
	if( s_cvarDebugCamPos == NULL )
		s_cvarDebugCamPos = gEngfuncs.pfnRegisterVariable( "csz_debugcam_pos", "", FCVAR_CLIENTDLL );
	if( s_cvarDebugCamAng == NULL )
		s_cvarDebugCamAng = gEngfuncs.pfnRegisterVariable( "csz_debugcam_ang", "", FCVAR_CLIENTDLL );
	if( s_cvarDebugCamDist == NULL )
		s_cvarDebugCamDist = gEngfuncs.pfnRegisterVariable( "csz_debugcam_dist", "260", FCVAR_CLIENTDLL );
	if( s_cvarDebugCamSide == NULL )
		s_cvarDebugCamSide = gEngfuncs.pfnRegisterVariable( "csz_debugcam_side", "260", FCVAR_CLIENTDLL );
	if( s_cvarDebugCamHeight == NULL )
		s_cvarDebugCamHeight = gEngfuncs.pfnRegisterVariable( "csz_debugcam_height", "80", FCVAR_CLIENTDLL );
}

void BuildViewFromPass( const struct ref_viewpass_s *rvp, ViewSetup &out )
{
	// The engine has already computed viewport/origin/angles/fov for this
	// pass (notes-renderapi A.7); we only derive matrices and the frustum.
	for( int i = 0; i < 3; i++ )
	{
		out.origin[i] = rvp->vieworigin[i];
		out.angles[i] = rvp->viewangles[i];
	}

	// csz_debugcam: deterministic capture override. Replaces origin/angles
	// BEFORE matrices/frustum are built (and before the caller derives PVS from
	// out.origin), so PVS + frustum + projection all follow the debug camera.
	{
		float dbgOrigin[3], dbgAngles[3];

		if( DebugCamOverride( dbgOrigin, dbgAngles ))
		{
			for( int i = 0; i < 3; i++ )
			{
				out.origin[i] = dbgOrigin[i];
				out.angles[i] = dbgAngles[i];
			}
		}
	}

	for( int i = 0; i < 4; i++ )
		out.viewport[i] = rvp->viewport[i];

	out.fovX = rvp->fov_x;
	out.fovY = rvp->fov_y;
	out.zNear = 4.0f;
	out.zFar = 16384.0f;

	Mat4Perspective( out.fovX, out.fovY, out.zNear, out.zFar, out.matProj );
	Mat4ViewQuake( out.origin, out.angles, out.matView );
	Mat4Multiply( out.matProj, out.matView, out.matViewProj );
	FrustumFromMatrix( out.matViewProj, false, out.frustum );
	out.pvs = NULL;		// caller decides (main view: UpdateFatPvs result)
	out.ambience = AmbienceNeutral();	// composition root overwrites from g_fog (slot 7.2)
}

void BuildSpotLightView( const float origin[3], const float anglesDeg[3],
                         float fovDeg, float radius, int resolution, ViewSetup &out )
{
	for( int i = 0; i < 3; i++ )
	{
		out.origin[i] = origin[i];
		out.angles[i] = anglesDeg[i];
	}

	out.viewport[0] = 0;
	out.viewport[1] = 0;
	out.viewport[2] = resolution;
	out.viewport[3] = resolution;
	out.fovX = fovDeg;
	out.fovY = fovDeg;
	out.zNear = 0.1f;
	out.zFar = radius;

	Mat4Perspective( out.fovX, out.fovY, out.zNear, out.zFar, out.matProj );
	Mat4ViewQuake( out.origin, out.angles, out.matView );
	Mat4Multiply( out.matProj, out.matView, out.matViewProj );
	// Far plane disabled for spot lights: attenuation handles the range and a
	// hard far clip would pop shadow casters (notes-mechanisms e).
	FrustumFromMatrix( out.matViewProj, true, out.frustum );
	out.pvs = NULL;		// shadow passes render all-visible (notes-mechanisms f-8)
	out.ambience = AmbienceNeutral();	// depth passes never read it; keep the struct fully defined
}

const unsigned char *UpdateFatPvs( const float origin[3] )
{
	if( gRenderAPI.R_FatPVS == NULL )
	{
		s_fatPvsValid = false;
		return NULL;
	}

	int bytes = gRenderAPI.R_FatPVS( origin, 2.0f, s_fatPvs, false, false );

	// The engine clamps its writes to world.visbytes <= MAX_MAP_LEAFS/8 which
	// is exactly our buffer size; a larger return means the contract changed
	// under us and memory is already stomped -- fail loud, never render on.
	if( bytes > kFatPvsBufferSize )
		CSZ_FatalInit( "view", "R_FatPVS wrote past the PVS buffer (engine contract changed)" );

	if( bytes <= 0 )
	{
		s_fatPvsValid = false;
		return NULL;
	}

	s_fatPvsValid = true;
	return s_fatPvs;
}

const unsigned char *CurrentFatPvs()
{
	return s_fatPvsValid ? s_fatPvs : NULL;
}

void ResetFatPvs()
{
	s_fatPvsValid = false;
}

}
