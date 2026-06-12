/*
 * csz_studio_mesh.cpp -- CSOZ renderer: studio mesh GPU cache
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
 * The .mdl tricmd format facts (strip/fan mixed stream, 4 shorts per vertex)
 * come from this fork's own HLSDK-lineage headers and the pinned engine
 * sources (interoperability facts).
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
#include "csz_studio_mesh.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"

#include <new>
#include <stdio.h>
#include <string.h>

namespace csz
{

struct StudioModelGpu
{
	model_t *mod;
	const studiohdr_t *hdr;
	int generation;
	bool valid;
	char name[64];
	StudioMeshGpu *meshes;
	int numMeshes;
	int numVerts;		// stats only
};

namespace
{

const int kMaxModels = 256;

StudioModelGpu s_models[kMaxModels];

// Interleaved vertex: pos3 + normal3 + uv2 (floats) + bone (int) = 36 bytes.
// Attribute locations are the plan 2.4 studio contract.
const int kVertexFloats = 8;
const int kVertexBytes = kVertexFloats * (int)sizeof( float ) + (int)sizeof( int );

struct VertexScratch
{
	float pos[3];
	float normal[3];
	float uv[2];
	int bone;
};

void ReleaseModel( StudioModelGpu &m )
{
	// T1/T2 generation rule: names from an older GPU generation are
	// FORGOTTEN, never deleted (stale names may alias foreign objects).
	bool sameContext = ( m.generation == GpuGeneration());

	if( m.meshes != NULL )
	{
		for( int i = 0; i < m.numMeshes; i++ )
		{
			StudioMeshGpu &mesh = m.meshes[i];

			if( sameContext )
			{
				if( mesh.vao != 0 )
					glDeleteVertexArrays( 1, &mesh.vao );
				if( mesh.vbo != 0 )
					glDeleteBuffers( 1, &mesh.vbo );
				if( mesh.ibo != 0 )
					glDeleteBuffers( 1, &mesh.ibo );
			}

			mesh.vao = mesh.vbo = mesh.ibo = 0;
		}

		delete[] m.meshes;
	}

	m.meshes = NULL;
	m.numMeshes = 0;
	m.numVerts = 0;
	m.mod = NULL;
	m.hdr = NULL;
	m.valid = false;
}

// Decodes one tricmd vertex (4 shorts) into the scratch layout.
void DecodeVertex( const studiohdr_t *hdr, const void *psubRaw, const short *cmd,
	float invW, float invH, VertexScratch &out )
{
	const mstudiomodel_t *psub = (const mstudiomodel_t *)psubRaw;
	const float *pverts = (const float *)((const byte *)hdr + psub->vertindex );
	const float *pnorms = (const float *)((const byte *)hdr + psub->normindex );
	const byte *pvertbone = (const byte *)hdr + psub->vertinfoindex;

	int vi = cmd[0];
	int ni = cmd[1];

	if( vi < 0 || vi >= psub->numverts )
		vi = 0;
	if( ni < 0 || ni >= psub->numnorms )
		ni = 0;

	out.pos[0] = pverts[vi * 3 + 0];
	out.pos[1] = pverts[vi * 3 + 1];
	out.pos[2] = pverts[vi * 3 + 2];
	out.normal[0] = pnorms[ni * 3 + 0];
	out.normal[1] = pnorms[ni * 3 + 1];
	out.normal[2] = pnorms[ni * 3 + 2];
	out.uv[0] = (float)cmd[2] * invW;
	out.uv[1] = (float)cmd[3] * invH;
	out.bone = pvertbone[vi];
}

void AppendVertex( float *dst, int slot, const VertexScratch &v )
{
	float *f = dst + (size_t)slot * ( kVertexBytes / sizeof( float ));

	f[0] = v.pos[0];
	f[1] = v.pos[1];
	f[2] = v.pos[2];
	f[3] = v.normal[0];
	f[4] = v.normal[1];
	f[5] = v.normal[2];
	f[6] = v.uv[0];
	f[7] = v.uv[1];
	memcpy( &f[8], &v.bone, sizeof( int ));	// int attribute aliased into the float stream
}

// CSO-lineage external studio texture ("#"-named placeholder). CSO/BTE
// asset packs embed only a tiny placeholder (4x1 / 8x5 pixels) whose name
// starts with '#' (e.g. "#256256balrog-11_p.bmp", "#M_S.bmp"); the real
// pixels ship as models/texture/<name> and the mesh UV shorts are authored
// against the REAL texture dimensions (verified against the deployed asset
// pack: v_ak47 hands UVs span 0..509 while the embedded entry is 8x5).
// The pinned engine has no such mechanism (ref/gl/gl_studio.c
// R_StudioLoadTexture loads embedded pixels only), so the stock path tiles
// the placeholder -- flat untextured-looking guns and black gloves. This is
// a CSOZ extension; when the external file is absent the embedded
// placeholder stays in use (stock behavior). Returns the engine texture
// slot, or 0 when unavailable; *width/*height get the real dimensions.
int ResolveExternalCsoTexture( const char *texName, int *width, int *height )
{
	if( gRenderAPI.GL_LoadTexture == NULL || gRenderAPI.RenderGetParm == NULL )
		return 0;

	char path[160];

	snprintf( path, sizeof( path ), "models/texture/%s", texName );

	// Engine texture manager dedups by name, so repeated resolves of the
	// same sleeve/skin file across models return one shared slot.
	int slot = gRenderAPI.GL_LoadTexture( path, NULL, 0, 0 );

	if( slot == 0 )
		return 0;

	int w = (int)gRenderAPI.RenderGetParm( PARM_TEX_WIDTH, slot );
	int h = (int)gRenderAPI.RenderGetParm( PARM_TEX_HEIGHT, slot );

	if( w <= 0 || h <= 0 )
		return 0;	// no queryable size: unusable as a UV basis

	*width = w;
	*height = h;
	return slot;
}

// Counts triangles in one tricmd stream (strips and fans mixed).
int CountTriangles( const short *ptricmds )
{
	int tris = 0;
	int i;

	while(( i = *ptricmds++ ) != 0 )
	{
		int n = ( i < 0 ) ? -i : i;

		if( n >= 3 )
			tris += n - 2;

		ptricmds += 4 * n;
	}

	return tris;
}

bool BuildOneMesh( const studiohdr_t *hdr, const mstudiomodel_t *psub,
	const mstudiomesh_t *pmesh, int texW, int texH, StudioMeshGpu &out )
{
	const short *ptricmds = (const short *)((const byte *)hdr + pmesh->triindex );
	int tris = CountTriangles( ptricmds );

	if( tris <= 0 )
	{
		out.indexCount = 0;
		return true;	// empty mesh: legal, nothing to draw
	}

	int vertCount = tris * 3;
	float *verts = new( std::nothrow ) float[(size_t)vertCount * ( kVertexBytes / sizeof( float ))];
	unsigned int *indices = new( std::nothrow ) unsigned int[(size_t)vertCount];

	if( verts == NULL || indices == NULL )
	{
		delete[] verts;
		delete[] indices;
		CSZ_LogError( "studio", "out of memory building mesh for %s", hdr->name );
		return false;
	}

	float invW = ( texW > 0 ) ? 1.0f / (float)texW : 0.0f;
	float invH = ( texH > 0 ) ? 1.0f / (float)texH : 0.0f;
	int cursor = 0;
	int i;

	while(( i = *ptricmds++ ) != 0 )
	{
		bool fan = ( i < 0 );
		int n = fan ? -i : i;
		VertexScratch v0, v1, v2;

		// Walk the primitive's vertex list, expanding to triangles with
		// the same facing the GL strip/fan rules would produce.
		for( int j = 0; j + 2 < n; j++ )
		{
			if( fan )
			{
				DecodeVertex( hdr, psub, ptricmds + 0, invW, invH, v0 );
				DecodeVertex( hdr, psub, ptricmds + ( j + 1 ) * 4, invW, invH, v1 );
				DecodeVertex( hdr, psub, ptricmds + ( j + 2 ) * 4, invW, invH, v2 );
			}
			else if(( j & 1 ) == 0 )
			{
				DecodeVertex( hdr, psub, ptricmds + j * 4, invW, invH, v0 );
				DecodeVertex( hdr, psub, ptricmds + ( j + 1 ) * 4, invW, invH, v1 );
				DecodeVertex( hdr, psub, ptricmds + ( j + 2 ) * 4, invW, invH, v2 );
			}
			else
			{
				// odd strip triangle: swap the first two to keep facing
				DecodeVertex( hdr, psub, ptricmds + ( j + 1 ) * 4, invW, invH, v0 );
				DecodeVertex( hdr, psub, ptricmds + j * 4, invW, invH, v1 );
				DecodeVertex( hdr, psub, ptricmds + ( j + 2 ) * 4, invW, invH, v2 );
			}

			AppendVertex( verts, cursor + 0, v0 );
			AppendVertex( verts, cursor + 1, v1 );
			AppendVertex( verts, cursor + 2, v2 );
			cursor += 3;
		}

		ptricmds += 4 * n;
	}

	for( int k = 0; k < cursor; k++ )
		indices[k] = (unsigned int)k;

	glGenVertexArrays( 1, &out.vao );
	BindVao( out.vao );
	glGenBuffers( 1, &out.vbo );
	glBindBuffer( GL_ARRAY_BUFFER, out.vbo );
	glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)cursor * kVertexBytes ), verts, GL_STATIC_DRAW );
	glGenBuffers( 1, &out.ibo );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, out.ibo );
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)cursor * sizeof( unsigned int )), indices, GL_STATIC_DRAW );

	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, kVertexBytes, (const void *)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 3, GL_FLOAT, GL_FALSE, kVertexBytes, (const void *)( 3 * sizeof( float )));
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 2, GL_FLOAT, GL_FALSE, kVertexBytes, (const void *)( 6 * sizeof( float )));
	glEnableVertexAttribArray( 3 );
	glVertexAttribIPointer( 3, 1, GL_INT, kVertexBytes, (const void *)( 8 * sizeof( float )));

	// VAO captures the element binding; unbind VAO first so the IBO unbind
	// below does not strip it out of the VAO state.
	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );

	delete[] verts;
	delete[] indices;

	out.indexCount = cursor;
	return true;
}

}

StudioModelGpu *GetOrBuild( model_t *mod, studiohdr_t *hdr )
{
	if( mod == NULL || hdr == NULL )
		return NULL;

	StudioModelGpu *slot = NULL;
	StudioModelGpu *freeSlot = NULL;

	for( int i = 0; i < kMaxModels; i++ )
	{
		if( s_models[i].valid && s_models[i].mod == mod )
		{
			slot = &s_models[i];
			break;
		}

		if( !s_models[i].valid && freeSlot == NULL )
			freeSlot = &s_models[i];
	}

	if( slot != NULL )
	{
		if( slot->generation == GpuGeneration() && slot->hdr == hdr )
			return slot;

		ReleaseModel( *slot );	// stale generation or reloaded header: rebuild
	}
	else
	{
		slot = freeSlot;

		if( slot == NULL )
		{
			static bool s_warned;

			if( !s_warned )
			{
				s_warned = true;
				CSZ_LogError( "studio", "model cache full (%d); refusing to build %s", kMaxModels, hdr->name );
			}

			return NULL;
		}
	}

	// --- build all submodel meshes ---
	if( hdr->numbodyparts <= 0 || hdr->numbodyparts > MAXSTUDIOBODYPARTS )
		return NULL;

	const mstudiobodyparts_t *pbodyparts = (const mstudiobodyparts_t *)((const byte *)hdr + hdr->bodypartindex );
	const mstudiotexture_t *ptextures = (const mstudiotexture_t *)((const byte *)hdr + hdr->textureindex );
	const short *pskinref = (const short *)((const byte *)hdr + hdr->skinindex );
	int totalMeshes = 0;

	for( int bp = 0; bp < hdr->numbodyparts; bp++ )
	{
		const mstudiobodyparts_t &part = pbodyparts[bp];

		for( int sm = 0; sm < part.nummodels; sm++ )
			totalMeshes += (( const mstudiomodel_t * )((const byte *)hdr + part.modelindex ))[sm].nummesh;
	}

	if( totalMeshes <= 0 )
		return NULL;

	StudioMeshGpu *meshes = new( std::nothrow ) StudioMeshGpu[totalMeshes];

	if( meshes == NULL )
	{
		CSZ_LogError( "studio", "out of memory building %s", hdr->name );
		return NULL;
	}

	memset( meshes, 0, sizeof( StudioMeshGpu ) * (size_t)totalMeshes );

	int meshCursor = 0;
	int totalVerts = 0;
	int badTexWarned = 0;
	int extTexWarned = 0;

	for( int bp = 0; bp < hdr->numbodyparts; bp++ )
	{
		const mstudiobodyparts_t &part = pbodyparts[bp];
		const mstudiomodel_t *psubs = (const mstudiomodel_t *)((const byte *)hdr + part.modelindex );

		for( int sm = 0; sm < part.nummodels; sm++ )
		{
			const mstudiomodel_t &sub = psubs[sm];
			const mstudiomesh_t *pmeshes = (const mstudiomesh_t *)((const byte *)hdr + sub.meshindex );

			for( int mi = 0; mi < sub.nummesh; mi++ )
			{
				StudioMeshGpu &out = meshes[meshCursor];
				const mstudiomesh_t &mesh = pmeshes[mi];

				// Skin family 0 only (M1); skinref indexes the skin table.
				int texIdx = 0;

				if( hdr->numskinref > 0 && mesh.skinref >= 0 && mesh.skinref < hdr->numskinref )
					texIdx = pskinref[mesh.skinref];

				int texW = 64, texH = 64;

				out.texSlot = 0;
				out.texFlags = 0;

				if( hdr->numtextures > 0 && texIdx >= 0 && texIdx < hdr->numtextures )
				{
					const mstudiotexture_t &tex = ptextures[texIdx];

					// Runtime verification point (plan section 6 step 3):
					// index holds the engine texture slot written by the ref
					// at model load (pinned gl_studio.c R_StudioLoadTexture).
					out.texSlot = tex.index;
					out.texFlags = tex.flags;
					texW = ( tex.width > 0 ) ? tex.width : 64;
					texH = ( tex.height > 0 ) ? tex.height : 64;

					// '#'-named entry: prefer the CSO external real texture
					// (defect batch 2 #11/#13; see ResolveExternalCsoTexture).
					if( tex.name[0] == '#' )
					{
						int extW = 0, extH = 0;
						int extSlot = ResolveExternalCsoTexture( tex.name, &extW, &extH );

						if( extSlot != 0 )
						{
							out.texSlot = extSlot;
							texW = extW;
							texH = extH;
							CSZ_LogDev( "studio", "%s: external texture models/texture/%s (%dx%d, slot %d)",
								hdr->name, tex.name, extW, extH, extSlot );
						}
						else if( extTexWarned++ == 0 )
						{
							CSZ_LogWarn( "studio", "%s: '#' texture %s has no models/texture/ file; using embedded placeholder",
								hdr->name, tex.name );
						}
					}
				}

				if( out.texSlot == 0 && badTexWarned++ == 0 )
					CSZ_LogWarn( "studio", "%s mesh %d has no engine texture slot; using *white", hdr->name, meshCursor );

				out.bodypart = bp;
				out.submodel = sm;

				if( !BuildOneMesh( hdr, &sub, &mesh, texW, texH, out ))
				{
					// Build failure degrades to an empty mesh (Error already
					// logged); keep going so the rest of the model draws.
					out.indexCount = 0;
				}

				totalVerts += out.indexCount;
				meshCursor++;
			}
		}
	}

	slot->mod = mod;
	slot->hdr = hdr;
	slot->generation = GpuGeneration();
	slot->meshes = meshes;
	slot->numMeshes = meshCursor;
	slot->numVerts = totalVerts;
	slot->valid = true;
	strncpy( slot->name, hdr->name, sizeof( slot->name ) - 1 );
	slot->name[sizeof( slot->name ) - 1] = '\0';

	CSZ_LogInfo( "studio", "built model %s: %d meshes, %d verts", slot->name, slot->numMeshes, slot->numVerts );
	return slot;
}

const StudioMeshGpu *Meshes( const StudioModelGpu *gpu, int *count )
{
	if( gpu == NULL || !gpu->valid )
	{
		*count = 0;
		return NULL;
	}

	*count = gpu->numMeshes;
	return gpu->meshes;
}

void Free( model_t *mod )
{
	for( int i = 0; i < kMaxModels; i++ )
	{
		if( s_models[i].valid && s_models[i].mod == mod )
		{
			ReleaseModel( s_models[i] );
			return;
		}
	}
}

void FreeAll()
{
	for( int i = 0; i < kMaxModels; i++ )
	{
		if( s_models[i].valid )
			ReleaseModel( s_models[i] );
	}
}

}
