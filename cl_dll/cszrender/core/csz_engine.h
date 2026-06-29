/*
 * csz_engine.h -- CSOZ renderer: upstream SDK/engine type aggregation point
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
// Single aggregation point for upstream SDK types and globals used by cszrender.
// Include order is HLSDK-sensitive; adjust ONLY here if compilation requires.
#include "wrect.h"
#include "cl_dll.h"        // gEngfuncs / gRenderAPI externs (cl_dll/include/cl_dll.h:100-105)
#include "const.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "com_model.h"
#include "studio.h"
#include "r_studioint.h"
#include "ref_params.h"    // ref_viewpass_t, RF_DRAW_WORLD...
#include "render_api.h"    // render_api_t / render_interface_t / PARM_* / TF_*
#include "cvardef.h"

extern engine_studio_api_t IEngineStudio;  // defined in GameStudioModelRenderer.cpp
extern bool g_bHoldingKnife;               // defined in cs_wpn/cs_weapons.cpp (viewmodel right-hand flip quirk)

namespace csz
{
// Shared cvar read with null-safe fallback (one definition for the whole
// renderer; replaces the per-TU anonymous-namespace copies).
inline float ReadCvar( cvar_t *cv, float fallback ) { return ( cv != NULL ) ? cv->value : fallback; }

model_t *WorldModel();                  // gRenderAPI.pfnGetModel( 1 ); NULL when no map
int TexSlotToGlName( int texSlot );     // RenderGetParm( PARM_TEX_TEXNUM, texSlot ); 0 on failure
float ClientTime();                     // gEngfuncs.GetClientTime()

// True when player entity index `idx` is currently DEAD (server-authoritative
// scoreboard flag g_PlayerExtraInfo[idx].dead, fed by the ScoreAttrib message for
// bots AND humans alike). Used to suppress the third-person flashlight lantern on
// corpses: a dead holder's cl_entity_t can retain a stale EF_DIMLIGHT in its last
// networked curstate, so the EF_DIMLIGHT bit alone is not a safe "glow" signal.
// Out-of-range / index 0 (worldspawn) -> false. Lives here because g_PlayerExtraInfo
// is a hud.h global and csz_engine.cpp is the only cszrender TU allowed to touch it.
bool PlayerIsDead( int idx );

// Local player's HUD health (gHUD.m_Health.m_iHealth, maintained by cl_dll/health.cpp
// from the Health usermsg). The composition root threads this into ViewSetup.localHealth
// so the polyblend damage-red shift has a RELIABLE local-health source: the server does
// NOT populate curstate.health for the local player. Lives here because m_Health is a
// hud.h global and csz_engine.cpp is the only cszrender TU allowed to touch it.
int LocalPlayerHealth();

// Active round timing: round start=sunset, end=dawn. false when no round is
// active (idle / between rounds) -> sky falls back to a free-running cycle.
bool RoundTiming( float &outDuration, float &outRemaining );

// fog M1 L4 moonlight Tyndall air-glow master toggle (cvar csz_moonshaft).
// Registered once at HUD init (csz_renderer OnHudInit). Returns 1.0 when the
// enhanced HG forward-scatter glow + cloud-gap gating are on (default), 0.0 for
// the EXACT pre-L4 behaviour (byte-identical clean A/B). Read live each frame by
// the surface in-scatter feed (world/studio) and the screen-space god rays.
float CszMoonShaftEnabled();
void  CszRegisterMoonShaftCvar();   // registers csz_moonshaft (default "0"); OnHudInit
}
