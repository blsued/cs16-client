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
#include "csz_studio_bones.h"	// SeqDesc / EstimateFrame + SetupBones (shared with the draw path)
#include "csz_sprite.h"		// SpritePushMuzzleFlash / DrawMuzzleFlashes (G-P8 own muzzle billboard)
#include "csz_particle.h"	// ParticleEmitSparkEffect (G-P8 muzzle sparks)
#include "../core/csz_engine.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_view.h"
// Seam note: geom consuming lighting/ here follows the documented v5.1 precedent
// in geom/csz_world.cpp (third-person fog glow). The viewmodel needs the SAME
// registry the world studio pass reads -- to publish its muzzle dlight (csz_muzzleflash)
// and to light the gun from the flashlight cone + engine dlights (csz_vmlight).
#include "../lighting/csz_light_registry.h"

#include <stdlib.h>	// rand / atoi / RAND_MAX
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

// ---------------------------------------------------------------------------
// G-P8 own viewmodel effects (W1 follow-up). Each behind a cvar (default 1):
//   csz_vm_events   -- re-dispatch the muzzle/spark studio events 5001/5011/
//                      5021/5031/5002 (previously skipped: no attachments yet).
//   csz_muzzleflash -- the muzzle flash EFFECT itself: additive sprite billboard
//                      (csz_sprite transient pool) + a brief omni dlight into the
//                      light registry + a few sparks (csz_particle).
//   csz_vmlight     -- light the gun from the registry (flashlight cone + engine
//                      dlights/muzzle) so it brightens in a beam / by a muzzle pop.
// Under takeover the stock HUD_StudioEvent muzzle path (gEngfuncs.pEfxAPI->
// R_MuzzleFlash, an engine temp-entity sprite) is never drawn, so these events
// must produce their effect in OUR layers instead. Non-effect client events
// (5004 reload sound, ...) still go to HUD_StudioEvent unchanged.
// ---------------------------------------------------------------------------
cvar_t *s_vmEventsCvar = NULL;	// csz_vm_events
cvar_t *s_muzzleCvar = NULL;	// csz_muzzleflash
cvar_t *s_vmLightCvar = NULL;	// csz_vmlight

// Reserved registry key for the viewmodel muzzle dlight. One key -> one slot
// reused every shot, so sustained fire is a single barrel-tracking glow (not a
// pile of slots). Test lights use -1..-3; engine bands own 32..63.
const int kMuzzleDlightKey = -20;

// Muzzle flashes fired since the last throttled [CSZ:vm] log (reset on log).
int s_muzzleCount = 0;

// Fail-safe ON until the cvar registers (HUD init runs before any frame).
bool ViewmodelEventsEnabled() { return ( s_vmEventsCvar == NULL ) || ( s_vmEventsCvar->value != 0.0f ); }
bool MuzzleFlashEnabled()     { return ( s_muzzleCvar   == NULL ) || ( s_muzzleCvar->value   != 0.0f ); }
bool ViewmodelLightEnabled()  { return ( s_vmLightCvar  == NULL ) || ( s_vmLightCvar->value  != 0.0f ); }

// 5001->attachment[0], 5011->[1], 5021->[2], 5031->[3] (stock HUD_StudioEvent map).
int AttachIndexForEvent( int event )
{
	switch( event )
	{
	case 5011: return 1;
	case 5021: return 2;
	case 5031: return 3;
	default:   return 0;	// 5001
	}
}

// Emits the muzzle flash at ent->attachment[attachIdx]: additive sprite + a brief
// omni dlight + a few sparks. Gated by csz_muzzleflash.
void EmitMuzzleFlash( cl_entity_t *ent, int attachIdx, const char *options )
{
	if( !MuzzleFlashEnabled())
		return;

	const float *pos = ent->attachment[attachIdx];
	float now = csz::ClientTime();

	// 1. Additive sprite billboard. The event option encodes the muzzleflash set
	// (0/1/2 -> muzzleflash1/2/3.spr), exactly the selector stock R_MuzzleFlash
	// uses. Random roll + scale so repeats never look stamped. A missing sprite
	// is logged-once and simply produces no billboard (dlight + sparks still read).
	int type = ( options != NULL && options[0] != '\0' ) ? atoi( options ) : 0;
	const char *sprName;

	switch((( type % 3 ) + 3 ) % 3 )
	{
	case 1:  sprName = "sprites/muzzleflash2.spr"; break;
	case 2:  sprName = "sprites/muzzleflash3.spr"; break;
	default: sprName = "sprites/muzzleflash1.spr"; break;
	}

	float frac = (float)rand() / ( (float)RAND_MAX + 1.0f );
	float scale = 0.5f + frac * 0.5f;		// 0.5 .. 1.0 (TUNABLE)
	float roll = (float)( rand() % 360 );

	SpritePushMuzzleFlash( sprName, pos, scale, roll, 0.06f );

	// 2. Brief warm omni dlight (~50 ms). kLightPoint lands in the direct point
	// pass (BuildPointParams); intensity is premultiplied into color (LightDesc
	// contract). RunLightPasses lights the WORLD from next frame; the viewmodel
	// lit pass (DrawViewmodelLit) lights the gun THIS frame from the same slot.
	LightDesc d;

	memset( &d, 0, sizeof( d ));
	d.type = kLightPoint;
	d.origin[0] = pos[0]; d.origin[1] = pos[1]; d.origin[2] = pos[2];
	d.color[0] = 2.0f; d.color[1] = 1.7f; d.color[2] = 1.0f;	// warm, intensity premultiplied (TUNABLE)
	d.radius = 200.0f;
	d.die = now + 0.05f;
	d.castShadow = false;
	g_lights.AddOrUpdate( kMuzzleDlightKey, d );

	// 3. A few muzzle sparks/embers (optional flavor; same csz_muzzleflash gate).
	csz::ParticleEmitSparkEffect( pos, 3, -90, 90 );

	s_muzzleCount++;
}

// 5002 spark event (some weapons). Sparks at attachment[0]; same gate.
void EmitMuzzleSpark( cl_entity_t *ent )
{
	if( !MuzzleFlashEnabled())
		return;

	csz::ParticleEmitSparkEffect( ent->attachment[0], 6, -100, 100 );
}

// Routes one in-window client event: muzzle/spark -> our own effects (gated),
// everything else (5004 reload sound, ...) -> the stock HUD_StudioEvent.
void DispatchOneViewmodelEvent( const mstudioevent_t *ev, cl_entity_t *ent )
{
	switch( ev->event )
	{
	case 5001:
	case 5011:
	case 5021:
	case 5031:
		if( ViewmodelEventsEnabled())
			EmitMuzzleFlash( ent, AttachIndexForEvent( ev->event ), ev->options );
		return;

	case 5002:
		if( ViewmodelEventsEnabled())
			EmitMuzzleSpark( ent );
		return;

	default:
		HUD_StudioEvent( ev, ent );
		return;
	}
}

// #4: light the viewmodel from the light registry (flashlight cone + engine
// dlights + our muzzle pop). The world studio pass (RunLightPasses) skips the
// gun -- it is not in m_frame.studio[] -- so we run the same additive lit pass
// here over the single viewmodel entity, in the gun's compressed vmView/depth.
// Returns the number of lights applied (throttled-log observability).
int DrawViewmodelLit( const ViewSetup &vmView, cl_entity_t *ent )
{
	if( !ViewmodelLightEnabled())
		return 0;

	float now = csz::ClientTime();
	cl_entity_t *one[1] = { ent };
	int applied = 0;

	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );

		if( !light->used )
			continue;

		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;	// expires this frame (RunLightPasses skips it too)

		SpotLightParams params;

		if( light->desc.type == kLightPoint )
		{
			g_lights.BuildPointParams( *light, params );
		}
		else if( light->desc.type == kLightSpot )
		{
			g_lights.BuildSpotParams( *light, params );
			// The gun is not in the world shadow map; sampling it would self-shadow
			// the viewmodel with garbage. Force shadowless for the first-person gun.
			params.shadowTexSlot = 0;
		}
		else
		{
			continue;
		}

		g_studio.DrawLitAdditive( vmView, params, one, 1 );
		applied++;
	}

	return applied;
}

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

		// Half-open window (oldFrame, newFrame]: each event fires exactly once
		// as the playhead crosses its frame. On a fresh sequence oldFrame is
		// -1, so the window is inclusive of the start frame.
		if( pevent[i].frame > oldFrame && pevent[i].frame <= newFrame )
		{
			// G-P8 re-enabled: viewmodel attachments are now computed each frame
			// (DrawViewModelPass calls SetupBones before this dispatch), so the
			// muzzle/spark events 5001/5011/5021/5031/5002 fire into our OWN
			// effects (DispatchOneViewmodelEvent). 5004 reload sound etc. still
			// go to the stock HUD_StudioEvent (PT-02).
			DispatchOneViewmodelEvent( &pevent[i], ent );
		}
	}

	s_vmEvents.oldFrame = newFrame;
}

}

void RegisterViewmodelDevCvars()
{
	if( s_devViewmodel == NULL )
		s_devViewmodel = gEngfuncs.pfnRegisterVariable( "csz_dev_viewmodel", "", FCVAR_CLIENTDLL );

	// G-P8 own viewmodel effects (default 1 each). Registered at HUD init so they
	// exist before the first frame; the read sites fail-safe ON until then.
	if( s_vmEventsCvar == NULL )
		s_vmEventsCvar = gEngfuncs.pfnRegisterVariable( "csz_vm_events", "1", FCVAR_CLIENTDLL );
	if( s_muzzleCvar == NULL )
		s_muzzleCvar = gEngfuncs.pfnRegisterVariable( "csz_muzzleflash", "1", FCVAR_CLIENTDLL );
	if( s_vmLightCvar == NULL )
		s_vmLightCvar = gEngfuncs.pfnRegisterVariable( "csz_vmlight", "1", FCVAR_CLIENTDLL );
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

	// MUST 1 (csz_attach, viewmodel): compute THIS frame's world attachment points
	// BEFORE dispatching studio events, so the muzzle events 5001/5011/5021/5031
	// read a FRESH ent->attachment[0..3] (the barrel right now) instead of last
	// frame's pose -- or a zero on the first frame a weapon is shown. SetupBones
	// caches by (ent,hdr); the DrawSingle below reuses this exact setup, and
	// BuildWorldBones -> StudioCalcAttachments writes the attachments as a side
	// effect (gated by csz_attach). No double bone cost.
	if( ent->model->type == mod_studio && IEngineStudio.Mod_Extradata != NULL )
	{
		studiohdr_t *vmHdr = (studiohdr_t *)IEngineStudio.Mod_Extradata( ent->model );

		if( vmHdr != NULL )
		{
			const BoneSetup *vmBones = NULL;
			SetupBones( ent, vmHdr, csz::ClientTime(), &vmBones );
		}
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

	// #4 (csz_vmlight): additive registry light onto the gun (flashlight cone +
	// engine dlights + our muzzle pop), in the SAME compressed vmView/depth so the
	// lit fragments land equal-depth (LEQUAL) on the opaque gun just drawn.
	int litCount = DrawViewmodelLit( vmView, ent );

	// #3 (csz_muzzleflash sprite): additive muzzle billboards, composited at the
	// barrel within the gun's depth band (so the gun never occludes the flash).
	DrawMuzzleFlashes( vmView );

	SetDepthRange( 0.0f, 1.0f );

	// Throttled dev log (R8): muzzle flashes fired + registry lights applied to
	// the gun this frame. [CSZ:vm] muzzle=N litpass=N.
	{
		static float s_nextLog;
		float now = csz::ClientTime();

		if( now >= s_nextLog )
		{
			s_nextLog = now + 1.0f;
			CSZ_LogDev( "viewmodel", "[CSZ:vm] muzzle=%d litpass=%d", s_muzzleCount, litCount );
			s_muzzleCount = 0;
		}
	}
}

}
