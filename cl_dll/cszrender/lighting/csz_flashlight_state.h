/*
 * csz_flashlight_state.h -- CSOZ renderer: decoupled per-flashlight state table (L6b)
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
// L6b decoupled per-flashlight state. This is the SINGLE source of "which players
// currently hold a lit flashlight, where, and how" -- separated from the renderer
// the same way serverFogMask (csz_fog.cpp) decouples fog drive from fog math:
//   producer  -> FlashlightSet()/FlashlightClear()  (one writer per frame)
//   consumer  -> FlashlightPublishToRegistry()       (renderer mirrors into g_lights)
//
// The table is fed each frame by the REAL per-player flashlight producer
// (CollectRealFlashlights in csz_light_pass.cpp): the local player's own beam at
// the view eye when its EF_DIMLIGHT is lit, plus one beam per other player whose
// curstate carries EF_DIMLIGHT. The csz_flashlight_test dev command writes the SAME
// FlashlightSet() seam and OVERRIDES the real feed while a fixture is placed
// (csz_flashlight_real 0 disables the real feed entirely). No renderer change needed.
struct FlashlightState
{
	bool  enabled;     // false clears the owner's beam (same as FlashlightClear)
	int   owner;       // entity/player index; table key (owner identity, not a slot)
	float origin[3];   // muzzle world position
	float angles[3];   // muzzle basis, quake pitch/yaw/roll
	float range;       // far range, world units; <= 0 -> module default
	float fov;         // full cone angle, degrees; <= 0 -> module default
	float color[3];    // linear, intensity premultiplied; all 0 -> warm-white default
	bool  isLocal;     // the local player's OWN beam -> budget top priority
	                   // (server hookup sets this for the viewer's entity)
};

// SERVER HOOKUP POINT (reserved). Upserts one owner's state. Real per-player /
// server-authoritative sync calls this each snapshot; the test command calls it now.
void FlashlightSet( const FlashlightState &st );
void FlashlightClear( int owner );       // drop one owner's beam
void FlashlightClearAll();               // drop every beam (test "off")
int  FlashlightCount();                  // enabled entries (debug/stats)

// Per-frame: mirror every enabled state into g_lights (the render-consumed spot
// registry) and remove entries that went disabled. Called before UpdateMatrices so
// the published lights get matrices + budget tiers the same frame.
void FlashlightPublishToRegistry();
}
