/*
 * csz_weather_types.h -- CSOZ renderer: weather surface-state POD shared with geom
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
 * csoz docs/notes/primext-render-mechanisms-m2.md); implemented by an agent
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
// Per-frame weather surface state: the READ-ONLY contract the world/turb
// shaders read. Deterministic derived numbers, NOT GL objects. Produced by
// weather/ (WeatherRenderer::Update) from the csz_weather* cvars + the
// published ambience tint, and exposed read-only through SurfaceState().
//
// Lives in core so geom never includes weather headers (spec 4.6 one-way rule:
// core -> geom -> lighting -> fog -> weather; geom must not reach downstream).
// The composition root copies it into ViewSetup.weather each frame, and geom
// reads it from the view -- same pattern as ViewSetup.ambience.
struct WeatherSurfaceState
{
	float wetness;       // 0..1 ground wetness from rain (>0 only in rain mode)
	float snowAmount;    // 0..1 snow coverage strength (>0 only in snow mode)
	float snowColor[3];  // linear RGB, already cooled/dimmed toward the night tint
};
}
