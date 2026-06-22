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
	int mCosInner, mCosOuter, mMatShadow, mSigmaE, mSigmaS, mHgG, mIntensity, mSteps, mMarchFar;
	int mDepthTex, mShadowMap, mZNear, mZFar, mInvProj, mInvViewProj;

	// upsample uniforms
	int uInscatter, uDepthTex, uFullSize, uHalfSize, uZNear, uZFar;
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
		s_cvarG = gEngfuncs.pfnRegisterVariable( "csz_fog_march_g", "0.55", FCVAR_CLIENTDLL );

	CSZ_LogDev( "fogvol", "cvars registered (csz_fog_quality/steps/halfres/march_intensity/march_g)" );
}

void FogVolumeRender( const ViewSetup &view )
{
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

	int steps = (int)( ReadCvar( s_cvarSteps, 14.0f ) + 0.5f );
	if( steps < 4 ) steps = 4;
	if( steps > 32 ) steps = 32;

	float intensity = ReadCvar( s_cvarIntensity, 1.5f );
	float hgG = ReadCvar( s_cvarG, 0.55f );
	if( hgG < -0.95f ) hgG = -0.95f;
	if( hgG > 0.95f ) hgG = 0.95f;
	float sigmaS = sigmaE;   // scattering = extinction (albedo 1); intensity tunes brightness

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
	if( s_gpu.mSpotRadius >= 0 ) glUniform1f( s_gpu.mSpotRadius, spot.radius );
	if( s_gpu.mCosInner >= 0 )   glUniform1f( s_gpu.mCosInner, spot.cosInner );
	if( s_gpu.mCosOuter >= 0 )   glUniform1f( s_gpu.mCosOuter, spot.cosOuter );
	if( s_gpu.mMatShadow >= 0 )  glUniformMatrix4fv( s_gpu.mMatShadow, 1, GL_FALSE, spot.matShadow.m );
	if( s_gpu.mSigmaE >= 0 )     glUniform1f( s_gpu.mSigmaE, sigmaE );
	if( s_gpu.mSigmaS >= 0 )     glUniform1f( s_gpu.mSigmaS, sigmaS );
	if( s_gpu.mHgG >= 0 )        glUniform1f( s_gpu.mHgG, hgG );
	if( s_gpu.mIntensity >= 0 )  glUniform1f( s_gpu.mIntensity, intensity );
	if( s_gpu.mSteps >= 0 )      glUniform1i( s_gpu.mSteps, steps );
	if( s_gpu.mMarchFar >= 0 )   glUniform1f( s_gpu.mMarchFar, view.zFar );

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
