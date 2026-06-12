/*
 * csz_shadowmap.cpp -- CSOZ renderer: spot light shadow map (depth pass driver)
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
#include "csz_shadowmap.h"
#include "csz_light_registry.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_view.h"
#include "../geom/csz_studio.h"
#include "../geom/csz_world.h"

namespace csz
{

SpotShadowMap g_spotShadow;

namespace
{

// Slope-scaled depth bias against self-shadow acne (plan section 10 step 2
// start values; live validation step 3 may retune toward (2.0, 4.0)).
const float kDepthBiasFactor = 1.0f;
const float kDepthBiasUnits = 2.0f;

// Module state lives here so the contract header stays member-free
// (same pattern as WorldRenderer / LightRegistry).
struct ShadowState
{
	int texSlot;		// engine texture slot (GL_CreateTexture), 0 = none
	unsigned int fbo;
	int gpuGeneration;	// generation the GL names belong to
	int failedGeneration;	// context where creation failed (retry only after vid restart)
	bool failLogged;
};

ShadowState s_shadow;

}

bool SpotShadowMap::EnsureCreated()
{
	if( s_shadow.texSlot != 0 && s_shadow.fbo != 0 && s_shadow.gpuGeneration == GpuGeneration())
		return true;

	// Stale generation: forget names (T1 rule), then rebuild below.
	if( s_shadow.gpuGeneration != GpuGeneration())
	{
		s_shadow.texSlot = 0;
		s_shadow.fbo = 0;
	}

	// One failure report per GL context (B-class degrade: shadowless lights).
	if( s_shadow.failedGeneration == GpuGeneration() && s_shadow.failLogged )
		return false;

	s_shadow.gpuGeneration = GpuGeneration();

	if( gRenderAPI.GL_CreateTexture == NULL )
	{
		s_shadow.failedGeneration = GpuGeneration();
		s_shadow.failLogged = true;
		CSZ_LogError( "lighting", "shadowmap unavailable: GL_CreateTexture is NULL; lights render shadowless" );
		return false;
	}

	// Engine-slot path (plan section 10 step 1): TF_DEPTHMAP gives
	// GL_DEPTH_COMPONENT24 + GL_COMPARE_R_TO_TEXTURE/GL_LEQUAL + LINEAR
	// filtering (hardware 2x2 PCF) + zero-depth border with TF_BORDER --
	// statically verified against the pinned engine ref/gl/gl_image.c
	// (GL_SetTextureFormat / GL_ApplyTextureParams). The FBO completeness
	// check below is the runtime probe: only a depth-renderable format can
	// complete as GL_DEPTH_ATTACHMENT.
	s_shadow.texSlot = gRenderAPI.GL_CreateTexture( "csz_shadow_spot0", kResolution, kResolution,
		NULL, (texFlags_t)( TF_DEPTHMAP | TF_CLAMP | TF_BORDER | TF_NOMIPMAP ));

	int glName = TexSlotToGlName( s_shadow.texSlot );

	if( s_shadow.texSlot == 0 || glName == 0 )
	{
		s_shadow.texSlot = 0;
		s_shadow.failedGeneration = GpuGeneration();
		s_shadow.failLogged = true;
		CSZ_LogError( "lighting", "shadowmap depth texture creation failed; lights render shadowless" );
		return false;
	}

	glGenFramebuffers( 1, &s_shadow.fbo );
	BindFbo( s_shadow.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, (GLuint)glName, 0 );

	// Depth-only framebuffer: no color attachment, so both draw and read
	// buffers must be GL_NONE for spec-level completeness.
	GLenum none = GL_NONE;

	glDrawBuffers( 1, &none );
	glReadBuffer( GL_NONE );

	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );

	BindFbo( 0 );

	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		// Same-context cleanup is safe here (we just created these names).
		glDeleteFramebuffers( 1, &s_shadow.fbo );

		if( gRenderAPI.GL_FreeTexture != NULL )
			gRenderAPI.GL_FreeTexture( s_shadow.texSlot );

		s_shadow.fbo = 0;
		s_shadow.texSlot = 0;
		s_shadow.failedGeneration = GpuGeneration();
		s_shadow.failLogged = true;
		CSZ_LogError( "lighting", "shadowmap FBO incomplete (status 0x%x); lights render shadowless",
			(unsigned int)status );
		return false;
	}

	CSZ_LogInfo( "lighting", "spot shadowmap ready (%dx%d, slot=%d)", kResolution, kResolution, s_shadow.texSlot );
	return true;
}

void SpotShadowMap::Destroy()
{
	// Generation rule (T1 calibration): forget, never delete, on a foreign
	// context; delete properly when the context that made the names is live.
	bool sameContext = ( s_shadow.gpuGeneration == GpuGeneration());

	if( s_shadow.fbo != 0 && sameContext )
		glDeleteFramebuffers( 1, &s_shadow.fbo );

	if( s_shadow.texSlot != 0 && sameContext && gRenderAPI.GL_FreeTexture != NULL )
		gRenderAPI.GL_FreeTexture( s_shadow.texSlot );

	s_shadow.fbo = 0;
	s_shadow.texSlot = 0;
	s_shadow.failLogged = false;
	s_shadow.failedGeneration = -1;
}

void SpotShadowMap::RenderDepth( ActiveLight &light, const ViewSetup &mainView,
	cl_entity_s *const *studioEnts, int studioCount )
{
	(void)mainView;	// reserved (screen scissor optimization is an M2 concern)

	light.shadowTexSlot = 0;	// stays shadowless unless the pass completes

	if( !EnsureCreated())
		return;		// B-class in-frame degrade (Error logged once)

	// Light view: same parameters the registry derives its matrices from
	// (proj fov/0.1/radius + Mat4ViewQuake), all-visible PVS on purpose
	// (notes-mechanisms f-8: a light-point PVS drops casters the camera can
	// see shadows of).
	ViewSetup lightView;

	BuildSpotLightView( light.desc.origin, light.desc.angles, light.desc.fov,
		light.desc.radius, kResolution, lightView );

	BindFbo( s_shadow.fbo );
	glViewport( 0, 0, kResolution, kResolution );
	glClearDepth( 1.0 );	// CSZ-PORT: GLES3 uses *f variant
	glClear( GL_DEPTH_BUFFER_BIT );

	// Acne policy: geometry passes render the light-facing-AWAY side only
	// (world: plane-side selection; studio: GL cull, see their DrawDepth),
	// so lit surfaces never compare against their own depth. Polygon offset
	// stays on as cheap extra margin for thin brushes.
	SetPolygonOffset( true, kDepthBiasFactor, kDepthBiasUnits );

	g_world.DrawDepth( lightView, light.frustum );
	g_studio.DrawDepth( lightView, light.frustum, studioEnts, studioCount );

	SetPolygonOffset( false, 0.0f, 0.0f );

	// Main framebuffer/viewport restore is the caller's contract: orchestration
	// slot 10 (ApplyMainViewport) runs right after the shadow passes.

	// Shadow lookup matrix derived from the very matrices we rendered with
	// (registry math is identical; this guarantees it stays so).
	Mat4ShadowBias( lightView.matProj, lightView.matView, light.matShadow );
	light.shadowTexSlot = s_shadow.texSlot;
}

int SpotShadowMap::TexSlot() const
{
	return ( s_shadow.gpuGeneration == GpuGeneration()) ? s_shadow.texSlot : 0;
}

}
