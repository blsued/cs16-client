/*
 * csz_renderer.h -- CSOZ renderer: frame orchestration (composition root)
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
#pragma once
#include "core/csz_engine.h"
namespace csz
{
struct FrameEntities
{
	static const int kMaxEntities = 1024;
	cl_entity_t *studio[kMaxEntities];  int numStudio;
	cl_entity_t *sprites[kMaxEntities]; int numSprites;
	cl_entity_t *brush[kMaxEntities];   int numBrush;
	void Clear();
};

class Renderer
{
public:
	// --- lifecycle ---
	bool OnHandshake( render_api_t *api );  // non-GL checks (audit/ref); stores state; GL init is lazy
	void OnHudInit();                       // register csz_renderer cvar + dev commands
	void OnVidInit();                       // bump GPU generation; destroy GL objects (rebuilt lazily)
	void Shutdown();
	// --- render_interface_t callbacks (registered by csz_render_iface.cpp) ---
	int  RenderFrame( const ref_viewpass_t *rvp );  // 1 = taken over; 0 = engine renders this pass
	void ClearScene();                              // per-frame: clear FrameEntities, decay lights
	void NewMap();                                  // eager world build + log
	void BuildLightmapsCallback();                  // forward to world MarkLightmapsDirty
	byte *GetCurrentVis();                          // CurrentFatPvs()
	void ProcessUserData( model_t *mod, qboolean create, const byte *buffer );
	// --- entity ingest (HUD_AddEntity hook, T3) ---
	void AddEntity( int type, cl_entity_t *ent );
	FrameEntities &Frame() { return m_frame; }
	int GpuGeneration() const { return m_gpuGeneration; }
private:
	bool EnsureGlReady();        // one-shot lazy GL init (loader+caps+shaders); FATAL inside on hard fail
	FrameEntities m_frame;
	int m_gpuGeneration;
	bool m_handshakeOk, m_glReady;
	cvar_t *m_cvarEnable;        // csz_renderer (dev-build escape hatch; release builds pin it, M2 concern)
};
extern Renderer g_renderer;
}
