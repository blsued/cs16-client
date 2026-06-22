/*
 * csz_fog_net.h -- CSOZ renderer: server CszFog channel decoder (fog M1 Step 6)
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
// Decode + apply an incoming server "CszFog" usermsg (fog M1 Step 6, spec 4.6').
// payload/size are the raw usermsg bytes (MsgFunc_CszFog forwards them). The
// decode is versioned and length-tolerant: it reads up to min(bytesForVersion,
// size), defaults the rest, and ignores trailing extra bytes (forward-compat).
// Malformed input (payload==NULL, size<5, or version==0) is logged and IGNORED
// -- the prior fog state is kept (no null/silent fallback, spec line 346).
void CszFogOnMessage( const unsigned char *payload, int size );

#ifdef CSZ_DEV_TOOLS
// Registers csz_devfognet_test: a self-contained protocol/contract test that
// feeds crafted byte buffers through CszFogOnMessage and asserts the decoded
// AmbienceParams + the precedence latch (compiled out of release via C9).
void CszFogNetRegisterDevCommands();
#endif
}
