/*
 * csz_beam.h -- CSOZ renderer: self-drawn beam pool (M2c decision C, C-BEAM)
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
// C-BEAM (architecture doc 2026-06-29 §3.3, decision C). Under full-frame
// takeover the engine simulates but never DRAWS beams (no fixed-function in
// GL3.3 core). The efx shim (csz_efx_shim) redirects the two-point beam
// creators into this fixed pool; we self-draw each beam as a camera-facing
// ribbon (subdivided, noise-perturbed, additive, depth-TEST on / WRITE off)
// in the transparent domain (slot 14.5).
//
// ABI NOTE (the reason the pool stores a real beam_s): the egon emit site
// (ev_hldm.cpp:1460-1495) keeps the returned BEAM* and writes pBeam->flags
// (FBEAM_SINENOISE) and pBeam->die (= 0 to stop firing). So BeamAlloc* MUST
// hand back a pointer to a real beam_s whose die/flags fields the client can
// mutate -- our pool slot embeds one as its first member and honours both.
//
// FIRST BATCH: two-point beams only (R_BeamPoints / R_BeamEntPoint) -- gauss
// and egon (ev_hldm.cpp), map TE_BEAMPOINTS, future CSOZ zombie skills.
// R_BeamRing / R_BeamFollow / R_BeamTorus / R_BeamCirclePoints are OWED
// (the shim passes them through to the engine + logs once; under takeover the
// engine does not draw them, so they are registered not-yet-visible).
// ---------------------------------------------------------------------------
struct beam_s;   // common/beamdef.h (typedef'd BEAM)
namespace csz
{
struct ViewSetup;

void BeamRegisterCvars();                 // csz_beam (default 1); call from OnHudInit
void BeamDraw( const ViewSetup &view );   // slot 14.5: transparent additive ribbon pass
void BeamShutdown();                      // generation-safe GL teardown (Renderer::Shutdown)
void BeamNewMap();                        // map change: drop every live beam (no cross-map carry)

// --- efx shim entry points (csz_efx_shim redirects the engine creators here) --
// Two-point beam: start/end are world points. Returns a beam_s* the client may
// read/write (egon mutates ->flags/->die), or NULL when the pool is saturated
// (the shim then logs + drops -- no engine fallback, the beam is simply absent).
beam_s *BeamAllocPoints( const float *start, const float *end, int modelIndex,
	float life, float width, float amplitude, float brightness, float speed,
	int startFrame, float framerate, float r, float g, float b );
// Entity-anchored start (startEnt encodes entity index in the low 12 bits and an
// attachment index in bits 12-15, engine convention). We re-resolve the start
// point from that entity every frame so the beam origin tracks the firer; the
// end stays the world point given at alloc (continuous-beam end tracking is OWED).
beam_s *BeamAllocEntPoint( int startEnt, const float *end, int modelIndex,
	float life, float width, float amplitude, float brightness, float speed,
	int startFrame, float framerate, float r, float g, float b );
}
