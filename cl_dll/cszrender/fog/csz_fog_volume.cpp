/*
 * csz_fog_volume.cpp -- CSOZ renderer: half-res flashlight ray-march (fog M1 Step 3)
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
#include "csz_fog_volume.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_math.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"
#include "../core/csz_light_types.h"
#include "../core/csz_ambience_types.h"
#include "../geom/csz_sky_compose.h"
#include "../lighting/csz_light_registry.h"
#include "../lighting/csz_light_budget.h"   // kBudgetCull (non-local defog enumerator)

#include <string>

namespace csz
{

#include "csz_fog_shaders.inl"          // kFogDepthReconstructGlsl (Step 1 helpers)
#include "csz_fog_volume_shaders.inl"   // kVolVs / kVolMarchFsBody / kVolUpsampleFsBody

namespace
{

// --- cvars (registered in FogVolumeRegisterCvars, read live each frame) -------
cvar_t *s_cvarQuality;     // csz_fog_quality       default "1": 0 Low (no march), >=1 Med (march on)
cvar_t *s_cvarSteps;       // csz_fog_steps         default "14" (clamped 4..32)
cvar_t *s_cvarHalfres;     // csz_fog_halfres       default "1": 1 half-res, 2 quarter-res, 0 full (debug)
cvar_t *s_cvarIntensity;   // csz_fog_march_intensity default "8.0": shaft brightness scale (dev tuning)
cvar_t *s_cvarG;           // csz_fog_march_g       default "0.55": HG forward anisotropy
cvar_t *s_cvarV2;          // csz_flashlight_v2     default "1": L5 enhanced march (0 = byte-for-byte pre-L5)
cvar_t *s_cvarRange;       // csz_flashlight_range  default "800": hard range cap (world units) bounding
cvar_t *s_cvarV3;          // csz_flashlight_v3 (owned by light_pass); L5R master A/B. Fetched lazily.
bool    s_lookedV3;
cvar_t *s_cvarUpSmooth;    // csz_fog_upsample_smooth default "1": 1 = 5x5 gaussian spatial avg (smooth shaft), 0 = legacy 2x2 bilinear
cvar_t *s_cvarUpSigma;     // csz_fog_upsample_sigma  default "1.5": gaussian spatial sigma (half-res texels) for the 5x5 path
cvar_t *s_cvarHalo;        // csz_fog_halo            default "0.6": §5.2 halo/glare strength (wider 2nd forward lobe; 0 = no halo)
cvar_t *s_cvarAir;         // csz_fog_march_air       default "0.18": §5.2 v3 air-vs-surface balance (was hardcoded kV3MarchScale)
cvar_t *s_cvarFpMarch;     // csz_flashlight_fpmarch  default "0" (v3.3): first-person air ray-march OFF. USER 2026-06-24:

// §5.2: the first-person march is the air-glow main act once the local world cone is dropped,
// but it must NOT wash the surface direct pool. The v3 surface-vs-air balance is now the live
// csz_fog_march_air cvar (default 0.18, was this hardcoded constant): with the §5.1 gray base
// layer the cone can be re-balanced brighter without the air glow washing the floor pool.
const float kV3MarchScaleDefault = 0.18f;
// Surface-proximity fade band (world units) for v3; 0 disables (legacy exact A/B).
const float kV3SurfFade = 40.0f;
                           //   spot attenuation / cone length / march steps (+L7 dust spawn volume)

// --- half-res in-scatter target (RGBA16F, no depth) ---------------------------
struct VolTarget
{
	GLuint fbo;
	GLuint colorTex;        // RGBA16F: rgb = in-scatter, a = linViewZ (for bilateral upsample)
	int    width, height;
	int    gpuGeneration;
	bool   valid;
	bool   failedThisGen;
};

VolTarget s_vol;

// --- march + upsample programs ------------------------------------------------
struct VolGpu
{
	ShaderProgram march;
	ShaderProgram upsample;
	GLuint vao;
	int    gpuGeneration;
	bool   built;

	// march uniforms
	int mTargetSize, mCamPos, mSpotOrigin, mSpotDir, mSpotColor, mSpotRadius;
	int mCosInner, mCosOuter, mMatShadow, mSigmaE, mSigmaS, mHgG, mIntensity, mSteps, mMarchFar, mEconserve;
	int mSurfFade;	// L5R surface-proximity fade (floor-dome fix)
	int mHalo, mFrame;	// §5.2 halo/glare strength + animated-IGN frame offset
	int mDepthTex, mShadowMap, mZNear, mZFar, mInvProj, mInvViewProj;

	// upsample uniforms
	int uInscatter, uDepthTex, uFullSize, uHalfSize, uZNear, uZFar;
	int uSmooth, uSmoothSigma;   // L-polish B: 5x5 gaussian spatial smoothing of the shaft
};

VolGpu s_gpu;

float ReadCvar( cvar_t *cv, float fallback )
{
	return ( cv != NULL ) ? cv->value : fallback;
}

void ForgetVol()
{
	s_vol.fbo = 0;
	s_vol.colorTex = 0;
	s_vol.width = 0;
	s_vol.height = 0;
	s_vol.valid = false;
	s_vol.failedThisGen = false;
}

void DestroyVolSameContext()
{
	if( s_vol.fbo != 0 )
		glDeleteFramebuffers( 1, &s_vol.fbo );
	if( s_vol.colorTex != 0 )
		glDeleteTextures( 1, &s_vol.colorTex );
	ForgetVol();
}

// Ensure the half-res in-scatter FBO at (w,h) on the live generation. Mirrors the
// generation rule + completeness check + B-class degrade of EnsureHdrTarget.
bool EnsureVolTarget( int w, int h )
{
	if( w < 1 ) w = 1;
	if( h < 1 ) h = 1;

	if( s_vol.gpuGeneration != GpuGeneration() )
	{
		ForgetVol();
		s_vol.gpuGeneration = GpuGeneration();
	}

	if( s_vol.valid && s_vol.width == w && s_vol.height == h )
		return true;

	if( s_vol.failedThisGen )
		return false;

	if( s_vol.fbo != 0 || s_vol.colorTex != 0 )
		DestroyVolSameContext();
	s_vol.gpuGeneration = GpuGeneration();

	// RGBA16F, LINEAR (the bilateral upsample samples it with texture()), CLAMP.
	// Bound on a sky unit for setup so the engine-tracked units 0..3 are untouched.
	glGenTextures( 1, &s_vol.colorTex );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_vol.colorTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	SkyComposeRestoreTmus();

	glGenFramebuffers( 1, &s_vol.fbo );
	BindFbo( s_vol.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_vol.colorTex, 0 );

	GLenum drawBuf = GL_COLOR_ATTACHMENT0;
	glDrawBuffers( 1, &drawBuf );
	glReadBuffer( GL_COLOR_ATTACHMENT0 );

	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	BindFbo( 0 );

	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		DestroyVolSameContext();
		s_vol.failedThisGen = true;
		CSZ_LogError( "fogvol", "half-res FBO incomplete (status 0x%x); flashlight march disabled this generation",
			(unsigned int)status );
		return false;
	}

	s_vol.width = w;
	s_vol.height = h;
	s_vol.valid = true;
	CSZ_LogInfo( "fogvol", "half-res in-scatter target ready (%dx%d RGBA16F, gpu gen %d)", w, h, s_vol.gpuGeneration );
	return true;
}

void BuildVolPrograms()
{
	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
		return;

	s_gpu.gpuGeneration = GpuGeneration();
	glGenVertexArrays( 1, &s_gpu.vao );

	// Final FS = "#version 330 core" + shared Step-1 reconstruct helpers + body.
	std::string marchFs = std::string( "#version 330 core\n" ) + kFogDepthReconstructGlsl + kVolMarchFsBody;
	std::string upFs    = std::string( "#version 330 core\n" ) + kFogDepthReconstructGlsl + kVolUpsampleFsBody;

	// Init-time programs: compile failure is FATAL (matches sky/world/resolve).
	BuildProgram( "csz_fog_march", kVolVs, marchFs.c_str(), true, s_gpu.march );
	BuildProgram( "csz_fog_upsample", kVolVs, upFs.c_str(), true, s_gpu.upsample );

	s_gpu.mTargetSize  = UniformLoc( s_gpu.march, "u_targetSize" );
	s_gpu.mCamPos      = UniformLoc( s_gpu.march, "u_camPos" );
	s_gpu.mSpotOrigin  = UniformLoc( s_gpu.march, "u_spotOrigin" );
	s_gpu.mSpotDir     = UniformLoc( s_gpu.march, "u_spotDir" );
	s_gpu.mSpotColor   = UniformLoc( s_gpu.march, "u_spotColor" );
	s_gpu.mSpotRadius  = UniformLoc( s_gpu.march, "u_spotRadius" );
	s_gpu.mCosInner    = UniformLoc( s_gpu.march, "u_cosInner" );
	s_gpu.mCosOuter    = UniformLoc( s_gpu.march, "u_cosOuter" );
	s_gpu.mMatShadow   = UniformLoc( s_gpu.march, "u_matShadow" );
	s_gpu.mSigmaE      = UniformLoc( s_gpu.march, "u_sigmaE" );
	s_gpu.mSigmaS      = UniformLoc( s_gpu.march, "u_sigmaS" );
	s_gpu.mHgG         = UniformLoc( s_gpu.march, "u_hgG" );
	s_gpu.mIntensity   = UniformLoc( s_gpu.march, "u_intensity" );
	s_gpu.mSteps       = UniformLoc( s_gpu.march, "u_steps" );
	s_gpu.mMarchFar    = UniformLoc( s_gpu.march, "u_marchFar" );
	s_gpu.mEconserve   = UniformLoc( s_gpu.march, "u_econserve" );
	s_gpu.mSurfFade    = UniformLoc( s_gpu.march, "u_surfFade" );	// L5R floor-dome fix
	s_gpu.mHalo        = UniformLoc( s_gpu.march, "u_halo" );	// §5.2 halo/glare
	s_gpu.mFrame       = UniformLoc( s_gpu.march, "u_frame" );	// §5.2 animated IGN
	s_gpu.mDepthTex    = UniformLoc( s_gpu.march, "u_depthTex" );
	s_gpu.mShadowMap   = UniformLoc( s_gpu.march, "u_shadowMap" );
	s_gpu.mZNear       = UniformLoc( s_gpu.march, "u_zNear" );
	s_gpu.mZFar        = UniformLoc( s_gpu.march, "u_zFar" );
	s_gpu.mInvProj     = UniformLoc( s_gpu.march, "u_invProj" );
	s_gpu.mInvViewProj = UniformLoc( s_gpu.march, "u_invViewProj" );

	s_gpu.uInscatter   = UniformLoc( s_gpu.upsample, "u_inscatter" );
	s_gpu.uDepthTex    = UniformLoc( s_gpu.upsample, "u_depthTex" );
	s_gpu.uFullSize    = UniformLoc( s_gpu.upsample, "u_fullSize" );
	s_gpu.uHalfSize    = UniformLoc( s_gpu.upsample, "u_halfSize" );
	s_gpu.uZNear       = UniformLoc( s_gpu.upsample, "u_zNear" );
	s_gpu.uZFar        = UniformLoc( s_gpu.upsample, "u_zFar" );
	s_gpu.uSmooth      = UniformLoc( s_gpu.upsample, "u_smooth" );
	s_gpu.uSmoothSigma = UniformLoc( s_gpu.upsample, "u_smoothSigma" );

	s_gpu.built = true;
	CSZ_LogDev( "fogvol", "march + upsample programs built (gpu gen %d)", s_gpu.gpuGeneration );
}

// Find this frame's shadow-casting flashlight: the single spot that claimed the
// one M1 shadow map (shadowTexSlot != 0; RenderShadowMaps sets it at slot 9). The
// shadowed shaft is the deliverable, so a shadowless spot is intentionally skipped.
bool FindShadowedSpot( SpotLightParams &out )
{
	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );

		if( !light->used || light->desc.type != kLightSpot )
			continue;
		if( light->shadowTexSlot == 0 )
			continue;

		g_lights.BuildSpotParams( *light, out );
		return true;
	}
	return false;
}

}  // anonymous namespace

// Public wrapper over the file-local FindShadowedSpot so the world base pass clears the black
// fog inside the EXACT same cone the volumetric march lights (single source of truth -> the
// defog path and the shaft can never disagree on which flashlight is active).
bool FogVolumeLocalSpot( SpotLightParams &out )
{
	return FindShadowedSpot( out );
}

// v3 third-person defog enumerator (see header). Mirrors the slot-13.4 eligibility set
// (used spot, not expired, budget-visible) MINUS the local cone, so the world/studio base
// pass clears the black fog inside exactly the cones the indicator draws. csz_flashlight_range
// caps each cone's clear length (shared with the first-person defog -> one reach value).
int FogVolumeNonLocalDefogCones( int maxN, float *apex3, float *dir3,
                                 float *len, float *cosInner, float *cosOuter )
{
	if( maxN <= 0 )
		return 0;

	static cvar_t *s_cvarRange = NULL;
	static bool    s_looked = false;
	if( !s_looked )
	{
		s_looked = true;
		s_cvarRange = gEngfuncs.pfnGetCvarPointer( "csz_flashlight_range" );	// owned by FogVolume
	}
	float range = ( s_cvarRange != NULL ) ? s_cvarRange->value : 1600.0f;
	float now = ClientTime();

	int n = 0;
	for( int i = 0; i < LightRegistry::kMaxLights && n < maxN; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );
		if( !light->used || light->desc.type != kLightSpot )
			continue;
		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;
		if( light->budgetTier == kBudgetCull )
			continue;                          // off-screen / beyond the visible-cone cap
		if( light->desc.isLocal )
			continue;                          // local cone -> FogVolumeLocalSpot view-ray defog

		SpotLightParams sp;
		g_lights.BuildSpotParams( *light, sp );
		float l = sp.radius;
		if( range > 0.0f && range < l ) l = range;	// cap the clear distance (matches first-person)
		if( l < 1.0f )
			continue;

		apex3[n * 3 + 0] = sp.origin[0]; apex3[n * 3 + 1] = sp.origin[1]; apex3[n * 3 + 2] = sp.origin[2];
		dir3[n * 3 + 0]  = sp.dir[0];    dir3[n * 3 + 1]  = sp.dir[1];    dir3[n * 3 + 2]  = sp.dir[2];
		len[n]      = l;
		cosInner[n] = sp.cosInner;
		cosOuter[n] = sp.cosOuter;
		n++;
	}
	return n;
}

void FogVolumeRegisterCvars()
{
	if( s_cvarQuality == NULL )
		s_cvarQuality = gEngfuncs.pfnRegisterVariable( "csz_fog_quality", "1", FCVAR_CLIENTDLL );
	if( s_cvarSteps == NULL )
		s_cvarSteps = gEngfuncs.pfnRegisterVariable( "csz_fog_steps", "14", FCVAR_CLIENTDLL );
	if( s_cvarHalfres == NULL )
		s_cvarHalfres = gEngfuncs.pfnRegisterVariable( "csz_fog_halfres", "1", FCVAR_CLIENTDLL );
	if( s_cvarIntensity == NULL )
		// Conservative default: the in-scatter is linear HDR radiance, so a high
		// scale clips to white under the identity tonemap (csz_tonemap 0). 1.5
		// gives a clearly visible cone at moderate fog without blowing out; ACES
		// (csz_tonemap 1) rolls off cleanly and tolerates much higher. Dev-tunable.
		s_cvarIntensity = gEngfuncs.pfnRegisterVariable( "csz_fog_march_intensity", "1.5", FCVAR_CLIENTDLL );
	if( s_cvarG == NULL )
		// §5.2: raised 0.55 -> 0.70 to physical fog anisotropy (HG g for fog/cloud is
		// 0.7-0.9). Higher g = tighter, brighter, crisper FORWARD beam + a stronger
		// up-beam halo -- exactly the "crisp 光柱 edges + visible scattering" ask. Clamped
		// to the physical 0.5-0.85 window in FogVolumeRender (fog is never near-isotropic).
		s_cvarG = gEngfuncs.pfnRegisterVariable( "csz_fog_march_g", "0.70", FCVAR_CLIENTDLL );
	if( s_cvarHalo == NULL )
		// §5.2: halo/glare around the spot source -- a wider, dimmer 2nd forward lobe in
		// the march phase that broadens the glow (multiple-scatter bloom). 0 = no halo
		// (legacy). 0.6 = a clearly visible but bounded halo; live-tunable.
		s_cvarHalo = gEngfuncs.pfnRegisterVariable( "csz_fog_halo", "0.6", FCVAR_CLIENTDLL );
	if( s_cvarAir == NULL )
		// §5.2: v3 surface-vs-air balance (was the hardcoded 0.18 kV3MarchScale). With the
		// §5.1 gray base layer the cone tolerates a brighter air glow; tune live against the
		// surface pool. >0; <=0 falls back to the 0.18 default to avoid a black cone.
		s_cvarAir = gEngfuncs.pfnRegisterVariable( "csz_fog_march_air", "0.18", FCVAR_CLIENTDLL );
	if( s_cvarV2 == NULL )
		// L5: enhanced first-person beam (energy-conserving slice + range-bounded
		// steps). 0 reproduces the pre-L5 march byte-for-byte for A/B.
		s_cvarV2 = gEngfuncs.pfnRegisterVariable( "csz_flashlight_v2", "1", FCVAR_CLIENTDLL );
	if( s_cvarRange == NULL )
		// L5: a single hard range cap (world units). Bounds the spot attenuation
		// falloff, the cone length (influence sphere), and the march step count;
		// reserved as the L7 dust spawn-volume ceiling. Clamped to the light radius.
		// Playtest r1 (operator ask 手电筒距离翻倍): doubled 800 -> 1600 so the lit
		// cone/pool/march reaches ~2x farther; the falloff SHAPE is unchanged (every
		// consumer normalizes by this length), only the throw extends. Live-tunable.
		s_cvarRange = gEngfuncs.pfnRegisterVariable( "csz_flashlight_range", "1600", FCVAR_CLIENTDLL );
	if( s_cvarUpSmooth == NULL )
		// L-polish B: smooth the half-res shaft. Default 1 (fix ON, 3x3 gaussian spatial
		// average); 0 reproduces the legacy 2x2 bilinear upsample byte-for-byte for A/B.
		s_cvarUpSmooth = gEngfuncs.pfnRegisterVariable( "csz_fog_upsample_smooth", "1", FCVAR_CLIENTDLL );
	if( s_cvarUpSigma == NULL )
		s_cvarUpSigma = gEngfuncs.pfnRegisterVariable( "csz_fog_upsample_sigma", "1.5", FCVAR_CLIENTDLL );
	if( s_cvarFpMarch == NULL )
		// v3.3 (USER 2026-06-24 拍板): the first-person volumetric march lit the air (cone-周
		// ~11x halo) and, with the floating dust (slot 13.6) already carrying the atmosphere,
		// the extra continuous haze read as redundant and tired the eye. Default OFF -> the
		// first-person flashlight clears fog + lights surfaces + keeps dust, with NO lit-air
		// shaft. Set 1 to restore the legacy march for A/B. (Third-person cone = slot 13.4,
		// untouched; surface lit pool = RunLightPasses slot 13, untouched -- surfFade kV3SurfFade
		// kept the march off the floor, so dropping it does not dim the surface pool.)
		s_cvarFpMarch = gEngfuncs.pfnRegisterVariable( "csz_flashlight_fpmarch", "0", FCVAR_CLIENTDLL );

	CSZ_LogDev( "fogvol", "cvars registered (csz_fog_quality/steps/halfres/march_intensity/march_g, csz_flashlight_v2/range/fpmarch, csz_fog_upsample_smooth/sigma)" );
}

void FogVolumeRender( const ViewSetup &view )
{
	// Gate 0 (v3.3, USER 2026-06-24): first-person air ray-march OFF by default. This pass is
	// the ONLY producer of the first-person lit-air shaft/halo (the cone-周 haze measured at
	// ~11x). The USER ruled the floating dust (slot 13.6) already supplies the atmosphere, so
	// the flashlight ALSO hazing the air is redundant + tiring -> drop it. Early-out at the very
	// top, before any GL state / FBO / program work, so the pass is a true no-op (the later
	// gates below all early-out here too -- proven side-effect-free). FogGodraysRender + DustRender
	// run as separate calls (csz_renderer.cpp), so the god rays and dust are unaffected. Set
	// csz_flashlight_fpmarch 1 to restore the legacy march for A/B.
	if( ReadCvar( s_cvarFpMarch, 0.0f ) < 0.5f )
		return;

	// Gate 1: quality tier (Step 3 lives at Med+; Low = analytic base fog only).
	if( ReadCvar( s_cvarQuality, 1.0f ) < 1.0f )
		return;

	// Gate 2: the march needs the HDR path (a linear RGBA16F target to add into)
	// and the Step-1 sampleable scene depth texture.
	if( !SkyComposeActive() )
		return;
	GLuint depthTex = SkyComposeDepthTex();
	GLuint hdrFbo = SkyComposeHdrFbo();
	if( depthTex == 0 || hdrFbo == 0 )
		return;

	// Gate 3: a shadow-casting flashlight this frame. No spot -> nothing to scatter.
	SpotLightParams spot;
	if( !FindShadowedSpot( spot ) )
		return;
	GLuint shadowGl = (GLuint)TexSlotToGlName( spot.shadowTexSlot );
	if( shadowGl == 0 )
		return;

	// Gate 4: a participating medium. The shaft is single-scatter off the fog; with
	// no fog density there is no medium and (physically) no visible cone.
	float sigmaE = FogExtinctionFromDensity( view.ambience.fogDensity );
	if( sigmaE <= 0.0f )
		return;

	// Inverse matrices for depth->world/view reconstruction (Step 1 helpers).
	Mat4 invProj, invViewProj;
	if( !Mat4Inverse( view.matProj, invProj ) || !Mat4Inverse( view.matViewProj, invViewProj ) )
		return;

	// Full extent = the HDR target size (BeginScene sized it to origin+size); the
	// main view origin is (0,0) in practice so uv = gl_FragCoord/size is 1:1 with
	// the depth/HDR textures. Half/quarter-res from csz_fog_halfres.
	int fullW = view.viewport[0] + view.viewport[2];
	int fullH = view.viewport[1] + view.viewport[3];
	if( fullW < 1 ) fullW = 1;
	if( fullH < 1 ) fullH = 1;

	int hr = (int)( ReadCvar( s_cvarHalfres, 1.0f ) + 0.5f );
	int div = ( hr == 2 ) ? 4 : ( hr == 0 ? 1 : 2 );
	int halfW = fullW / div; if( halfW < 1 ) halfW = 1;
	int halfH = fullH / div; if( halfH < 1 ) halfH = 1;

	if( !EnsureVolTarget( halfW, halfH ) )
		return;   // B-class degrade: no shaft this generation (Error logged once)

	BuildVolPrograms();

	// L5 enhanced first-person beam (csz_flashlight_v2 1, default). A single
	// csz_flashlight_range scalar caps the spot attenuation falloff, the cone length
	// (the ray-sphere influence radius fed as u_spotRadius), the hard march far, and
	// the step count -- nothing past range is marched (the perf lever). v2 0 feeds
	// exactly the pre-L5 uniforms (spot.radius / view.zFar / csz_fog_steps / linear
	// accumulation) so the output is byte-for-byte identical for A/B.
	bool v2 = ( ReadCvar( s_cvarV2, 1.0f ) >= 0.5f );

	float effRadius  = spot.radius;
	float effMarchFar = view.zFar;
	int   steps;
	int   econserve;

	if( v2 )
	{
		float range = ReadCvar( s_cvarRange, 1600.0f );
		if( range < 1.0f ) range = 1.0f;
		// range is a CAP, never an extension beyond the light's own radius.
		effRadius   = ( range < spot.radius ) ? range : spot.radius;
		effMarchFar = effRadius;
		// Steps scale with range (~1 step / 110u): 600..900u -> 6..8 steps. Clamped
		// 6..16 so a tiny range still anti-bands and a long one stays bounded.
		steps = (int)( effRadius / 110.0f + 0.5f );
		if( steps < 6 )  steps = 6;
		if( steps > 16 ) steps = 16;
		econserve = 1;
	}
	else
	{
		steps = (int)( ReadCvar( s_cvarSteps, 14.0f ) + 0.5f );
		if( steps < 4 ) steps = 4;
		if( steps > 32 ) steps = 32;
		econserve = 0;
	}

	// L5R master switch: compress the march so the air glow supports (not washes) the crisp
	// surface pool, and enable the surface-proximity fade that kills the floor dome. v3 0 ->
	// exact pre-L5R (raw cvar intensity, fade disabled) for A/B.
	if( !s_lookedV3 )
	{
		s_lookedV3 = true;
		s_cvarV3 = gEngfuncs.pfnGetCvarPointer( "csz_flashlight_v3" );
	}
	bool v3 = ( ReadCvar( s_cvarV3, 1.0f ) >= 0.5f );

	float intensity = ReadCvar( s_cvarIntensity, 1.5f );
	if( v3 )
	{
		float air = ReadCvar( s_cvarAir, kV3MarchScaleDefault );   // §5.2: surface-vs-air balance (live cvar, was hardcoded 0.18)
		if( air <= 0.0f ) air = kV3MarchScaleDefault;             // guard a 0 that would black out the cone
		intensity *= air;
	}
	float surfFade = v3 ? kV3SurfFade : 0.0f;
	// §5.2: clamp g to the physical fog window 0.5-0.85 (fog is strongly forward, never
	// near-isotropic). Default 0.70. (Was -0.95..0.95; the wide range only served pre-
	// rewrite A/B and let g drift unphysically low / isotropic.)
	float hgG = ReadCvar( s_cvarG, 0.70f );
	if( hgG < 0.50f ) hgG = 0.50f;
	if( hgG > 0.85f ) hgG = 0.85f;
	float halo = ReadCvar( s_cvarHalo, 0.6f );   // §5.2 halo/glare strength
	if( halo < 0.0f ) halo = 0.0f;
	if( halo > 4.0f ) halo = 4.0f;               // bound so the 2nd lobe can't dominate
	float sigmaS = sigmaE;   // scattering = extinction (albedo 1); intensity tunes brightness

	// §5.2 animated-IGN frame offset: a monotonically-advancing counter (wrapped to keep
	// float precision) so the dither lattice shifts every frame -- no temporal history.
	static unsigned int s_marchFrame = 0u;
	s_marchFrame = ( s_marchFrame + 1u ) & 1023u;
	float frame = (float)s_marchFrame;

	float fTarget[2]  = { (float)halfW, (float)halfH };
	float fFullSize[2] = { (float)fullW, (float)fullH };
	float fHalfSize[2] = { (float)halfW, (float)halfH };

	// ===================== Pass 1: half-res ray-march =========================
	BindFbo( s_vol.fbo );
	glViewport( 0, 0, halfW, halfH );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendNone );
	SetCull( false );
	glDisable( GL_SCISSOR_TEST );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	UseProgram( s_gpu.march.program );
	BindVao( s_gpu.vao );

	// Scene depth on sky unit 0, spot shadow (HW-PCF compare) on sky unit 1.
	SkyComposeBindTex( 0, GL_TEXTURE_2D, depthTex );
	SkyComposeBindTex( 1, GL_TEXTURE_2D, shadowGl );
	if( s_gpu.mDepthTex >= 0 )  glUniform1i( s_gpu.mDepthTex, kSkyTmuBase + 0 );
	if( s_gpu.mShadowMap >= 0 ) glUniform1i( s_gpu.mShadowMap, kSkyTmuBase + 1 );

	if( s_gpu.mZNear >= 0 )       glUniform1f( s_gpu.mZNear, view.zNear );
	if( s_gpu.mZFar >= 0 )        glUniform1f( s_gpu.mZFar, view.zFar );
	if( s_gpu.mInvProj >= 0 )     glUniformMatrix4fv( s_gpu.mInvProj, 1, GL_FALSE, invProj.m );
	if( s_gpu.mInvViewProj >= 0 ) glUniformMatrix4fv( s_gpu.mInvViewProj, 1, GL_FALSE, invViewProj.m );

	if( s_gpu.mTargetSize >= 0 ) glUniform2fv( s_gpu.mTargetSize, 1, fTarget );
	if( s_gpu.mCamPos >= 0 )     glUniform3fv( s_gpu.mCamPos, 1, view.origin );
	if( s_gpu.mSpotOrigin >= 0 ) glUniform3fv( s_gpu.mSpotOrigin, 1, spot.origin );
	if( s_gpu.mSpotDir >= 0 )    glUniform3fv( s_gpu.mSpotDir, 1, spot.dir );
	if( s_gpu.mSpotColor >= 0 )  glUniform3fv( s_gpu.mSpotColor, 1, spot.color );
	if( s_gpu.mSpotRadius >= 0 ) glUniform1f( s_gpu.mSpotRadius, effRadius );
	if( s_gpu.mCosInner >= 0 )   glUniform1f( s_gpu.mCosInner, spot.cosInner );
	if( s_gpu.mCosOuter >= 0 )   glUniform1f( s_gpu.mCosOuter, spot.cosOuter );
	if( s_gpu.mMatShadow >= 0 )  glUniformMatrix4fv( s_gpu.mMatShadow, 1, GL_FALSE, spot.matShadow.m );
	if( s_gpu.mSigmaE >= 0 )     glUniform1f( s_gpu.mSigmaE, sigmaE );
	if( s_gpu.mSigmaS >= 0 )     glUniform1f( s_gpu.mSigmaS, sigmaS );
	if( s_gpu.mHgG >= 0 )        glUniform1f( s_gpu.mHgG, hgG );
	if( s_gpu.mIntensity >= 0 )  glUniform1f( s_gpu.mIntensity, intensity );
	if( s_gpu.mSteps >= 0 )      glUniform1i( s_gpu.mSteps, steps );
	if( s_gpu.mMarchFar >= 0 )   glUniform1f( s_gpu.mMarchFar, effMarchFar );
	if( s_gpu.mEconserve >= 0 )  glUniform1i( s_gpu.mEconserve, econserve );
	if( s_gpu.mSurfFade >= 0 )   glUniform1f( s_gpu.mSurfFade, surfFade );	// L5R floor-dome fix
	if( s_gpu.mHalo >= 0 )       glUniform1f( s_gpu.mHalo, halo );	// §5.2 halo/glare
	if( s_gpu.mFrame >= 0 )      glUniform1f( s_gpu.mFrame, frame );	// §5.2 animated IGN

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	BindVao( 0 );
	SkyComposeRestoreTmus();

	// =============== Pass 2: bilateral upsample + additive composite ==========
	// Bind the HDR FBO and ADD the upsampled in-scatter. kBlendAddPremul = ONE,ONE
	// (the source rgb is added as-is; kBlendAdditive = SRC_ALPHA,ONE would scale by
	// our alpha, which carries linViewZ -- WRONG here). Depth test/write OFF: we
	// never write the depth attachment, so sampling it for the bilateral edge test
	// is not a feedback loop (only written texels are unsafe to read).
	BindFbo( hdrFbo );
	glViewport( view.viewport[0], view.viewport[1], view.viewport[2], view.viewport[3] );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendAddPremul );
	SetCull( false );

	UseProgram( s_gpu.upsample.program );
	BindVao( s_gpu.vao );

	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_vol.colorTex );
	SkyComposeBindTex( 1, GL_TEXTURE_2D, depthTex );
	if( s_gpu.uInscatter >= 0 ) glUniform1i( s_gpu.uInscatter, kSkyTmuBase + 0 );
	if( s_gpu.uDepthTex >= 0 )  glUniform1i( s_gpu.uDepthTex, kSkyTmuBase + 1 );
	if( s_gpu.uFullSize >= 0 )  glUniform2fv( s_gpu.uFullSize, 1, fFullSize );
	if( s_gpu.uHalfSize >= 0 )  glUniform2fv( s_gpu.uHalfSize, 1, fHalfSize );
	if( s_gpu.uZNear >= 0 )      glUniform1f( s_gpu.uZNear, view.zNear );
	if( s_gpu.uZFar >= 0 )       glUniform1f( s_gpu.uZFar, view.zFar );
	// L-polish B: 5x5 gaussian spatial smoothing of the half-res shaft (default on).
	if( s_gpu.uSmooth >= 0 )
		glUniform1i( s_gpu.uSmooth, ( ReadCvar( s_cvarUpSmooth, 1.0f ) != 0.0f ) ? 1 : 0 );
	if( s_gpu.uSmoothSigma >= 0 )
	{
		float sig = ReadCvar( s_cvarUpSigma, 1.5f );
		if( sig < 0.25f ) sig = 0.25f;   // keep the gaussian non-degenerate
		if( sig > 3.0f )  sig = 3.0f;    // beyond the 5x5 footprint adds no taps, only over-flattens
		glUniform1f( s_gpu.uSmoothSigma, sig );
	}

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	BindVao( 0 );
	SetBlend( kBlendNone );
	SkyComposeRestoreTmus();

	// Restore the EnterTakeover baseline for the following transparent/viewmodel
	// passes: HDR FBO still bound, main viewport, depth test+write back on.
	SetDepthTest( true );
	SetDepthWrite( true );
}

void FogVolumeShutdown()
{
	bool sameContext = ( s_vol.gpuGeneration == GpuGeneration() );

	if( sameContext )
		DestroyVolSameContext();
	else
		ForgetVol();

	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
	{
		if( s_gpu.vao != 0 )
			glDeleteVertexArrays( 1, &s_gpu.vao );
		DestroyProgram( s_gpu.march );
		DestroyProgram( s_gpu.upsample );
	}
	s_gpu.vao = 0;
	s_gpu.built = false;
}

}  // namespace csz
