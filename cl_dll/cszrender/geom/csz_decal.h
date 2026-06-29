/*
 * csz_decal.h -- CSOZ renderer: world/brush BSP decals (self-draw, public interface)
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
namespace csz
{
struct ViewSetup;

// World/brush BSP decals (M2c decision A): reuse the engine's CPU-clipped decal
// geometry (gRenderAPI.R_DecalSetupVerts over surf->pdecals) and self-draw in
// GL3.3 core, modulated by the face's engine lightmap (坑22). The engine still
// owns R_DecalShoot (bullet-hole/blood spawn + net + CPU clipping stays
// engine-side); we only DRAW what it clipped.
//
// Two blend classes split across the additive light pass (坑21):
//   * alpha decals (blood/scorch, TF_HAS_ALPHA texture) draw BEFORE the
//     additive light pass with SRC_ALPHA blend, so they get lit -> DrawDecalsAlpha.
//   * classic no-alpha bullet holes draw AFTER the additive light pass with
//     DST_COLOR x SRC_COLOR modulate (the engine's 2x decal blend), else the
//     additive light over-brightens them -> DrawDecalsModulate.
// Polygon offset (-1,-1) beats z-fight against the host surface.
//
// DrawDecalsAlpha does the ONLY per-frame BSP walk: it enumerates this frame's
// visible world faces, walks each face's surf->pdecals chain, copies every
// decal's clipped verts (R_DecalSetupVerts returns a STATIC buffer, so the copy
// is immediate), classifies each into the alpha or modulate batch, then uploads
// + draws the ALPHA batch (slot 11.6). DrawDecalsModulate draws the modulate
// batch the same-frame DrawDecalsAlpha built (slot 13.x); it is a no-op if
// DrawDecalsAlpha did not run this frame (cvar off / no decals).
//
// visibleFaces/numFaces come from g_world.VisibleFaces(): a per-local-face
// camera-visibility array (1 = visible), length == world nummodelsurfaces. The
// composition root supplies them so this module needs no geom sibling include
// (it reads the BSP itself through the core EngBsp mirror).
//
// STUDIO decals (R_CreateStudioDecalList) are OUT OF SCOPE this task (OWED).
void DrawDecalsAlpha( const ViewSetup &view, const unsigned char *visibleFaces, int numFaces );
void DrawDecalsModulate( const ViewSetup &view );
void DecalRegisterCvars();   // csz_decal (default 1) at HUD init
void DecalShutdown();        // generation-safe GL teardown at HUD shutdown
}
