/*
 * csz_renderer.cpp -- CSOZ renderer: frame orchestration (T0 lifecycle stub)
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
#include "core/csz_shader.h"
#include "core/csz_log.h"
#include "core/csz_fatal.h"

namespace csz
{

Renderer g_renderer;	// zero-initialized (static storage duration)

namespace
{

// ---------------------------------------------------------------------------
// T1 spike pipeline -- proves GLSL 330 compile/link/draw works inside the
// engine GL context (UNKNOWN-1). The whole block (sources, objects, draw
// call) is DELETED in T2 when the world pass lands (plan R18).
// The triangle is gl_VertexID-generated (no VBO/attributes; core profile
// still requires a bound VAO) and intentionally covers only part of the
// viewport so the magenta feature-color clear stays visible around it:
// takeover evidence needs BOTH the clear and the shader-drawn gradient.
// ---------------------------------------------------------------------------
const char kSpikeVs[] =
	"#version 330 core\n"
	"out vec2 v_uv;\n"
	"void main()\n"
	"{\n"
	"	const vec2 verts[3] = vec2[3]( vec2( -0.8, -0.8 ), vec2( 0.8, -0.8 ), vec2( 0.0, 0.8 ) );\n"
	"	vec2 pos = verts[gl_VertexID];\n"
	"	v_uv = pos * 0.5 + 0.5;\n"
	"	gl_Position = vec4( pos, 0.0, 1.0 );\n"
	"}\n";

const char kSpikeFs[] =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"out vec4 fragColor;\n"
	"void main()\n"
	"{\n"
	"	fragColor = vec4( v_uv, 0.0, 1.0 );\n"
	"}\n";

ShaderProgram s_spikeProgram;	// T1 spike only (deleted in T2)
GLuint s_spikeVao;		// T1 spike only (deleted in T2)
bool s_takeoverLogged;

void DrawSpikeTriangle()
{
	UseProgram( s_spikeProgram.program );
	BindVao( s_spikeVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
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
			CSZ_LogDev( "fps", "fps=%.1f avg_ms=%.2f worst_ms=%.2f frames=%d",
				(double)s_frames / span, span * 1000.0 / (double)s_frames, s_worstMs, s_frames );
		}

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
}

void Renderer::OnVidInit()
{
	// All GPU objects die with the GL context on vid_restart; later tasks
	// key their caches off GpuGeneration() and rebuild lazily.
	m_gpuGeneration++;
	m_glReady = false;

	// Old GL object names died with the context: forget them (deleting in
	// the NEW context would touch unrelated objects); EnsureGlReady rebuilds.
	s_spikeProgram.program = 0;
	s_spikeVao = 0;

	CSZ_LogDev( "core", "vid init: GPU generation now %d", m_gpuGeneration );
}

void Renderer::Shutdown()
{
	// GL context is still current during HUD_Shutdown; destroy our objects.
	if( m_glReady )
	{
		DestroyProgram( s_spikeProgram );

		if( s_spikeVao != 0 )
		{
			glDeleteVertexArrays( 1, &s_spikeVao );
			s_spikeVao = 0;
		}

		m_glReady = false;
	}

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

	// pass slot: view setup + fat PVS update (T2)
	// pass slot: world ensure-built + visible set (T2)
	// pass slot: studio begin-frame + light matrix update (T3/T6)

	EnterTakeover();						// slot 8

	// pass slot: shadow map passes (T7; before main clear)

	static const float kClearMagenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };	// takeover feature color (notes-mechanisms f-1)
	ApplyMainViewport( rvp, kClearMagenta );			// slot 10

	// pass slot: world opaque (T2)
	// pass slot: studio opaque (T3)
	// pass slot: additive light passes (T6)
	// pass slot: sprites (T5)
	// pass slot: viewmodel (T4; last, own depth range)

	DrawSpikeTriangle();	// T1 spike only; deleted in T2 (R18)

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
	// T2 adds the eager world build here.
	CSZ_LogDev( "core", "R_NewMap callback (T0 stub)" );
}

void Renderer::BuildLightmapsCallback()
{
	// T2 forwards this to WorldRenderer::MarkLightmapsDirty.
	CSZ_LogDev( "core", "GL_BuildLightmaps callback (T0 stub)" );
}

byte *Renderer::GetCurrentVis()
{
	// T0: no own PVS yet (csz_view lands in T2). The engine consults this
	// only for frames we actually took over, which never happens at T0.
	return NULL;
}

void Renderer::ProcessUserData( model_t *mod, qboolean create, const byte *buffer )
{
	// T3 builds/destroys per-model GPU resources here.
	(void)buffer;
	CSZ_LogDev( "core", "Mod_ProcessUserData %s (create=%d)",
		( mod != NULL ) ? mod->name : "<null>", (int)create );
}

void Renderer::AddEntity( int type, cl_entity_t *ent )
{
	// Entity ingest is wired and implemented in T3 (HUD_AddEntity hook).
	(void)type;
	(void)ent;
}

bool Renderer::EnsureGlReady()
{
	if( m_glReady )
		return true;

	if( !ProbeGlCaps() )	// loads all GL functions first; FATAL inside on any hard failure
		return false;

	// T1 spike pipeline (deleted in T2, R18). failFatal: an init-time shader
	// that cannot compile means the takeover route is dead (spec 3.2).
	if( !BuildProgram( "csz_spike", kSpikeVs, kSpikeFs, true, s_spikeProgram ))
		return false;

	if( s_spikeVao == 0 )
		glGenVertexArrays( 1, &s_spikeVao );	// core profile has no default VAO; bind our own before drawing

	m_glReady = true;
	return true;
}

}
