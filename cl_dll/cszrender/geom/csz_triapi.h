/*
 * csz_triapi.h -- CSOZ renderer: self-drawn TriAPI emulation layer (public interface)
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
#include "triangleapi.h"	// triangleapi_t (our own implementation of this table)
namespace csz
{
struct ViewSetup;

// Self-drawn TriAPI emulation layer (M2c decision B). Under whole-frame takeover
// the engine never calls HUD_DrawNormalTriangles / HUD_DrawTransparentTriangles,
// and those go through gEngfuncs.pTriAPI (engine fixed-function TriAPI). We supply
// our OWN triangleapi_t: Begin/End accumulate verts into a dynamic VBO and flush
// on End; RenderMode -> blend state; Color/TexCoord/Vertex -> attrs;
// SpriteTexture/Brightness -> texture bind via gRenderAPI.GL_Bind; CullFace -> cull;
// Fog/FogParams feed OUR fog uniform (this is what fixes G-P7 -- particleman /
// g_Environment content now matches the foggy world instead of staying unfogged).
// The five read-only query members (WorldToScreen/ScreenToWorld/GetMatrix/
// BoxInPVS/LightAtPoint) DELEGATE to the saved engine table -- they touch no GL
// draw state, so the engine's real implementations are both correct and free.
//
// Only the two 3D-world dispatch points are swapped (spectator overview +
// particleman/environment). The 2D HUD TriAPI consumers (ammo/draw_util/radar/
// sniperscope/spectator_gui) keep the engine's real pTriAPI: they run in the
// engine 2D pass AFTER RenderFrame returns and are never swapped (spike S2).
//
// Composition root owns the pointer swap (it is the only bridge point):
//   triangleapi_t *saved = gEngfuncs.pTriAPI;
//   CszTriApiBeginDispatch( view, saved );
//   gEngfuncs.pTriAPI = CszTriApiTable();
//   HUD_DrawNormalTriangles();            // or HUD_DrawTransparentTriangles()
//   gEngfuncs.pTriAPI = saved;
//   CszTriApiEndDispatch();
// HUD_DrawTransparentTriangles steps particleman/environment by GetFrameTime();
// because the composition root calls each dispatch EXACTLY ONCE per real frame,
// the simulation advances once -- no double-step (design §4 frametime rule).
triangleapi_t *CszTriApiTable();
void CszTriApiBeginDispatch( const ViewSetup &view, triangleapi_t *engineTri );
void CszTriApiEndDispatch();
bool CszTriApiEnabled();	// csz_triapi != 0 (dev escape hatch); composition root skips both dispatches when false
void TriApiRegisterCvars();	// csz_triapi (default 1) at HUD init
void TriApiShutdown();		// generation-safe GL teardown at HUD shutdown
}
