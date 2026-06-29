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
#include "csz_engine.h"		// gRenderAPI.R_FatPVS, ref_viewpass_t
#include "csz_fatal.h"

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

// --- dev-only sky-aim camera override (csz_devcam) ----------------------------
// Default OFF. When csz_devcam == 0 the override is a no-op and BuildViewFromPass
// leaves out.angles exactly as the engine produced them (byte-identical to prior
// behavior). When enabled it rewrites the sky-render PITCH/YAW *after* the engine
// pitch clamp, so a headless screenshot can aim the sky passes at any angle (e.g.
// zenith) to frame the Milky Way band. Lazy-registered once on first use, modeled
// on EnsureCvars() in geom/csz_sunmoon.cpp. Adds ZERO GL calls (pure CPU angle
// assignment before the existing matrix math).
cvar_t *s_cvDevcam;       // csz_devcam        master enable gate (0 = inactive)
cvar_t *s_cvDevcamPitch;  // csz_devcam_pitch  override pitch deg (Quake: negative = up)
cvar_t *s_cvDevcamYaw;    // csz_devcam_yaw    override yaw deg
bool    s_devcamCvarsReady;

void EnsureDevcamCvars()
{
	if( s_devcamCvarsReady )
		return;
	s_devcamCvarsReady = true;
	s_cvDevcam      = gEngfuncs.pfnRegisterVariable( "csz_devcam", "0", FCVAR_CLIENTDLL );
	s_cvDevcamPitch = gEngfuncs.pfnRegisterVariable( "csz_devcam_pitch", "0", FCVAR_CLIENTDLL );
	s_cvDevcamYaw   = gEngfuncs.pfnRegisterVariable( "csz_devcam_yaw", "0", FCVAR_CLIENTDLL );
}

// Shared view-finalization tail: derive the projection/view/viewProj matrices and
// frustum from the already-populated fov/near/far/origin/angles, then reset the
// per-view pvs/ambience slots. disableFar drops the far culling plane (spot views).
void FinalizeView( ViewSetup &out, bool disableFar )
{
	Mat4Perspective( out.fovX, out.fovY, out.zNear, out.zFar, out.matProj );
	Mat4ViewQuake( out.origin, out.angles, out.matView );
	Mat4Multiply( out.matProj, out.matView, out.matViewProj );
	FrustumFromMatrix( out.matViewProj, disableFar, out.frustum );
	out.pvs = NULL;
	out.ambience = AmbienceNeutral();
	out.localHealth = 0;	// composition root sets the main view's value; default 0 = unknown
}

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

	for( int i = 0; i < 4; i++ )
		out.viewport[i] = rvp->viewport[i];

	out.fovX = rvp->fov_x;
	out.fovY = rvp->fov_y;
	out.zNear = 4.0f;
	out.zFar = 16384.0f;

	// Dev-only sky-aim override (default OFF). This ViewSetup is the SINGLE source
	// all sky passes read (AtmosDrawSky, StarsContribute incl. Milky Way,
	// SunMoonContribute, legacy g_sky.DrawSky); each calls AngleVectors(out.angles,..)
	// independently, so rewriting the angles here -- AFTER the engine pitch clamp and
	// BEFORE the matrices are built below -- aims every sky pass at the chosen angle.
	// csz_devcam == 0 (default) leaves out.angles untouched: byte-identical behavior.
	// Roll (out.angles[2]) is never touched. Null cvar pointer is treated as "off".
	EnsureDevcamCvars();
	if( ReadCvar( s_cvDevcam, 0.0f ) != 0.0f )
	{
		out.angles[0] = ReadCvar( s_cvDevcamPitch, 0.0f );	// PITCH (negative = look up)
		out.angles[1] = ReadCvar( s_cvDevcamYaw, 0.0f );	// YAW
	}

	// caller decides pvs (main view: UpdateFatPvs result); composition root
	// overwrites ambience from g_fog (slot 7.2).
	FinalizeView( out, false );
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

	// Far plane disabled for spot lights: attenuation handles the range and a
	// hard far clip would pop shadow casters (notes-mechanisms e). Shadow passes
	// render all-visible (pvs NULL); depth passes never read ambience.
	FinalizeView( out, true );
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
