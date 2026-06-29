/*
 * csz_fog_godrays.cpp -- CSOZ renderer: sun/moon screen-space god rays (fog M1 Step 4)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original work written for
 * CSOZ; no code in this file is copied or translated from PrimeXT, Paranoia,
 * Trinity, retail/leaked sources, or any other license-tainted source
 * (see csoz docs/provenance.md, section 6).
 * Clean-room implementation. The screen-space radial-scatter method is the
 * textbook GPU Gems 3 Ch.13 light-shaft loop, re-expressed from the published
 * algorithm; the half-res FBO lifecycle/state mechanics clone the Step-3
 * flashlight march (csz_fog_volume.cpp) written for CSOZ.
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
#include "csz_fog_godrays.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"
#include "../geom/csz_sky_compose.h"
#include "../geom/csz_sunmoon.h"       // CszGodraySource (FOG Step 4 body contract)

#include <math.h>
#include <stdio.h>

namespace csz
{

#include "csz_fog_godrays_shaders.inl"   // kGrVs / kGrOcclFsBody / kGrScatterFsBody / kGrCompositeFsBody

namespace
{

// --- cvars (registered in FogGodraysRegisterCvars, read live each frame) -------
cvar_t *s_cvarEnable;     // csz_fog_godrays            default "1": 0 skips (zero cost)
cvar_t *s_cvarIntensity;  // csz_fog_godrays_intensity  default "1.0" [0..4] -> scales u_exposure
cvar_t *s_cvarDev;        // csz_fog_godrays_dev        "<density decay weight glowFalloff>" (clamped)
cvar_t *s_cvarHalfres;    // csz_fog_halfres (registered by FogVolume); looked up
bool    s_halfresLookedUp;

// --- half-res RGBA16F target (no depth); two instances (occlusion + scatter) ---
struct GrTarget
{
	GLuint fbo;
	GLuint colorTex;
	int    width, height;
	int    gpuGeneration;
	bool   valid;
	bool   failedThisGen;
};

GrTarget s_occl;       // FBO #1: sky-gated broad-glow emitter (vpUV)
GrTarget s_scatter;    // FBO #2: 49-tap radial scatter (vpUV)

// --- programs -----------------------------------------------------------------
struct GrGpu
{
	ShaderProgram occl;
	ShaderProgram scatter;
	ShaderProgram composite;
	GLuint vao;
	int    gpuGeneration;
	bool   built;

	// occlusion uniforms
	int oDepthTex, oHalfSize, oViewOrigin, oViewSize, oFullSize, oLightVpUV;
	int oAspect, oSkyDepthEps, oBodyColor, oSourceIntensity, oGlowFalloff;

	// scatter uniforms
	int sOcclTex, sHalfSize, sLightVpUV, sDensity, sDecay, sWeight;

	// composite uniforms
	int cScatterTex, cViewOrigin, cViewSize, cExposure, cBodyVis;
};

GrGpu s_gpu;

float Clampf( float v, float lo, float hi )
{
	return ( v < lo ) ? lo : ( v > hi ? hi : v );
}

void ForgetTarget( GrTarget &t )
{
	t.fbo = 0;
	t.colorTex = 0;
	t.width = 0;
	t.height = 0;
	t.valid = false;
	t.failedThisGen = false;
}

void DestroyTargetSameContext( GrTarget &t )
{
	if( t.fbo != 0 )
		glDeleteFramebuffers( 1, &t.fbo );
	if( t.colorTex != 0 )
		glDeleteTextures( 1, &t.colorTex );
	ForgetTarget( t );
}

// Ensure a half-res RGBA16F FBO at (w,h) on the live generation. Mirrors the
// generation rule + completeness check + B-class degrade of EnsureVolTarget
// (csz_fog_volume.cpp). LINEAR + CLAMP_TO_EDGE (the scatter/composite sample it
// with texture()). Bound on a sky unit so engine units 0..3 are untouched.
bool EnsureTarget( GrTarget &t, const char *tag, int w, int h )
{
	if( w < 1 ) w = 1;
	if( h < 1 ) h = 1;

	if( t.gpuGeneration != GpuGeneration() )
	{
		ForgetTarget( t );
		t.gpuGeneration = GpuGeneration();
	}

	if( t.valid && t.width == w && t.height == h )
		return true;

	if( t.failedThisGen )
		return false;

	if( t.fbo != 0 || t.colorTex != 0 )
		DestroyTargetSameContext( t );
	t.gpuGeneration = GpuGeneration();

	glGenTextures( 1, &t.colorTex );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, t.colorTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	SkyComposeRestoreTmus();

	glGenFramebuffers( 1, &t.fbo );
	BindFbo( t.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.colorTex, 0 );

	GLenum drawBuf = GL_COLOR_ATTACHMENT0;
	glDrawBuffers( 1, &drawBuf );
	glReadBuffer( GL_COLOR_ATTACHMENT0 );

	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	BindFbo( 0 );

	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		DestroyTargetSameContext( t );
		t.failedThisGen = true;
		CSZ_LogError( "f250godray", "half-res %s FBO incomplete (status 0x%x); god rays disabled this generation",
			tag, (unsigned int)status );
		return false;
	}

	t.width = w;
	t.height = h;
	t.valid = true;
	CSZ_LogInfo( "f250godray", "half-res %s target ready (%dx%d RGBA16F, gpu gen %d)", tag, w, h, t.gpuGeneration );
	return true;
}

void BuildPrograms()
{
	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
		return;

	s_gpu.gpuGeneration = GpuGeneration();
	glGenVertexArrays( 1, &s_gpu.vao );

	// Init-time programs: compile failure is FATAL (matches the other fog/sky passes).
	BuildProgram( "csz_godray_occl", kGrVs, kGrOcclFsBody, true, s_gpu.occl );
	BuildProgram( "csz_godray_scatter", kGrVs, kGrScatterFsBody, true, s_gpu.scatter );
	BuildProgram( "csz_godray_composite", kGrVs, kGrCompositeFsBody, true, s_gpu.composite );

	s_gpu.oDepthTex        = UniformLoc( s_gpu.occl, "u_depthTex" );
	s_gpu.oHalfSize        = UniformLoc( s_gpu.occl, "u_halfSize" );
	s_gpu.oViewOrigin      = UniformLoc( s_gpu.occl, "u_viewOrigin" );
	s_gpu.oViewSize        = UniformLoc( s_gpu.occl, "u_viewSize" );
	s_gpu.oFullSize        = UniformLoc( s_gpu.occl, "u_fullSize" );
	s_gpu.oLightVpUV       = UniformLoc( s_gpu.occl, "u_lightVpUV" );
	s_gpu.oAspect          = UniformLoc( s_gpu.occl, "u_aspect" );
	s_gpu.oSkyDepthEps     = UniformLoc( s_gpu.occl, "u_skyDepthEps" );
	s_gpu.oBodyColor       = UniformLoc( s_gpu.occl, "u_bodyColor" );
	s_gpu.oSourceIntensity = UniformLoc( s_gpu.occl, "u_sourceIntensity" );
	s_gpu.oGlowFalloff     = UniformLoc( s_gpu.occl, "u_glowFalloff" );

	s_gpu.sOcclTex   = UniformLoc( s_gpu.scatter, "u_occlusionTex" );
	s_gpu.sHalfSize  = UniformLoc( s_gpu.scatter, "u_halfSize" );
	s_gpu.sLightVpUV = UniformLoc( s_gpu.scatter, "u_lightVpUV" );
	s_gpu.sDensity   = UniformLoc( s_gpu.scatter, "u_density" );
	s_gpu.sDecay     = UniformLoc( s_gpu.scatter, "u_decay" );
	s_gpu.sWeight    = UniformLoc( s_gpu.scatter, "u_weight" );

	s_gpu.cScatterTex = UniformLoc( s_gpu.composite, "u_scatterTex" );
	s_gpu.cViewOrigin = UniformLoc( s_gpu.composite, "u_viewOrigin" );
	s_gpu.cViewSize   = UniformLoc( s_gpu.composite, "u_viewSize" );
	s_gpu.cExposure   = UniformLoc( s_gpu.composite, "u_exposure" );
	s_gpu.cBodyVis    = UniformLoc( s_gpu.composite, "u_bodyVis" );

	s_gpu.built = true;
	CSZ_LogDev( "f250godray", "occl + scatter + composite programs built (gpu gen %d)", s_gpu.gpuGeneration );
}

int HalfresDivisor()
{
	if( !s_halfresLookedUp )
	{
		s_halfresLookedUp = true;
		s_cvarHalfres = gEngfuncs.pfnGetCvarPointer( "csz_fog_halfres" );
	}
	int hr = (int)( ReadCvar( s_cvarHalfres, 1.0f ) + 0.5f );
	return ( hr == 2 ) ? 4 : ( hr == 0 ? 1 : 2 );
}

}  // anonymous namespace

void FogGodraysRegisterCvars()
{
	if( s_cvarEnable == NULL )
		s_cvarEnable = gEngfuncs.pfnRegisterVariable( "csz_fog_godrays", "1", FCVAR_CLIENTDLL );
	if( s_cvarIntensity == NULL )
		s_cvarIntensity = gEngfuncs.pfnRegisterVariable( "csz_fog_godrays_intensity", "1.0", FCVAR_CLIENTDLL );
	if( s_cvarDev == NULL )
		// "density decay weight glowFalloff" (each clamped to the spec §6 range on read).
		s_cvarDev = gEngfuncs.pfnRegisterVariable( "csz_fog_godrays_dev", "0.85 0.95 0.45 6.0", FCVAR_CLIENTDLL );

	CSZ_LogDev( "f250godray", "cvars registered (csz_fog_godrays/_intensity/_dev)" );
}

void FogGodraysRender( const ViewSetup &view )
{
	// Gate 1: master toggle. 0 -> zero cost (no bind/clear/draw; HDR untouched).
	if( ReadCvar( s_cvarEnable, 1.0f ) == 0.0f )
		return;

	// Gate 2: the additive god-ray composite needs the HDR path (a linear RGBA16F
	// target to add into) and the sampleable scene depth texture.
	if( !SkyComposeActive() )
		return;
	GLuint depthTex = SkyComposeDepthTex();
	GLuint hdrFbo   = SkyComposeHdrFbo();
	if( depthTex == 0 || hdrFbo == 0 )
		return;

	// Gate 3: the single dominant active body (sun/moon; none at new moon / below
	// horizon / faded out). §-BodyContract. No body -> nothing to scatter.
	CszGodraySrc src = CszGodraySource( view );
	if( !src.present || src.vis <= 0.0f )
		return;

	// Gate 4: project the body dir (Z-up, w_world = 0) through the host view-proj to
	// the vanishing point. Column-major matViewProj * vec4(worldDir, 0).
	const float *m = view.matViewProj.m;
	float dx = src.worldDir[0], dy = src.worldDir[1], dz = src.worldDir[2];
	float clipX = m[0] * dx + m[4] * dy + m[8]  * dz;
	float clipY = m[1] * dx + m[5] * dy + m[9]  * dz;
	float clipW = m[3] * dx + m[7] * dy + m[11] * dz;
	if( clipW <= 1e-5f )        // body behind the camera -> no shaft
		return;
	float ndcX = clipX / clipW;
	float ndcY = clipY / clipW;
	float lightVpUV[2] = { ndcX * 0.5f + 0.5f, ndcY * 0.5f + 0.5f };   // NDC -> viewport vpUV
	if( lightVpUV[0] < -0.15f || lightVpUV[0] > 1.15f ||
	    lightVpUV[1] < -0.15f || lightVpUV[1] > 1.15f )
		return;

	// Gate 5: viewport/extent sizes. Cheap-skip on any non-positive component so we
	// never divide by a degenerate dimension.
	int viewOx = view.viewport[0], viewOy = view.viewport[1];
	int viewW  = view.viewport[2], viewH  = view.viewport[3];
	int fullW  = viewOx + viewW;
	int fullH  = viewOy + viewH;
	if( viewW < 1 || viewH < 1 || fullW < 1 || fullH < 1 )
		return;

	int div = HalfresDivisor();
	int halfW = ( viewW + div - 1 ) / div;    // ceil(viewSize / divisor) -- from viewSize, NOT fullSize
	int halfH = ( viewH + div - 1 ) / div;
	if( halfW < 1 || halfH < 1 )
		return;

	if( !EnsureTarget( s_occl, "occl", halfW, halfH ) )
		return;   // B-class degrade: no shaft this generation (Error logged once)
	if( !EnsureTarget( s_scatter, "scatter", halfW, halfH ) )
		return;

	BuildPrograms();

	// Tunables (clamped to the spec §6 ranges CPU-side).
	float density = 0.85f, decay = 0.95f, weight = 0.45f, glowFalloff = 6.0f;
	if( s_cvarDev != NULL && s_cvarDev->string != NULL )
		sscanf( s_cvarDev->string, "%f %f %f %f", &density, &decay, &weight, &glowFalloff );
	density     = Clampf( density, 0.1f, 1.5f );
	decay       = Clampf( decay, 0.80f, 0.99f );
	weight      = Clampf( weight, 0.0f, 2.0f );
	glowFalloff = Clampf( glowFalloff, 2.0f, 20.0f );

	const float sourceIntensity = 1.0f;   // spec §6 default (clamped [0..4]); fixed.
	float intensity = Clampf( ReadCvar( s_cvarIntensity, 1.0f ), 0.0f, 4.0f );
	float exposure  = Clampf( 0.30f * intensity, 0.0f, 3.0f );
	// fog M1 L4: cloud-gap gating of the screen-space radial. The occlusion stage
	// already sky-masks (geometry/viewmodel/near walls < far depth emit 0 -> no
	// foreground smeared into fake shafts), so the radial source is sky/moon only.
	// Here we additionally suppress the shaft under thick cloud (L3a shaftMask:
	// gap=1, thick=0), consumed NOT recomputed. Gated by csz_moonshaft so at 0 the
	// gate is exactly 1.0 -> byte-for-byte the pre-L4 Step-4 behaviour (clean A/B).
	float shaftGate = ( CszMoonShaftEnabled() != 0.0f ) ? Clampf( view.ambience.shaftMask, 0.0f, 1.0f ) : 1.0f;
	exposure *= shaftGate;

	float aspect = ( viewH > 0 ) ? ( (float)viewW / (float)viewH ) : 1.0f;
	const float kSkyDepthEps = 0.999999f;

	float fHalfSize[2]   = { (float)halfW, (float)halfH };
	float fViewOrigin[2] = { (float)viewOx, (float)viewOy };
	float fViewSize[2]   = { (float)viewW, (float)viewH };
	float fFullSize[2]   = { (float)fullW, (float)fullH };

	// ===================== Stage 1: half-res sky-gated occlusion ==============
	BindFbo( s_occl.fbo );
	glViewport( 0, 0, halfW, halfH );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendNone );
	SetCull( false );
	glDisable( GL_SCISSOR_TEST );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	UseProgram( s_gpu.occl.program );
	BindVao( s_gpu.vao );

	SkyComposeBindTex( 0, GL_TEXTURE_2D, depthTex );
	if( s_gpu.oDepthTex >= 0 )        glUniform1i( s_gpu.oDepthTex, kSkyTmuBase + 0 );
	if( s_gpu.oHalfSize >= 0 )        glUniform2fv( s_gpu.oHalfSize, 1, fHalfSize );
	if( s_gpu.oViewOrigin >= 0 )      glUniform2fv( s_gpu.oViewOrigin, 1, fViewOrigin );
	if( s_gpu.oViewSize >= 0 )        glUniform2fv( s_gpu.oViewSize, 1, fViewSize );
	if( s_gpu.oFullSize >= 0 )        glUniform2fv( s_gpu.oFullSize, 1, fFullSize );
	if( s_gpu.oLightVpUV >= 0 )       glUniform2fv( s_gpu.oLightVpUV, 1, lightVpUV );
	if( s_gpu.oAspect >= 0 )          glUniform1f( s_gpu.oAspect, aspect );
	if( s_gpu.oSkyDepthEps >= 0 )     glUniform1f( s_gpu.oSkyDepthEps, kSkyDepthEps );
	if( s_gpu.oBodyColor >= 0 )       glUniform3fv( s_gpu.oBodyColor, 1, src.color );
	if( s_gpu.oSourceIntensity >= 0 ) glUniform1f( s_gpu.oSourceIntensity, sourceIntensity );
	if( s_gpu.oGlowFalloff >= 0 )     glUniform1f( s_gpu.oGlowFalloff, glowFalloff );

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	BindVao( 0 );
	SkyComposeRestoreTmus();

	// ===================== Stage 2: half-res 49-tap radial scatter ============
	BindFbo( s_scatter.fbo );
	glViewport( 0, 0, halfW, halfH );
	SetBlend( kBlendNone );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	UseProgram( s_gpu.scatter.program );
	BindVao( s_gpu.vao );

	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_occl.colorTex );
	if( s_gpu.sOcclTex >= 0 )   glUniform1i( s_gpu.sOcclTex, kSkyTmuBase + 0 );
	if( s_gpu.sHalfSize >= 0 )  glUniform2fv( s_gpu.sHalfSize, 1, fHalfSize );
	if( s_gpu.sLightVpUV >= 0 ) glUniform2fv( s_gpu.sLightVpUV, 1, lightVpUV );
	if( s_gpu.sDensity >= 0 )   glUniform1f( s_gpu.sDensity, density );
	if( s_gpu.sDecay >= 0 )     glUniform1f( s_gpu.sDecay, decay );
	if( s_gpu.sWeight >= 0 )    glUniform1f( s_gpu.sWeight, weight );

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	BindVao( 0 );
	SkyComposeRestoreTmus();

	// ===================== Stage 3: full-res ADDITIVE composite into HDR ======
	BindFbo( hdrFbo );
	glViewport( viewOx, viewOy, viewW, viewH );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendAddPremul );   // glBlendFunc(GL_ONE, GL_ONE); fragColor.a = 0
	SetCull( false );

	UseProgram( s_gpu.composite.program );
	BindVao( s_gpu.vao );

	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_scatter.colorTex );
	if( s_gpu.cScatterTex >= 0 ) glUniform1i( s_gpu.cScatterTex, kSkyTmuBase + 0 );
	if( s_gpu.cViewOrigin >= 0 ) glUniform2fv( s_gpu.cViewOrigin, 1, fViewOrigin );
	if( s_gpu.cViewSize >= 0 )   glUniform2fv( s_gpu.cViewSize, 1, fViewSize );
	if( s_gpu.cExposure >= 0 )   glUniform1f( s_gpu.cExposure, exposure );
	if( s_gpu.cBodyVis >= 0 )    glUniform1f( s_gpu.cBodyVis, src.vis );

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	BindVao( 0 );
	SetBlend( kBlendNone );
	SkyComposeRestoreTmus();

	// Restore the takeover baseline for the following transparent/viewmodel passes:
	// HDR FBO still bound, main viewport, depth test+write back on (mirror Step-3).
	SetDepthTest( true );
	SetDepthWrite( true );
}

void FogGodraysShutdown()
{
	bool sameContext = ( s_occl.gpuGeneration == GpuGeneration() );
	if( sameContext )
		DestroyTargetSameContext( s_occl );
	else
		ForgetTarget( s_occl );

	sameContext = ( s_scatter.gpuGeneration == GpuGeneration() );
	if( sameContext )
		DestroyTargetSameContext( s_scatter );
	else
		ForgetTarget( s_scatter );

	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
	{
		if( s_gpu.vao != 0 )
			glDeleteVertexArrays( 1, &s_gpu.vao );
		DestroyProgram( s_gpu.occl );
		DestroyProgram( s_gpu.scatter );
		DestroyProgram( s_gpu.composite );
	}
	s_gpu.vao = 0;
	s_gpu.built = false;
}

}  // namespace csz
