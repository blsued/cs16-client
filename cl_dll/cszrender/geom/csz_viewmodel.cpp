/*
 * csz_viewmodel.cpp -- CSOZ renderer: first-person viewmodel pass
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
#include "csz_viewmodel.h"
#include "csz_studio.h"
#include "../core/csz_engine.h"
#include "../core/csz_glstate.h"
#include "../core/csz_view.h"

namespace csz
{

namespace
{

// Engine-owned hide switch, mirrored from the stock viewmodel path (pinned
// ref/gl/gl_studio.c R_DrawViewModel). Looked up once; the engine registers
// it at startup so a NULL result is permanent.
cvar_t *s_drawViewModel;
bool s_cvarQueried;

}

void DrawViewModelPass( const ViewSetup &mainView )
{
	// Hide conditions (stock parity, pinned R_DrawViewModel): cvar off,
	// thirdperson, no model. Death (server zeroes the viewmodel index),
	// sniper scope (view.cpp:905) and intermission (view.cpp:501) all
	// surface here as model == NULL, so no health/scope checks are needed.
	// Known gap (T3 lesson): cl.viewentity is unreadable client-side, so a
	// trigger_camera view still shows the viewmodel (M1 accepted).
	if( !s_cvarQueried )
	{
		s_cvarQueried = true;
		s_drawViewModel = gEngfuncs.pfnGetCvarPointer( "r_drawviewmodel" );
	}

	if( s_drawViewModel != NULL && s_drawViewModel->value == 0.0f )
		return;

	if( CL_IsThirdPerson())
		return;

	cl_entity_t *ent = gEngfuncs.GetViewModel();

	if( ent == NULL || ent->model == NULL )
		return;

	// Dedicated projection: same fov as the main view, zNear=4 so the gun
	// body never crosses the near plane, zFar=4096 (a viewmodel lives within
	// arm's reach; the short range keeps depth precision high).
	ViewSetup vmView = mainView;

	vmView.zNear = 4.0f;
	vmView.zFar = 4096.0f;
	Mat4Perspective( vmView.fovX, vmView.fovY, vmView.zNear, vmView.zFar, vmView.matProj );
	Mat4Multiply( vmView.matProj, vmView.matView, vmView.matViewProj );
	FrustumFromMatrix( vmView.matViewProj, false, vmView.frustum );

	// Compressed depth range: every gun fragment lands in [0, 0.3] while the
	// world occupies [0, 1], so the gun wins the depth test against any wall
	// the player hugs instead of poking through it (notes-mechanisms c; same
	// trick as the stock path). Depth TEST stays on.
	SetDepthRange( 0.0f, 0.3f );
	g_studio.DrawSingle( vmView, ent );
	SetDepthRange( 0.0f, 1.0f );
}

}
