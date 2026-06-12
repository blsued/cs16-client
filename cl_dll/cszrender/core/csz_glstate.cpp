/*
 * csz_glstate.cpp -- CSOZ renderer: GL state wrapper implementation
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
#include "csz_glstate.h"
#include "csz_engine.h"		// gRenderAPI (GL_Bind / GL_CleanUpTextureUnits), ref_viewpass_t
#include "csz_glfuncs.h"

namespace csz
{

namespace
{

// Shadow ("software-cached") GL state behind the thin wrappers. Cached values
// are only trusted between EnterTakeover and LeaveTakeover; both endpoints
// invalidate the cache because the engine owns GL outside that window.
const unsigned int kUnknownHandle = 0xffffffffu;

struct GlShadowState
{
	int blend;              // BlendMode or -1 = unknown
	int depthWrite;         // 0/1 or -1
	int depthTest;          // 0/1 or -1
	int cull;               // 0/1 or -1
	int cullFront;          // 0/1 or -1
	int polygonOffset;      // 0/1 or -1
	float polygonFactor, polygonUnits;
	float depthMin, depthMax;
	bool depthRangeKnown;
	unsigned int fbo;       // kUnknownHandle = unknown
	unsigned int program;   // kUnknownHandle = unknown
	unsigned int vao;       // kUnknownHandle = unknown
};

GlShadowState s_state;

void InvalidateShadowState()
{
	s_state.blend = -1;
	s_state.depthWrite = -1;
	s_state.depthTest = -1;
	s_state.cull = -1;
	s_state.cullFront = -1;
	s_state.polygonOffset = -1;
	s_state.polygonFactor = 0.0f;
	s_state.polygonUnits = 0.0f;
	s_state.depthRangeKnown = false;
	s_state.fbo = kUnknownHandle;
	s_state.program = kUnknownHandle;
	s_state.vao = kUnknownHandle;
}

}

void EnterTakeover()
{
	// Baseline for CSZ passes (plan section 4 step 2; calibrated in T1).
	// Raw GL on purpose: the engine may have left anything behind, so the
	// cache must not short-circuit these.
	glDisable( GL_SCISSOR_TEST );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	glDepthMask( GL_TRUE );
	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );

	InvalidateShadowState();
	// Latch what we just set; everything else stays unknown so the first
	// wrapper call hits real GL.
	s_state.blend = kBlendNone;
	s_state.depthWrite = 1;
	s_state.depthTest = 1;
	s_state.cull = 0;
}

void ApplyMainViewport( const struct ref_viewpass_s *rvp, const float clearRgba[4] )
{
	BindFbo( 0 );
	glViewport( rvp->viewport[0], rvp->viewport[1], rvp->viewport[2], rvp->viewport[3] );
	glClearColor( clearRgba[0], clearRgba[1], clearRgba[2], clearRgba[3] );
	glClearDepth( 1.0 );	// CSZ-PORT: GLES3 uses *f variant
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
}

void LeaveTakeover()
{
	// Restore-for-engine whitelist. The engine 2D layer (HUD/VGUI/console)
	// runs on the legacy ref_gl path right after we return; ref_gl tracks
	// only its own glState (TMU bindings etc.) and knows nothing about
	// VAO/program/FBO objects (notes-renderapi, engine-side facts), so we
	// must hand GL back in a legacy-friendly configuration.
	// Initial whitelist per plan section 4 step 2; calibration verdicts from
	// the T1 step 4 experiment are recorded in progress-m1.md.
	// T1 UNKNOWN-2 calibration verdicts (dirty-state + skip-restore probe per
	// item, evidence in _scratch\m1-render-takeover\t1\wl_bit*.png):
	//   program        REQUIRED  (2D drawn through our shader: HUD destroyed)
	//   FBO            REQUIRED  (engine 2D rendered into our FBO: lost)
	//   scissor        REQUIRED  (engine 2D clipped to stale scissor box)
	//   TMU/active tex REQUIRED+ (ref glState cache desync; console/HUD
	//                  textures broke PERSISTENTLY -- engine has no
	//                  vid_restart command to heal it, so TMU state must
	//                  ONLY ever change through engine wrappers)
	//   VAO/buffers/depth-range/depth-mask/blend/cull/poly-offset/unpack:
	//                  no visible 2D damage (engine 2D path is immediate
	//                  mode and sets its own blend/cull) -- kept anyway as
	//                  cheap insurance for OUR next frame and future refs.
	glUseProgram( 0 );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	glDepthRange( 0.0, 1.0 );	// CSZ-PORT: GLES3 uses *f variant
	glDepthMask( GL_TRUE );
	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );
	glDisable( GL_POLYGON_OFFSET_FILL );
	glDisable( GL_SCISSOR_TEST );
	// TMU hygiene STRICTLY via engine wrappers (calibration showed raw
	// glActiveTexture desync against ref glState is unrecoverable):
	gRenderAPI.GL_CleanUpTextureUnits( 0 );
	gRenderAPI.GL_SelectTexture( 0 );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );

	InvalidateShadowState();
}

void SetBlend( BlendMode mode )
{
	if( s_state.blend == (int)mode )
		return;

	switch( mode )
	{
	case kBlendNone:
		glDisable( GL_BLEND );
		break;
	case kBlendAlpha:
		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		break;
	case kBlendAdditive:
		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE );
		break;
	}

	s_state.blend = (int)mode;
}

void SetDepthWrite( bool enable )
{
	if( s_state.depthWrite == ( enable ? 1 : 0 ))
		return;

	glDepthMask( enable ? GL_TRUE : GL_FALSE );
	s_state.depthWrite = enable ? 1 : 0;
}

void SetDepthTest( bool enable )
{
	if( s_state.depthTest == ( enable ? 1 : 0 ))
		return;

	if( enable )
		glEnable( GL_DEPTH_TEST );
	else
		glDisable( GL_DEPTH_TEST );

	s_state.depthTest = enable ? 1 : 0;
}

void SetDepthRange( float zmin, float zmax )
{
	if( s_state.depthRangeKnown && s_state.depthMin == zmin && s_state.depthMax == zmax )
		return;

	glDepthRange( (double)zmin, (double)zmax );	// CSZ-PORT: GLES3 uses *f variant
	s_state.depthMin = zmin;
	s_state.depthMax = zmax;
	s_state.depthRangeKnown = true;
}

void SetCull( bool enable )
{
	if( s_state.cull == ( enable ? 1 : 0 ))
		return;

	if( enable )
		glEnable( GL_CULL_FACE );
	else
		glDisable( GL_CULL_FACE );

	s_state.cull = enable ? 1 : 0;
}

void SetCullFront( bool cullFront )
{
	if( s_state.cullFront == ( cullFront ? 1 : 0 ))
		return;

	glCullFace( cullFront ? GL_FRONT : GL_BACK );
	s_state.cullFront = cullFront ? 1 : 0;
}

void SetPolygonOffset( bool enable, float factor, float units )
{
	if( s_state.polygonOffset == ( enable ? 1 : 0 ) &&
		( !enable || ( s_state.polygonFactor == factor && s_state.polygonUnits == units )))
		return;

	if( enable )
	{
		glEnable( GL_POLYGON_OFFSET_FILL );
		glPolygonOffset( factor, units );
	}
	else
	{
		glDisable( GL_POLYGON_OFFSET_FILL );
	}

	s_state.polygonOffset = enable ? 1 : 0;
	s_state.polygonFactor = factor;
	s_state.polygonUnits = units;
}

void BindFbo( unsigned int fbo )
{
	if( s_state.fbo == fbo )
		return;

	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	s_state.fbo = fbo;
}

void UseProgram( unsigned int program )
{
	if( s_state.program == program )
		return;

	glUseProgram( program );
	s_state.program = program;
}

void BindVao( unsigned int vao )
{
	if( s_state.vao == vao )
		return;

	glBindVertexArray( vao );
	s_state.vao = vao;
}

void BindTextureSlot( int tmu, int texSlot )
{
	// Engine texture slot, not a raw GL name. GL_Bind keeps the ref-side
	// glState TMU cache coherent (render_api.h contract: texture-unit state
	// must go through the engine wrappers).
	gRenderAPI.GL_Bind( tmu, (unsigned int)texSlot );
}

}
