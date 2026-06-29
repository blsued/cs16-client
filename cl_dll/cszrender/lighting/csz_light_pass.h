/*
 * csz_light_pass.h -- CSOZ renderer: light pass orchestration
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
struct cl_entity_s;
namespace csz
{
struct ViewSetup;
// Studio entity list is passed down from the composition root (lighting never
// reads the Renderer; one-way dependency rule).
void RenderShadowMaps( const ViewSetup &mainView, cl_entity_s *const *studioEnts, int studioCount,
	cl_entity_s *localPlayerShadow ); // T7; honors cvar csz_light_shadow (B-class, default 1).
	// localPlayerShadow (M2, may be NULL): the local first-person body, filtered out of the
	// color/lit passes (camera-inside-own-head) but ingested SHADOW-ONLY into the depth pass
	// so it casts into the spot shadow map. Viewmodel shadow is a separate cluster.
void RunLightPasses( const ViewSetup &mainView, cl_entity_s *const *studioEnts, int studioCount,
	cl_entity_s *const *brushEnts, int brushCount );   // T6; per active light: cull -> world+brush+studio additive
void CollectRealFlashlights( const ViewSetup &mainView );  // L6c: feed the per-flashlight state table from live player state (local eye + other players' EF_DIMLIGHT) each frame; dev csz_flashlight_test overrides
void RegisterLightingCommands();                     // command csz_testspot + cvar csz_testlight (demo spot at T spawn) + cvar csz_light_shadow (T7)
}
