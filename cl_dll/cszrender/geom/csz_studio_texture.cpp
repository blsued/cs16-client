/*
 * csz_studio_texture.cpp -- CSOZ renderer: layered external studio texture resolution
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original work written for
 * CSOZ; no code in this file is copied or translated from PrimeXT, Paranoia,
 * Trinity, retail/leaked sources, or any other license-tainted source
 * (see csoz docs/provenance.md, section 6).
 *
 * The texture.ini conventions ([<model>] sections mapping embedded texture
 * names to files under models/texture/, '#'-named placeholder textures) are
 * data-format facts observed from the deployed BTE/CSO-lineage asset pack
 * itself (interoperability facts, no foreign code involved).
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
#include "csz_studio_texture.h"
#include "csz_studio_mesh.h"
#include "../core/csz_log.h"

#include <new>
#include <stdio.h>
#include <string.h>

namespace csz
{

namespace
{

// ---------------------------------------------------------------------------
// texture.ini storage. One flat entry table, linear lookup: resolution only
// runs at model GPU-build time (rare), the deployed file is ~750 lines.
// ---------------------------------------------------------------------------

struct IniEntry
{
	char section[64];	// lowercase, no brackets
	char key[64];		// lowercase texture name
	char value[160];	// external file under models/texture/ (case as authored)
};

IniEntry *s_ini;
int s_iniCount;
bool s_iniLoaded;

// Runtime override set (layer 1).
ViewmodelTextureOverride s_overrides[kMaxViewmodelOverrides];
int s_overrideCount;

cvar_t *s_devArmSkin;
char s_devArmSkinApplied[64];

// Warn-once dedup registry (load failures / missing files).
const int kMaxWarned = 64;
char s_warned[kMaxWarned][96];
int s_warnedCount;

bool WarnOnce( const char *what )
{
	for( int i = 0; i < s_warnedCount; i++ )
	{
		if( !strcmp( s_warned[i], what ))
			return false;
	}

	if( s_warnedCount >= kMaxWarned )
		return false;	// registry full: stay silent rather than spam

	strncpy( s_warned[s_warnedCount], what, sizeof( s_warned[0] ) - 1 );
	s_warned[s_warnedCount][sizeof( s_warned[0] ) - 1] = '\0';
	s_warnedCount++;
	return true;
}

void StrLower( char *s )
{
	for( ; *s; s++ )
	{
		if( *s >= 'A' && *s <= 'Z' )
			*s += 'a' - 'A';
	}
}

// Trims leading/trailing whitespace in place, returns the start.
char *Trim( char *s )
{
	while( *s == ' ' || *s == '\t' || *s == '\r' )
		s++;

	char *end = s + strlen( s );

	while( end > s && ( end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ))
		*--end = '\0';

	return s;
}

// Parses one ini text buffer into the entry table (appending; later files
// and later duplicate keys win by being matched first in reverse lookup).
void ParseIniBuffer( char *text, const char *fileTag )
{
	char section[64] = "";
	char *line = text;
	int added = 0;

	while( line != NULL && *line != '\0' )
	{
		char *next = strchr( line, '\n' );

		if( next != NULL )
			*next++ = '\0';

		char *t = Trim( line );

		// Comments: ';' or '//' only. '#' is NOT a comment marker here --
		// CSO texture names routinely start with '#' (e.g. "#M_S.bmp").
		if( t[0] == '\0' || t[0] == ';' || ( t[0] == '/' && t[1] == '/' ))
		{
			line = next;
			continue;
		}

		if( t[0] == '[' )
		{
			char *close = strchr( t, ']' );

			if( close != NULL )
			{
				*close = '\0';
				strncpy( section, Trim( t + 1 ), sizeof( section ) - 1 );
				section[sizeof( section ) - 1] = '\0';
				StrLower( section );
			}

			line = next;
			continue;
		}

		char *eq = strchr( t, '=' );

		if( eq != NULL && section[0] != '\0' )
		{
			*eq = '\0';
			char *key = Trim( t );
			char *val = Trim( eq + 1 );

			if( key[0] != '\0' && val[0] != '\0' && s_iniCount < 4096 )
			{
				IniEntry &e = s_ini[s_iniCount++];

				strncpy( e.section, section, sizeof( e.section ) - 1 );
				e.section[sizeof( e.section ) - 1] = '\0';
				strncpy( e.key, key, sizeof( e.key ) - 1 );
				e.key[sizeof( e.key ) - 1] = '\0';
				StrLower( e.key );
				strncpy( e.value, val, sizeof( e.value ) - 1 );
				e.value[sizeof( e.value ) - 1] = '\0';
				added++;
			}
		}

		line = next;
	}

	CSZ_LogInfo( "studio", "%s: %d texture mapping entries", fileTag, added );
}

void EnsureIniLoaded()
{
	if( s_iniLoaded )
		return;

	s_iniLoaded = true;

	// Two files: the asset pack's own texture.ini plus the CSOZ repo-owned
	// extension texture_csoz.ini (arm sets, user-authored custom weapons).
	static const char *kFiles[2] = { "texture.ini", "texture_csoz.ini" };
	byte *raw[2] = { NULL, NULL };
	int lens[2] = { 0, 0 };
	int entryCap = 0;

	for( int i = 0; i < 2; i++ )
	{
		raw[i] = gEngfuncs.COM_LoadFile( kFiles[i], 5, &lens[i] );

		if( raw[i] != NULL )
		{
			// '=' count is a safe upper bound on entries.
			for( int k = 0; k < lens[i]; k++ )
			{
				if( raw[i][k] == '=' )
					entryCap++;
			}
		}
	}

	if( entryCap > 0 )
	{
		if( entryCap > 4096 )
			entryCap = 4096;

		s_ini = new( std::nothrow ) IniEntry[entryCap];

		if( s_ini == NULL )
		{
			CSZ_LogError( "studio", "out of memory for texture.ini table (%d entries)", entryCap );
			entryCap = 0;
		}
	}

	for( int i = 0; i < 2; i++ )
	{
		if( raw[i] == NULL )
			continue;

		if( s_ini != NULL )
		{
			// COM_LoadFile buffers are NUL-terminated by the engine; parse a
			// private copy because parsing splits lines in place.
			char *copy = new( std::nothrow ) char[lens[i] + 1];

			if( copy != NULL )
			{
				memcpy( copy, raw[i], lens[i] );
				copy[lens[i]] = '\0';
				ParseIniBuffer( copy, kFiles[i] );
				delete[] copy;
			}
		}

		gEngfuncs.COM_FreeFile( raw[i] );
	}

	if( s_ini == NULL )
		CSZ_LogInfo( "studio", "no texture.ini mappings present" );
}

// Reverse linear scan so texture_csoz.ini (parsed last) and later duplicate
// lines override earlier ones.
const char *IniValue( const char *sectionLower, const char *keyLower )
{
	for( int i = s_iniCount - 1; i >= 0; i-- )
	{
		if( !strcmp( s_ini[i].section, sectionLower ) && !strcmp( s_ini[i].key, keyLower ))
			return s_ini[i].value;
	}

	return NULL;
}

// Extracts the lowercase base name (no path, no extension) of a model name.
void BaseName( const char *name, char *out, int outSize )
{
	const char *slash = strrchr( name, '/' );
	const char *bslash = strrchr( name, '\\' );

	if( bslash != NULL && ( slash == NULL || bslash > slash ))
		slash = bslash;

	const char *start = ( slash != NULL ) ? slash + 1 : name;
	int i = 0;

	for( ; start[i] != '\0' && i < outSize - 1; i++ )
		out[i] = start[i];

	out[i] = '\0';

	char *dot = strrchr( out, '.' );

	if( dot != NULL )
		*dot = '\0';

	StrLower( out );
}

bool HasModelPrefix( const char *base )
{
	return ( base[0] == 'v' || base[0] == 'p' || base[0] == 'w' ) && base[1] == '_';
}

// texture.ini layer: section candidates per spec 4.3.1 / pack convention, in
// order: disk base name; disk base minus v_/p_/w_ (the pack authors one
// section per weapon covering its v_/p_/w_ trio); internal studiohdr name
// base; internal base minus prefix. First section hit wins.
const char *IniLookup( model_t *mod, const studiohdr_t *hdr, const char *texKeyLower )
{
	EnsureIniLoaded();

	if( s_iniCount == 0 )
		return NULL;

	char candidates[4][64];
	int numCand = 0;

	if( mod != NULL && mod->name[0] != '\0' )
	{
		BaseName( mod->name, candidates[numCand], sizeof( candidates[0] ));
		numCand++;

		if( HasModelPrefix( candidates[numCand - 1] ))
		{
			strncpy( candidates[numCand], candidates[numCand - 1] + 2, sizeof( candidates[0] ) - 1 );
			candidates[numCand][sizeof( candidates[0] ) - 1] = '\0';
			numCand++;
		}
	}

	if( hdr != NULL && hdr->name[0] != '\0' )
	{
		BaseName( hdr->name, candidates[numCand], sizeof( candidates[0] ));

		if( strcmp( candidates[numCand], candidates[0] ) != 0 )
		{
			numCand++;

			if( HasModelPrefix( candidates[numCand - 1] ))
			{
				strncpy( candidates[numCand], candidates[numCand - 1] + 2, sizeof( candidates[0] ) - 1 );
				candidates[numCand][sizeof( candidates[0] ) - 1] = '\0';
				numCand++;
			}
		}
	}

	for( int c = 0; c < numCand; c++ )
	{
		const char *v = IniValue( candidates[c], texKeyLower );

		if( v != NULL )
			return v;
	}

	return NULL;
}

bool IsViewmodel( model_t *mod )
{
	if( mod == NULL )
		return false;

	char base[64];

	BaseName( mod->name, base, sizeof( base ));
	return base[0] == 'v' && base[1] == '_';
}

const char *OverrideLookup( const char *texKeyLower )
{
	for( int i = 0; i < s_overrideCount; i++ )
	{
		if( !strcmp( s_overrides[i].texName, texKeyLower ))
			return s_overrides[i].file;
	}

	return NULL;
}

// Loads models/texture/<file> through the engine texture manager (dedups by
// name). Returns the engine slot, 0 on failure; *w/*h get the real size.
int LoadExternalTexture( const char *file, int *w, int *h )
{
	if( gRenderAPI.GL_LoadTexture == NULL || gRenderAPI.RenderGetParm == NULL )
		return 0;

	char path[224];

	snprintf( path, sizeof( path ), "models/texture/%s", file );

	int slot = gRenderAPI.GL_LoadTexture( path, NULL, 0, 0 );

	if( slot == 0 )
		return 0;

	int tw = (int)gRenderAPI.RenderGetParm( PARM_TEX_WIDTH, slot );
	int th = (int)gRenderAPI.RenderGetParm( PARM_TEX_HEIGHT, slot );

	if( tw <= 0 || th <= 0 )
		return 0;	// no queryable size: unusable as a UV basis

	*w = tw;
	*h = th;
	return slot;
}

}

void SetViewmodelTextureOverrides( const ViewmodelTextureOverride *table, int count )
{
	if( count > kMaxViewmodelOverrides )
		count = kMaxViewmodelOverrides;

	s_overrideCount = 0;

	for( int i = 0; i < count; i++ )
	{
		s_overrides[s_overrideCount] = table[i];
		StrLower( s_overrides[s_overrideCount].texName );
		s_overrideCount++;
	}

	// Cached meshes bake texture slots and UV scale; rebuild them all lazily
	// with the new resolution (cheap, happens once per set change).
	FreeAll();
	CSZ_LogInfo( "studio", "viewmodel texture overrides set: %d entries", s_overrideCount );
}

int ResolveStudioTexture( model_t *mod, const studiohdr_t *hdr, const char *texName,
	int *width, int *height, const char **source )
{
	char key[64];

	strncpy( key, texName, sizeof( key ) - 1 );
	key[sizeof( key ) - 1] = '\0';
	StrLower( key );

	// Ordered candidates, top layer first; a failed load falls through.
	const char *files[3];
	const char *tags[3];
	int num = 0;

	if( s_overrideCount > 0 && IsViewmodel( mod ))
	{
		const char *o = OverrideLookup( key );

		if( o != NULL )
		{
			files[num] = o;
			tags[num] = "override";
			num++;
		}
	}

	const char *ini = IniLookup( mod, hdr, key );

	if( ini != NULL )
	{
		files[num] = ini;
		tags[num] = "texture.ini";
		num++;
	}

	if( texName[0] == '#' )
	{
		// CSO '#' placeholder: the embedded entry is a tiny dummy and the
		// real pixels ship as models/texture/<embedded name>.
		files[num] = texName;
		tags[num] = "auto-#";
		num++;
	}

	for( int i = 0; i < num; i++ )
	{
		int slot = LoadExternalTexture( files[i], width, height );

		if( slot != 0 )
		{
			*source = tags[i];
			return slot;
		}

		if( WarnOnce( files[i] ))
		{
			CSZ_LogWarn( "studio", "%s: %s texture '%s' -> models/texture/%s failed to load; falling back",
				hdr != NULL ? hdr->name : "?", tags[i], texName, files[i] );
		}
	}

	return 0;	// caller keeps the embedded texture (stock behavior)
}

void RegisterStudioTextureCvars()
{
	if( s_devArmSkin == NULL )
		s_devArmSkin = gEngfuncs.pfnRegisterVariable( "csz_dev_armskin", "", FCVAR_CLIENTDLL );
}

void StudioTexturePollDevCvars()
{
	if( s_devArmSkin == NULL || s_devArmSkin->string == NULL )
		return;

	const char *want = s_devArmSkin->string;

	if( !strcmp( want, "0" ))
		want = "";

	if( !strcmp( want, s_devArmSkinApplied ))
		return;

	strncpy( s_devArmSkinApplied, want, sizeof( s_devArmSkinApplied ) - 1 );
	s_devArmSkinApplied[sizeof( s_devArmSkinApplied ) - 1] = '\0';

	if( want[0] == '\0' )
	{
		SetViewmodelTextureOverrides( NULL, 0 );
		return;
	}

	// Arm set convention: ini section "[arms:<set>]" maps arm texture names
	// to variant files (see csoz docs/custom-weapon-textures.md).
	EnsureIniLoaded();

	char section[80];

	snprintf( section, sizeof( section ), "arms:%s", want );
	StrLower( section );

	ViewmodelTextureOverride table[kMaxViewmodelOverrides];
	int count = 0;

	for( int i = 0; i < s_iniCount && count < kMaxViewmodelOverrides; i++ )
	{
		if( strcmp( s_ini[i].section, section ) != 0 )
			continue;

		// Later duplicate keys already shadow earlier ones in lookup order;
		// here keep the first hit per key (reverse iteration not needed for
		// the small dev table - last writer wins via OverrideLookup order).
		strncpy( table[count].texName, s_ini[i].key, sizeof( table[0].texName ) - 1 );
		table[count].texName[sizeof( table[0].texName ) - 1] = '\0';
		strncpy( table[count].file, s_ini[i].value, sizeof( table[0].file ) - 1 );
		table[count].file[sizeof( table[0].file ) - 1] = '\0';
		count++;
	}

	if( count == 0 && WarnOnce( section ))
		CSZ_LogWarn( "studio", "csz_dev_armskin '%s': no [%s] section in texture.ini/texture_csoz.ini", want, section );

	SetViewmodelTextureOverrides( table, count );
	CSZ_LogInfo( "studio", "csz_dev_armskin '%s': %d arm texture overrides applied", want, count );
}

}
