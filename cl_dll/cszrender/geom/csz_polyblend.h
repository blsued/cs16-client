/*
 * csz_polyblend.h -- CSOZ renderer: fullscreen screen-tint / polyblend pass
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
// Fullscreen screen-tint pass: GoldSrc V_CalcRefdef's R_PolyBlend reproduced in
// GL3 core. Combines the engine color-shifts the client DLL can observe under
// takeover -- content tint (water/lava/slime, from PM_PointContents) and the
// damage-red flash (local-player health drop) -- into one V_CalcBlend-equivalent
// rgba and draws a fullscreen alpha quad over the resolved backbuffer.
//
// Call ordering (composition root): AFTER SkyComposeResolve (the final tonemapped
// LDR image is on FBO 0, the same display space the engine's cl.cshifts live in)
// and BEFORE LeaveTakeover. A no-op (no draw) when csz_polyblend is 0 or the
// combined blend is fully transparent.
void DrawPolyblend( const ViewSetup &view );
void RegisterPolyblendCvars();   // csz_polyblend (default 1)
}
