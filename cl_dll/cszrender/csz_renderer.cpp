/*
 * csz_renderer.cpp -- CSOZ renderer: frame orchestration (composition root)
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
#include "csz_renderer.h"
#include "core/csz_engine_bsp.h"	// EngBsp/EngModel world AABB (camgate bounds test)
#include "core/csz_glcaps.h"
#include "core/csz_glfuncs.h"
#include "core/csz_glstate.h"
#include "core/csz_log.h"
#include "core/csz_fatal.h"
#include "core/csz_view.h"
#include "fog/csz_fog.h"
#include "geom/csz_sky.h"
#include "geom/csz_sprite.h"
#include "geom/csz_studio.h"
#include "geom/csz_studio_texture.h"
#include "geom/csz_viewmodel.h"
#include "geom/csz_water.h"
#include "geom/csz_world.h"
#include "weather/csz_weather.h"
#include "lighting/csz_light_pass.h"
#include "lighting/csz_light_registry.h"
#include "lighting/csz_shadowmap.h"

#include <stdio.h>	// sscanf (csz_debugcam_pos/_ang parse)
#include <math.h>	// sqrtf/atan2f/cosf (camgate auto-frame aim)

namespace csz
{

Renderer g_renderer;	// zero-initialized (static storage duration)

namespace
{

bool s_takeoverLogged;

// The world model we last handed to g_world (identity for the
// Mod_ProcessUserData create=false teardown path).
model_t *s_worldModel;

// --- camera-legitimacy gate helpers (csz_debugcam) -------------------------
// degrees -> radians (kDegToRad lives in csz_math.cpp's TU, not the header).
const float kCamDegToRad = 3.14159265358979323846f / 180.0f;

// Map an engine CONTENTS_* int to a short readable token for the camgate log.
const char *ContentsName( int c )
{
	switch( c )
	{
	case CONTENTS_EMPTY:       return "EMPTY";
	case CONTENTS_SOLID:       return "SOLID";
	case CONTENTS_WATER:       return "WATER";
	case CONTENTS_SLIME:       return "SLIME";
	case CONTENTS_LAVA:        return "LAVA";
	case CONTENTS_SKY:         return "SKY";
	case CONTENTS_TRANSLUCENT: return "TRANSLUCENT";
	default:                   return "OTHER";
	}
}

// Sample engine contents at a world point (same API as csz_sky.cpp). Returns
// CONTENTS_EMPTY when PM_PointContents is unavailable (parity with sky).
int EyeContents( const float pos[3] )
{
	return ( gEngfuncs.PM_PointContents != NULL )
		? gEngfuncs.PM_PointContents( (float *)pos, NULL ) : CONTENTS_EMPTY;
}

// Point inside [mins,maxs] expanded by eps (world-bounds membership test).
bool PointInBox( const float p[3], const float mins[3], const float maxs[3], float eps )
{
	return p[0] >= mins[0] - eps && p[0] <= maxs[0] + eps
		&& p[1] >= mins[1] - eps && p[1] <= maxs[1] + eps
		&& p[2] >= mins[2] - eps && p[2] <= maxs[2] + eps;
}

// A pose is "legit" when the eye is not embedded in SOLID/SKY and lies inside
// the world AABB (the V5 over-the-edge skybox shots failed exactly this).
bool ContentsLegit( int contents )
{
	return contents != CONTENTS_SOLID && contents != CONTENTS_SKY;
}

// Build [pitch yaw roll] (engine view-angle order) so that forward points from
// eye toward target. Quake yaw is atan2(dy,dx); pitch is positive looking DOWN
// (engine convention -> negate the vertical component).
void AimAnglesToward( const float eye[3], const float target[3], float anglesOut[3] )
{
	float dx = target[0] - eye[0];
	float dy = target[1] - eye[1];
	float dz = target[2] - eye[2];
	float horiz = sqrtf( dx * dx + dy * dy );

	float yaw = atan2f( dy, dx ) / kCamDegToRad;
	// Engine pitch is positive downward; looking down at water from above => dz<0
	// => positive pitch.
	float pitch = -atan2f( dz, horiz ) / kCamDegToRad;

	anglesOut[0] = pitch;
	anglesOut[1] = yaw;
	anglesOut[2] = 0.0f;
}

// csz_debugcam 2: pick the first candidate eye that (a) sits in EMPTY/WATER
// (not SOLID/SKY) and (b), aiming at the water centroid, has the centroid in
// front within the view half-FOV. Candidates orbit the water-surface center
// (water AABB top). Returns true + fills origin/angles; logs the verdict.
bool AutoFrameWater( ViewSetup &view )
{
	float wc[3], wmin[3], wmax[3];
	bool hasWater = false;
	g_water.GetWaterBounds( wc, wmin, wmax, hasWater );

	if( !hasWater )
	{
		CSZ_LogInfo( "camgate", "auto-frame: NO WATER IN MAP" );
		return false;
	}

	// Surface center = water-body XY centroid at the AABB top (maxs.z).
	const float surf[3] = { wc[0], wc[1], wmax[2] };
	// Aim target = water-body centroid (mid-Z), so the surface fills the shot.
	const float target[3] = { wc[0], wc[1], ( wmin[2] + wmax[2] ) * 0.5f };

	// Ordered candidate eye offsets from the surface center (overhead first,
	// then progressively oblique orbits). ~12 candidates.
	static const float kOff[][3] = {
		{    0.0f,    0.0f,   96.0f },
		{    0.0f,    0.0f,  160.0f },
		{    0.0f,    0.0f,   64.0f },
		{  250.0f,    0.0f,  120.0f },
		{ -250.0f,    0.0f,  120.0f },
		{    0.0f,  250.0f,  120.0f },
		{    0.0f, -250.0f,  120.0f },
		{  250.0f,  250.0f,  140.0f },
		{ -250.0f, -250.0f,  140.0f },
		{  450.0f,    0.0f,  200.0f },
		{    0.0f,  450.0f,  200.0f },
		{    0.0f,    0.0f,  280.0f },
	};
	const int kNumCand = (int)( sizeof( kOff ) / sizeof( kOff[0] ));

	// Half the wider FOV (with a small margin) as the in-front cone threshold.
	float halfFov = 0.5f * ( ( view.fovX > view.fovY ) ? view.fovX : view.fovY );
	if( halfFov < 1.0f || halfFov > 89.0f ) halfFov = 45.0f;	// guard bad/zero FOV
	float cosHalf = cosf( ( halfFov - 3.0f ) * kCamDegToRad );	// 3deg margin in

	int tried = 0;
	for( int i = 0; i < kNumCand; i++ )
	{
		float eye[3] = { surf[0] + kOff[i][0], surf[1] + kOff[i][1], surf[2] + kOff[i][2] };
		tried = i + 1;

		int c = EyeContents( eye );
		if( c != CONTENTS_EMPTY && c != CONTENTS_WATER )
			continue;	// embedded in solid/sky or other -> reject

		float ang[3];
		AimAnglesToward( eye, target, ang );

		// Verify the target projects inside the frustum cone: forward . dir.
		float fwd[3], right[3], up[3];
		AngleVectors( ang, fwd, right, up );
		float dir[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
		float len = sqrtf( dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2] );
		if( len <= 0.0f )
			continue;
		float inv = 1.0f / len;
		float dot = fwd[0] * dir[0] * inv + fwd[1] * dir[1] * inv + fwd[2] * dir[2] * inv;

		bool inFront = ( dot > cosHalf ) && ( len < view.zFar );
		if( !inFront )
			continue;

		// Accept this candidate.
		view.origin[0] = eye[0]; view.origin[1] = eye[1]; view.origin[2] = eye[2];
		view.angles[0] = ang[0]; view.angles[1] = ang[1]; view.angles[2] = ang[2];
		CSZ_LogInfo( "camgate", "auto-frame chosen pos=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) from %d candidates (tried=%d)",
			eye[0], eye[1], eye[2], ang[0], ang[1], ang[2], kNumCand, tried );
		return true;
	}

	CSZ_LogInfo( "camgate", "auto-frame FAILED all %d candidates", kNumCand );
	return false;
}

// ---------------------------------------------------------------------------
// Per-pass CPU ms accounting (M2 plan 2.7; composition-root internal, not a
// public interface). Clock = gRenderAPI.pfnTime, the engine's QPC-backed
// high-resolution timer (same source as SampleFps). CPU side only: GPU work
// is asynchronous, so suspicious numbers must be re-checked with feature
// on/off A/B (plan 2.7 caveat). Slots without an owner pass yet (brush/decal/
// volume/delegate/triapi; sky until A3) simply accumulate zero.
// ---------------------------------------------------------------------------
enum PassTimer { kTmShadow, kTmSky, kTmWorld, kTmBrush, kTmDecal, kTmStudio,
                 kTmWater, kTmLights, kTmVolume, kTmTrans, kTmWeather,
                 kTmDelegate, kTmTriapi, kTmViewmodel, kTmCount };

double s_passAccumMs[kTmCount];
double s_passBeginTime[kTmCount];

void BeginPass( PassTimer t )
{
	if( gRenderAPI.pfnTime != NULL )
		s_passBeginTime[t] = gRenderAPI.pfnTime();
}

void EndPass( PassTimer t )
{
	if( gRenderAPI.pfnTime != NULL )
		s_passAccumMs[t] += ( gRenderAPI.pfnTime() - s_passBeginTime[t] ) * 1000.0;
}

// ---------------------------------------------------------------------------
// Standard-load fps sampling (plan amendment 2 / spec 8.6): aggregates the
// per-frame path into one Dev-level line per second (throttling rule R8).
// Counts every GL_RenderFrame call, so it measures both the takeover path
// and the engine path (csz_renderer 0) for same-condition A/B numbers.
// ---------------------------------------------------------------------------
void SampleFps()
{
	static double s_lastFrame;
	static double s_windowStart;
	static int s_frames;
	static double s_worstMs;

	if( gRenderAPI.pfnTime == NULL )
		return;

	double now = gRenderAPI.pfnTime();

	if( s_lastFrame > 0.0 )
	{
		double frameMs = ( now - s_lastFrame ) * 1000.0;

		s_frames++;

		if( frameMs > s_worstMs )
			s_worstMs = frameMs;
	}

	s_lastFrame = now;

	if( s_windowStart <= 0.0 )
	{
		s_windowStart = now;
	}
	else if( now - s_windowStart >= 1.0 )
	{
		double span = now - s_windowStart;

		if( s_frames > 0 )
		{
			// Existing line format frozen (M1 recompute scripts parse it);
			// the pass breakdown is a SEPARATE appended line (plan 2.7).
			CSZ_LogDev( "fps", "fps=%.1f avg_ms=%.2f worst_ms=%.2f frames=%d",
				(double)s_frames / span, span * 1000.0 / (double)s_frames, s_worstMs, s_frames );

			double inv = 1.0 / (double)s_frames;

			CSZ_LogDev( "fps", "pass-ms avg: shadow=%.2f sky=%.2f world=%.2f brush=%.2f decal=%.2f "
				"studio=%.2f water=%.2f lights=%.2f volume=%.2f trans=%.2f weather=%.2f delegate=%.2f triapi=%.2f viewmodel=%.2f",
				s_passAccumMs[kTmShadow] * inv, s_passAccumMs[kTmSky] * inv,
				s_passAccumMs[kTmWorld] * inv, s_passAccumMs[kTmBrush] * inv,
				s_passAccumMs[kTmDecal] * inv, s_passAccumMs[kTmStudio] * inv,
				s_passAccumMs[kTmWater] * inv, s_passAccumMs[kTmLights] * inv,
				s_passAccumMs[kTmVolume] * inv, s_passAccumMs[kTmTrans] * inv,
				s_passAccumMs[kTmWeather] * inv, s_passAccumMs[kTmDelegate] * inv,
				s_passAccumMs[kTmTriapi] * inv, s_passAccumMs[kTmViewmodel] * inv );

			// Runtime A/B proof for the batched water draw: separate fields, never
			// conflated. draw_batches = batches actually issued last frame (0 when
			// csz_water 0 skips the pass, 1 when the single-texture liquid map
			// draws); turb_verts = triangle-list vertex count in the VBO.
			CSZ_LogDev( "fps", "water_draw: draw_batches=%d turb_verts=%d",
				g_water.DrawBatchesLastFrame(), g_water.TurbVerts() );
		}

		for( int i = 0; i < kTmCount; i++ )
			s_passAccumMs[i] = 0.0;

		s_windowStart = now;
		s_frames = 0;
		s_worstMs = 0.0;
	}
}

}

void FrameEntities::Clear()
{
	numStudio = 0;
	numSprites = 0;
	numBrush = 0;
}

bool Renderer::OnHandshake( render_api_t *api )
{
	// Audit and ref_gl verification already ran in csz_render_iface.cpp;
	// here we only latch the state. GL init stays lazy (first taken frame).
	if( api == NULL )
		return false;

	m_handshakeOk = true;
	return true;
}

void Renderer::OnHudInit()
{
	// Catch the engine-side silent fallback: if the engine never called
	// HUD_GetRenderInterface the game would keep running on the stock
	// renderer and takeover would silently never engage (notes-mechanisms f-1).
	if( !m_handshakeOk )
		CSZ_FatalInit( "core", "render interface handshake never ran (engine fell back to its own renderer)" );

	if( m_cvarEnable == NULL )
		m_cvarEnable = gEngfuncs.pfnRegisterVariable( "csz_renderer", "1", FCVAR_CLIENTDLL );

	// csz_water (default 1): toggles the custom turb/water pass. 0 fully skips
	// DrawWater so the underlying surface / engine water shows -- the OFF baseline
	// for A/B capture. Same FCVAR_CLIENTDLL convention as the other csz cvars.
	if( m_cvarWater == NULL )
		m_cvarWater = gEngfuncs.pfnRegisterVariable( "csz_water", "1", FCVAR_CLIENTDLL );

	// Renderer-side debug camera (capture-rig aim): setpos/setang are
	// "Unknown command" in this build, so an external cfg drives the view by
	// setting these cvars. RenderFrame overrides view.origin/angles when
	// csz_debugcam != 0. Same double-register guard / FCVAR_CLIENTDLL convention.
	if( m_cvarDebugCam == NULL )
		m_cvarDebugCam = gEngfuncs.pfnRegisterVariable( "csz_debugcam", "0", FCVAR_CLIENTDLL );
	if( m_cvarDebugCamPos == NULL )
		m_cvarDebugCamPos = gEngfuncs.pfnRegisterVariable( "csz_debugcam_pos", "0 0 0", FCVAR_CLIENTDLL );
	if( m_cvarDebugCamAng == NULL )
		m_cvarDebugCamAng = gEngfuncs.pfnRegisterVariable( "csz_debugcam_ang", "0 0 0", FCVAR_CLIENTDLL );

	// Camera-gate state defaults (only meaningful once a debugcam pose is set).
	m_camGateLegit = false;
	m_camGateContents = CONTENTS_EMPTY;

	RegisterSpriteCommands();	// csz_testsprite (T5)
	RegisterLightingCommands();	// csz_testspot + csz_testlight (T6)
	RegisterStudioTextureCvars();	// csz_dev_armskin (spec 4.3.1 layer 1 dev probe)
	RegisterViewmodelDevCvars();	// csz_dev_viewmodel (dev stand-in model)
	g_fog.RegisterDevCommands();	// csz_devfog/csz_devtint/csz_devmoon (A1; CSZ_DEV_TOOLS only)
	g_sky.RegisterDevCvars();	// csz_sky_phase (always) + csz_devsun (CSZ_DEV_TOOLS only)
	g_weather.RegisterCvars();	// csz_weather/_intensity/_quality + csz_devweather (CSZ_DEV_TOOLS only)
}

void Renderer::OnVidInit()
{
	// GPU objects may have died with the GL context; GL-object owners key
	// their caches off the core generation counter and rebuild lazily,
	// FORGETTING stale names instead of deleting them (T1 finding).
	BumpGpuGeneration();
	m_gpuGeneration = GpuGeneration();
	m_glReady = false;

	CSZ_LogDev( "core", "vid init: GPU generation now %d", m_gpuGeneration );
}

void Renderer::Shutdown()
{
	// GL context is still current during HUD_Shutdown; destroy our objects.
	if( m_glReady )
	{
		g_world.Destroy();
		g_studio.DestroyAll();
		g_spotShadow.Destroy();
		g_water.Shutdown();
		g_weather.Shutdown();
		m_glReady = false;
	}

	s_worldModel = NULL;
	CSZ_LogDev( "core", "shutdown" );
}

int Renderer::RenderFrame( const ref_viewpass_t *rvp )
{
	SampleFps();

	// Orchestration contract (plan section 2.2, fixed order). T1 fills
	// slots 1/2/3/4/8/10/16; the remaining slots are marked below and are
	// filled by T2-T7 (placeholder comments, not TODOs: each lands inside
	// its own task).
	if( m_cvarEnable != NULL && m_cvarEnable->value == 0.0f )	// slot 1: dev escape hatch
		return 0;

	// slot 2: menu model preview (flags=0) / cubemap / overview passes stay
	// on the engine path (UNKNOWN-3 strategy: explicit return 0).
	if( rvp == NULL || !( rvp->flags & RF_DRAW_WORLD ))
		return 0;

	if( WorldModel() == NULL || !m_handshakeOk )			// slot 3
		return 0;

	if( !EnsureGlReady() )						// slot 4 (FATAL inside on hard failure)
		return 0;

	ViewSetup view;							// slot 5: view + fat PVS
	BuildViewFromPass( rvp, view );

	// Debug-camera override (capture rig): no setpos/setang in this build, so an
	// external cfg aims the view via csz_debugcam_pos/_ang. Done BEFORE
	// UpdateFatPvs so the fat PVS is computed from the overridden origin (else the
	// world around the debug viewpoint gets culled away). angles are
	// [pitch yaw roll], matching rvp->viewangles -> view.angles (BuildViewFromPass
	// copies them straight through, then Mat4ViewQuake consumes that order).
	if( m_cvarDebugCam != NULL && m_cvarDebugCam->value != 0.0f )
	{
		const bool autoFrame = ( m_cvarDebugCam->value == 2.0f );
		bool poseChanged = false;	// did we actually move the view this pass?
		bool autoFrameOk = true;	// csz_debugcam 2 self-correct verdict

		if( autoFrame )
		{
			// csz_debugcam 2 = auto-frame the map water. Build water now (idempotent
			// when already built) so GetWaterBounds() is populated, then pick a
			// legitimate eye that frames the surface. Ignore the manual pos/ang.
			g_water.EnsureBuilt( WorldModel() );
			autoFrameOk = AutoFrameWater( view );
			poseChanged = autoFrameOk;	// only moved if a candidate was accepted
		}
		else
		{
			float px = 0.0f, py = 0.0f, pz = 0.0f;
			float ax = 0.0f, ay = 0.0f, az = 0.0f;
			bool posOk = m_cvarDebugCamPos != NULL && m_cvarDebugCamPos->string != NULL
				&& sscanf( m_cvarDebugCamPos->string, "%f %f %f", &px, &py, &pz ) == 3;
			bool angOk = m_cvarDebugCamAng != NULL && m_cvarDebugCamAng->string != NULL
				&& sscanf( m_cvarDebugCamAng->string, "%f %f %f", &ax, &ay, &az ) == 3;

			if( posOk )
			{
				view.origin[0] = px; view.origin[1] = py; view.origin[2] = pz;
			}
			if( angOk )
			{
				view.angles[0] = ax; view.angles[1] = ay; view.angles[2] = az;
			}
			// Parse failure = ignore that override (keep the engine's value).
			poseChanged = ( posOk || angOk );
		}

		if( poseChanged )
		{
			// Recompute matrices/frustum from the overridden view (BuildViewFromPass
			// already built them from the engine origin/angles).
			Mat4ViewQuake( view.origin, view.angles, view.matView );
			Mat4Multiply( view.matProj, view.matView, view.matViewProj );
			FrustumFromMatrix( view.matViewProj, false, view.frustum );
		}

		// --- camera-legitimacy gate ---------------------------------------------
		// Validate the FINAL eye (whether manual override or auto-framed): sample
		// the engine contents at the eye and test against the world AABB. This is
		// the guard the V5 over-the-edge skybox shots lacked (no PM_PointContents,
		// no bounds). legit = not SOLID/SKY at the eye AND inside the world bounds.
		int contents = EyeContents( view.origin );
		const EngModel *bsp = EngBsp( WorldModel() );
		float wmin[3] = { bsp->mins[0], bsp->mins[1], bsp->mins[2] };
		float wmax[3] = { bsp->maxs[0], bsp->maxs[1], bsp->maxs[2] };
		bool insideWorld = PointInBox( view.origin, wmin, wmax, 64.0f );
		bool legit = ContentsLegit( contents ) && insideWorld && ( !autoFrame || autoFrameOk );

		m_camGateLegit = legit;
		m_camGateContents = contents;

		// Log ONCE PER CHANGE so the capture script can grep the active aim
		// without spamming the console every frame.
		static float s_lastPos[3] = { 0.0f, 0.0f, 0.0f };
		static float s_lastAng[3] = { 0.0f, 0.0f, 0.0f };
		static bool s_logged = false;

		if( !s_logged
			|| s_lastPos[0] != view.origin[0] || s_lastPos[1] != view.origin[1] || s_lastPos[2] != view.origin[2]
			|| s_lastAng[0] != view.angles[0] || s_lastAng[1] != view.angles[1] || s_lastAng[2] != view.angles[2] )
		{
			s_logged = true;
			s_lastPos[0] = view.origin[0]; s_lastPos[1] = view.origin[1]; s_lastPos[2] = view.origin[2];
			s_lastAng[0] = view.angles[0]; s_lastAng[1] = view.angles[1]; s_lastAng[2] = view.angles[2];
			CSZ_LogInfo( "debugcam", "active pos=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f)",
				view.origin[0], view.origin[1], view.origin[2],
				view.angles[0], view.angles[1], view.angles[2] );
			CSZ_LogInfo( "camgate", "pos=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) contents=%s inside_world=%d world_min=(%.0f %.0f %.0f) world_max=(%.0f %.0f %.0f) legit=%d",
				view.origin[0], view.origin[1], view.origin[2],
				view.angles[0], view.angles[1], view.angles[2],
				ContentsName( contents ), insideWorld ? 1 : 0,
				wmin[0], wmin[1], wmin[2], wmax[0], wmax[1], wmax[2],
				legit ? 1 : 0 );
		}
	}

	view.pvs = UpdateFatPvs( view.origin );

	model_t *world = WorldModel();					// slot 6: world build + visible set
	s_worldModel = world;
	g_world.EnsureBuilt( world );
	g_world.BuildVisibleSet( view );

	g_studio.BeginFrame( ClientTime());				// slot 7: studio begin-frame
	StudioTexturePollDevCvars();					// slot 7: csz_dev_armskin change check

	view.ambience = g_fog.Current();				// slot 7.2: ambience snapshot (A1)

	// Sky overwrites the snapshot with the phase-driven night tint + dominant
	// celestial light dir/color BEFORE any pass uploads it (A3): the world/
	// studio base passes consume amb.tint + amb.moonlightDir/Color.
	float ph = g_sky.ComputePhase();
	g_sky.PublishLighting( view.ambience, ph );

	// Disposable observability hook (csz_sky_debug, default 0; registered in
	// csz_sky.cpp RegisterDevCvars). When armed, dump the PUBLISHED per-phase
	// ambience -- the runtime ground truth the test agent reads -- once per
	// second (throttled off ClientTime, never per-frame). Cleanly gated, so it
	// ships harmlessly. Goes through the CSZ_Log facade (the only console path;
	// README/code-standards 2.6 R8), carrying the exact greppable token.
	{
		static cvar_t *s_skyDebug;

		if( s_skyDebug == NULL )
			s_skyDebug = gEngfuncs.pfnGetCvarPointer( "csz_sky_debug" );

		if( s_skyDebug != NULL && s_skyDebug->value != 0.0f )
		{
			static float s_nextSkyDebug;
			float now = ClientTime();

			if( now >= s_nextSkyDebug )
			{
				const AmbienceParams &a = view.ambience;

				s_nextSkyDebug = now + 1.0f;
				CSZ_LogInfo( "sky", "[csz_sky_debug] ph=%.3f tint=(%.3f,%.3f,%.3f) fog d=%.3f c=(%.3f,%.3f,%.3f)",
					ph, a.tint[0], a.tint[1], a.tint[2], a.fogDensity, a.fogColor[0], a.fogColor[1], a.fogColor[2] );
			}
		}
	}

	g_lights.UpdateMatrices();					// slot 7.6: light matrix update

	// slot 7.8: weather state update. Derives the WeatherSurfaceState contract
	// (wetness/snowAmount/snowColor) from the csz_weather* cvars + published
	// ambience tint BEFORE takeover; the world/turb shaders read it this frame.
	g_weather.Update( view, ph, ClientTime() );
	view.weather = g_weather.SurfaceState();			// geom reads wet/snow from the view (one-way layering)

	EnterTakeover();						// slot 8

	BeginPass( kTmShadow );
	RenderShadowMaps( view, m_frame.studio, m_frame.numStudio );	// slot 9: shadow maps (before main clear)
	EndPass( kTmShadow );

	// Slot 10 clear: with fog on, the clear color IS the fog color -- the sky
	// region is still an M1 gap, and a fog-colored backdrop is the correct
	// intermediate state until A3 lands the sky pass. Without fog keep the
	// dark gray-blue "no content here" tell (anything left this color is a
	// known gap or a regression; magenta retired with T1).
	static const float kClearNoContent[4] = { 0.05f, 0.05f, 0.08f, 1.0f };
	const float *clearColor = kClearNoContent;
	float fogClear[4];

	if( view.ambience.fogDensity > 0.0f )
	{
		fogClear[0] = view.ambience.fogColor[0];
		fogClear[1] = view.ambience.fogColor[1];
		fogClear[2] = view.ambience.fogColor[2];
		fogClear[3] = 1.0f;
		clearColor = fogClear;
	}

	ApplyMainViewport( rvp, clearColor );				// slot 10

	BeginPass( kTmSky );						// slot 10.5: procedural day/night sky (A3)
	g_sky.EnsureBuilt();
	g_sky.DrawSky( view );
	EndPass( kTmSky );

	BeginPass( kTmWorld );
	g_world.DrawOpaque( view );					// slot 11: world opaque
	EndPass( kTmWorld );

	BeginPass( kTmBrush );
	g_world.DrawBrushOpaque( view, m_frame.brush, m_frame.numBrush );	// slot 11.5: opaque brush submodels (E1)
	EndPass( kTmBrush );

	BeginPass( kTmStudio );
	g_studio.DrawOpaque( view, m_frame.studio, m_frame.numStudio );	// slot 12: studio opaque
	EndPass( kTmStudio );

	BeginPass( kTmWater );						// slot 12.5: animated turb/water surface (B1)
	g_water.EnsureBuilt( world );					// same world model g_world.EnsureBuilt used (~252)
	if( m_cvarWater == NULL || m_cvarWater->value != 0.0f )		// csz_water 0 = skip custom water (OFF baseline)
		g_water.DrawWater( view, g_weather.RainIntensity(), ph );
	else
		g_water.MarkNotDrawn();					// OFF baseline: water pass skipped -> draw_batches=0
	EndPass( kTmWater );

	BeginPass( kTmLights );
	RunLightPasses( view, m_frame.studio, m_frame.numStudio );	// slot 13: additive light passes
	EndPass( kTmLights );

	BeginPass( kTmTrans );
	DrawSprites( view, m_frame.sprites, m_frame.numSprites );	// slot 14: sprites (trans domain)
	g_world.DrawBrushTransparent( view, m_frame.brush, m_frame.numBrush );	// slot 14: transparent brush (trans domain, E1)
	g_weather.DrawPrecip( view );					// slot 14.5: precipitation particles (trans domain, B2)
	EndPass( kTmTrans );

	BeginPass( kTmViewmodel );
	DrawViewModelPass( view );					// slot 15: viewmodel (last; own depth range)
	EndPass( kTmViewmodel );

	g_sky.DrawDebugFullscreen( view );				// dev: csz_sky_fullscreen overlay (proof/showcase)

	LeaveTakeover();						// slot 16

	if( !s_takeoverLogged )
	{
		s_takeoverLogged = true;
		CSZ_LogInfo( "core", "frame takeover active (first taken-over frame)" );

		// One-shot sanity probe; per-frame error polling stays out of the
		// hot path (Dev-level + throttled rule).
		GLenum err = glGetError();

		if( err != GL_NO_ERROR )
			CSZ_LogError( "core", "GL error 0x%x after first taken-over frame", (unsigned int)err );
	}

	return 1;
}

void Renderer::ClearScene()
{
	m_frame.Clear();
	g_lights.DecayFrame( ClientTime());	// expire die>0 lights (plan 2.2)

	// Per-frame callback: log wiring exactly once (throttling rule, R8).
	static bool s_logged = false;

	if( !s_logged )
	{
		s_logged = true;
		CSZ_LogDev( "core", "R_ClearScene callback wired" );
	}
}

void Renderer::NewMap()
{
	// Old world data (and any cached cross-map PVS) is invalid now. The GL
	// context is alive at R_NewMap, so Destroy frees properly; the rebuild
	// happens eagerly when GL is ready, else lazily in frame slot 6.
	ResetFatPvs();
	g_fog.Reset();		// never carry one map's ambience into the next (A1)
	g_world.Destroy();
	s_worldModel = NULL;

	CSZ_LogDev( "core", "R_NewMap: world invalidated" );

	if( m_glReady )
	{
		model_t *world = WorldModel();

		if( world != NULL )
		{
			s_worldModel = world;
			g_world.EnsureBuilt( world );	// eager build (plan 2.2)
		}
	}
}

void Renderer::BuildLightmapsCallback()
{
	// Gamma or lightstyle config changed engine-side: re-upload our atlas.
	g_world.MarkLightmapsDirty();
	CSZ_LogDev( "core", "GL_BuildLightmaps: lightmaps marked dirty" );
}

byte *Renderer::GetCurrentVis()
{
	// Feeds engine tempent/beam culling while we own the frame
	// (notes-renderapi A table row 9).
	return (byte *)CurrentFatPvs();
}

void Renderer::ProcessUserData( model_t *mod, qboolean create, const byte *buffer )
{
	(void)buffer;

	// Tear down GPU caches when the engine unloads a model we built from.
	if( !create && mod != NULL )
	{
		if( mod == s_worldModel )
		{
			g_world.Destroy();
			s_worldModel = NULL;
			CSZ_LogDev( "core", "world model unloaded; world GPU data destroyed" );
		}
		else
		{
			g_studio.OnModelUnloaded( mod );	// no-op for non-cached models
		}
	}
}

void Renderer::AddEntity( int type, cl_entity_t *ent )
{
	// HUD_AddEntity hook: collect this frame's renderables by model type
	// (entity type is irrelevant for the M1 draw lists). ClearScene resets
	// the lists every frame (engine calls it first, notes-renderapi A 11).
	(void)type;

	if( ent == NULL || ent->model == NULL )
		return;

	if( ent->model->type == mod_studio )
	{
		// Engine parity (pinned engine cl_frame.c CL_AddVisibleEntity): in
		// firstperson the engine still dispatches the local player to
		// HUD_AddEntity "for use in custom renderers" but never hands it to
		// its own ref. Mirror that filter, or the camera sits inside its own
		// skinned head (T3 live finding). The engine's extra check is
		// "ent->index == cl.viewentity", which the client cannot read
		// (IEngineStudio.GetViewEntity returns the VIEWMODEL: pinned
		// cl_game.c wires it to CL_GetViewModel); M1 simplification: a
		// trigger_camera view would hide the local body, accepted gap.
		if( ent->player && ent == gEngfuncs.GetLocalPlayer() && !CL_IsThirdPerson())
			return;

		if( m_frame.numStudio < FrameEntities::kMaxEntities )
		{
			m_frame.studio[m_frame.numStudio++] = ent;
		}
		else
		{
			static float s_nextWarn;
			float now = ClientTime();

			if( now >= s_nextWarn )
			{
				s_nextWarn = now + 1.0f;
				CSZ_LogWarn( "core", "studio entity list full (%d); dropping entities", FrameEntities::kMaxEntities );
			}
		}
	}
	else if( ent->model->type == mod_sprite )
	{
		// Collected now, drawn from T5 on.
		if( m_frame.numSprites < FrameEntities::kMaxEntities )
			m_frame.sprites[m_frame.numSprites++] = ent;
	}
	else if( ent->model->type == mod_brush )
	{
		// Brush submodels (func_*, doors, rotating brushes). On the engine
		// path R_DrawBrushModel draws these; under takeover we self-supply
		// them from g_world's shared VBO (E1). No local-player filter: that
		// is studio-only (the camera-inside-own-head case), brushes are never
		// the view entity.
		if( m_frame.numBrush < FrameEntities::kMaxEntities )
		{
			m_frame.brush[m_frame.numBrush++] = ent;
		}
		else
		{
			static float s_nextWarn;
			float now = ClientTime();

			if( now >= s_nextWarn )
			{
				s_nextWarn = now + 1.0f;
				CSZ_LogWarn( "core", "brush entity list full (%d); dropping entities", FrameEntities::kMaxEntities );
			}
		}
	}
}

bool Renderer::EnsureGlReady()
{
	if( m_glReady )
		return true;

	if( !ProbeGlCaps() )	// loads all GL functions first; FATAL inside on any hard failure
		return false;

	// World/studio GPU resources (shaders included) build lazily inside
	// their owners, keyed by model + GpuGeneration().
	m_glReady = true;
	return true;
}

}
