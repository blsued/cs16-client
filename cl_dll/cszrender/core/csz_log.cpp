/*
 * csz_log.cpp -- CSOZ renderer: logging facade implementation
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
#include "csz_log.h"
#include "csz_engine.h"

#include <stdio.h>
#include <stdarg.h>

namespace csz
{

// This file is the ONLY place in cszrender allowed to call
// gEngfuncs.Con_Printf directly (code-standards 2.6 R8).
void Log( LogLevel level, const char *subsystem, const char *fmt, ... )
{
	static const char *const kLevelNames[] = { "FATAL", "Error", "Warn", "Info", "Dev" };
	char message[1024];
	va_list args;

	if( level == kLogDev )
	{
		if( gEngfuncs.pfnGetCvarFloat == NULL || gEngfuncs.pfnGetCvarFloat( "developer" ) < 1.0f )
			return;
	}

	va_start( args, fmt );
	vsnprintf( message, sizeof( message ), fmt, args );
	va_end( args );

	if( gEngfuncs.Con_Printf != NULL )
		gEngfuncs.Con_Printf( "[CSZ:%s] %s: %s\n", subsystem, kLevelNames[level], message );
}

}
