/*
 * csz_spike.h -- CSOZ renderer: M2c de-risk spikes (S1/S2/S3), cvar-gated probe
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
// M2c de-risk spikes (architecture doc 2026-06-29 §6.0). Everything here is
// gated behind the dev cvar `csz_spike` (default 0) and writes ONLY to the log
// (the [CSZ:spike] subsystem token). It draws nothing and mutates no render
// state, so the whole module is droppable once the three feasibility questions
// are answered:
//   S1  -- is gRenderAPI.R_DecalSetupVerts non-null + callable, is surf->pdecals
//          walkable, and does swapping gEngfuncs.pEfxAPI actually intercept the
//          client's live emitters (efx shim hit proof)?
//   S2  -- runtime sanity for the TriAPI mirror: which triangleapi_t members the
//          engine actually fills (the source-level 3D-vs-2D split is in the spike
//          report doc, not here).
//   S3  -- empirical efx call-frequency calibration: the shim counts every
//          intercepted beam/spark/particle emitter so the first-batch takeover
//          list is grounded in real per-second combat rates, not a guess.
// ---------------------------------------------------------------------------
namespace csz
{
void SpikeRegisterCvars();	// registers csz_spike (default 0); call from OnHudInit
void SpikeFrame();		// per taken-over frame: manage efx shim + throttled probes
void SpikeShutdown();		// restore the engine efx pointer if we swapped it (safety)
}
