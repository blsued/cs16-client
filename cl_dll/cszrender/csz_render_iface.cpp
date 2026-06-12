/*
 * csz_render_iface.cpp -- CSOZ renderer: handshake, render_api audit, ref_gl check
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
#include "csz_render_iface.h"
#include "csz_renderer.h"
#include "core/csz_engine.h"
#include "core/csz_log.h"
#include "core/csz_fatal.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// render_api_t audit table: every member of the struct, in declaration order
// (common/render_api.h, interface frozen at version 37). Members in the NULL
// whitelist are unfilled reserved slots (notes-renderapi B).
// ---------------------------------------------------------------------------
#define CSZ_RENDER_API_MEMBERS( X ) \
	X( RenderGetParm ) \
	X( GetDetailScaleForTexture ) \
	X( GetExtraParmsForTexture ) \
	X( GetLightStyle ) \
	X( GetDynamicLight ) \
	X( GetEntityLight ) \
	X( LightToTexGamma ) \
	X( GetFrameTime ) \
	X( R_SetCurrentEntity ) \
	X( R_SetCurrentModel ) \
	X( R_FatPVS ) \
	X( R_StoreEfrags ) \
	X( GL_FindTexture ) \
	X( GL_TextureName ) \
	X( GL_TextureData ) \
	X( GL_LoadTexture ) \
	X( GL_CreateTexture ) \
	X( GL_LoadTextureArray ) \
	X( GL_CreateTextureArray ) \
	X( GL_FreeTexture ) \
	X( DrawSingleDecal ) \
	X( R_DecalSetupVerts ) \
	X( R_EntityRemoveDecals ) \
	X( AVI_LoadVideo ) \
	X( AVI_GetVideoInfo ) \
	X( AVI_GetVideoFrameNumber ) \
	X( AVI_GetVideoFrame ) \
	X( AVI_UploadRawFrame ) \
	X( AVI_FreeVideo ) \
	X( AVI_IsActive ) \
	X( AVI_StreamSound ) \
	X( AVI_Reserved0 ) \
	X( AVI_Reserved1 ) \
	X( GL_Bind ) \
	X( GL_SelectTexture ) \
	X( GL_LoadTextureMatrix ) \
	X( GL_TexMatrixIdentity ) \
	X( GL_CleanUpTextureUnits ) \
	X( GL_TexGen ) \
	X( GL_TextureTarget ) \
	X( GL_TexCoordArrayMode ) \
	X( GL_GetProcAddress ) \
	X( GL_UpdateTexSize ) \
	X( GL_Reserved0 ) \
	X( GL_Reserved1 ) \
	X( GL_DrawParticles ) \
	X( EnvShot ) \
	X( SPR_LoadExt ) \
	X( LightVec ) \
	X( StudioGetTexture ) \
	X( GetOverviewParms ) \
	X( GetFileByIndex ) \
	X( pfnSaveFile ) \
	X( R_Reserved0 ) \
	X( pfnMemAlloc ) \
	X( pfnMemFree ) \
	X( pfnGetFilesList ) \
	X( pfnFileBufferCRC32 ) \
	X( COM_CompareFileTime ) \
	X( Host_Error ) \
	X( pfnGetModel ) \
	X( pfnTime ) \
	X( Cvar_Set ) \
	X( S_FadeMusicVolume ) \
	X( SetRandomSeed )

namespace
{

bool IsWhitelistedNull( const char *member )
{
	static const char *const kNullWhitelist[] =
	{
		"GL_Reserved0", "GL_Reserved1", "R_Reserved0", "AVI_Reserved0", "AVI_Reserved1"
	};

	for( size_t i = 0; i < sizeof( kNullWhitelist ) / sizeof( kNullWhitelist[0] ); i++ )
	{
		if( strcmp( kNullWhitelist[i], member ) == 0 )
			return true;
	}

	return false;
}

// FATALs on the first non-whitelisted NULL member; logs whitelisted NULLs.
void AuditRenderApi( const render_api_t *api )
{
	int unexpectedNull = 0;
	char reason[128];

#define CSZ_AUDIT_MEMBER( name ) \
	if( api->name == NULL ) \
	{ \
		if( IsWhitelistedNull( #name ) ) \
		{ \
			CSZ_LogInfo( "core", "render_api member %s is NULL (reserved, whitelisted)", #name ); \
		} \
		else \
		{ \
			unexpectedNull++; \
			snprintf( reason, sizeof( reason ), "%s is NULL in render_api_t", #name ); \
			CSZ_FatalInit( "core", reason ); \
		} \
	}
	CSZ_RENDER_API_MEMBERS( CSZ_AUDIT_MEMBER )
#undef CSZ_AUDIT_MEMBER

	CSZ_LogInfo( "core", "render_api audit OK (unexpected NULL: %d)", unexpectedNull );
}

// ---------------------------------------------------------------------------
// Static trampolines registered into render_interface_t (engine -> renderer).
// ---------------------------------------------------------------------------
int RenderFrameThunk( const struct ref_viewpass_s *rvp )
{
	return csz::g_renderer.RenderFrame( rvp );
}

void BuildLightmapsThunk( void )
{
	csz::g_renderer.BuildLightmapsCallback();
}

void ModProcessUserDataThunk( struct model_s *mod, qboolean create, const byte *buffer )
{
	csz::g_renderer.ProcessUserData( mod, create, buffer );
}

byte *GetCurrentVisThunk( void )
{
	return csz::g_renderer.GetCurrentVis();
}

void NewMapThunk( void )
{
	csz::g_renderer.NewMap();
}

void ClearSceneThunk( void )
{
	csz::g_renderer.ClearScene();
}

}

extern "C"
{

int CSZ_GetRenderInterface( int version, struct render_api_s *renderfuncs, struct render_interface_s *callback )
{
	// 1) Version gate: hard mismatch is an explicit failure (spec 3.2);
	//    never fall back silently (notes-mechanisms f-1).
	if( version != CL_RENDER_INTERFACE_VERSION )
		CSZ_FatalInit( "core", "render interface version mismatch (engine != 37)" );

	if( renderfuncs == NULL || callback == NULL )
		CSZ_FatalInit( "core", "render interface handshake got NULL table pointer" );

	// 2) Audit the engine-provided function table (FATAL inside on bad NULL).
	AuditRenderApi( renderfuncs );

	// 3) Active renderer must be ref_gl: full-frame takeover renders GLSL in
	//    the engine's GL context (notes-renderapi D; PARM_GL_CONTEXT_TYPE is
	//    ambiguous under ref_soft, so the loaded-refdll cvar is the truth).
	{
		const char *refdll = gEngfuncs.pfnGetCvarString( "r_refdll_loaded" );

		if( refdll == NULL || strcmp( refdll, "gl" ) != 0 )
			CSZ_FatalInit( "core", "active renderer is not ref_gl; launch with -ref gl" );

		CSZ_LogInfo( "core", "active renderer check OK (r_refdll_loaded=%s)", refdll );
	}

	// 4) Register our callbacks. Unset members stay NULL; the engine checks
	//    every slot before calling (notes-renderapi A).
	memset( callback, 0, sizeof( *callback ) );
	callback->version = CL_RENDER_INTERFACE_VERSION;
	callback->GL_RenderFrame = RenderFrameThunk;
	callback->GL_BuildLightmaps = BuildLightmapsThunk;
	callback->Mod_ProcessUserData = ModProcessUserDataThunk;
	callback->Mod_GetCurrentVis = GetCurrentVisThunk;
	callback->R_NewMap = NewMapThunk;
	callback->R_ClearScene = ClearSceneThunk;

	// 5) Hand state to the renderer (GL init stays lazy until first frame).
	csz::g_renderer.OnHandshake( renderfuncs );
	CSZ_LogInfo( "core", "takeover handshake complete" );
	return 1;
}

void CSZ_HudInit( void )
{
	csz::g_renderer.OnHudInit();
}

void CSZ_VidInit( void )
{
	csz::g_renderer.OnVidInit();
}

void CSZ_Shutdown( void )
{
	csz::g_renderer.Shutdown();
}

void CSZ_AddEntity( int type, struct cl_entity_s *ent )
{
	csz::g_renderer.AddEntity( type, ent );
}

int CSZ_MotdContentIsBlank( const char *text )
{
	// Whitespace-only counts as blank: ReGameDLL appends a '\n' per sent
	// chunk, so an empty motd.txt still arrives as "\n".
	if( text == NULL )
		return 1;

	for( ; *text != '\0'; text++ )
	{
		if( *text != '\n' && *text != '\r' && *text != ' ' && *text != '\t' )
			return 0;
	}

	return 1;
}

}
