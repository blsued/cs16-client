/*
 * csz_glcaps.cpp -- CSOZ renderer: GL capability probe implementation
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
#include "csz_glcaps.h"
#include "csz_glfuncs.h"
#include "csz_log.h"
#include "csz_fatal.h"

#include <stdio.h>
#include <string.h>

namespace csz
{

static GlCaps s_caps;
static bool s_probed = false;
static bool s_haveTimerQuery = false;

// One-time probe: can this context actually run a GL_TIME_ELAPSED query? The GL
// loader binds glBeginQuery/glEndQuery unconditionally, so on a context without
// ARB_timer_query the entry points exist but raise GL_INVALID_ENUM. Run a real
// throwaway begin/end and check glGetError; clean -> latch true. Must be called
// with a drained error queue (ProbeGlCaps drains right before).
static void ProbeTimerQuery()
{
	s_haveTimerQuery = false;

	if( glGenQueries == NULL || glBeginQuery == NULL || glEndQuery == NULL || glDeleteQueries == NULL )
		return;

	GLuint q = 0;
	glGenQueries( 1, &q );

	if( q == 0 )
		return;

	glBeginQuery( GL_TIME_ELAPSED, q );
	glEndQuery( GL_TIME_ELAPSED );

	GLenum err = glGetError();
	glDeleteQueries( 1, &q );

	// Drain any residue the throwaway query left so later users start clean.
	while( glGetError() != GL_NO_ERROR ) { }

	s_haveTimerQuery = ( err == GL_NO_ERROR );
}

static void CopyGlString( char *dst, size_t dstSize, GLenum name )
{
	const char *value = (const char *)glGetString( name );

	if( value == NULL )
		value = "<null>";

	snprintf( dst, dstSize, "%s", value );
}

bool ProbeGlCaps()
{
	char missing[128];
	char reason[256];
	GLint value;

	if( s_probed )
		return true;

	if( !LoadGlFunctions( missing, sizeof( missing ) ) )
	{
		snprintf( reason, sizeof( reason ), "required GL function missing: %s", missing );
		CSZ_FatalInit( "glcaps", reason );
	}

	memset( &s_caps, 0, sizeof( s_caps ) );
	CopyGlString( s_caps.versionString, sizeof( s_caps.versionString ), GL_VERSION );
	CopyGlString( s_caps.rendererString, sizeof( s_caps.rendererString ), GL_RENDERER );

	// GL_MAJOR/MINOR_VERSION need GL >= 3.0; fall back to parsing the
	// version string on older contexts (the query raises GL_INVALID_ENUM).
	value = 0;
	glGetIntegerv( GL_MAJOR_VERSION, &value );
	s_caps.major = (int)value;
	value = 0;
	glGetIntegerv( GL_MINOR_VERSION, &value );
	s_caps.minor = (int)value;

	if( glGetError() != GL_NO_ERROR || s_caps.major == 0 )
		sscanf( s_caps.versionString, "%d.%d", &s_caps.major, &s_caps.minor );

	// GL_CONTEXT_PROFILE_MASK needs GL >= 3.2; keep 0 on older contexts.
	value = 0;
	glGetIntegerv( GL_CONTEXT_PROFILE_MASK, &value );

	if( glGetError() == GL_NO_ERROR )
		s_caps.profileMask = (int)value;

	value = 0;
	glGetIntegerv( GL_MAX_TEXTURE_SIZE, &value );
	s_caps.maxTextureSize = (int)value;

	value = 0;
	glGetIntegerv( GL_MAX_VERTEX_UNIFORM_COMPONENTS, &value );
	s_caps.maxVertexUniformComponents = (int)value;

	// Drain any leftover probe errors so later glGetError users start clean.
	while( glGetError() != GL_NO_ERROR ) { }

	// GL-ERR-2: latch whether GL_TIME_ELAPSED queries are usable (drained queue
	// required; this leaves it drained again).
	ProbeTimerQuery();

	if( s_caps.maxTextureSize < 1024 )
	{
		snprintf( reason, sizeof( reason ), "GL_MAX_TEXTURE_SIZE too small: %d (need >= 1024)", s_caps.maxTextureSize );
		CSZ_FatalInit( "glcaps", reason );
	}

	if( s_caps.maxVertexUniformComponents < 1664 )
	{
		snprintf( reason, sizeof( reason ), "GL_MAX_VERTEX_UNIFORM_COMPONENTS too small: %d (need >= 1664)", s_caps.maxVertexUniformComponents );
		CSZ_FatalInit( "glcaps", reason );
	}

	CSZ_LogInfo( "glcaps", "GL %s | %s | profile=0x%x | maxtex=%d | maxvtxuniform=%d | timerquery=%d",
		s_caps.versionString, s_caps.rendererString, s_caps.profileMask,
		s_caps.maxTextureSize, s_caps.maxVertexUniformComponents, s_haveTimerQuery ? 1 : 0 );

	s_probed = true;
	return true;
}

const GlCaps &Caps()
{
	return s_caps;
}

bool HaveTimerQuery()
{
	return s_haveTimerQuery;
}

static int s_gpuGeneration = 0;

int GpuGeneration()
{
	return s_gpuGeneration;
}

void BumpGpuGeneration()
{
	s_gpuGeneration++;
}

}
