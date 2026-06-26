/*
 * csz_volcloud.cpp -- CSOZ renderer: volumetric raymarch clouds + measurement harness
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Clean-room implementation written
 * from PUBLISHED physical/algorithm descriptions ONLY (Beer-Lambert, Henyey-
 * Greenstein, fBm); no code copied or translated from any license-tainted source
 * (Shadertoy/iQ, Unreal/Unity/Frostbite/Hillaire samples, GPU-Gems, PrimeXT,
 * Paranoia, Trinity, retail/leaked). See the header of csz_volcloud_shaders.inl.
 *
 * This program is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * In addition, as a special exception, the author gives permission to link the code
 * of this program with the Half-Life Game Engine ("HL Engine") and Modified Game
 * Libraries ("MODs") developed by Valve, L.L.C ("Valve"). You must obey the GNU
 * General Public License in all respects for all of the code used other than the HL
 * Engine and MODs from Valve. If you modify this file, you may extend this exception
 * to your version of the file, but you are not obligated to do so. If you do not wish
 * to do so, delete this exception statement from your version.
 */
// Render order: csz_renderer.cpp inserts g_volcloud.Contribute(view) between
// StarsContribute(view) and SunMoonContribute(view) -- the same seam the deleted
// dome-shell clouds used: AFTER the panorama backdrop + live stars (so the clouds
// occlude the Milky Way / stars), BEFORE the moon disc (which draws crisply on top).
#include "csz_volcloud.h"
#include "csz_sky.h"          // g_sky.ComputePhase()
#include "csz_sky_math.h"     // skymath::SunDir
#include "csz_stars_math.h"   // starsmath::NightFactorFromSunElev (shared night curve)
#include "csz_sky_compose.h"  // kSkyTmuBase, SkyComposeBindTex/RestoreTmus, SkyComposePerfDumpEnabled
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
#include <vector>
#include <algorithm>

namespace csz
{

#include "csz_volcloud_shaders.inl"

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

float ReadCvar( cvar_t *cv, float fallback ) { return ( cv != NULL ) ? cv->value : fallback; }
float clampf( float v, float lo, float hi ) { return ( v < lo ) ? lo : ( v > hi ) ? hi : v; }
int   clampi( int v, int lo, int hi )       { return ( v < lo ) ? lo : ( v > hi ) ? hi : v; }

// ---- cvars (Part 5; FCVAR_CLIENTDLL, registered eagerly, read live each frame) ----
bool    s_cvarsReady = false;
cvar_t *s_cvMaster;    // csz_volcloud         "0"  master on/off (0 = production byte-identical)
cvar_t *s_cvPerf;      // csz_volcloud_perf    "0"  0 off / 1 per-frame timer log / 2 autosweep
cvar_t *s_cvRes;       // csz_volcloud_res     "4"  resolution divisor (quarter-res)
cvar_t *s_cvSteps;     // csz_volcloud_steps   "32" view march steps
cvar_t *s_cvLight;     // csz_volcloud_light   "6"  cone light march steps
cvar_t *s_cvOct;       // csz_volcloud_oct     "2"  density fBm octaves
cvar_t *s_cvCover;     // csz_volcloud_cover   "0.62" heavier oppressive overcast (keeps gaps)
cvar_t *s_cvDensity;   // csz_volcloud_density "1.35" denser dark cores
cvar_t *s_cvSilver;    // csz_volcloud_silver  "0.9"  silver-lining rim strength (dedicated rim term)
cvar_t *s_cvTint;      // csz_volcloud_tint    "0.45" brooding storm tint (desat + cold teal)
cvar_t *s_cvDetail;    // csz_volcloud_detail  "0.7"  high-freq Worley edge-erosion + warp amount
cvar_t *s_cvBackend;   // csz_volcloud_backend "0"  0 proc / 1 cheap-hash / 2 const-slab / 3 3dtex
cvar_t *s_cvEarlyout;  // csz_volcloud_earlyout "1"

void RegisterCvarsImpl()
{
	if( s_cvarsReady )
		return;
	s_cvMaster   = gEngfuncs.pfnRegisterVariable( "csz_volcloud",         "0",   FCVAR_CLIENTDLL );
	s_cvPerf     = gEngfuncs.pfnRegisterVariable( "csz_volcloud_perf",     "0",   FCVAR_CLIENTDLL );
	s_cvRes      = gEngfuncs.pfnRegisterVariable( "csz_volcloud_res",      "4",   FCVAR_CLIENTDLL );
	s_cvSteps    = gEngfuncs.pfnRegisterVariable( "csz_volcloud_steps",    "40",  FCVAR_CLIENTDLL );
	s_cvLight    = gEngfuncs.pfnRegisterVariable( "csz_volcloud_light",    "6",   FCVAR_CLIENTDLL );
	s_cvOct      = gEngfuncs.pfnRegisterVariable( "csz_volcloud_oct",      "3",   FCVAR_CLIENTDLL );
	s_cvCover    = gEngfuncs.pfnRegisterVariable( "csz_volcloud_cover",    "0.70", FCVAR_CLIENTDLL );
	s_cvDensity  = gEngfuncs.pfnRegisterVariable( "csz_volcloud_density",  "1.90", FCVAR_CLIENTDLL );
	s_cvSilver   = gEngfuncs.pfnRegisterVariable( "csz_volcloud_silver",   "1.0",  FCVAR_CLIENTDLL );
	s_cvTint     = gEngfuncs.pfnRegisterVariable( "csz_volcloud_tint",     "0.25", FCVAR_CLIENTDLL );
	s_cvDetail   = gEngfuncs.pfnRegisterVariable( "csz_volcloud_detail",   "0.38", FCVAR_CLIENTDLL );
	s_cvBackend  = gEngfuncs.pfnRegisterVariable( "csz_volcloud_backend",  "0",   FCVAR_CLIENTDLL );
	s_cvEarlyout = gEngfuncs.pfnRegisterVariable( "csz_volcloud_earlyout", "1",   FCVAR_CLIENTDLL );
	s_cvarsReady = true;
	CSZ_LogDev( "volcloud", "cvars registered (csz_volcloud + _perf/_res/_steps/_light/_oct/_cover/_density/_silver/_tint/_detail/_backend/_earlyout)" );
}

// ---- quarter-res RGBA16F march target (generation-keyed) --------------------------
struct VolTarget
{
	GLuint fbo;
	GLuint colorTex;       // RGBA16F: rgb = premul in-scatter, a = coverage (1-Tview)
	int    width, height;
	int    gpuGeneration;
	bool   valid;
	bool   failedThisGen;
};
VolTarget s_tgt;

// ---- march + upsample programs + timer ring + 3D probe ----------------------------
const int kRing = 3;   // 3-deep GL_TIME_ELAPSED ring (non-blocking readback)

struct VolGpu
{
	ShaderProgram march;
	ShaderProgram upsample;
	GLuint vao;

	// march uniforms
	int mCamFwd, mCamRight, mCamUp, mCamPos, mLightDir, mLightColor, mAmbGround, mAmbSky;
	int mTime, mJitterFrame, mCover, mDensity, mSilver, mTint, mDetail, mSigmaT, mSlabBase, mSlabThick;
	int mLightReach, mCoverScale;
	int mNoiseFreq, mSteps, mLightSteps, mOct, mMsOct, mBackend, mEarlyout, mNoise3d;
	// upsample uniforms
	int uCamFwd, uCamRight, uCamUp, uCloudTex, uFullSize;

	// GPU timer ring
	GLuint   query[kRing];
	bool     qInFlight[kRing];
	unsigned qFrame[kRing];
	int      ringHead;

	// 3D-texture capability probe / microbench backend
	GLuint   noise3dTex;
	int      max3d;
	bool     tex3dOk;        // bind+upload succeeded with no GL error
	bool     probeDone;

	int  gpuGeneration;
	bool built;
};
VolGpu s_gpu;

// dynamically-loaded glTexImage3D (NOT in the static csz GL func table).
typedef void ( APIENTRY *CSZ_PFNTEXIMAGE3D )( GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void * );
CSZ_PFNTEXIMAGE3D s_glTexImage3D = NULL;
bool s_triedLoad3d = false;

unsigned s_frame = 0;   // monotonic Contribute frame counter

void ForgetGpuTimers()
{
	for( int i = 0; i < kRing; i++ ) { s_gpu.query[i] = 0; s_gpu.qInFlight[i] = false; s_gpu.qFrame[i] = 0; }
	s_gpu.ringHead = 0;
}

void ForgetTarget()
{
	s_tgt.fbo = 0; s_tgt.colorTex = 0; s_tgt.width = 0; s_tgt.height = 0;
	s_tgt.valid = false; s_tgt.failedThisGen = false;
}

void DestroyTargetSameContext()
{
	if( s_tgt.fbo )      glDeleteFramebuffers( 1, &s_tgt.fbo );
	if( s_tgt.colorTex ) glDeleteTextures( 1, &s_tgt.colorTex );
	ForgetTarget();
}

bool EnsureTarget( int w, int h )
{
	if( w < 1 ) w = 1;
	if( h < 1 ) h = 1;
	if( s_tgt.gpuGeneration != GpuGeneration() )
	{
		ForgetTarget();
		s_tgt.gpuGeneration = GpuGeneration();
	}
	if( s_tgt.valid && s_tgt.width == w && s_tgt.height == h )
		return true;
	if( s_tgt.failedThisGen )
		return false;
	if( s_tgt.fbo != 0 || s_tgt.colorTex != 0 )
		DestroyTargetSameContext();
	s_tgt.gpuGeneration = GpuGeneration();

	glGenTextures( 1, &s_tgt.colorTex );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_tgt.colorTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	SkyComposeRestoreTmus();

	glGenFramebuffers( 1, &s_tgt.fbo );
	BindFbo( s_tgt.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tgt.colorTex, 0 );
	GLenum drawBuf = GL_COLOR_ATTACHMENT0;
	glDrawBuffers( 1, &drawBuf );
	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	BindFbo( 0 );
	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		DestroyTargetSameContext();
		s_tgt.failedThisGen = true;
		CSZ_LogError( "volcloud", "quarter-res FBO incomplete (0x%x); clouds disabled this generation", (unsigned)status );
		return false;
	}
	s_tgt.width = w; s_tgt.height = h; s_tgt.valid = true;
	CSZ_LogInfo( "volcloud", "quarter-res march target ready (%dx%d RGBA16F, gpu gen %d)", w, h, s_tgt.gpuGeneration );
	return true;
}

// -- 3D-texture capability probe + microbench texture build (Part 3 d/e) -----------
// Query GL_MAX_3D_TEXTURE_SIZE (valid enum even without glTexImage3D), then attempt a
// real glTexImage3D bind of a small procedurally-filled 32^3 RGBA texture + LINEAR
// filtering. Logged once per generation. Errors drained + attributed.
void RunTex3dProbe()
{
	if( s_gpu.probeDone )
		return;
	s_gpu.probeDone = true;
	s_gpu.tex3dOk = false;
	s_gpu.noise3dTex = 0;

	while( glGetError() != GL_NO_ERROR ) { }
	GLint max3d = 0;
	glGetIntegerv( GL_MAX_3D_TEXTURE_SIZE, &max3d );
	while( glGetError() != GL_NO_ERROR ) { }
	s_gpu.max3d = (int)max3d;

	if( !s_triedLoad3d )
	{
		s_triedLoad3d = true;
		if( gRenderAPI.GL_GetProcAddress != NULL )
			s_glTexImage3D = (CSZ_PFNTEXIMAGE3D)gRenderAPI.GL_GetProcAddress( "glTexImage3D" );
	}

	if( s_glTexImage3D == NULL )
	{
		CSZ_LogInfo( "volcloud", "[csz_volcloud] max3d=%d tex3d_bind=fail:no_glTexImage3D tex3d_fetch=skipped", s_gpu.max3d );
		return;
	}

	// Fill a 32^3 RGBA8 procedural noise (CPU-side value noise; clean-room).
	const int N = 32;
	std::vector<unsigned char> px( (size_t)N * N * N * 4 );
	for( int z = 0; z < N; z++ )
		for( int y = 0; y < N; y++ )
			for( int x = 0; x < N; x++ )
			{
				unsigned int h = (unsigned int)( x * 374761393 + y * 668265263 + z * 2147483647u );
				h = ( h ^ ( h >> 13 ) ) * 1274126177u; h ^= ( h >> 16 );
				unsigned char v = (unsigned char)( h & 0xFF );
				size_t idx = ( ( (size_t)z * N + y ) * N + x ) * 4;
				px[idx + 0] = v; px[idx + 1] = v; px[idx + 2] = v; px[idx + 3] = 255;
			}

	GLuint tex = 0;
	glGenTextures( 1, &tex );
	SkyComposeBindTex( 1, GL_TEXTURE_3D, tex );
	while( glGetError() != GL_NO_ERROR ) { }
	s_glTexImage3D( GL_TEXTURE_3D, 0, GL_RGBA8, N, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px[0] );
	GLenum upErr = glGetError();
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT );
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT );
	GLenum parErr = glGetError();
	SkyComposeRestoreTmus();

	if( upErr == GL_NO_ERROR && parErr == GL_NO_ERROR )
	{
		s_gpu.noise3dTex = tex;
		s_gpu.tex3dOk = true;
		CSZ_LogInfo( "volcloud", "[csz_volcloud] max3d=%d tex3d_bind=ok tex3d_size=32^3_RGBA8 (filtered fetch exercised by backend=3 microbench)", s_gpu.max3d );
	}
	else
	{
		glDeleteTextures( 1, &tex );
		CSZ_LogInfo( "volcloud", "[csz_volcloud] max3d=%d tex3d_bind=fail:up=0x%x par=0x%x tex3d_fetch=skipped", s_gpu.max3d, (unsigned)upErr, (unsigned)parErr );
	}
}

void BuildPrograms()
{
	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
		return;
	if( s_gpu.gpuGeneration != GpuGeneration() )
	{
		// foreign generation: forget all GL names (never glDelete a dead context)
		s_gpu.march.program = 0; s_gpu.upsample.program = 0; s_gpu.vao = 0;
		ForgetGpuTimers();
		s_gpu.noise3dTex = 0; s_gpu.probeDone = false; s_gpu.tex3dOk = false;
		s_gpu.gpuGeneration = GpuGeneration();
	}

	// Guard: only mint a VAO name once per GPU generation (the foreign-generation
	// branch above zeroes it). Without the guard, a shader-compile failure with the
	// cvar ON returns below WITHOUT setting s_gpu.built, so BuildPrograms re-enters
	// every frame and would leak a fresh VAO name each time.
	if( !s_gpu.vao )
		glGenVertexArrays( 1, &s_gpu.vao );
	// Spike-quality: a compile failure disables the pass this generation (NOT fatal --
	// production is OFF by default, so a broken cloud shader must never brick the game).
	if( !BuildProgram( "csz_volcloud_march", kVolCloudVs, kVolCloudMarchFs, false, s_gpu.march ) ||
	    !BuildProgram( "csz_volcloud_upsample", kVolCloudVs, kVolCloudUpsampleFs, false, s_gpu.upsample ) )
	{
		CSZ_LogError( "volcloud", "shader build failed; volumetric clouds disabled this generation" );
		s_gpu.built = false;
		return;
	}

	s_gpu.mCamFwd     = UniformLoc( s_gpu.march, "u_camFwd" );
	s_gpu.mCamRight   = UniformLoc( s_gpu.march, "u_camRight" );
	s_gpu.mCamUp      = UniformLoc( s_gpu.march, "u_camUp" );
	s_gpu.mCamPos     = UniformLoc( s_gpu.march, "u_camPos" );
	s_gpu.mLightDir   = UniformLoc( s_gpu.march, "u_lightDir" );
	s_gpu.mLightColor = UniformLoc( s_gpu.march, "u_lightColor" );
	s_gpu.mAmbGround  = UniformLoc( s_gpu.march, "u_ambGround" );
	s_gpu.mAmbSky     = UniformLoc( s_gpu.march, "u_ambSky" );
	s_gpu.mTime       = UniformLoc( s_gpu.march, "u_time" );
	s_gpu.mJitterFrame= UniformLoc( s_gpu.march, "u_jitterFrame" );
	s_gpu.mCover      = UniformLoc( s_gpu.march, "u_cover" );
	s_gpu.mDensity    = UniformLoc( s_gpu.march, "u_density" );
	s_gpu.mSilver     = UniformLoc( s_gpu.march, "u_silver" );
	s_gpu.mTint       = UniformLoc( s_gpu.march, "u_tint" );
	s_gpu.mDetail     = UniformLoc( s_gpu.march, "u_detail" );
	s_gpu.mSigmaT     = UniformLoc( s_gpu.march, "u_sigmaT" );
	s_gpu.mSlabBase   = UniformLoc( s_gpu.march, "u_slabBase" );
	s_gpu.mSlabThick  = UniformLoc( s_gpu.march, "u_slabThick" );
	s_gpu.mLightReach = UniformLoc( s_gpu.march, "u_lightReach" );
	s_gpu.mCoverScale = UniformLoc( s_gpu.march, "u_coverScale" );
	s_gpu.mNoiseFreq  = UniformLoc( s_gpu.march, "u_noiseFreq" );
	s_gpu.mSteps      = UniformLoc( s_gpu.march, "u_steps" );
	s_gpu.mLightSteps = UniformLoc( s_gpu.march, "u_lightSteps" );
	s_gpu.mOct        = UniformLoc( s_gpu.march, "u_oct" );
	s_gpu.mMsOct      = UniformLoc( s_gpu.march, "u_msOct" );
	s_gpu.mBackend    = UniformLoc( s_gpu.march, "u_backend" );
	s_gpu.mEarlyout   = UniformLoc( s_gpu.march, "u_earlyout" );
	s_gpu.mNoise3d    = UniformLoc( s_gpu.march, "u_noise3d" );

	s_gpu.uCamFwd     = UniformLoc( s_gpu.upsample, "u_camFwd" );
	s_gpu.uCamRight   = UniformLoc( s_gpu.upsample, "u_camRight" );
	s_gpu.uCamUp      = UniformLoc( s_gpu.upsample, "u_camUp" );
	s_gpu.uCloudTex   = UniformLoc( s_gpu.upsample, "u_cloudTex" );
	s_gpu.uFullSize   = UniformLoc( s_gpu.upsample, "u_fullSize" );

	glGenQueries( kRing, s_gpu.query );
	for( int i = 0; i < kRing; i++ ) { s_gpu.qInFlight[i] = false; s_gpu.qFrame[i] = 0; }
	s_gpu.ringHead = 0;

	s_gpu.built = true;
	CSZ_LogDev( "volcloud", "march + upsample programs + timer ring built (gpu gen %d)", s_gpu.gpuGeneration );
}

// Shared night gate (identical curve the bright stars use), so the lit body picks the
// moon at night and the sun by day exactly like the rest of the sky.
float NightFactor( float phase )
{
	float sun[3];
	skymath::SunDir( phase, sun );
	float sz = clampf( sun[2], -1.0f, 1.0f );
	return starsmath::NightFactorFromSunElev( asinf( sz ) / kDegToRad );
}

// ---- per-frame resolved parameters (live cvars OR autosweep override) -------------
struct Params
{
	int   res, steps, light, oct, backend, earlyout;
	float cover, density;
	float silver, tint, detail;
	float phase;       // measurement phase (autosweep overrides)
	bool  forcePhase;  // true => use 'phase' instead of g_sky.ComputePhase()
};

// =============================================================================
// MEASUREMENT HARNESS -- autosweep state (csz_volcloud_perf 2). Drives a fixed
// config list deterministically, one config per window, accumulating GPU-ms samples
// from the timer ring, emitting median+p95 per config, then self-quitting. Part 3.
// =============================================================================
struct SweepCfg
{
	const char *label;
	int   phaseMode;   // 0 night (0.0), 1 day (0.5)
	int   res, steps, light, oct, backend, earlyout;
	int   window;      // GPU-ms samples to collect (after warmup) before advancing
};

std::vector<SweepCfg> s_sweep;
bool   s_sweepBuilt = false;
int    s_sweepIdx = 0;
int    s_sweepFrame = 0;        // frames spent in the current config
std::vector<double> s_sweepSamples;
bool   s_sweepDone = false;

const int kHeadlineWindow = 250;   // headline preset / forced-full / 3D microbench
const int kLadderWindow   = 64;    // cost-ladder sweep (coarser; clearly labeled)
const int kWarmupFrames   = 24;    // ignored before sampling starts each config

void BuildSweep()
{
	if( s_sweepBuilt )
		return;
	s_sweepBuilt = true;
	s_sweep.clear();

	// --- Headline deterministic preset: default config, DAY + NIGHT (>=250 samples) ---
	// RE-FOUNDED default = res4, steps40, light6, oct3, backend0(procedural), earlyout1
	// (matches the shipping cvar defaults so the headline preset measures the real cost).
	SweepCfg pn = { "PRESET_NIGHT",       0, 4, 40, 6, 3, 0, 1, kHeadlineWindow }; s_sweep.push_back( pn );
	SweepCfg pd = { "PRESET_DAY",         1, 4, 40, 6, 3, 0, 1, kHeadlineWindow }; s_sweep.push_back( pd );
	// --- Forced-full-step (early-out DISABLED) worst case, day + night ---
	SweepCfg fn = { "FORCED_FULL_NIGHT",  0, 4, 40, 6, 3, 0, 0, kHeadlineWindow }; s_sweep.push_back( fn );
	SweepCfg fd = { "FORCED_FULL_DAY",    1, 4, 40, 6, 3, 0, 0, kHeadlineWindow }; s_sweep.push_back( fd );
	// --- 3D-texture-fetch microbench: preset config, backend 3 (only if 3D bound) ---
	if( s_gpu.tex3dOk )
	{
		SweepCfg t3 = { "TEX3D_MICROBENCH", 0, 4, 40, 6, 3, 3, 1, kHeadlineWindow }; s_sweep.push_back( t3 );
	}

	// --- Cost ladder (coarser window): light x oct x backend x res x earlyout, night ---
	const int lights[4]   = { 0, 1, 3, 6 };
	const int octs[3]     = { 1, 2, 4 };
	const int backends[4] = { 0, 1, 2, 3 };   // proc / cheap-hash / const-slab / 3dtex
	const int divs[3]     = { 1, 2, 4 };
	const int eos[2]      = { 1, 0 };
	for( int li = 0; li < 4; li++ )
	for( int oi = 0; oi < 3; oi++ )
	for( int bi = 0; bi < 4; bi++ )
	for( int ri = 0; ri < 3; ri++ )
	for( int ei = 0; ei < 2; ei++ )
	{
		if( backends[bi] == 3 && !s_gpu.tex3dOk )
			continue;   // skip 3D backend when the bind failed (logged in the probe line)
		SweepCfg c = { "LADDER", 0, divs[ri], 32, lights[li], octs[oi], backends[bi], eos[ei], kLadderWindow };
		s_sweep.push_back( c );
	}
	CSZ_LogInfo( "volcloud", "[csz_volcloud] SWEEP_BEGIN configs=%d (headline_window=%d ladder_window=%d warmup=%d tex3d=%s)",
		(int)s_sweep.size(), kHeadlineWindow, kLadderWindow, kWarmupFrames, s_gpu.tex3dOk ? "ok" : "unavailable" );
}

void EmitSweepResult( const SweepCfg &c )
{
	int n = (int)s_sweepSamples.size();
	if( n <= 0 )
	{
		CSZ_LogInfo( "volcloud", "[csz_volcloud] LADDER label=%s res=%d steps=%d light=%d oct=%d backend=%d eo=%d phase=%.1f n=0 med_ms=NA p95_ms=NA",
			c.label, c.res, c.steps, c.light, c.oct, c.backend, c.earlyout, c.phaseMode ? 0.5f : 0.0f );
		return;
	}
	std::sort( s_sweepSamples.begin(), s_sweepSamples.end() );
	double med = s_sweepSamples[ n / 2 ];
	double p95 = s_sweepSamples[ clampi( (int)( 0.95 * n ), 0, n - 1 ) ];
	double mn  = s_sweepSamples[0];
	double mx  = s_sweepSamples[n - 1];
	CSZ_LogInfo( "volcloud", "[csz_volcloud] LADDER label=%s res=%d steps=%d light=%d oct=%d backend=%d eo=%d phase=%.1f n=%d med_ms=%.4f p95_ms=%.4f min_ms=%.4f max_ms=%.4f",
		c.label, c.res, c.steps, c.light, c.oct, c.backend, c.earlyout, c.phaseMode ? 0.5f : 0.0f,
		n, med, p95, mn, mx );
}

}  // anonymous namespace

VolCloudRenderer g_volcloud;

void VolCloudRenderer::RegisterCvars()
{
	RegisterCvarsImpl();
}

void VolCloudRenderer::Shutdown()
{
	bool live = ( s_tgt.gpuGeneration == GpuGeneration() );
	if( live ) DestroyTargetSameContext(); else ForgetTarget();

	if( s_gpu.gpuGeneration == GpuGeneration() )
	{
		if( s_gpu.vao ) glDeleteVertexArrays( 1, &s_gpu.vao );
		if( s_gpu.march.program )    DestroyProgram( s_gpu.march );
		if( s_gpu.upsample.program ) DestroyProgram( s_gpu.upsample );
		if( s_gpu.built )            glDeleteQueries( kRing, s_gpu.query );
		if( s_gpu.noise3dTex )       glDeleteTextures( 1, &s_gpu.noise3dTex );
	}
	s_gpu.vao = 0; s_gpu.march.program = 0; s_gpu.upsample.program = 0;
	s_gpu.noise3dTex = 0; s_gpu.probeDone = false; s_gpu.tex3dOk = false;
	s_gpu.built = false;
	ForgetGpuTimers();
}

// =============================================================================
// CONTRIBUTE -- quarter-res march -> horizon-aware upsample -> premultiplied
// composite into the currently-bound (HDR) scene FBO. csz_volcloud 0 early-outs on
// the first line (production byte-identical). Full GL state guard + pre-pass error
// attribution + dedicated GPU timer wrapping ONLY the cloud GPU work.
// =============================================================================
void VolCloudRenderer::Contribute( const ViewSetup &view )
{
	RegisterCvarsImpl();

	// --- master early-out: production byte-identical when off -----------------------
	if( ReadCvar( s_cvMaster, 0.0f ) < 0.5f )
		return;

	BuildPrograms();
	if( !s_gpu.built )
		return;
	RunTex3dProbe();   // once per generation (after a program exists so a GL context is live)

	s_frame++;

	int perf = (int)( ReadCvar( s_cvPerf, 0.0f ) + 0.5f );

	// --- resolve parameters: live cvars, or autosweep override (perf 2) -------------
	Params P;
	P.forcePhase = false;
	P.phase = 0.0f;
	if( perf >= 2 )
	{
		BuildSweep();
		if( s_sweepDone )
		{
			// sweep finished: keep rendering a benign default so the frame is valid.
			P.res = 4; P.steps = 32; P.light = 6; P.oct = 2; P.backend = 0; P.earlyout = 1;
			P.cover = 0.70f; P.density = 1.90f; P.forcePhase = true; P.phase = 0.0f;
			P.silver = 1.0f; P.tint = 0.25f; P.detail = 0.38f;
		}
		else
		{
			const SweepCfg &c = s_sweep[ s_sweepIdx ];
			P.res = c.res; P.steps = c.steps; P.light = c.light; P.oct = c.oct;
			P.backend = c.backend; P.earlyout = c.earlyout;
			P.cover = 0.70f; P.density = 1.90f;   // shipping coverage/density for the preset
			P.silver = 1.0f; P.tint = 0.25f; P.detail = 0.38f;   // dramatic-storm look = the real measured cost
			P.forcePhase = true; P.phase = c.phaseMode ? 0.5f : 0.0f;
		}
	}
	else
	{
		P.res      = clampi( (int)( ReadCvar( s_cvRes, 4.0f ) + 0.5f ), 1, 8 );
		P.steps    = clampi( (int)( ReadCvar( s_cvSteps, 64.0f ) + 0.5f ), 1, 96 );
		P.light    = clampi( (int)( ReadCvar( s_cvLight, 6.0f ) + 0.5f ), 0, 8 );
		P.oct      = clampi( (int)( ReadCvar( s_cvOct, 2.0f ) + 0.5f ), 1, 6 );
		P.backend  = clampi( (int)( ReadCvar( s_cvBackend, 0.0f ) + 0.5f ), 0, 3 );
		P.earlyout = ( ReadCvar( s_cvEarlyout, 1.0f ) >= 0.5f ) ? 1 : 0;
		P.cover    = clampf( ReadCvar( s_cvCover, 0.70f ), 0.0f, 1.0f );
		P.density  = clampf( ReadCvar( s_cvDensity, 1.90f ), 0.0f, 4.0f );
		P.silver   = clampf( ReadCvar( s_cvSilver, 1.0f ), 0.0f, 1.5f );
		P.tint     = clampf( ReadCvar( s_cvTint,   0.25f ), 0.0f, 1.0f );
		P.detail   = clampf( ReadCvar( s_cvDetail, 0.38f ), 0.0f, 1.0f );
	}
	if( P.backend == 3 && !s_gpu.tex3dOk )
		P.backend = 0;   // 3D unavailable: fall back to procedural (never sample an unbound 3D tex)

	// clamp light steps to 0 only meaningfully; multiscatter octaves fixed at 2.
	int msOct = 2;

	// --- viewport / target sizing ---------------------------------------------------
	int fullW = view.viewport[2]; if( fullW < 1 ) fullW = 1;
	int fullH = view.viewport[3]; if( fullH < 1 ) fullH = 1;
	int qW = fullW / P.res; if( qW < 1 ) qW = 1;
	int qH = fullH / P.res; if( qH < 1 ) qH = 1;
	if( !EnsureTarget( qW, qH ) )
		return;

	// --- phase-derived bodies / colors ----------------------------------------------
	// NOTE on phase convention: the sky uses phase 0.5 = MIDNIGHT, ~1.0 = daylight (NOT
	// the spec's "day = 0.5" -- documented deviation). Cloud-pass GPU cost is phase-
	// independent (same step count / coverage; phase only swaps the lit body + ambient
	// magnitude), so the autosweep FORCES the day-vs-night LIGHTING PATH from the preset
	// label (phase>=0.4 => day/sun-lit, else night/moon-lit) to faithfully exercise both
	// branches deterministically. The live (non-sweep) path uses the real night curve.
	float phase = P.forcePhase ? P.phase : g_sky.ComputePhase();
	float night = P.forcePhase ? ( ( P.phase >= 0.4f ) ? 0.0f : 1.0f ) : NightFactor( phase );
	float sunDir[3];
	skymath::SunDir( phase, sunDir );
	float moonDir[3] = { -sunDir[0], -sunDir[1], -sunDir[2] };
	// lit body: moon at night, sun by day (smooth handoff by the night curve).
	float lightDir[3];
	for( int i = 0; i < 3; i++ ) lightDir[i] = ( night > 0.5f ) ? moonDir[i] : sunDir[i];
	// light color: cool moonlight at night, warm-white sun by day, scaled to HDR.
	float lc = ( night > 0.5f ) ? ( 1.2f + 1.0f * night ) : 3.2f;
	float lightColor[3];
	if( night > 0.5f ) { lightColor[0] = 0.85f * lc; lightColor[1] = 0.90f * lc; lightColor[2] = 1.00f * lc; }
	else               { lightColor[0] = 1.00f * lc; lightColor[1] = 0.97f * lc; lightColor[2] = 0.90f * lc; }
	// height-aware ambient: darker ground bounce vs cool sky zenith.
	float ambSky[3], ambGround[3];
	if( night > 0.5f )
	{
		ambGround[0] = 0.010f; ambGround[1] = 0.014f; ambGround[2] = 0.024f;
		ambSky[0]    = 0.040f; ambSky[1]    = 0.055f; ambSky[2]    = 0.090f;
	}
	else
	{
		ambGround[0] = 0.18f; ambGround[1] = 0.20f; ambGround[2] = 0.24f;
		ambSky[0]    = 0.45f; ambSky[1]    = 0.55f; ambSky[2]    = 0.75f;
	}

	// camera basis (Quake Z-up), right/up pre-scaled by the half-FOV tangents.
	float fwd[3], right[3], up[3];
	AngleVectors( view.angles, fwd, right, up );
	float tanX = tanf( view.fovX * kDegToRad * 0.5f );
	float tanY = tanf( view.fovY * kDegToRad * 0.5f );
	float rightS[3] = { right[0] * tanX, right[1] * tanX, right[2] * tanX };
	float upS[3]    = { up[0] * tanY,    up[1] * tanY,    up[2] * tanY };

	// cloud slab (world units above the camera) + extinction.
	// RE-FOUNDED SCALE (2026-06-26): converged in-engine from the old self-defeating
	// far/flat/thin scale (slabBase 2400, slabThick 1600, feature ~900u, sigmaT 0.0019,
	// steps 32, light 6, oct 2) -- which read as a flat 2D texture pasted on the dome --
	// to a CLOSE + THICK + FINE + STRONGLY-SELF-SHADOWED scale that reads as real volume:
	//   slabBase  700  : clouds brought CLOSE so there is NEAR cloud overhead (was 2400).
	//   slabThick 3200 : a deep slab the ray integrates many feature-diameters INTO so
	//                    vertical billowing towers rise (was 1600 = ~1 feature thick).
	//   featSize  300  : finer base feature (~300u, noiseFreq 1/300; was ~900u) so a ray
	//                    cuts MANY feature-diameters => depth + parallax structure.
	//   sigmaT  0.016  : ~8x stronger extinction so dense cores self-shadow to near-black
	//                    (was 0.0019 => cores never went dark); THE cue that sells volume.
	//   lReach    1.3  : cone light-march spans several feature-diameters toward the sun.
	//   coverScale 0.14: coverage field is LOW-frequency (~7x bigger than the body) so the
	//                    sky reads as big coherent MASSES with clear GAPS (was 0.46 = small
	//                    holes). The body fBM (oct 3) adds the billow inside each mass.
	const float slabBase  = 700.0f;
	const float slabThick = 3200.0f;
	const float featSize  = 300.0f;
	const float noiseFreq = 1.0f / featSize;
	const float sigmaT    = 0.0160f;
	const float lReach    = 1.3f;
	const float coverScale= 0.14f;

	float t = fmodf( ClientTime(), 3600.0f );
	float jitterFrame = (float)( s_frame & 1023u );

	// ================================================================================
	// FULL STATE GUARD -- capture every state the two-pass detour perturbs, restore it
	// exactly on exit (validated by off->on->off: the later sky/world frames are
	// unchanged). csz Set*/Bind* wrappers keep the engine-facing shadow state coherent;
	// the raw-only states (viewport/scissor/colormask/sRGB/drawbuffer/FBO) are saved +
	// restored with raw GL. (Part 4.)
	// ================================================================================
	GLint  prevFbo = 0;          glGetIntegerv( GL_FRAMEBUFFER_BINDING, &prevFbo );
	GLint  prevViewport[4];      glGetIntegerv( GL_VIEWPORT, prevViewport );
	GLint  prevColorMask[4];     glGetIntegerv( GL_COLOR_WRITEMASK, prevColorMask );
	GLboolean prevScissor = glIsEnabled( GL_SCISSOR_TEST );
	GLboolean prevSrgb    = glIsEnabled( GL_FRAMEBUFFER_SRGB );
	// Toggle state this two-pass detour perturbs through the csz Set* wrappers. Snapshot
	// the ACTUAL entry values (not assumed-baseline constants) and restore them on exit
	// via the SAME wrappers, so the shadow cache stays coherent for SunMoonContribute,
	// which runs next inside this takeover. Symmetric with the raw saves above.
	GLboolean prevDepthTest = glIsEnabled( GL_DEPTH_TEST );
	GLboolean prevBlend     = glIsEnabled( GL_BLEND );
	GLboolean prevCull      = glIsEnabled( GL_CULL_FACE );
	GLint     prevDepthMask = GL_TRUE; glGetIntegerv( GL_DEPTH_WRITEMASK, &prevDepthMask );

	// --- pre-pass GL-error drain + attribution marker (Part 4) ----------------------
	{
		GLenum e; int drained = 0;
		while( ( e = glGetError() ) != GL_NO_ERROR && drained < 8 )
		{
			if( perf > 0 )
				CSZ_LogInfo( "volcloud", "[csz_volcloud] pre-existing GL 0x%x (predates the cloud pass)", (unsigned)e );
			drained++;
		}
	}

	bool timerOn = ( perf > 0 );
	// GL_TIME_ELAPSED cannot NEST: the compose-span timer (csz_perf_dump) wraps this
	// whole region, so skip our timer when it is active (logged once).
	if( timerOn && SkyComposePerfDumpEnabled() )
	{
		static bool warned = false;
		if( !warned ) { warned = true; CSZ_LogInfo( "volcloud", "[csz_volcloud] timer SKIPPED: csz_perf_dump active (GL_TIME_ELAPSED cannot nest)" ); }
		timerOn = false;
	}

	// pick a ring slot + read back its prior (3-frames-old) result, non-blocking.
	int slot = s_gpu.ringHead;
	double readMs = -1.0; unsigned readAge = 0; bool gotSample = false;
	if( timerOn && s_gpu.qInFlight[slot] )
	{
		GLint avail = 0;
		glGetQueryObjectiv( s_gpu.query[slot], GL_QUERY_RESULT_AVAILABLE, &avail );
		if( avail )
		{
			GLuint64 ns = 0;
			glGetQueryObjectui64v( s_gpu.query[slot], GL_QUERY_RESULT, &ns );
			readMs = (double)ns / 1.0e6;
			readAge = s_frame - s_gpu.qFrame[slot];
			s_gpu.qInFlight[slot] = false;
			gotSample = true;
		}
		else
		{
			timerOn = false;   // slot still busy: skip issuing this frame (don't clobber)
		}
	}

	bool issuing = ( timerOn && !s_gpu.qInFlight[slot] );
	if( issuing )
		glBeginQuery( GL_TIME_ELAPSED, s_gpu.query[slot] );

	// ================================ Pass 1: march =================================
	BindFbo( s_tgt.fbo );
	glViewport( 0, 0, qW, qH );
	glDisable( GL_FRAMEBUFFER_SRGB );
	glDisable( GL_SCISSOR_TEST );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendNone );
	SetCull( false );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	UseProgram( s_gpu.march.program );
	BindVao( s_gpu.vao );
	if( P.backend == 3 && s_gpu.noise3dTex )
		SkyComposeBindTex( 1, GL_TEXTURE_3D, s_gpu.noise3dTex );

	if( s_gpu.mCamFwd >= 0 )     glUniform3fv( s_gpu.mCamFwd, 1, fwd );
	if( s_gpu.mCamRight >= 0 )   glUniform3fv( s_gpu.mCamRight, 1, rightS );
	if( s_gpu.mCamUp >= 0 )      glUniform3fv( s_gpu.mCamUp, 1, upS );
	if( s_gpu.mCamPos >= 0 )     glUniform3fv( s_gpu.mCamPos, 1, view.origin );
	if( s_gpu.mLightDir >= 0 )   glUniform3fv( s_gpu.mLightDir, 1, lightDir );
	if( s_gpu.mLightColor >= 0 ) glUniform3fv( s_gpu.mLightColor, 1, lightColor );
	if( s_gpu.mAmbGround >= 0 )  glUniform3fv( s_gpu.mAmbGround, 1, ambGround );
	if( s_gpu.mAmbSky >= 0 )     glUniform3fv( s_gpu.mAmbSky, 1, ambSky );
	if( s_gpu.mTime >= 0 )       glUniform1f( s_gpu.mTime, t );
	if( s_gpu.mJitterFrame >= 0 )glUniform1f( s_gpu.mJitterFrame, jitterFrame );
	if( s_gpu.mCover >= 0 )      glUniform1f( s_gpu.mCover, P.cover );
	if( s_gpu.mDensity >= 0 )    glUniform1f( s_gpu.mDensity, P.density );
	if( s_gpu.mSilver >= 0 )     glUniform1f( s_gpu.mSilver, P.silver );
	if( s_gpu.mTint >= 0 )       glUniform1f( s_gpu.mTint, P.tint );
	if( s_gpu.mDetail >= 0 )     glUniform1f( s_gpu.mDetail, P.detail );
	if( s_gpu.mSigmaT >= 0 )     glUniform1f( s_gpu.mSigmaT, sigmaT );
	if( s_gpu.mSlabBase >= 0 )   glUniform1f( s_gpu.mSlabBase, slabBase );
	if( s_gpu.mSlabThick >= 0 )  glUniform1f( s_gpu.mSlabThick, slabThick );
	// light-march reach = several feature-diameters toward the sun (strong self-shadow),
	// scaled by the dev lReach knob during bisection. featSize is the base feature size (u).
	if( s_gpu.mLightReach >= 0 ) glUniform1f( s_gpu.mLightReach, featSize * 7.0f * lReach );
	if( s_gpu.mCoverScale >= 0 ) glUniform1f( s_gpu.mCoverScale, coverScale );
	if( s_gpu.mNoiseFreq >= 0 )  glUniform1f( s_gpu.mNoiseFreq, noiseFreq );
	if( s_gpu.mSteps >= 0 )      glUniform1i( s_gpu.mSteps, P.steps );
	if( s_gpu.mLightSteps >= 0 ) glUniform1i( s_gpu.mLightSteps, P.light );
	if( s_gpu.mOct >= 0 )        glUniform1i( s_gpu.mOct, P.oct );
	if( s_gpu.mMsOct >= 0 )      glUniform1i( s_gpu.mMsOct, msOct );
	if( s_gpu.mBackend >= 0 )    glUniform1i( s_gpu.mBackend, P.backend );
	if( s_gpu.mEarlyout >= 0 )   glUniform1i( s_gpu.mEarlyout, P.earlyout );
	if( s_gpu.mNoise3d >= 0 )    glUniform1i( s_gpu.mNoise3d, kSkyTmuBase + 1 );

	glDrawArrays( GL_TRIANGLES, 0, 3 );
	BindVao( 0 );
	SkyComposeRestoreTmus();

	// ============== Pass 2: horizon-aware upsample + premultiplied composite ========
	BindFbo( (GLuint)prevFbo );
	glViewport( prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3] );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendPremulOver );   // dst = src.rgb + dst*(1-src.a) -- premultiplied over
	SetCull( false );

	UseProgram( s_gpu.upsample.program );
	BindVao( s_gpu.vao );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_tgt.colorTex );
	float fSize[2] = { (float)fullW, (float)fullH };
	if( s_gpu.uCamFwd >= 0 )   glUniform3fv( s_gpu.uCamFwd, 1, fwd );
	if( s_gpu.uCamRight >= 0 ) glUniform3fv( s_gpu.uCamRight, 1, rightS );
	if( s_gpu.uCamUp >= 0 )    glUniform3fv( s_gpu.uCamUp, 1, upS );
	if( s_gpu.uCloudTex >= 0 ) glUniform1i( s_gpu.uCloudTex, kSkyTmuBase + 0 );
	if( s_gpu.uFullSize >= 0 ) glUniform2fv( s_gpu.uFullSize, 1, fSize );

	glDrawArrays( GL_TRIANGLES, 0, 3 );
	BindVao( 0 );
	SkyComposeRestoreTmus();

	if( issuing )
	{
		glEndQuery( GL_TIME_ELAPSED );
		s_gpu.qInFlight[slot] = true;
		s_gpu.qFrame[slot] = s_frame;
		s_gpu.ringHead = ( slot + 1 ) % kRing;
	}

	// --- restore the snapshotted entry state + the raw-only saved state --------------
	UseProgram( 0 );
	// Restore the values snapshotted at pass entry through the wrappers (keeps the
	// shadow cache coherent for the SunMoon pass that runs next). The blend ENABLE bit
	// is all glIsEnabled gives us; in this takeover blend is always OFF at entry
	// (EnterTakeover baseline + every prior pass restores), so the live restore is
	// kBlendNone -- the enabled branch is a defensive fallback only.
	SetBlend( prevBlend ? kBlendAlpha : kBlendNone );
	SetDepthTest( prevDepthTest != GL_FALSE );
	SetDepthWrite( prevDepthMask != GL_FALSE );
	SetCull( prevCull != GL_FALSE );
	BindFbo( (GLuint)prevFbo );
	glViewport( prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3] );
	glColorMask( (GLboolean)prevColorMask[0], (GLboolean)prevColorMask[1], (GLboolean)prevColorMask[2], (GLboolean)prevColorMask[3] );
	if( prevScissor ) glEnable( GL_SCISSOR_TEST ); else glDisable( GL_SCISSOR_TEST );
	if( prevSrgb )    glEnable( GL_FRAMEBUFFER_SRGB ); else glDisable( GL_FRAMEBUFFER_SRGB );

	// --- post-pass GL-error check: the cloud pass MUST contribute zero ---------------
	{
		GLenum e; int n = 0;
		while( ( e = glGetError() ) != GL_NO_ERROR && n < 8 )
		{
			CSZ_LogError( "volcloud", "[csz_volcloud] CLOUD-PASS GL ERROR 0x%x (frame=%u backend=%d)", (unsigned)e, s_frame, P.backend );
			n++;
		}
	}

	// --- timer readout: per-frame log + autosweep accumulation ----------------------
	if( gotSample )
	{
		if( perf >= 1 )
			CSZ_LogInfo( "volcloud",
				"[csz_volcloud] frame=%u qidx=%d ms=%.4f res=%d steps=%d light=%d oct=%d cover=%.2f phase=%.2f earlyout=%d avail_age=%u",
				s_frame, slot, readMs, P.res, P.steps, P.light, P.oct, P.cover, phase, P.earlyout, readAge );

		if( perf >= 2 && !s_sweepDone )
		{
			if( s_sweepFrame >= kWarmupFrames )
				s_sweepSamples.push_back( readMs );
		}
	}

	// --- autosweep advance ----------------------------------------------------------
	if( perf >= 2 && !s_sweepDone )
	{
		s_sweepFrame++;
		const SweepCfg &c = s_sweep[ s_sweepIdx ];
		bool enough = ( (int)s_sweepSamples.size() >= c.window );
		bool safety = ( s_sweepFrame > c.window + kWarmupFrames + 400 );   // never hang on a stalled ring
		if( enough || safety )
		{
			EmitSweepResult( c );
			s_sweepIdx++;
			s_sweepFrame = 0;
			s_sweepSamples.clear();
			if( s_sweepIdx >= (int)s_sweep.size() )
			{
				s_sweepDone = true;
				CSZ_LogInfo( "volcloud", "[csz_volcloud] SWEEP_DONE configs=%d -- self-quitting", (int)s_sweep.size() );
				if( gEngfuncs.pfnClientCmd != NULL )
					gEngfuncs.pfnClientCmd( (char *)"quit\n" );
			}
		}
	}
}

}  // namespace csz
