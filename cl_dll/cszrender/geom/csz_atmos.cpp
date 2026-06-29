/*
 * csz_atmos.cpp -- CSOZ renderer: physically-based atmospheric scattering (C2)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). CLEAN-ROOM implementation of a
 * precomputed-LUT atmosphere (Bruneton 2008 transmittance + Hillaire 2020
 * multiple-scattering & sky-view LUTs), written FROM THE PAPERS' mathematics.
 * No code/shader source/data is copied or translated from their MIT/BSD
 * reference implementations, from Unreal's EULA tree, or from any other
 * license-tainted source (see csoz docs/provenance.md). Mechanism for the GL
 * takeover/TMU/generation discipline studied from PrimeXT (see csoz
 * docs/notes/primext-render-mechanisms-m2.md); implemented by an agent that has
 * not read that source.
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
// C2. Builds the transmittance / multiple-scattering / sky-view LUTs on the GPU
// (RGBA16F 2D textures via FBO render passes), publishes them through the frozen
// SkyAtmosResources ABI, and draws the physically-based sky background at slot
// 10.5. The transmittance + multiple-scattering LUTs are sun-independent and
// built ONCE per GpuGeneration(); the sky-view LUT is rebuilt only when the sun
// elevation changes. No per-frame heap allocation. All texture binds go through
// SkyComposeBindTex / SkyComposeRestoreTmus (never raw glActiveTexture), no
// glEnable(GL_TEXTURE_*) on a sky unit, and every GL name is keyed on
// GpuGeneration() (forget -- never glDelete -- on a foreign context).
//
// 3D-texture note: the loaded GL function table (csz_glfuncs.h) exposes no
// glTexImage3D / glFramebufferTextureLayer, so the Hillaire aerial-perspective
// FROXEL (a 3D volume) is intentionally NOT built here -- it is deferred. The
// sky-view LUT already provides full physically-based sky radiance per view ray
// for the C2 deliverable (sky color/gradient); aerial perspective only affects
// fogging of distant world geometry, which is out of C2's "sky color" scope.
#include "csz_sky_compose.h"
#include "csz_sky.h"          // g_sky.ComputePhase() (honors csz_sky_phase)
#include "csz_sky_math.h"     // skymath::SunDir
#include "csz_atmos_math.h"   // CPU coupling evaluator + shared constants
#include "../core/csz_engine.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_log.h"
#include "../core/csz_math.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"

#include <math.h>
#include <string.h>

namespace csz
{

#include "csz_atmos_shaders.inl"

namespace
{

// --- LUT dimensions -----------------------------------------------------------
const int kTransW = 256, kTransH = 64;   // transmittance (mu, altitude)
const int kMsW    = 32,  kMsH    = 32;   // multiple-scattering (muSun, altitude)
const int kSkyW   = 192, kSkyH   = 108;  // sky-view (azimuth, view-zenith)

// --- viewer altitude (km above sea level). GoldSrc maps are tiny vs the
//     atmosphere; a fixed small eye height keeps horizon rays well-conditioned. -
const float kViewHeightKm = 0.5f;

// --- cvars (registered in AtmosRegisterCvars, read live) ----------------------
cvar_t *s_cvarAtmos;     // csz_atmos          default "1": 1 = PB atmosphere sky background, 0 = legacy csz_sky
cvar_t *s_cvarExposure;  // csz_atmos_exposure default tuned: linear radiance -> display scale
cvar_t *s_cvarMs;        // csz_atmos_ms       default "1": multiple-scattering on/off
cvar_t *s_cvarTiming;    // csz_atmos_timing   default "0": log the LUT-build GPU ms (Dev)
cvar_t *s_cvarNavy;      // csz_sky_navy       default "1.0": night-navy-floor multiplier (regrade G2 fine-tune)

// --- GPU resources (generation-keyed; forget on foreign context) --------------
struct AtmosGpu
{
	GLuint texTrans;
	GLuint texMs;
	GLuint texSky;
	GLuint fbo;
	GLuint vao;

	ShaderProgram progTrans;
	ShaderProgram progMs;
	ShaderProgram progSky;     // sky-view LUT build
	ShaderProgram progDraw;    // full-screen sky background

	// sky-view build uniforms
	int uSky_trans, uSky_ms, uSky_sunCos, uSky_viewR, uSky_msScale;
	// multi-scatter build uniforms
	int uMs_trans;
	// sky background uniforms
	int uDraw_camFwd, uDraw_camRight, uDraw_camUp, uDraw_skyView, uDraw_sunDir, uDraw_viewR, uDraw_exposure, uDraw_skyNavy;

	int  gpuGeneration;
	bool created;         // textures/fbo/vao/programs exist for this generation
	bool staticBuilt;     // transmittance + MS LUTs built for this generation
	bool skyBuilt;        // sky-view LUT built (for s_lastSunCos / s_lastMsScale)
	bool failedThisGen;   // creation failed; do not retry, fall back to legacy sky
	bool published;       // resources published with ready=true

	float lastSunCos;     // sun zenith cosine the sky-view LUT was built for
	float lastMsScale;
};

AtmosGpu s_gpu;

float s_sunWorld[3] = { 0.0f, 0.0f, 1.0f };  // world sun dir (Z-up) the LUTs are built for, fresh per AtmosBuildLuts

// --- GPU timer ring for the LUT-build region (red-team fix #11 style) ---------
const int kQueryRing = 3;
struct AtmosTimer
{
	GLuint query[kQueryRing];
	bool   issued[kQueryRing];
	int    writeIndex;
	int    gpuGeneration;
	bool   created;
	bool   inSpan;
	double lastMs;
};
AtmosTimer s_timer;

cvar_t *s_cvarFullscreen;   // csz_sky_fullscreen (CSZ_DEV_TOOLS): repaint sky over the whole frame
bool s_fullscreenLookedUp;

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// Diagnostic: log a non-clean glGetError at a named step, only when
// csz_atmos_timing != 0 (off in normal play). Clears the error (probe semantics).
void DbgErr( const char *where )
{
	if( ReadCvar( s_cvarTiming, 0.0f ) == 0.0f )
		return;
	GLenum e = glGetError();
	if( e != GL_NO_ERROR )
		CSZ_LogDev( "atmos", "GL error 0x%x at %s", (unsigned int)e, where );
}

void ForgetGpu()
{
	memset( &s_gpu, 0, sizeof( s_gpu ) );
	s_gpu.lastSunCos = 2.0f;   // impossible cos -> forces a sky-view rebuild
	s_gpu.lastMsScale = -1.0f;
}

void DestroyGpuSameContext()
{
	if( s_gpu.texTrans ) glDeleteTextures( 1, &s_gpu.texTrans );
	if( s_gpu.texMs )    glDeleteTextures( 1, &s_gpu.texMs );
	if( s_gpu.texSky )   glDeleteTextures( 1, &s_gpu.texSky );
	if( s_gpu.fbo )      glDeleteFramebuffers( 1, &s_gpu.fbo );
	if( s_gpu.vao )      glDeleteVertexArrays( 1, &s_gpu.vao );
	if( s_gpu.progTrans.program ) DestroyProgram( s_gpu.progTrans );
	if( s_gpu.progMs.program )    DestroyProgram( s_gpu.progMs );
	if( s_gpu.progSky.program )   DestroyProgram( s_gpu.progSky );
	if( s_gpu.progDraw.program )  DestroyProgram( s_gpu.progDraw );
	ForgetGpu();
}

// Allocate one RGBA16F 2D LUT texture on a sky unit (filter LINEAR, given wrapS).
GLuint MakeLut( int w, int h, GLenum wrapS )
{
	GLuint t = 0;
	glGenTextures( 1, &t );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, t );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	SkyComposeRestoreTmus();
	return t;
}

// GL-ERR-2: live gate -- the timer-query capability latch AND csz_atmos_timing.
bool AtmosTimingActive()
{
	return HaveTimerQuery() && ReadCvar( s_cvarTiming, 0.0f ) != 0.0f;
}

void EnsureTimer()
{
	// GL-ERR-2: never touch timer-query objects without the capability latch.
	if( !HaveTimerQuery() )
	{
		s_timer.created = false;
		return;
	}

	if( s_timer.created && s_timer.gpuGeneration == GpuGeneration() )
		return;
	for( int i = 0; i < kQueryRing; i++ )
		s_timer.issued[i] = false;
	s_timer.gpuGeneration = GpuGeneration();
	s_timer.writeIndex = 0;
	s_timer.inSpan = false;
	s_timer.lastMs = -1.0;
	glGenQueries( kQueryRing, s_timer.query );
	s_timer.created = true;
}

// Build programs + textures + fbo + vao for the live generation. FATAL on shader
// compile failure (matches sky/world/studio init-time programs).
bool EnsureCreated()
{
	if( s_gpu.gpuGeneration != GpuGeneration() )
	{
		ForgetGpu();               // foreign generation: forget, never glDelete
		s_gpu.gpuGeneration = GpuGeneration();
	}
	if( s_gpu.created )
		return true;
	if( s_gpu.failedThisGen )
		return false;

	glGenVertexArrays( 1, &s_gpu.vao );

	BuildProgram( "csz_atmos_transmittance", kAtmosLutVs, kAtmosTransmittanceFs, true, s_gpu.progTrans );
	BuildProgram( "csz_atmos_multiscatter",  kAtmosLutVs, kAtmosMultiScatterFs,  true, s_gpu.progMs );
	BuildProgram( "csz_atmos_skyview",       kAtmosLutVs, kAtmosSkyViewFs,       true, s_gpu.progSky );
	BuildProgram( "csz_atmos_skydraw",       kAtmosSkyVs, kAtmosSkyFs,           true, s_gpu.progDraw );

	s_gpu.uMs_trans      = UniformLoc( s_gpu.progMs,  "u_trans" );
	s_gpu.uSky_trans     = UniformLoc( s_gpu.progSky, "u_trans" );
	s_gpu.uSky_ms        = UniformLoc( s_gpu.progSky, "u_ms" );
	s_gpu.uSky_sunCos    = UniformLoc( s_gpu.progSky, "u_sunCosZenith" );
	s_gpu.uSky_viewR     = UniformLoc( s_gpu.progSky, "u_viewR" );
	s_gpu.uSky_msScale   = UniformLoc( s_gpu.progSky, "u_msScale" );
	s_gpu.uDraw_camFwd   = UniformLoc( s_gpu.progDraw, "u_camFwd" );
	s_gpu.uDraw_camRight = UniformLoc( s_gpu.progDraw, "u_camRight" );
	s_gpu.uDraw_camUp    = UniformLoc( s_gpu.progDraw, "u_camUp" );
	s_gpu.uDraw_skyView  = UniformLoc( s_gpu.progDraw, "u_skyView" );
	s_gpu.uDraw_sunDir   = UniformLoc( s_gpu.progDraw, "u_sunDir" );
	s_gpu.uDraw_viewR    = UniformLoc( s_gpu.progDraw, "u_viewR" );
	s_gpu.uDraw_exposure = UniformLoc( s_gpu.progDraw, "u_exposure" );
	s_gpu.uDraw_skyNavy  = UniformLoc( s_gpu.progDraw, "u_skyNavy" );

	s_gpu.texTrans = MakeLut( kTransW, kTransH, GL_CLAMP_TO_EDGE );
	s_gpu.texMs    = MakeLut( kMsW,    kMsH,    GL_CLAMP_TO_EDGE );
	s_gpu.texSky   = MakeLut( kSkyW,   kSkyH,   GL_REPEAT );   // azimuth wraps

	glGenFramebuffers( 1, &s_gpu.fbo );

	s_gpu.created = true;
	s_gpu.staticBuilt = false;
	s_gpu.skyBuilt = false;
	s_gpu.published = false;
	s_gpu.lastSunCos = 2.0f;
	s_gpu.lastMsScale = -1.0f;
	CSZ_LogDev( "atmos", "atmosphere GL resources created (gpu gen %d)", s_gpu.gpuGeneration );
	return true;
}

// Bind a LUT texture as the FBO color attachment and run a full-screen pass into
// it. `prog` already in use; caller sets uniforms + binds input LUTs first.
// PIT-1 (fail-open fix): returns false if the FBO is not complete after the
// attach. Previously RenderLut attached + drew with NO completeness check and
// Publish() then set ready=true unconditionally, so a silently-incomplete FBO
// would render a black/garbage LUT and publish it as usable -> a black sky with
// no fallback. The check is cheap: LUT builds run only on generation/phase
// change, not per frame. The caller aborts the build + leaves ready unpublished
// (AtmosReady() stays false) so the renderer falls back to the legacy sky.
bool RenderLut( GLuint tex, int w, int h )
{
	BindFbo( s_gpu.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0 );
	GLenum drawBuf = GL_COLOR_ATTACHMENT0;
	glDrawBuffers( 1, &drawBuf );

	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		CSZ_LogError( "atmos", "LUT FBO incomplete (status 0x%x); atmosphere disabled this "
			"generation (legacy-sky fallback)", (unsigned int)status );
		return false;
	}

	glViewport( 0, 0, w, h );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	return true;
}

// Publish the current LUTs + CPU coupling values through the frozen ABI.
void Publish()
{
	SkyAtmosResources res = SkyComposeAtmosResources();   // start from defaults (keeps samplers)
	int gen = GpuGeneration();

	res.ready = true;

	res.transmittanceLut.target = GL_TEXTURE_2D;
	res.transmittanceLut.name = s_gpu.texTrans;
	res.transmittanceLut.width = kTransW; res.transmittanceLut.height = kTransH;
	res.transmittanceLut.layers = 1; res.transmittanceLut.internalFormat = GL_RGBA16F;
	res.transmittanceLut.gpuGeneration = gen;

	res.multiScatteringLut.target = GL_TEXTURE_2D;
	res.multiScatteringLut.name = s_gpu.texMs;
	res.multiScatteringLut.width = kMsW; res.multiScatteringLut.height = kMsH;
	res.multiScatteringLut.layers = 1; res.multiScatteringLut.internalFormat = GL_RGBA16F;
	res.multiScatteringLut.gpuGeneration = gen;

	res.skyViewLut.target = GL_TEXTURE_2D;
	res.skyViewLut.name = s_gpu.texSky;
	res.skyViewLut.width = kSkyW; res.skyViewLut.height = kSkyH;
	res.skyViewLut.layers = 1; res.skyViewLut.internalFormat = GL_RGBA16F;
	res.skyViewLut.gpuGeneration = gen;

	// CPU coupling (DEAD-3 decision, infra pass 2 2026-06-18): res.horizonColor +
	// res.sunTransmittance are produced by atmosmath::EvalCoupling -- a full 24-step
	// horizon single-scattering march + per-sample transmittance, run on EVERY
	// phase change. The ABI fields are a FROZEN reserved seam for the planned C3
	// fog-horizon coupling / disc reddening, but NO consumer reads them today
	// (grep: only writers; C3/sun-moon read .ready/.transmittanceLut/.fallback*).
	// So the march is pure per-phase CPU waste. We KEEP the ABI fields (frozen
	// layout, reserved) but GATE THE COMPUTATION OFF until a consumer is wired:
	// res starts from SkyComposeAtmosResources() defaults, so horizonColor stays
	// {0,0,0} (additive identity) and sunTransmittance {1,1,1} (no extinction) --
	// exactly the documented "no contribution" neutral state a future consumer
	// must already tolerate. To re-enable, restore the EvalCoupling call below.
	//   atmosmath::EvalCoupling( s_sunWorld, kViewHeightKm, res.horizonColor, res.sunTransmittance );
	res.sunDirection[0] = s_sunWorld[0];
	res.sunDirection[1] = s_sunWorld[1];
	res.sunDirection[2] = s_sunWorld[2];

	SkyComposePublishAtmosResources( res );
	s_gpu.published = true;
}

}  // anonymous namespace

// =============================================================================
// PUBLIC: cvars + activity / readiness
// =============================================================================
void AtmosRegisterCvars()
{
	if( s_cvarAtmos == NULL )
		s_cvarAtmos = gEngfuncs.pfnRegisterVariable( "csz_atmos", "1", FCVAR_CLIENTDLL );
	if( s_cvarExposure == NULL )
		s_cvarExposure = gEngfuncs.pfnRegisterVariable( "csz_atmos_exposure", "1.0", FCVAR_CLIENTDLL );
	if( s_cvarMs == NULL )
		s_cvarMs = gEngfuncs.pfnRegisterVariable( "csz_atmos_ms", "1", FCVAR_CLIENTDLL );
	if( s_cvarTiming == NULL )
		s_cvarTiming = gEngfuncs.pfnRegisterVariable( "csz_atmos_timing", "0", FCVAR_CLIENTDLL );
	if( s_cvarNavy == NULL )
		s_cvarNavy = gEngfuncs.pfnRegisterVariable( "csz_sky_navy", "1.0", FCVAR_CLIENTDLL );
	CSZ_LogDev( "atmos", "cvars registered (csz_atmos/atmos_exposure/atmos_ms/atmos_timing/sky_navy)" );
}

bool AtmosActive()
{
	return ReadCvar( s_cvarAtmos, 1.0f ) != 0.0f;
}

bool AtmosReady()
{
	return s_gpu.created && !s_gpu.failedThisGen && s_gpu.gpuGeneration == GpuGeneration()
	       && s_gpu.staticBuilt && s_gpu.skyBuilt && s_gpu.published;
}

// =============================================================================
// PUBLIC: build/refresh LUTs (own FBO/TMU juggling; call at slot 10 BEFORE
// SkyComposeBeginScene binds the HDR scene target).
// =============================================================================
void AtmosBuildLuts()
{
	if( !AtmosActive() )
		return;

	if( !EnsureCreated() )
		return;   // creation failed this generation -> renderer falls back to legacy sky

	// GL-ERR-2: only create/run the GPU timer when the latch AND csz_atmos_timing
	// are on. Off-by-default play touches no query code.
	bool timing = AtmosTimingActive();
	if( timing )
		EnsureTimer();
	DbgErr( "ensure-created" );

	// Current sun direction from the live phase (honors csz_sky_phase).
	float phase = g_sky.ComputePhase();
	skymath::SunDir( phase, s_sunWorld );
	float sunCos = s_sunWorld[2];                       // world Z = up = sun zenith cosine
	float msScale = ( ReadCvar( s_cvarMs, 1.0f ) != 0.0f ) ? 1.0f : 0.0f;

	bool needStatic = !s_gpu.staticBuilt;
	bool needSky = needStatic || !s_gpu.skyBuilt
	               || fabsf( sunCos - s_gpu.lastSunCos ) > 5.0e-4f
	               || msScale != s_gpu.lastMsScale;

	if( !needStatic && !needSky )
		return;   // fully cached: nothing to rebuild this frame (steady state)

	// Open the GPU-timer span around the LUT build region.
	if( timing && s_timer.created )
	{
		glBeginQuery( GL_TIME_ELAPSED, s_timer.query[s_timer.writeIndex] );
		s_timer.inSpan = true;
	}

	// Clean full-screen state for the LUT passes (no depth attachment on the FBO).
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendNone );
	SetCull( false );
	glDisable( GL_SCISSOR_TEST );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

	BindVao( s_gpu.vao );
	DbgErr( "build-enter" );

	// PIT-1: track FBO completeness across the LUT passes. A single incomplete
	// attach aborts the rest of the build; ready is NOT published below.
	bool ok = true;

	if( needStatic )
	{
		// Pass 1: transmittance (no inputs).
		UseProgram( s_gpu.progTrans.program );
		ok = RenderLut( s_gpu.texTrans, kTransW, kTransH );
		DbgErr( "transmittance" );

		// Pass 2: multiple-scattering (samples transmittance).
		if( ok )
		{
			UseProgram( s_gpu.progMs.program );
			SkyComposeBindTex( 0, GL_TEXTURE_2D, s_gpu.texTrans );
			if( s_gpu.uMs_trans >= 0 ) glUniform1i( s_gpu.uMs_trans, kSkyTmuBase + 0 );
			ok = RenderLut( s_gpu.texMs, kMsW, kMsH );
			DbgErr( "multiscatter" );
		}

		if( ok )
			s_gpu.staticBuilt = true;
	}

	if( ok && needSky )
	{
		// Pass 3: sky-view (samples transmittance + MS for the current sun).
		UseProgram( s_gpu.progSky.program );
		SkyComposeBindTex( 0, GL_TEXTURE_2D, s_gpu.texTrans );
		SkyComposeBindTex( 1, GL_TEXTURE_2D, s_gpu.texMs );
		if( s_gpu.uSky_trans >= 0 )   glUniform1i( s_gpu.uSky_trans, kSkyTmuBase + 0 );
		if( s_gpu.uSky_ms >= 0 )      glUniform1i( s_gpu.uSky_ms, kSkyTmuBase + 1 );
		if( s_gpu.uSky_sunCos >= 0 )  glUniform1f( s_gpu.uSky_sunCos, sunCos );
		if( s_gpu.uSky_viewR >= 0 )   glUniform1f( s_gpu.uSky_viewR, atmosmath::kRg + kViewHeightKm );
		if( s_gpu.uSky_msScale >= 0 ) glUniform1f( s_gpu.uSky_msScale, msScale );
		ok = RenderLut( s_gpu.texSky, kSkyW, kSkyH );
		DbgErr( "skyview" );

		if( ok )
		{
			s_gpu.skyBuilt = true;
			s_gpu.lastSunCos = sunCos;
			s_gpu.lastMsScale = msScale;
		}
	}

	// Restore: unbind FBO, resync TMUs, return to the takeover depth baseline.
	BindFbo( 0 );
	BindVao( 0 );
	SkyComposeRestoreTmus();
	SetDepthTest( true );
	SetDepthWrite( true );
	DbgErr( "build-restore" );

	// PIT-1: publish ready ONLY when every pass had a complete FBO. On failure,
	// latch failedThisGen so we neither retry+spam nor publish a black LUT as
	// usable -- AtmosReady() stays false and the renderer draws the legacy sky.
	if( ok )
		Publish();
	else
		s_gpu.failedThisGen = true;

	// Close the timer span + read the oldest available result.
	if( s_timer.created && s_timer.inSpan )
	{
		glEndQuery( GL_TIME_ELAPSED );
		s_timer.inSpan = false;
		s_timer.issued[s_timer.writeIndex] = true;

		int readIndex = ( s_timer.writeIndex + 1 ) % kQueryRing;
		if( s_timer.issued[readIndex] )
		{
			GLint available = 0;
			glGetQueryObjectiv( s_timer.query[readIndex], GL_QUERY_RESULT_AVAILABLE, &available );
			if( available == GL_TRUE )
			{
				GLuint64 ns = 0;
				glGetQueryObjectui64v( s_timer.query[readIndex], GL_QUERY_RESULT, &ns );
				s_timer.lastMs = (double)ns / 1.0e6;
				s_timer.issued[readIndex] = false;
				if( ReadCvar( s_cvarTiming, 0.0f ) != 0.0f )
					CSZ_LogDev( "atmos", "LUT-build GPU span = %.3f ms (phase=%.3f sunCos=%.3f rebuild=%s%s)",
						s_timer.lastMs, phase, sunCos,
						needStatic ? "static+" : "", needSky ? "skyview" : "none" );
			}
		}
		s_timer.writeIndex = ( s_timer.writeIndex + 1 ) % kQueryRing;
	}
}

// =============================================================================
// PUBLIC: slot 10.5 full-screen sky background into the bound HDR FBO.
// =============================================================================
bool AtmosDrawSky( const ViewSetup &view )
{
	if( !AtmosActive() || !AtmosReady() )
		return false;

	// Camera basis (Quake world space, Z up), right/up pre-scaled by the
	// half-FOV tangents so the VS ray = fwd + right*ndc.x + up*ndc.y (same
	// convention as csz_sky_shaders.inl).
	float fwd[3], right[3], up[3];
	AngleVectors( view.angles, fwd, right, up );
	float tanX = tanf( view.fovX * kDegToRad * 0.5f );
	float tanY = tanf( view.fovY * kDegToRad * 0.5f );
	float rightS[3] = { right[0] * tanX, right[1] * tanX, right[2] * tanX };
	float upS[3]    = { up[0] * tanY,    up[1] * tanY,    up[2] * tanY };

	// Depth OFF + write OFF + no blend: opaque full-screen background; the world
	// overwrites by depth afterwards. Restore the takeover baseline after.
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendNone );
	SetCull( false );

	UseProgram( s_gpu.progDraw.program );
	BindVao( s_gpu.vao );

	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_gpu.texSky );
	if( s_gpu.uDraw_skyView >= 0 )  glUniform1i( s_gpu.uDraw_skyView, kSkyTmuBase + 0 );
	if( s_gpu.uDraw_camFwd >= 0 )   glUniform3fv( s_gpu.uDraw_camFwd, 1, fwd );
	if( s_gpu.uDraw_camRight >= 0 ) glUniform3fv( s_gpu.uDraw_camRight, 1, rightS );
	if( s_gpu.uDraw_camUp >= 0 )    glUniform3fv( s_gpu.uDraw_camUp, 1, upS );
	if( s_gpu.uDraw_sunDir >= 0 )   glUniform3fv( s_gpu.uDraw_sunDir, 1, s_sunWorld );
	if( s_gpu.uDraw_viewR >= 0 )    glUniform1f( s_gpu.uDraw_viewR, atmosmath::kRg + kViewHeightKm );

	// Adaptive exposure (eye-adaptation / auto-exposure keyed on sun elevation).
	// The sky's absolute radiance spans ~2-3 decades from a sun-above-horizon
	// sunset to civil/nautical twilight, so a single fixed exposure either blows
	// out the day or crushes twilight to black. We model the adaptation the eye/
	// camera performs: a smooth ~1-decade-per-6deg falloff of the dominant
	// daylight key as the sun sinks, capped so deep night stays dark (the night
	// sky's own light -- moon/stars -- is C3/C4, not the empty atmosphere). The
	// per-channel hue/gradient (the exposure-INVARIANT reference metrics) are
	// untouched; only the overall scale adapts. csz_atmos_exposure is a manual
	// EV multiplier on top (default 1.0).
	float sunElevDeg = asinf( ( s_sunWorld[2] < -1.0f ) ? -1.0f : ( s_sunWorld[2] > 1.0f ? 1.0f : s_sunWorld[2] ) ) / kDegToRad;
	float key = ( sunElevDeg < 5.0f ) ? sunElevDeg : 5.0f;          // cap the bright-end influence
	float proxy = powf( 10.0f, key / 6.0f );                        // ~1 decade brighter per +6deg
	float adaptive = 150.0f / ( ( proxy > 1.0e-3f ) ? proxy : 1.0e-3f );
	if( adaptive < 20.0f )   adaptive = 20.0f;                      // sunset/dawn floor (matches the tuned look)
	if( adaptive > 5000.0f ) adaptive = 5000.0f;                    // twilight ceiling (avoid amplifying noise)
	float exposure = adaptive * ReadCvar( s_cvarExposure, 1.0f );
	if( s_gpu.uDraw_exposure >= 0 ) glUniform1f( s_gpu.uDraw_exposure, exposure );
	if( s_gpu.uDraw_skyNavy >= 0 ) glUniform1f( s_gpu.uDraw_skyNavy, ReadCvar( s_cvarNavy, 1.0f ) );

	glDrawArrays( GL_TRIANGLES, 0, 3 );
	DbgErr( "drawsky" );

	BindVao( 0 );
	SkyComposeRestoreTmus();
	SetDepthTest( true );
	SetDepthWrite( true );
	return true;
}

// Dev proof/showcase: repaint the atmosphere over the whole finished frame when
// csz_sky_fullscreen != 0 (CSZ_DEV_TOOLS). Drawn with depth OFF so it paints over
// everything; used only for the visual-acceptance capture (clean horizon->zenith
// gradient independent of map geometry). The sky-view LUT is already current
// (AtmosBuildLuts ran this frame at slot 10).
void AtmosDrawDebugFullscreen( const ViewSetup &view )
{
	if( !s_fullscreenLookedUp )
	{
		s_fullscreenLookedUp = true;
		s_cvarFullscreen = gEngfuncs.pfnGetCvarPointer( "csz_sky_fullscreen" );
	}
	if( s_cvarFullscreen == NULL || s_cvarFullscreen->value == 0.0f )
		return;
	AtmosDrawSky( view );
}

double AtmosLastGpuMs() { return s_timer.lastMs; }

// =============================================================================
// PUBLIC: generation-safe shutdown.
// =============================================================================
void AtmosShutdown()
{
	if( s_gpu.created && s_gpu.gpuGeneration == GpuGeneration() )
		DestroyGpuSameContext();
	else
		ForgetGpu();

	if( s_timer.created && s_timer.gpuGeneration == GpuGeneration() )
		glDeleteQueries( kQueryRing, s_timer.query );
	s_timer.created = false;
	s_timer.inSpan = false;
	for( int i = 0; i < kQueryRing; i++ )
		s_timer.issued[i] = false;
}

}  // namespace csz
