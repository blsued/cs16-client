/*
 * csz_glfuncs.cpp -- CSOZ renderer: GL function loader via GL_GetProcAddress
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
#include "csz_engine.h"
#include "csz_glfuncs.h"

#include <stdio.h>

// Definitions for every pointer declared by the X-macro table.
#define CSZ_GL_DEFINE( type, name ) type name = NULL;
CSZ_GL_FUNCTIONS( CSZ_GL_DEFINE )
#undef CSZ_GL_DEFINE

namespace csz
{

bool LoadGlFunctions( char *missingOut, int missingOutSize )
{
	struct GlFuncEntry
	{
		const char *name;
		void **slot;
	};
	// GL1.1 names (glClear etc.) go through GL_GetProcAddress as well: the
	// engine implementation has an opengl32 fallback (notes-renderapi B).
	// If a name still comes back NULL the caller FATALs - explicit failure
	// by design, no silent degradation (spec 3.2).
	static const GlFuncEntry kTable[] =
	{
#define CSZ_GL_ENTRY( type, name ) { #name, (void **)&name },
		CSZ_GL_FUNCTIONS( CSZ_GL_ENTRY )
#undef CSZ_GL_ENTRY
	};

	if( gRenderAPI.GL_GetProcAddress == NULL )
	{
		snprintf( missingOut, (size_t)missingOutSize, "%s", "GL_GetProcAddress (render_api_t)" );
		return false;
	}

	for( size_t i = 0; i < sizeof( kTable ) / sizeof( kTable[0] ); i++ )
	{
		void *proc = gRenderAPI.GL_GetProcAddress( kTable[i].name );

		if( proc == NULL )
		{
			snprintf( missingOut, (size_t)missingOutSize, "%s", kTable[i].name );
			return false;
		}

		*kTable[i].slot = proc;
	}

	return true;
}

}
