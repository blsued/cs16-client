/*
 * csz_engine_bsp.h -- CSOZ renderer: engine-ABI BSP structure mirror
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
#include "csz_engine.h"

#include <stdint.h>

// ---------------------------------------------------------------------------
// WHY THIS FILE EXISTS (read before touching BSP data anywhere in cszrender):
//
// This fork ships the OLD HLSDK common/com_model.h. The pinned engine
// (xash3d-fwgs @ b2a5f0db, see csoz docs/engine-pin.md) lays out model_t's
// POINTED-TO structures differently: texture_t carries gl_texturenum,
// msurface_t carries light_s/polys/lightmaptexturenum/cached_light/info,
// mleaf_t has float minmaxs[6] and `cluster`, edges are a 16/32-bit union
// selected by model flag bit 28 (QBSP2). Dereferencing engine pointers
// through the HLSDK structs reads garbage.
//
// The structs below mirror the ENGINE's layout (interoperability facts,
// transcribed from the pinned engine's common/com_model.h). The top-level
// model_t itself IS offset-compatible between both headers on 32-bit
// (engine poolhandle_t = uint32_t sits in the HLSDK synctype slot), which is
// asserted below. The build is Win32-only for M1; all hard size asserts are
// gated on sizeof(void*) == 4.
//
// The engine binary is the stock FWGS CI build: SUPPORT_HL25_EXTENDED_STRUCTS
// is off (wscript default, no extra CI flag), so EngSurface has NO HL25 tail.
// WorldRenderer additionally runs a runtime ABI self-check (info->surf
// back-pointer roundtrip) before trusting any of this; mismatch is FATAL.
// ---------------------------------------------------------------------------

namespace csz
{

// msurface_s::flags bits (engine common/bspfile.h, ABI facts).
const int kSurfPlaneBack = ( 1 << 1 );		// plane normal points away from the face
const int kSurfDrawSky = ( 1 << 2 );		// sky surface (M1: skipped, clear color shows)
const int kSurfDrawTurb = ( 1 << 4 );		// warped water surface (M1: skipped)
const int kSurfConveyor = ( 1 << 6 );		// SURF_CONVEYOR: scrolling/flowing texture (func_conveyor, "scroll*"); set by ref_gl surface load

// model_s::flags bit: BSP uses 32-bit clipnode/edge types (QBSP2 maps).
const int kModelQbsp2 = ( 1 << 28 );

struct EngSurface;

struct EngTexture
{
	char name[16];
	unsigned int width, height;
	int gl_texturenum;		// ENGINE (ref) texture slot for GL_Bind, NOT a raw GL name
	EngSurface *texturechain;
	int anim_total;			// total tenths in sequence (0 = not animated)
	int anim_min, anim_max;
	EngTexture *anim_next;
	EngTexture *alternate_anims;
	unsigned short fb_texturenum;	// auto-luma slot
	unsigned short dt_texturenum;	// detail-texture slot
	unsigned int unused[3];
};

struct EngTexinfo
{
	float vecs[2][4];		// s/t = dot( pos, vecs[i] ) + vecs[i][3]
	void *faceinfo;			// mfaceinfo_t* landscape extension (unused in M1)
	EngTexture *texture;
	int flags;
};

struct EngLeaf
{
	int contents;
	int visframe;
	float minmaxs[6];		// float here; the HLSDK header has short
	void *parent;			// mnode_t* (engine node layout; not mirrored, unused)
	unsigned char *compressed_vis;
	void *efrags;
	EngSurface **firstmarksurface;
	int nummarksurfaces;
	int cluster;			// index into decompressed PVS bitmap
	unsigned char ambient_sound_level[4];
};

struct EngExtraSurf
{
	float mins[3], maxs[3];		// surface bounds (used by light culling in T6)
	float origin[3];
	EngSurface *surf;		// back pointer; WorldRenderer uses it as ABI self-check
	int dlight_s, dlight_t;
	short lightmapmins[2];		// lightmap-space mins (== texturemins on standard maps)
	short lightextents[2];		// lightmap-space extents (== extents on standard maps)
	float lmvecs[2][4];		// lightmap matrix (== texinfo vecs on standard maps)
	color24 *deluxemap;
	unsigned char *shadowmap;
	EngSurface *lightmapchain;
	EngExtraSurf *detailchain;
	void *bevel;
	EngExtraSurf *lumachain;
	void *parent;			// cl_entity_t* owner (brush entities)
	int mirrortexturenum;
	float mirrormatrix[4][4];
	void *grass;
	unsigned short grasscount;
	unsigned short numverts;
	int firstvertex;
	intptr_t reserved[32];
};

struct EngSurface
{
	int visframe;
	mplane_t *plane;		// HLSDK mplane_t layout matches the engine's
	int flags;			// kSurf* bits above
	int firstedge;			// into model surfedges[]; negative = reversed edge
	int numedges;
	short texturemins[2];
	short extents[2];
	int light_s, light_t;		// engine's own lightmap atlas coords (not ours)
	void *polys;			// glpoly2_t* (engine ref data; unused here)
	EngSurface *texturechain;
	EngTexinfo *texinfo;
	int dlightframe;
	int dlightbits;
	int lightmaptexturenum;
	unsigned char styles[4];	// 255 terminates; M1 renders style 0 only
	int cached_light[4];
	EngExtraSurf *info;
	color24 *samples;		// style blocks, RGB888, smax*tmax per style
	void *pdecals;
};

struct EngEdge16
{
	unsigned short v[2];
	unsigned int cachededgeoffset;
};

struct EngEdge32
{
	unsigned int v[2];
};

struct EngModel
{
	char name[64];
	qboolean needload;
	modtype_t type;
	int numframes;
	unsigned int mempool;		// engine poolhandle_t; HLSDK header calls this slot synctype
	int flags;			// kModelQbsp2 selects the 32-bit edge union member
	vec3_t mins, maxs;
	float radius;
	int firstmodelsurface;
	int nummodelsurfaces;
	int numsubmodels;
	dmodel_t *submodels;
	int numplanes;
	mplane_t *planes;
	int numleafs;			// not counting the solid leaf 0
	EngLeaf *leafs;
	int numvertexes;
	mvertex_t *vertexes;
	int numedges;
	union
	{
		EngEdge16 *edges16;
		EngEdge32 *edges32;
	};
	int numnodes;
	void *nodes;			// engine mnode_t layout differs from HLSDK; unused here
	int numtexinfo;
	EngTexinfo *texinfo;
	int numsurfaces;
	EngSurface *surfaces;
	int numsurfedges;
	int *surfedges;
	int numclipnodes;
	void *clipnodes;
	int nummarksurfaces;
	EngSurface **marksurfaces;
	hull_t hulls[MAX_MAP_HULLS];	// HLSDK hull_t layout matches (pointer-sized first member)
	int numtextures;
	EngTexture **textures;
	unsigned char *visdata;
	color24 *lightdata;
	char *entities;
	cache_user_t cache;
};

// 32-bit layout pins (engine STATIC_CHECK_SIZEOF values where the engine has
// them; EngSurface/EngLeaf derived field-by-field from the pinned header).
static_assert( sizeof( void * ) != 4 || sizeof( EngSurface ) == 92,
	"EngSurface must match engine msurface_t (no HL25 tail) on 32-bit" );
static_assert( sizeof( void * ) != 4 || sizeof( EngExtraSurf ) == 324,
	"EngExtraSurf must match engine mextrasurf_t STATIC_CHECK_SIZEOF on 32-bit" );
static_assert( sizeof( void * ) != 4 || sizeof( EngLeaf ) == 60,
	"EngLeaf must match engine mleaf_t on 32-bit" );
static_assert( sizeof( EngModel ) == sizeof( model_t ),
	"EngModel must stay offset-compatible with the HLSDK model_t this fork compiles against" );

// The only sanctioned way to view an engine model as its real layout.
inline const EngModel *EngBsp( const model_t *mod )
{
	return reinterpret_cast<const EngModel *>( mod );
}

}
