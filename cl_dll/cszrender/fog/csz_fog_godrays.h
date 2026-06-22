/*
 * csz_fog_godrays.h -- CSOZ renderer: sun/moon screen-space god rays (fog M1 Step 4)
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

// fog M1 Step 4 -- additive sun/moon god rays (screen-space radial scattering,
// GPU Gems 3 Ch.13). A NEW half-res pass cloning Step-3 mechanics; gated behind
// csz_fog_godrays. Three stages (FOG-STEP4-GODRAYS-SPEC.md §2): half-res sky-gated
// occlusion -> half-res 49-tap radial scatter -> full-res cheap ADDITIVE composite
// into the HDR buffer. Hooked at the kTmVolume seam AFTER FogVolumeRender(), HDR FBO
// still bound. Purely additive linear HDR radiance (fragColor.a = 0); NEVER touches
// scene transmittance / maxOpacity (the Step-2 analytic base fog owns extinction).
// The single dominant active body (sun by day / moon by night, none at new moon)
// comes from CszGodraySource() (csz_sunmoon.h). A pure no-op when gated off or when
// the body is off-screen / below horizon / faded out.
void FogGodraysRegisterCvars();                  // csz_fog_godrays / _intensity / _dev (OnHudInit)
void FogGodraysRender( const ViewSetup &view );  // the pass; no-op when gated off
void FogGodraysShutdown();                        // generation-safe GL teardown (Renderer::Shutdown)
}
