/*
 * csz_efx_shim.h -- CSOZ renderer: pEfxAPI interception table (M2c decision C, C-SHIM)
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
// ---------------------------------------------------------------------------
// C-SHIM (architecture doc 2026-06-29 §3.3, decision C). cszrender is a STATIC
// lib linked into client.dll, so it SHARES gEngfuncs / gEngfuncs.pEfxAPI with
// the client. Swapping that one pointer to our shim table intercepts EVERY
// emitter call site (ev_hldm.cpp / events/ev_cs16.cpp / entity.cpp) with ZERO
// edits at the emit sites. MOST functions PASS THROUGH to the saved real table;
// only the first-batch beam + particle/tracer creators are redirected into the
// self-drawn pools (csz_beam / csz_particle). The spike's 18-emitter wrap
// (SPIKE-RESULTS-m2.md, S1) proved the swap + passthrough + A/B toggle.
//
// NOT redirected (decisions A/B + tempent path): R_DecalShoot (engine decal
// clip, decision A), R_MuzzleFlash (CL_AllocDlight mirror), R_Sprite_Trail
// (creates FTENT sprite tempents that HUD_AddEntity already draws) -- these
// pass through, counted but not re-drawn. Un-redirected beam-family / generic
// efx (ring/follow/torus/lightning/circle, R_RunParticleEffect) pass through
// AND log once (OWED long tail; under takeover the engine does not draw them).
//
// Lifecycle: the swap STAYS installed across consecutive taken-over frames so
// inter-frame emit events are captured (the install is persistent, not per
// frame). EfxShimSetActive() reconciles the install with takeover state + the
// csz_efx_shim cvar each RenderFrame; EfxShimShutdown() force-restores.
// ---------------------------------------------------------------------------
namespace csz
{
void EfxShimRegisterCvars();          // csz_efx_shim (default 1; 0 = engine real table = full revert)
// Reconcile install state. takeoverActive = is this a taken-over frame. Installs
// when (takeoverActive && csz_efx_shim != 0), uninstalls otherwise. Idempotent;
// call once per RenderFrame (both the takeover and the engine-render path).
void EfxShimSetActive( bool takeoverActive );
void EfxShimShutdown();               // force-restore the engine pEfxAPI (Renderer::Shutdown)
}
