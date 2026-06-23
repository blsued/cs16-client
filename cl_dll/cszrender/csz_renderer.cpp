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
#include "core/csz_glcaps.h"
#include "core/csz_glfuncs.h"
#include "core/csz_glstate.h"
#include "core/csz_log.h"
#include "core/csz_fatal.h"
#include "core/csz_view.h"
#include "fog/csz_fog.h"
#include "fog/csz_fog_volume.h"
#include "fog/csz_fog_godrays.h"
#include "geom/csz_sky.h"
#include "geom/csz_clouds.h"
#include "geom/csz_sky_compose.h"
#include "geom/csz_sprite.h"
#include "geom/csz_studio.h"
#include "geom/csz_studio_texture.h"
#include "geom/csz_viewmodel.h"
#include "geom/csz_world.h"
#include "lighting/csz_light_pass.h"
#include "lighting/csz_light_cone.h"
#include "lighting/csz_dust.h"
#include "lighting/csz_light_budget.h"
#include "lighting/csz_flashlight_state.h"
#include "lighting/csz_light_registry.h"
#include "lighting/csz_shadowmap.h"

namespace csz
{

// C3 entry points (defined in geom/csz_sunmoon.cpp; not in the frozen sky ABI):
// SunMoonRegisterCvars registers csz_moon/sun/* at HUD init; SunMoonDrawDebugFullscreen
// repaints the sun/moon over the finished frame when csz_sky_fullscreen != 0.
void SunMoonRegisterCvars();
void SunMoonDrawDebugFullscreen( const ViewSetup &view );

// C4 entry points (defined in geom/csz_stars.cpp; not in the frozen sky ABI):
// StarsRegisterCvars registers all csz_stars_* cvars at HUD init so they are
// available before the first rendered frame (no "Unknown command" on console).
// StarsShutdown deletes the star/Milky Way GL objects (generation-safe) at HUD
// shutdown while the owning context is still current.
void StarsRegisterCvars();
void StarsShutdown();

// MW-rework entry points (defined in geom/csz_panorama.cpp; PanoramaContribute is in
// the sky ABI header). PanoramaRegisterCvars registers csz_pano* at HUD init;
// PanoramaShutdown deletes the panorama GL objects (generation-safe) at HUD shutdown.
void PanoramaRegisterCvars();
void PanoramaShutdown();

Renderer g_renderer;	// zero-initialized (static storage duration)

namespace
{

bool s_takeoverLogged;

// The world model we last handed to g_world (identity for the
// Mod_ProcessUserData create=false teardown path).
model_t *s_worldModel;

// ---------------------------------------------------------------------------
// Per-pass CPU ms accounting (M2 plan 2.7; composition-root internal, not a
// public interface). Clock = gRenderAPI.pfnTime, the engine's QPC-backed
// high-resolution timer (same source as SampleFps). CPU side only: GPU work
// is asynchronous, so suspicious numbers must be re-checked with feature
// on/off A/B (plan 2.7 caveat). Slots without an owner pass yet (brush/decal/
// volume/delegate/triapi; sky until A3) simply accumulate zero.
// ---------------------------------------------------------------------------
enum PassTimer { kTmShadow, kTmSky, kTmWorld, kTmBrush, kTmDecal, kTmStudio,
                 kTmLights, kTmVolume, kTmTrans, kTmDelegate, kTmTriapi,
                 kTmViewmodel, kTmCount };

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
				"studio=%.2f lights=%.2f volume=%.2f trans=%.2f delegate=%.2f triapi=%.2f viewmodel=%.2f",
				s_passAccumMs[kTmShadow] * inv, s_passAccumMs[kTmSky] * inv,
				s_passAccumMs[kTmWorld] * inv, s_passAccumMs[kTmBrush] * inv,
				s_passAccumMs[kTmDecal] * inv, s_passAccumMs[kTmStudio] * inv,
				s_passAccumMs[kTmLights] * inv, s_passAccumMs[kTmVolume] * inv,
				s_passAccumMs[kTmTrans] * inv, s_passAccumMs[kTmDelegate] * inv,
				s_passAccumMs[kTmTriapi] * inv, s_passAccumMs[kTmViewmodel] * inv );

			// L0 observability (csz_perf_dump, default 0; registered in
			// csz_sky_compose.cpp). When armed, emit ONE parseable line per sample
			// window through the always-visible CSZ_LogInfo facade (the same console
			// path the csz_sky_debug hook uses, so the test harness captures it
			// regardless of the `developer` level). Values: gpu_frame_ms = the
			// whole-frame in-scene GL_TIME_ELAPSED span (SkyComposeLastGpuMs; -1.0
			// until the first ring result resolves); the per-pass numbers are the
			// SAME per-frame-averaged CPU ms the Dev "fps" line above prints (the
			// accumulators are reset together just below). Pure logging -> zero
			// render change. Reuses the single non-nestable compose GPU timer
			// (forced on via ComposeTimingActive when csz_perf_dump != 0).
			if( SkyComposePerfDumpEnabled())
				CSZ_LogInfo( "perf", "[csz_perf] gpu_frame_ms=%.3f shadow=%.3f sky=%.3f world=%.3f brush=%.3f decal=%.3f "
					"studio=%.3f lights=%.3f volume=%.3f trans=%.3f delegate=%.3f triapi=%.3f viewmodel=%.3f",
					SkyComposeLastGpuMs(),
					s_passAccumMs[kTmShadow] * inv, s_passAccumMs[kTmSky] * inv,
					s_passAccumMs[kTmWorld] * inv, s_passAccumMs[kTmBrush] * inv,
					s_passAccumMs[kTmDecal] * inv, s_passAccumMs[kTmStudio] * inv,
					s_passAccumMs[kTmLights] * inv, s_passAccumMs[kTmVolume] * inv,
					s_passAccumMs[kTmTrans] * inv, s_passAccumMs[kTmDelegate] * inv,
					s_passAccumMs[kTmTriapi] * inv, s_passAccumMs[kTmViewmodel] * inv );
		}

		for( int i = 0; i < kTmCount; i++ )
			s_passAccumMs[i] = 0.0;

		s_windowStart = now;
		s_frames = 0;
		s_worstMs = 0.0;
	}
}

}

namespace
{

// A2 hardening: cvar-gated runtime glGetError bisection across the sky sub-passes.
// Default OFF (csz_sky_glcheck 0); ships harmless like csz_sky_debug. When armed it
// drains + reports glGetError after each sky stage, so a night/sunrise/day sweep
// pinpoints WHICH pass (pre-sky/atmos-lut/sky-bg/stars/sunmoon/resolve) raised an
// error -- a "pre-sky" hit proves the error predates our sky passes (engine-side).
// Only armed in dev, so unthrottled logging is fine (a clean run prints nothing;
// a dirty one we want to see in full). Off = one cheap cvar read per frame.
bool SkyGlCheckArmed()
{
	static cvar_t *s_cv;
	static bool s_looked;

	if( !s_looked )
	{
		s_looked = true;
		s_cv = gEngfuncs.pfnGetCvarPointer( "csz_sky_glcheck" );
	}

	return s_cv != NULL && s_cv->value != 0.0f;
}

void SkyGlCheck( const char *tag )
{
	GLenum e;

	while( ( e = glGetError() ) != GL_NO_ERROR )
		CSZ_LogError( "skyglcheck", "GL error 0x%x after %s", (unsigned int)e, tag );
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

	RegisterSpriteCommands();	// csz_testsprite (T5)
	RegisterLightingCommands();	// csz_testspot + csz_testlight (T6)
	LightConeRegisterCvars();	// L6a: csz_flashlight_tp (default 1 = world-space visible beam) + _intensity
	DustRegisterCvars();		// L7: csz_dust (default 1 = gated airborne dust) + _count/_intensity/_size
	RegisterStudioTextureCvars();	// csz_dev_armskin (spec 4.3.1 layer 1 dev probe)
	RegisterViewmodelDevCvars();	// csz_dev_viewmodel (dev stand-in model)
	g_fog.RegisterDevCommands();	// csz_devfog/csz_devtint/csz_devmoon (A1; CSZ_DEV_TOOLS only)
	CszFogRegisterCvars();		// L0: csz_fog_server_mask (black-fog decouple seam; always, Release-safe)
	g_sky.RegisterDevCvars();	// csz_sky_phase (always) + csz_devsun (CSZ_DEV_TOOLS only)
	g_clouds.RegisterCvars();	// L3a: csz_clouds (default 1) + csz_cloud_cover + csz_clouds_dump
	SkyComposeRegisterCvars();	// csz_hdr/exposure/tonemap/encode/dither/hdr_timing (C1)
	AtmosRegisterCvars();		// csz_atmos/atmos_exposure/atmos_ms/atmos_timing (C2)
	SunMoonRegisterCvars();		// csz_moon/sun + gain/size/halo/aureole/debug (C3)
	StarsRegisterCvars();		// csz_stars/intensity/size/color_sat/twinkle/pano_twinkle_maglimit/diag (C4)
	PanoramaRegisterCvars();	// MW-rework: csz_pano/pano_intensity/pano_lon_offset (must follow StarsRegisterCvars: fetches the moon-wash cvar pointers it registers)
	FogVolumeRegisterCvars();	// fog M1 Step 3: csz_fog_quality/steps/halfres/march_intensity/march_g
	FogGodraysRegisterCvars();	// fog M1 Step 4: csz_fog_godrays/_intensity/_dev (sun/moon god rays)
	CszRegisterMoonShaftCvar();	// fog M1 L4: csz_moonshaft (default 1 = enhanced moon Tyndall air-glow; 0 = exact pre-L4)
	gEngfuncs.pfnRegisterVariable( "csz_sky_glcheck", "0", FCVAR_CLIENTDLL );	// A2: per-sky-pass glGetError bisection (dev, default off)
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
		LightConeShutdown();	// L6a: world beam program + VAO (generation-safe)
		DustShutdown();		// L7: dust program + stream VBO/VAO (generation-safe)
		FogVolumeShutdown();	// fog M1 Step 3: half-res FBO + march/upsample programs (generation-safe)
		FogGodraysShutdown();	// fog M1 Step 4: half-res occl/scatter FBOs + 3 programs (generation-safe)
		AtmosShutdown();	// atmosphere LUTs + programs + GPU timer (C2, generation-safe)
		StarsShutdown();	// star field twinkle VAOs/VBO/programs (C4, generation-safe)
		PanoramaShutdown();	// MW-rework panorama texture/VAO/program (generation-safe)
		g_clouds.Shutdown();	// L3a: cloud noise texture/VAO/program (generation-safe)
		SkyComposeShutdown();	// HDR FBO + resolve program + GPU timer (C1, generation-safe)
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
	view.pvs = UpdateFatPvs( view.origin );

	model_t *world = WorldModel();					// slot 6: world build + visible set
	s_worldModel = world;
	g_world.EnsureBuilt( world );
	g_world.BuildVisibleSet( view );

	g_studio.BeginFrame( ClientTime());				// slot 7: studio begin-frame
	StudioTexturePollDevCvars();					// slot 7: csz_dev_armskin change check

	view.ambience = g_fog.Current();				// slot 7.2: ambience snapshot (A1)
	// S3 (REWORK-SPEC §S3, finding 5): the §5.1 achromatic-gray ambient in-scatter is
	// RETIRED. PublishLighting now computes the full sky/moon-coupled fog (per-channel
	// extinction + HG + height + noise) as the SINGLE in-scatter owner, so the old flat
	// CszFogComputeAmbient(view.ambience.fogAmbient) fold is gone (it was the "single flat
	// gray wash" disease). fogAmbient/CszFogComputeAmbient remain defined but unconsumed.

	// Phase drives both the cloud-state scalars and the published night tint /
	// celestial light below, all BEFORE any pass uploads the snapshot (A3): the
	// world/studio base passes consume amb.tint + amb.moonlightDir/Color.
	float ph = g_sky.ComputePhase();

	// L3a: compute the OWNED cloud-state scalars (directTransmittance /
	// skyAmbientScale / shaftMask) AND the L3b moon dimmer (cloudDim) into the
	// ambience snapshot BEFORE PublishLighting. ORDERING (L3b): PublishLighting
	// reads amb.cloudDim (csz_sky.cpp: moonLit *= cloudDim), so the writer MUST run
	// first. UpdateScalars computes purely from cvars+phase and does NOT read any
	// field PublishLighting writes, so the swap is safe (disjoint read/write sets).
	// SINGLE OWNERSHIP -- L3a still only WRITES these; clouds off => every scalar is
	// the neutral 1.0 identity (c=cloudsOn*cover*night=0), so the approved look holds.
	g_clouds.UpdateScalars( view.ambience, ph );

	// Sky overwrites the snapshot with the phase-driven night tint + dominant
	// celestial light dir/color. PublishLighting consumes amb.cloudDim (set just
	// above) to dim the moon-direct term under cloud cover.
	g_sky.PublishLighting( view.ambience, ph );

	// L0 black-fog decouple seam (CONVENTIONS.md). SINGLE chokepoint: every fog
	// consumer -- world/studio/sprite analytic-fog uniforms (CszFogUniformVecs),
	// the sky fog band (csz_sky.cpp), the volumetric flashlight march
	// (csz_fog_volume.cpp) -- reads view.ambience.fogDensity, so one multiply here
	// scales them all, consistently, live each frame. serverFogMask defaults to 1.0
	// (csz_fog_server_mask "1", no server override) => density*1.0 is IEEE-exact
	// identity => the frame is pixel-for-pixel the pre-L0 output. Reserved hook for
	// a future server-authoritative blackout (CszFogSetServerMask, MsgFunc_Fog).
	view.ambience.fogDensity *= CszFogServerMask();

	// L1 fog-base correctness A/B switch (csz_fog_base, default 1 = corrected).
	// Same single chokepoint as serverFogMask. The Step 2 analytic base fog audited
	// correct on all five optical points, so 1 is the verbatim corrected path (no
	// change to the snapshot => IEEE-exact identity). 0 forces heightFalloff to 0 so
	// every consumer's shader takes the uniform-density branch (F = a*t) = the
	// pre-analytic look, isolating exactly what the exponential-height integral buys
	// (visible only when b>0). No wrong path is manufactured; radial distance and the
	// closed-form integral stay structurally correct in both states.
	if( !CszFogBaseCorrected() )
		view.ambience.heightFalloff = 0.0f;

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

	CollectRealFlashlights( view );					// slot 7.5: L6c -- feed the per-flashlight state
									// table from live player flashlights (local eye +
									// other players' EF_DIMLIGHT); dev csz_flashlight_test overrides
	FlashlightPublishToRegistry();					// slot 7.55: L6b -- mirror the decoupled
									// per-flashlight state table into g_lights
									// (real per-player flashlight feed wired at slot 7.5)
	g_lights.UpdateMatrices();					// slot 7.6: light matrix update
	LightBudgetCompute( view );					// slot 7.65: L6b -- rank visible beams,
									// assign budgetTier (full/cheap/cull) before any pass reads it

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

	// slot 10: HDR compositing foundation (C1). When csz_hdr 1 (default) this
	// binds the RGBA16F HDR scene target + clears it with the same clearColor
	// the engine would use, so the scene (sky 10.5 -> viewmodel 15) renders into
	// the HDR FBO and the identity resolve (before slot 16) reproduces the C0
	// baseline. When csz_hdr 0 it is a pure passthrough to ApplyMainViewport
	// (straight-to-backbuffer baseline). The resolve below also takes ownership
	// of clearing FBO 0 (the HDR path no longer clears it at slot 10).
	// C2 atmosphere: build/refresh the precomputed LUTs (transmittance +
	// multiple-scattering once per generation, sky-view when the sun moves)
	// BEFORE the HDR FBO is bound -- AtmosBuildLuts does its own FBO/TMU juggling
	// and leaves FBO 0 + TMUs resynced. No-op when csz_atmos 0.
	bool atmos = AtmosActive();
	bool glCheck = SkyGlCheckArmed();				// A2: per-sky-pass glGetError bisection (dev)
	if( glCheck )
		SkyGlCheck( "pre-sky (engine / prior passes)" );	// drain: a hit here predates our sky work
	if( atmos )
		AtmosBuildLuts();					// slot 10 (pre-BeginScene)
	if( glCheck && atmos )
		SkyGlCheck( "atmos LUT build" );

	SkyComposeBeginScene( rvp, clearColor );			// slot 10
	if( glCheck )
		SkyGlCheck( "compose begin-scene (HDR FBO bind/clear)" );

	BeginPass( kTmSky );						// slot 10.5: sky background
	// C2: physically-based atmosphere sky background (samples the sky-view LUT
	// per view ray) into the HDR FBO. Falls back to the legacy csz_sky monolithic
	// background (gradient + discs + stars) when csz_atmos 0 or the LUTs are not
	// ready -- that legacy path is also the C1-identity baseline. NOTE: with
	// csz_atmos 1 the legacy sun/moon discs + hash stars are not drawn (C3 owns
	// the physically-based bodies, C4 the stars; both still no-op). Set
	// csz_atmos 0 to restore the full legacy sky meanwhile.
	bool skyDrawn = false;
	if( atmos )
		skyDrawn = AtmosDrawSky( view );
	if( !skyDrawn )
	{
		g_sky.EnsureBuilt();
		g_sky.DrawSky( view );
	}
	// FIX (no-stars structural, GPT-5.5 Pro §2a, 2026-06-19): draw C4 stars on the
	// COMMON sky path -- whether or not the physically-based atmosphere dome drew --
	// so the star field no longer silently depends on atmos LUT success (the old code
	// only composited stars in the atmos-success branch). In the DEFAULT path
	// (csz_atmos 1, dome OK) only these C4 stars draw. In the legacy fallback
	// (csz_atmos 0 / LUTs not ready) g_sky.DrawSky also draws its own faint hash
	// stars; those live in a frozen file outside C4's edit surface, so a minor
	// double-up is accepted there (non-default path, both are night-gated).
	// MW-rework: sampled all-sky panorama backdrop (static deep-space; replaces the
	// deleted per-pixel procedural Milky Way). Drawn BEFORE the live twinkle stars so it
	// sits behind them and the moon; additive into the same HDR FBO, night-gated on the
	// same curve. Also emits the relocated moon outer sky-glow (MoonSkyGlow).
	PanoramaContribute( view );
	if( glCheck )
		SkyGlCheck( "panorama backdrop (MW-rework)" );
	StarsContribute( view );
	if( glCheck )
		SkyGlCheck( atmos ? "sky background (atmos dome + stars)" : "sky background (legacy fallback)" );
	// L3a: drifting night cloud dome. Drawn AFTER the panorama backdrop + live stars
	// (so the clouds alpha-over-occlude the Milky Way / stars) and BEFORE the moon
	// disc (which then renders crisply on top). csz_clouds 0 early-outs (A/B-off =
	// current sky exactly). Nit 1 (L3b): the unconditional g_clouds.EnsureBuilt()
	// was removed -- Contribute lazily calls EnsureCreated() AFTER its csz_clouds/
	// night early-outs, so csz_clouds=0 now does ZERO cloud GL (no program/noise/VAO
	// build). The sky-dome build (g_sky.EnsureBuilt above) is separate and untouched.
	g_clouds.Contribute( view );
	if( glCheck )
		SkyGlCheck( "night clouds (L3a)" );
	// C3 sun/moon bodies draw additively after EITHER sky background. The legacy
	// fallback FS retired its own discs (C3 owns the physically-based bodies), so
	// without this the sun/moon would VANISH whenever the atmosphere path is not
	// taken: csz_atmos 0, the LUTs not ready, or AtmosDrawSky() failing. The bodies
	// composite into whatever target the scene renders to (HDR FBO or backbuffer).
	SunMoonContribute( view );
	if( glCheck )
		SkyGlCheck( "sunmoon bodies (C3)" );
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

	BeginPass( kTmLights );
	RunLightPasses( view, m_frame.studio, m_frame.numStudio );	// slot 13: additive light passes (spot DIRECT: world+studio lit, world-space)
	// slot 13.4 (L6a): world-space visible flashlight beam VOLUME. After the spot
	// direct add (the lit pool + holder body), before the Step-3 first-person march.
	// Additive air in-scatter cone-mesh, visible from ANY camera angle, occluded
	// camera-side by the scene depth (soft fade). Gated by csz_flashlight_tp.
	LightConeRender( view );
	EndPass( kTmLights );

	// slot 13.5 (kTmVolume seam): fog M1 Step 3 half-res flashlight ray-march.
	// After all opaque depth + additive flashlight lighting, before transparent/
	// viewmodel, HDR FBO still bound. Reconstructs the view ray from the Step-1
	// depth texture, accumulates single-scatter from the shadowed spot, and ADDS
	// the in-scatter into the HDR buffer (never re-attenuates the scene -- the
	// Step-2 analytic base fog owns scene transmittance + maxOpacity). Gated behind
	// csz_fog_quality >= 1; a pure no-op without the HDR path / a shadowed spot.
	BeginPass( kTmVolume );
	FogVolumeRender( view );
	// fog M1 Step 4: additive sun/moon god rays (screen-space radial scattering),
	// AFTER the flashlight march, HDR FBO still bound. Three half-res stages add
	// linear radiance into the HDR buffer (never re-attenuates -- the Step-2 base
	// fog owns extinction). Gated behind csz_fog_godrays; a no-op without the HDR
	// path / an on-screen above-horizon body. Restores HDR FBO + main viewport +
	// blend + TMUs + depth before the transparent/viewmodel passes.
	FogGodraysRender( view );
	// slot 13.6 (L7): gated airborne dust. After the cone (13.4) + fog march/god rays
	// (13.5), before the transparent pass. A separate additive soft-particle pass into
	// the HDR FBO; motes materialise ONLY inside a flashlight cone or the moon Tyndall
	// shaft (gate in the CPU spawn/cull fill -> unlit motes never reach the VBO).
	DustRender( view );
	EndPass( kTmVolume );

	BeginPass( kTmTrans );
	DrawSprites( view, m_frame.sprites, m_frame.numSprites );	// slot 14: sprites (trans domain)
	g_world.DrawBrushTransparent( view, m_frame.brush, m_frame.numBrush );	// slot 14: transparent brush (trans domain, E1)
	EndPass( kTmTrans );

	BeginPass( kTmViewmodel );
	DrawViewModelPass( view );					// slot 15: viewmodel (last; own depth range)
	EndPass( kTmViewmodel );

	if( atmos )
		AtmosDrawDebugFullscreen( view );			// dev: csz_sky_fullscreen overlay -> physically-based atmosphere (C2)
	else
		g_sky.DrawDebugFullscreen( view );			// dev: csz_sky_fullscreen overlay (legacy sky proof/showcase)
	SunMoonDrawDebugFullscreen( view );				// dev: csz_sky_fullscreen overlay -> C3 sun/moon over the frame, EITHER sky path (capture aid)

	// HDR resolve (C1): when csz_hdr 1, bind backbuffer 0, clear it (the HDR
	// path no longer cleared FBO 0 at slot 10), run the fullscreen resolve chain
	// (exposure -> purkinje(id) -> tonemap(0=id) -> [OETF] -> [dither]) sampling
	// the HDR color via the safe sky-unit bind + texelFetch, and resync TMUs
	// before LeaveTakeover (which uses the engine GL_Bind wrappers). When
	// csz_hdr 0 this is a no-op (scene already on FBO 0).
	SkyComposeResolve( rvp, clearColor );
	if( glCheck )
		SkyGlCheck( "compose resolve (HDR -> backbuffer)" );

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
