/*
 * csz_fog_volume.h -- CSOZ renderer: half-res flashlight ray-march (fog M1 Step 3)
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

// fog M1 Step 3 -- half-res flashlight ray-march producing shadowed light shafts
// (the "Unreal-like" volumetric layer). Gated behind csz_fog_quality >= 1; needs
// the HDR path (csz_hdr 1) and a shadow-casting spot this frame, else a no-op.
//
// Called at the kTmVolume seam in Renderer::RenderFrame -- after opaque + the
// additive light passes, before the transparent/viewmodel passes -- while the HDR
// FBO is still bound. Marches the view ray reconstructed from the Step-1 scene
// depth texture, accumulates single-scatter from the spot (cone + the existing
// g_spotShadow HW-PCF shadow map), then bilateral-upsamples and ADDITIVELY
// composites the in-scatter into the HDR buffer (never re-attenuates the scene --
// the Step-2 analytic base fog owns scene transmittance + the maxOpacity floor).
void FogVolumeRegisterCvars();                  // csz_fog_quality/steps/halfres/march_* (OnHudInit)
void FogVolumeRender( const ViewSetup &view );  // the pass; no-op when gated off
void FogVolumeShutdown();                        // generation-safe GL teardown (Renderer::Shutdown)
}
