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
#include "csz_studio_bones.h"	// SeqDesc / EstimateFrame (shared with the draw path)
#include "../core/csz_engine.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_view.h"

#include <string.h>

namespace csz
{

namespace
{

// Engine-owned hide switch, mirrored from the stock viewmodel path (pinned
// ref/gl/gl_studio.c R_DrawViewModel). Looked up once; the engine registers
// it at startup so a NULL result is permanent.
cvar_t *s_drawViewModel;
bool s_cvarQueried;

// csz_dev_viewmodel (dev tool): render an arbitrary studio model in the
// viewmodel slot, e.g. "models/v_frostblade.mdl". Lets headless sessions put
// any texture.ini-mapped weapon on screen without server-side give support
// (defect batch 3 #16 acceptance). Empty / "0" = off.
cvar_t *s_devViewmodel;
char s_devLoadedName[128];
model_t *s_devModel;

model_t *DevViewmodel()
{
	if( s_devViewmodel == NULL || s_devViewmodel->string == NULL )
		return NULL;

	const char *want = s_devViewmodel->string;

	if( want[0] == '\0' || !strcmp( want, "0" ))
	{
		s_devLoadedName[0] = '\0';
		s_devModel = NULL;
		return NULL;
	}

	if( strcmp( want, s_devLoadedName ) != 0 )
	{
		strncpy( s_devLoadedName, want, sizeof( s_devLoadedName ) - 1 );
		s_devLoadedName[sizeof( s_devLoadedName ) - 1] = '\0';

		int index = 0;

		s_devModel = gEngfuncs.CL_LoadModel( want, &index );

		if( s_devModel == NULL )
			CSZ_LogWarn( "studio", "csz_dev_viewmodel: cannot load '%s'", want );
		else
			CSZ_LogInfo( "studio", "csz_dev_viewmodel: substituting '%s'", want );
	}

	return s_devModel;
}

// ---------------------------------------------------------------------------
// Viewmodel studio-event dispatch (W1 fix, PT-02 + G-P8). After takeover the
// engine early-returns in R_RenderFrame, so R_RunViewmodelEvents never runs and
// the studio sequence's client events (5004 reload/clip sounds, 5001/5011/
// 5021/5031 muzzle flashes) never fire. render_interface_t has no event
// callback, so the takeover path must walk the event list itself and invoke the
// stock client-DLL handler HUD_StudioEvent (cl_dll/entity.cpp:342, exported via
// cdll_int.cpp:571). The frame/seqdesc math is REUSED from the draw path
// (csz_studio_bones EstimateFrame/SeqDesc) so the event window tracks the drawn
// pose exactly -- a second formula would drift.
//
// R4 guardrail: this is the ONLY source of these events on the engine path.
// Do NOT also emit reload sound in cs_wpn/event_*.cpp -- that double-fires and
// re-creates the 0/1 asymmetry this fix closes.
// ---------------------------------------------------------------------------

// monsterevent.h:EVENT_CLIENT (5000); not pulled in here to avoid a dlls/
// dependency from the renderer layer -- mirrored as a local constant (R6).
const int kEventClient = 5000;

// Half-open event window [low endpoint] state, tracking the entity ACTUALLY
// drawn (the dev-substitution path swaps in s_devEnt). When the drawn model or
// its sequence changes (weapon switch / new action), the window resets so the
// new sequence's start-of-clip events fire (R8). oldFrame == -1 means "freshly
// reset": the next window is (-1, newFrame], inclusive of the sequence start,
// which also guards the r_studio_lerping 0 window collapse (R9) -- with
// interpolation off newFrame can stall at the start frame, and a strictly
// (oldFrame, newFrame] window with oldFrame == newFrame would drop a start
// event; seeding below frame 0 keeps the first window non-empty.
struct ViewmodelEventState
{
	const model_t *model;
	int modelindex;
	int sequence;
	float oldFrame;
	bool valid;
};

ViewmodelEventState s_vmEvents;

void DispatchViewmodelEvents( cl_entity_t *ent, float time )
{
	if( ent == NULL || ent->model == NULL || IEngineStudio.Mod_Extradata == NULL )
		return;

	if( ent->model->type != mod_studio )
		return;

	studiohdr_t *hdr = (studiohdr_t *)IEngineStudio.Mod_Extradata( ent->model );

	if( hdr == NULL || hdr->numseq <= 0 )
		return;

	// Out-of-range sequence -> 0, the same clamp the draw path uses
	// (csz_studio_bones EvaluatePose; engine StudioSetupBones parity).
	int seq = ent->curstate.sequence;

	if( seq < 0 || seq >= hdr->numseq )
		seq = 0;

	// Reset the window when the drawn model/sequence changes (R8). Keyed off
	// the drawn entity so dev (s_devEnt, sequence forced to 0) stays in sync.
	if( !s_vmEvents.valid || s_vmEvents.model != ent->model ||
		s_vmEvents.modelindex != ent->curstate.modelindex ||
		s_vmEvents.sequence != seq )
	{
		s_vmEvents.valid = true;
		s_vmEvents.model = ent->model;
		s_vmEvents.modelindex = ent->curstate.modelindex;
		s_vmEvents.sequence = seq;
		s_vmEvents.oldFrame = -1.0f;	// (-1, newFrame] fires start-of-clip events (R9)
	}

	const mstudioseqdesc_t *pseqdesc = SeqDesc( hdr, seq );

	if( pseqdesc->numevents <= 0 )
	{
		// Still advance the cursor so a later event-bearing sequence on the
		// SAME seq index (reused model slot) starts from the right frame.
		s_vmEvents.oldFrame = EstimateFrame( pseqdesc, ent, time );
		return;
	}

	float oldFrame = s_vmEvents.oldFrame;
	float newFrame = EstimateFrame( pseqdesc, ent, time );

	const mstudioevent_t *pevent =
		(const mstudioevent_t *)((const byte *)hdr + pseqdesc->eventindex );

	for( int i = 0; i < pseqdesc->numevents; i++ )
	{
		// Client events only (>= EVENT_CLIENT). Server-side events (< 5000)
		// are the server's to run and must not fire client-side.
		if( pevent[i].event < kEventClient )
			continue;

		// G-P8 muzzle-flash/spark deferred: viewmodel attachments are not
		// computed in the takeover path yet; firing these would place the effect
		// at a stale attachment. Re-enable when a StudioCalcAttachments-equivalent
		// lands. (5001/5011/5021/5031 = muzzle flash, 5002 = spark; all read
		// entity->attachment[N] inside HUD_StudioEvent.) The reload SOUND event
		// 5004 has no attachment dependency and keeps firing (PT-02).
		if( pevent[i].event == 5001 || pevent[i].event == 5002 ||
			pevent[i].event == 5011 || pevent[i].event == 5021 ||
			pevent[i].event == 5031 )
			continue;

		// Half-open window (oldFrame, newFrame]: each event fires exactly once
		// as the playhead crosses its frame. On a fresh sequence oldFrame is
		// -1, so the window is inclusive of the start frame.
		if( pevent[i].frame > oldFrame && pevent[i].frame <= newFrame )
			HUD_StudioEvent( &pevent[i], ent );
	}

	s_vmEvents.oldFrame = newFrame;
}

}

void RegisterViewmodelDevCvars()
{
	if( s_devViewmodel == NULL )
		s_devViewmodel = gEngfuncs.pfnRegisterVariable( "csz_dev_viewmodel", "", FCVAR_CLIENTDLL );
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

	// Dev substitution (csz_dev_viewmodel): draw a stand-in model with the
	// real viewmodel's transform. Sequence/body/skin reset to 0 (foreign
	// indices would be meaningless; out-of-range is clamped anyway).
	model_t *devModel = DevViewmodel();
	static cl_entity_t s_devEnt;

	if( devModel != NULL )
	{
		s_devEnt = *ent;
		s_devEnt.model = devModel;
		s_devEnt.curstate.sequence = 0;
		s_devEnt.curstate.frame = 0.0f;
		s_devEnt.curstate.body = 0;
		s_devEnt.curstate.skin = 0;
		s_devEnt.latched.prevsequence = 0;
		ent = &s_devEnt;
	}

	// Studio client-event pass on the resolved (post-substitution) entity:
	// must run BEFORE the draw and on the SAME entity DrawSingle gets, so the
	// event window matches the drawn pose and the dev path keys correctly (W1,
	// PT-02 reload sound + G-P8 own muzzle flash).
	DispatchViewmodelEvents( ent, csz::ClientTime());

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
