/*
 * csz_glfuncs.h -- CSOZ renderer: GL function pointer table (X-macro)
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
// Keep the vendored csz_glcorearb.h from pulling in windows.h: the HLSDK
// already owns the HSPRITE name (typedef int, engine/cdll_int.h) and
// windows.h would redeclare it as a handle type. glcorearb only includes
// windows.h when APIENTRY is undefined, so define it up front; __stdcall
// matches the windows.h definition for 32-bit GL entry points.
#if defined( _WIN32 ) && !defined( APIENTRY )
#define APIENTRY __stdcall
#endif
#include "csz_glcorearb.h"   // vendored; GL_GLEXT_PROTOTYPES must stay undefined

// Every GL entry point used anywhere in cszrender MUST be declared via this
// X-macro and loaded through gRenderAPI.GL_GetProcAddress (notes-client c).
// The full required list is fixed in plan section 2.3; missing any -> FATAL.
// glClearDepth/glDepthRange are the desktop double-precision variants.
// CSZ-PORT: GLES3 uses *f variant (compile-time switch when porting, M2+).
#define CSZ_GL_FUNCTIONS( X ) \
	X( PFNGLGETSTRINGPROC, glGetString ) \
	X( PFNGLGETINTEGERVPROC, glGetIntegerv ) \
	X( PFNGLGETERRORPROC, glGetError ) \
	X( PFNGLENABLEPROC, glEnable ) \
	X( PFNGLDISABLEPROC, glDisable ) \
	X( PFNGLCLEARPROC, glClear ) \
	X( PFNGLCLEARCOLORPROC, glClearColor ) \
	X( PFNGLCLEARDEPTHPROC, glClearDepth ) \
	X( PFNGLVIEWPORTPROC, glViewport ) \
	X( PFNGLSCISSORPROC, glScissor ) \
	X( PFNGLDEPTHFUNCPROC, glDepthFunc ) \
	X( PFNGLDEPTHMASKPROC, glDepthMask ) \
	X( PFNGLDEPTHRANGEPROC, glDepthRange ) \
	X( PFNGLBLENDFUNCPROC, glBlendFunc ) \
	X( PFNGLCULLFACEPROC, glCullFace ) \
	X( PFNGLFRONTFACEPROC, glFrontFace ) \
	X( PFNGLPOLYGONOFFSETPROC, glPolygonOffset ) \
	X( PFNGLCOLORMASKPROC, glColorMask ) \
	X( PFNGLPIXELSTOREIPROC, glPixelStorei ) \
	X( PFNGLGENBUFFERSPROC, glGenBuffers ) \
	X( PFNGLBINDBUFFERPROC, glBindBuffer ) \
	X( PFNGLBUFFERDATAPROC, glBufferData ) \
	X( PFNGLBUFFERSUBDATAPROC, glBufferSubData ) \
	X( PFNGLDELETEBUFFERSPROC, glDeleteBuffers ) \
	X( PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays ) \
	X( PFNGLBINDVERTEXARRAYPROC, glBindVertexArray ) \
	X( PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays ) \
	X( PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray ) \
	X( PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer ) \
	X( PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer ) \
	X( PFNGLDRAWARRAYSPROC, glDrawArrays ) \
	X( PFNGLDRAWELEMENTSPROC, glDrawElements ) \
	X( PFNGLCREATESHADERPROC, glCreateShader ) \
	X( PFNGLSHADERSOURCEPROC, glShaderSource ) \
	X( PFNGLCOMPILESHADERPROC, glCompileShader ) \
	X( PFNGLGETSHADERIVPROC, glGetShaderiv ) \
	X( PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog ) \
	X( PFNGLDELETESHADERPROC, glDeleteShader ) \
	X( PFNGLCREATEPROGRAMPROC, glCreateProgram ) \
	X( PFNGLATTACHSHADERPROC, glAttachShader ) \
	X( PFNGLLINKPROGRAMPROC, glLinkProgram ) \
	X( PFNGLGETPROGRAMIVPROC, glGetProgramiv ) \
	X( PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog ) \
	X( PFNGLUSEPROGRAMPROC, glUseProgram ) \
	X( PFNGLDELETEPROGRAMPROC, glDeleteProgram ) \
	X( PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation ) \
	X( PFNGLUNIFORM1IPROC, glUniform1i ) \
	X( PFNGLUNIFORM1FPROC, glUniform1f ) \
	X( PFNGLUNIFORM2FPROC, glUniform2f ) \
	X( PFNGLUNIFORM3FVPROC, glUniform3fv ) \
	X( PFNGLUNIFORM4FVPROC, glUniform4fv ) \
	X( PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv ) \
	X( PFNGLGENTEXTURESPROC, glGenTextures ) \
	X( PFNGLBINDTEXTUREPROC, glBindTexture ) \
	X( PFNGLTEXIMAGE2DPROC, glTexImage2D ) \
	X( PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D ) \
	X( PFNGLTEXPARAMETERIPROC, glTexParameteri ) \
	X( PFNGLCOPYTEXSUBIMAGE2DPROC, glCopyTexSubImage2D ) \
	X( PFNGLDELETETEXTURESPROC, glDeleteTextures ) \
	X( PFNGLACTIVETEXTUREPROC, glActiveTexture ) \
	X( PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers ) \
	X( PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer ) \
	X( PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D ) \
	X( PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus ) \
	X( PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers ) \
	X( PFNGLDRAWBUFFERSPROC, glDrawBuffers ) \
	X( PFNGLREADBUFFERPROC, glReadBuffer )

#define CSZ_GL_DECLARE( type, name ) extern type name;
CSZ_GL_FUNCTIONS( CSZ_GL_DECLARE )
#undef CSZ_GL_DECLARE

namespace csz
{
// Loads all table entries. On failure writes the first missing name into
// missingOut and returns false (caller escalates to CSZ_FatalInit).
bool LoadGlFunctions( char *missingOut, int missingOutSize );
}
