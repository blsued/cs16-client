/*
 * csz_dust.h -- CSOZ renderer: gated airborne dust motes (L7)
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
// L7 gated airborne dust. A persistent, fixed-capacity, world-space-anchored pool of
// dust motes that ONLY materialise (sim + VBO + draw) where there is light to scatter:
// inside a registered flashlight cone (the USER's primary ask: 手电照见飞尘) OR inside
// the L4 moonlight Tyndall shaft. Per mote bright = max(coneIllum, shaftIllum); motes
// below epsilon are dropped BEFORE the VBO fill -- the perf gate is in spawn/cull, not
// "draw everything then dim". Additive soft particles into the HDR buffer, drawn AFTER
// the L6a cone + the fog march/god rays (a separate pass), before the transparent pass.
// Gated by csz_dust (default 1; 0 = no dust, clean A/B).
void DustRegisterCvars();                 // csz_dust / _count / _intensity / _size (OnHudInit)
void DustRender( const ViewSetup &view ); // slot 13.6: after FogGodraysRender, before transparent
void DustShutdown();                      // generation-safe GL teardown (Renderer::Shutdown)
}
