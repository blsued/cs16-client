/*
 * csz_shader.cpp -- CSOZ renderer: GLSL program helper implementation
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
#include "csz_shader.h"
#include "csz_glfuncs.h"
#include "csz_log.h"
#include "csz_fatal.h"

#include <stdio.h>
#include <string.h>

namespace csz
{

namespace
{

// Prints a GL info log line by line through the CSZ log facade so the FULL
// text reaches the console/engine.log (compile failures must never truncate
// silently, code-standards 2.5).
void LogInfoLogLines( const char *name, const char *what, const char *infoLog )
{
	char line[512];
	const char *cursor = infoLog;

	while( *cursor != '\0' )
	{
		const char *eol = strchr( cursor, '\n' );
		size_t len = ( eol != NULL ) ? (size_t)( eol - cursor ) : strlen( cursor );

		if( len >= sizeof( line ))
			len = sizeof( line ) - 1;

		memcpy( line, cursor, len );
		line[len] = '\0';

		if( len > 0 )
			CSZ_LogError( "shader", "%s %s: %s", name, what, line );

		if( eol == NULL )
			break;

		cursor = eol + 1;
	}
}

// Returns 0 on failure (info log already printed).
GLuint CompileStage( const char *name, GLenum stage, const char *source )
{
	GLuint shader = glCreateShader( stage );
	GLint status = GL_FALSE;

	glShaderSource( shader, 1, &source, NULL );
	glCompileShader( shader );
	glGetShaderiv( shader, GL_COMPILE_STATUS, &status );

	if( status != GL_TRUE )
	{
		char infoLog[4096];
		GLsizei written = 0;

		glGetShaderInfoLog( shader, sizeof( infoLog ), &written, infoLog );
		infoLog[( written > 0 && written < (GLsizei)sizeof( infoLog )) ? written : sizeof( infoLog ) - 1] = '\0';
		LogInfoLogLines( name, ( stage == GL_VERTEX_SHADER ) ? "VS compile failed" : "FS compile failed", infoLog );
		glDeleteShader( shader );
		return 0;
	}

	return shader;
}

}

bool BuildProgram( const char *name, const char *vsSource, const char *fsSource,
                   bool failFatal, ShaderProgram &out )
{
	char reason[256];

	out.program = 0;

	GLuint vs = CompileStage( name, GL_VERTEX_SHADER, vsSource );

	if( vs == 0 )
	{
		if( failFatal )
		{
			snprintf( reason, sizeof( reason ), "shader '%s': vertex stage failed to compile (full info log above)", name );
			CSZ_FatalInit( "shader", reason );
		}
		return false;
	}

	GLuint fs = CompileStage( name, GL_FRAGMENT_SHADER, fsSource );

	if( fs == 0 )
	{
		glDeleteShader( vs );
		if( failFatal )
		{
			snprintf( reason, sizeof( reason ), "shader '%s': fragment stage failed to compile (full info log above)", name );
			CSZ_FatalInit( "shader", reason );
		}
		return false;
	}

	GLuint program = glCreateProgram();
	GLint status = GL_FALSE;

	glAttachShader( program, vs );
	glAttachShader( program, fs );
	glLinkProgram( program );

	// Stages are owned by the program now; flag for deletion either way.
	glDeleteShader( vs );
	glDeleteShader( fs );

	glGetProgramiv( program, GL_LINK_STATUS, &status );

	if( status != GL_TRUE )
	{
		char infoLog[4096];
		GLsizei written = 0;

		glGetProgramInfoLog( program, sizeof( infoLog ), &written, infoLog );
		infoLog[( written > 0 && written < (GLsizei)sizeof( infoLog )) ? written : sizeof( infoLog ) - 1] = '\0';
		LogInfoLogLines( name, "link failed", infoLog );
		glDeleteProgram( program );

		if( failFatal )
		{
			snprintf( reason, sizeof( reason ), "shader '%s': program link failed (full info log above)", name );
			CSZ_FatalInit( "shader", reason );
		}
		return false;
	}

	out.program = program;
	CSZ_LogDev( "shader", "built program '%s' (id=%u)", name, program );
	return true;
}

void DestroyProgram( ShaderProgram &prog )
{
	if( prog.program != 0 )
	{
		glDeleteProgram( prog.program );
		prog.program = 0;
	}
}

int UniformLoc( const ShaderProgram &prog, const char *name )
{
	int loc = glGetUniformLocation( prog.program, name );

	if( loc == -1 )
		CSZ_LogDev( "shader", "uniform '%s' not found in program %u (inactive or optimized out)", name, prog.program );

	return loc;
}

}
