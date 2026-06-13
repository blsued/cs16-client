/*
 * csz_fog.h -- CSOZ renderer: client-side ambience state (fog/night/moon)
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
#include "../core/csz_ambience_types.h"
namespace csz
{
// Client-side ambience state. The ONLY production writer is the server "CSZ"
// envelope (spec 3.2); there is no enable/disable cvar (A-class, spec 4.1).
// csz_devfog/csz_devtint/csz_devmoon (A1) and csz_devmoonlight (A4) exist
// solely in CSZ_DEV_TOOLS builds for pre-A5 verification (compiled out via C9).
class FogController
{
public:
	void Reset();                          // map change / disconnect -> Neutral
	// payload = AMBIENCE cmd body (45 bytes, layout 2.6), cmd/version already
	// stripped by the dispatcher. Logs decoded values once at Info level.
	void OnAmbienceEnvelope( const unsigned char *payload, int size );
	const AmbienceParams &Current() const;
	void RegisterDevCommands();            // no-op unless CSZ_DEV_TOOLS
};
extern FogController g_fog;
}
