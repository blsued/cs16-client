/*
 * csz_render_iface.h -- CSOZ renderer: C entry points for upstream hook sites
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
// C entry points called from upstream hook sites (cdll_int.cpp / entity.cpp).
// Composition root: the .cpp may include core/ + geom/ + lighting/.
// Upstream files may reference ONLY these CSZ_* symbols (code-standards 2.2).
struct render_api_s;
struct render_interface_s;
struct cl_entity_s;

extern "C"
{
// Called inside HUD_GetRenderInterface after upstream copies gRenderAPI.
// Verifies version == 37, audits the render_api_t table (NULL members),
// checks cvar r_refdll_loaded == "gl", fills *callback with CSZ callbacks.
// Hard failure does not return (CSZ_FatalInit). Returns 1 on success.
int CSZ_GetRenderInterface( int version, struct render_api_s *renderfuncs, struct render_interface_s *callback );
// V1 (M0, v18 0.3.8): returns 1 when r_refdll_loaded=="csz" was detected during the
// render-interface handshake -- the legacy takeover is then fully disabled by contract
// (zero callbacks registered) and the "handshake never ran" watchdog must not fatal.
int CSZ_V1TakeoverDisabled( void );
void CSZ_HudInit( void );    // from HUD_Init: register cvars/commands; FATAL if handshake never ran
void CSZ_VidInit( void );    // from HUD_VidInit: invalidate ALL GPU resources (vid_restart safety)
void CSZ_Shutdown( void );   // from HUD_Shutdown: destroy GL objects and caches
void CSZ_AddEntity( int type, struct cl_entity_s *ent );  // from HUD_AddEntity (wired in T3)
// From CHud::MsgFunc_CszFog: raw bytes of a server "CszFog" usermsg (Step 6,
// spec 4.6'); decoded + applied to the server-authoritative black-fog ambience.
void CSZ_OnCszFogMessage( const unsigned char *payload, int size );
// From CHudMOTD::MsgFunc_MOTD: CSOZ ships an empty motd.txt (no join
// announcement window, M1 defect batch 2 #14); returns nonzero when the
// assembled MOTD has no printable content so the HUD skips showing it
// (an empty MOTD would still draw the window frame otherwise).
int CSZ_MotdContentIsBlank( const char *text );
}
