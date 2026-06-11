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
#include "core/csz_log.h"
#include "core/csz_fatal.h"

namespace csz
{

Renderer g_renderer;	// zero-initialized (static storage duration)

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
	CSZ_LogDev( "core", "vid init: GPU generation now %d", m_gpuGeneration );
}

void Renderer::Shutdown()
{
	// T0: no GL objects or caches exist yet; later tasks destroy theirs here.
	CSZ_LogDev( "core", "shutdown" );
}

int Renderer::RenderFrame( const ref_viewpass_t *rvp )
{
	// T0 scaffold: never take over - the engine renders every pass.
	// T1 implements the orchestration contract from plan section 2.2.
	(void)rvp;
	return 0;
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

	if( !ProbeGlCaps() )	// FATAL inside on any hard failure
		return false;

	m_glReady = true;
	return true;
}

}
