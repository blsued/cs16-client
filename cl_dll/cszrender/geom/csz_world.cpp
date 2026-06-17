/*
 * csz_world.cpp -- CSOZ renderer: BSP world rendering implementation
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
#include "csz_world.h"
#include "csz_lightmap.h"
#include "../core/csz_engine.h"
#include "../core/csz_engine_bsp.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_fatal.h"
#include "../core/csz_shader.h"

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace csz
{

WorldRenderer g_world;

#include "csz_world_shaders.inl"

namespace
{

const float kBackfaceEpsilon = 0.01f;

// Interleaved vertex layout (plan section 5 step 2.4): pos3 + uv2 + lmuv2 +
// normal3 = 10 floats, 40 bytes. Attribute locations are the plan 2.4 contract.
const int kVertexFloats = 10;
const int kVertexStride = kVertexFloats * (int)sizeof( float );

struct FaceRec
{
	int surfIndex;			// LOCAL index (surfaces[firstmodelsurface + i])
	int firstVert, numVerts;	// into the world VBO; -1/0 for sky/turb
	int texSlot;			// engine texture slot for unit 0
	int lmPage, lmX, lmY;		// atlas block, luxel units; lmPage -1 = none
	int smax, tmax;			// luxel block dimensions
	float alphaTest;		// 0 = opaque, else '{' discard threshold
	float planeNormal[3];
	float planeDist;
	bool planeBack;
	float mins[3], maxs[3];		// surface bounds (engine mextrasurf, light cull)
};

// All persistent state lives here (single g_world instance; header stays
// contract-exact with no private members, same pattern as the atlas).
struct WorldState
{
	model_t *model;			// engine model identity for rebuild detection
	char name[64];
	const EngModel *bsp;
	int gpuGeneration;		// generation our GL names belong to
	bool built;
	bool lightmapsDirty;

	unsigned int vao, vbo;
	ShaderProgram program;
	int uViewProj, uAlphaTest;
	int uModel;			// base-pass model->world transform (identity for world; per-entity for brush, E1)
	int uFog, uAmbTint;		// base pass only (M2a fog/night; lit/depth stay fog-free, pitfall 23)
	int uSunDir, uSunColor;		// base pass only (sky 档1 directional N.L; lit/depth exempt, pitfall 23)
	int uBrushAlpha;		// per-entity translucency for blended brush modes (renderamt); 1.0 = opaque/world
	ShaderProgram litProgram;	// additive per-light pass (T6)
	int litUViewProj, litUAlphaTest;
	int litULightOrigin, litULightDir, litULightColor;
	int litULightRadius, litUCosInner, litUCosOuter;
	int litUMatShadow, litUHasShadow;
	ShaderProgram depthProgram;	// shadow map depth pass (T7)
	int depthUViewProj;

	FaceRec *faces;			// [numFaces], indexed by local face index
	int *opaque;			// sorted local indices (texture, lightmap page)
	unsigned char *visible;		// per local face, rebuilt by BuildVisibleSet
	int numFaces, numOpaque;
	int numSky, numTurb;
	int whiteTexSlot;

	// Brush submodels (E1): every BSP surface NOT owned by worldmodel
	// submodel 0, built into the SAME VBO. brushFaces[] is dense and ordered
	// by global surface index; brushForGlobal[] maps a global surface index to
	// its brushFaces[] slot (-1 for world surfaces), so a brush entity's
	// contiguous [firstmodelsurface, +nummodelsurfaces) range resolves to a
	// contiguous brushFaces[] run.
	FaceRec *brushFaces;
	int *brushForGlobal;		// [bsp->numsurfaces]; -1 = world surface
	int numBrushFaces;
};

WorldState s_world;

// Conversion scratch for one lightmap block (engine standard maps: smax/tmax
// <= 17; clamp guard in csz_lightmap.cpp covers exotic sample sizes).
unsigned char s_blockScratch[128 * 128 * 3];

// ---------------------------------------------------------------------------
// Runtime ABI self-check: the static_asserts in csz_engine_bsp.h pin our
// mirror's size, but only the live engine can prove the field offsets. The
// info->surf back-pointer roundtrip fails loudly on any layout drift
// (e.g. an engine rebuilt with HL25 extended structs).
// ---------------------------------------------------------------------------
void ValidateEngineAbi( const EngModel *bsp )
{
	char reason[192];

	if( bsp->numsurfaces <= 0 || bsp->numleafs <= 0 || bsp->surfaces == NULL || bsp->leafs == NULL )
		CSZ_FatalInit( "world", "world model has no surfaces/leafs (ABI or load failure)" );

	int surfChecks = ( bsp->numsurfaces < 8 ) ? bsp->numsurfaces : 8;

	for( int i = 0; i < surfChecks; i++ )
	{
		const EngSurface &s = bsp->surfaces[i];

		if( s.plane < bsp->planes || s.plane >= bsp->planes + bsp->numplanes ||
			s.texinfo < bsp->texinfo || s.texinfo >= bsp->texinfo + bsp->numtexinfo ||
			s.numedges < 3 || s.numedges > 512 ||
			s.info == NULL || s.info->surf != &s )
		{
			snprintf( reason, sizeof( reason ),
				"engine BSP ABI mismatch at surface %d (plane/texinfo/info fields are garbage); "
				"engine com_model.h layout differs from csz_engine_bsp.h", i );
			CSZ_FatalInit( "world", reason );
		}

		const EngTexture *tex = s.texinfo->texture;

		if( tex == NULL || tex->width == 0 || tex->width > 4096 || tex->height == 0 || tex->height > 4096 )
		{
			snprintf( reason, sizeof( reason ),
				"engine BSP ABI mismatch at surface %d (texture pointer/size is garbage)", i );
			CSZ_FatalInit( "world", reason );
		}
	}

	int leafChecks = ( bsp->numleafs < 8 ) ? bsp->numleafs : 8;

	for( int i = 1; i <= leafChecks; i++ )
	{
		const EngLeaf &leaf = bsp->leafs[i];

		if( leaf.contents >= 0 || leaf.cluster < -1 || leaf.cluster >= bsp->numleafs ||
			leaf.nummarksurfaces < 0 || leaf.nummarksurfaces > bsp->nummarksurfaces )
		{
			snprintf( reason, sizeof( reason ),
				"engine BSP ABI mismatch at leaf %d (contents/cluster fields are garbage)", i );
			CSZ_FatalInit( "world", reason );
		}
	}

	CSZ_LogDev( "world", "engine BSP ABI self-check OK (%d surfaces, %d leafs probed)",
		surfChecks, leafChecks );
}

int FetchEdgeVertex( const EngModel *bsp, int surfEdge )
{
	// surfedges sign selects edge direction (plan section 5 step 2.2).
	if(( bsp->flags & kModelQbsp2 ) != 0 )
	{
		if( surfEdge >= 0 )
			return (int)bsp->edges32[surfEdge].v[0];
		return (int)bsp->edges32[-surfEdge].v[1];
	}

	if( surfEdge >= 0 )
		return (int)bsp->edges16[surfEdge].v[0];
	return (int)bsp->edges16[-surfEdge].v[1];
}

int FaceSampleSize( int globalSurfIndex )
{
	if( gRenderAPI.RenderGetParm == NULL )
		return 16;

	int sampleSize = (int)gRenderAPI.RenderGetParm( PARM_SURF_SAMPLESIZE, globalSurfIndex );

	return ( sampleSize > 0 ) ? sampleSize : 16;
}

// Fills the conversion scratch with the face's style-0 block, or full white
// when the face has no lightmap data (fullbright fallback, plan step 2.6).
const unsigned char *FaceLightBlock( const EngSurface &surf, int smax, int tmax )
{
	int size = smax * tmax;

	if( size > (int)( sizeof( s_blockScratch ) / 3 ))
		size = (int)( sizeof( s_blockScratch ) / 3 );

	if( surf.samples == NULL || surf.styles[0] == 255 )
	{
		memset( s_blockScratch, 255, (size_t)size * 3 );
		return s_blockScratch;
	}

	// color24 is packed RGB888; style 0 is the first smax*tmax block.
	memcpy( s_blockScratch, surf.samples, (size_t)size * 3 );
	return s_blockScratch;
}

// Fills a FaceRec from one BSP surface (plane/bounds/texture/alpha-test +
// lightmap atlas allocation/upload). Returns false for sky/turb surfaces (no
// geometry emitted). Shared by the world (submodel 0) and brush (E1) builds so
// neither path duplicates the per-surface setup. globalIndex indexes into
// bsp->surfaces[]; localSlot is the value stored in FaceRec.surfIndex (the
// world uses its local face index for visible[]; brush stores globalIndex).
// maxPage tracks the highest atlas page touched across both builds.
bool BuildFaceRec( const EngModel *bsp, int globalIndex, int localSlot, FaceRec &f,
	int *badTexWarned, int *atlasFullWarned, int *maxPage )
{
	const EngSurface &surf = bsp->surfaces[globalIndex];

	memset( &f, 0, sizeof( f ));
	f.surfIndex = localSlot;
	f.firstVert = -1;
	f.lmPage = -1;

	const mplane_t *plane = surf.plane;

	f.planeBack = ( surf.flags & kSurfPlaneBack ) != 0;
	f.planeNormal[0] = plane->normal[0];
	f.planeNormal[1] = plane->normal[1];
	f.planeNormal[2] = plane->normal[2];
	f.planeDist = plane->dist;

	for( int j = 0; j < 3; j++ )
	{
		f.mins[j] = surf.info->mins[j];
		f.maxs[j] = surf.info->maxs[j];
	}

	if( surf.flags & kSurfDrawSky )
		return false;

	if( surf.flags & kSurfDrawTurb )
		return false;

	const EngTexinfo *ti = surf.texinfo;
	const EngTexture *tex = ti->texture;

	f.texSlot = ( tex != NULL ) ? tex->gl_texturenum : 0;

	if( f.texSlot == 0 )
	{
		f.texSlot = s_world.whiteTexSlot;

		if( badTexWarned != NULL && (*badTexWarned)++ == 0 )
			CSZ_LogWarn( "world", "surface %d has no engine texture slot; using *white", globalIndex );
	}

	f.alphaTest = ( tex != NULL && tex->name[0] == '{' ) ? 0.25f : 0.0f;

	const EngExtraSurf *info = surf.info;
	int sampleSize = FaceSampleSize( globalIndex );
	int smax = ( info->lightextents[0] / sampleSize ) + 1;
	int tmax = ( info->lightextents[1] / sampleSize ) + 1;
	int page = 0, bx = 0, by = 0;

	if( g_lightmaps.Allocate( smax, tmax, &page, &bx, &by ))
	{
		f.lmPage = page;
		f.lmX = bx;
		f.lmY = by;
		f.smax = smax;
		f.tmax = tmax;
		g_lightmaps.UploadBlock( page, bx, by, smax, tmax, FaceLightBlock( surf, smax, tmax ));

		if( maxPage != NULL && page > *maxPage )
			*maxPage = page;
	}
	else if( atlasFullWarned != NULL && (*atlasFullWarned)++ == 0 )
	{
		CSZ_LogError( "world", "lightmap atlas full at surface %d (%dx%d); face samples page 0 origin",
			globalIndex, smax, tmax );
	}

	return true;
}

// Packs one face's vertices (10-float interleaved layout) into verts[] at
// vertCursor and stamps f.firstVert/f.numVerts. Vertices are emitted in the
// surface's local model space; the world's submodel-0 origin is (0,0,0) so its
// faces are effectively world-space, while brush submodels carry their offset
// in the per-draw u_model matrix.
void EmitFaceVerts( const EngModel *bsp, int globalIndex, FaceRec &f, float *verts, int vertCursor )
{
	const EngSurface &surf = bsp->surfaces[globalIndex];
	const EngTexinfo *ti = surf.texinfo;
	const EngTexture *tex = ti->texture;
	const EngExtraSurf *info = surf.info;

	f.firstVert = vertCursor;
	f.numVerts = surf.numedges;

	float nx = f.planeBack ? -f.planeNormal[0] : f.planeNormal[0];
	float ny = f.planeBack ? -f.planeNormal[1] : f.planeNormal[1];
	float nz = f.planeBack ? -f.planeNormal[2] : f.planeNormal[2];

	for( int e = 0; e < surf.numedges; e++ )
	{
		int vi = FetchEdgeVertex( bsp, bsp->surfedges[surf.firstedge + e] );
		const float *pos = bsp->vertexes[vi].position;
		float *out = &verts[(size_t)( vertCursor + e ) * kVertexFloats];

		out[0] = pos[0];
		out[1] = pos[1];
		out[2] = pos[2];

		float su = pos[0] * ti->vecs[0][0] + pos[1] * ti->vecs[0][1] + pos[2] * ti->vecs[0][2] + ti->vecs[0][3];
		float tv = pos[0] * ti->vecs[1][0] + pos[1] * ti->vecs[1][1] + pos[2] * ti->vecs[1][2] + ti->vecs[1][3];

		out[3] = ( tex != NULL ) ? su / (float)tex->width : 0.0f;
		out[4] = ( tex != NULL ) ? tv / (float)tex->height : 0.0f;

		if( f.lmPage >= 0 )
		{
			float ls = pos[0] * info->lmvecs[0][0] + pos[1] * info->lmvecs[0][1] +
				pos[2] * info->lmvecs[0][2] + info->lmvecs[0][3] - (float)info->lightmapmins[0];
			float lt = pos[0] * info->lmvecs[1][0] + pos[1] * info->lmvecs[1][1] +
				pos[2] * info->lmvecs[1][2] + info->lmvecs[1][3] - (float)info->lightmapmins[1];

			int sampleSize = FaceSampleSize( globalIndex );

			out[5] = ((float)f.lmX * sampleSize + ls + 0.5f * sampleSize ) /
				(float)( LightmapAtlas::kPageSize * sampleSize );
			out[6] = ((float)f.lmY * sampleSize + lt + 0.5f * sampleSize ) /
				(float)( LightmapAtlas::kPageSize * sampleSize );
		}
		else
		{
			out[5] = 0.0f;
			out[6] = 0.0f;
		}

		out[7] = nx;
		out[8] = ny;
		out[9] = nz;
	}
}

struct OpaqueSortKey
{
	int faceIndex;
	int texSlot;
	int lmPage;
};

int CompareOpaque( const void *a, const void *b )
{
	const OpaqueSortKey *ka = (const OpaqueSortKey *)a;
	const OpaqueSortKey *kb = (const OpaqueSortKey *)b;

	if( ka->texSlot != kb->texSlot )
		return ( ka->texSlot < kb->texSlot ) ? -1 : 1;
	if( ka->lmPage != kb->lmPage )
		return ( ka->lmPage < kb->lmPage ) ? -1 : 1;
	return ( ka->faceIndex < kb->faceIndex ) ? -1 : ( ka->faceIndex > kb->faceIndex );
}

// Conservative spot-light vs AABB test (T6). SpotLightParams carries no
// frustum (core POD contract), so the cone is approximated by its bounding
// sphere plus the apex half-space: boxes fully outside the radius or fully
// behind the cone apex cannot receive light.
bool SpotTouchesBox( const SpotLightParams &light, const float mins[3], const float maxs[3] )
{
	float distSq = 0.0f;

	for( int j = 0; j < 3; j++ )
	{
		float v = light.origin[j];
		float e = 0.0f;

		if( v < mins[j] )
			e = mins[j] - v;
		else if( v > maxs[j] )
			e = v - maxs[j];

		distSq += e * e;
	}

	if( distSq > light.radius * light.radius )
		return false;

	// Positive vertex along the cone direction: if even the farthest corner
	// sits behind the apex plane, the box is fully behind the light.
	float d = 0.0f;

	for( int j = 0; j < 3; j++ )
		d += light.dir[j] * ((( light.dir[j] >= 0.0f ) ? maxs[j] : mins[j] ) - light.origin[j] );

	return d >= 0.0f;
}

void ReuploadLightmaps()
{
	for( int i = 0; i < s_world.numFaces; i++ )
	{
		const FaceRec &f = s_world.faces[i];

		if( f.lmPage < 0 || f.smax <= 0 )
			continue;

		const EngSurface &surf = s_world.bsp->surfaces[s_world.bsp->firstmodelsurface + f.surfIndex];

		g_lightmaps.UploadBlock( f.lmPage, f.lmX, f.lmY, f.smax, f.tmax,
			FaceLightBlock( surf, f.smax, f.tmax ));
	}

	// Brush submodel faces share the atlas; surfIndex holds the GLOBAL index.
	for( int i = 0; i < s_world.numBrushFaces; i++ )
	{
		const FaceRec &f = s_world.brushFaces[i];

		if( f.lmPage < 0 || f.smax <= 0 )
			continue;

		const EngSurface &surf = s_world.bsp->surfaces[f.surfIndex];

		g_lightmaps.UploadBlock( f.lmPage, f.lmX, f.lmY, f.smax, f.tmax,
			FaceLightBlock( surf, f.smax, f.tmax ));
	}

	CSZ_LogDev( "world", "lightmaps re-uploaded (%d world + %d brush faces)",
		s_world.numFaces, s_world.numBrushFaces );
}

// ---------------------------------------------------------------------------
// Brush entity helpers (E1). A brush submodel's geometry is baked in its local
// model space; the entity carries an origin/angles transform that the engine's
// R_DrawBrushModel would apply. We rebuild that transform as a per-draw model
// matrix (translate * rotate), default identity for entities sitting at the
// origin with no angles (the common func_wall / static brush case).
// ---------------------------------------------------------------------------

// Column-major model->world matrix: basis columns forward / -right / up
// (GoldSrc brush orientation; right is negated because Quake's right vector
// points to the entity's right while the +Y model axis points left), column 3
// = entity origin. Identity falls out for zero origin/angles.
void BuildBrushModelMatrix( const cl_entity_t *ent, Mat4 &out )
{
	float fwd[3], right[3], up[3];

	AngleVectors( ent->angles, fwd, right, up );

	out.m[0] = fwd[0];   out.m[1] = fwd[1];   out.m[2] = fwd[2];   out.m[3] = 0.0f;
	out.m[4] = -right[0]; out.m[5] = -right[1]; out.m[6] = -right[2]; out.m[7] = 0.0f;
	out.m[8] = up[0];    out.m[9] = up[1];    out.m[10] = up[2];   out.m[11] = 0.0f;
	out.m[12] = ent->origin[0]; out.m[13] = ent->origin[1]; out.m[14] = ent->origin[2]; out.m[15] = 1.0f;
}

// The only two real FWGS R_GetEntityRenderMode overrides that matter to brush
// models (clean-room, pitfall: do NOT invent EF_*/renderamt/brightness rules,
// they cause csz_renderer 0/1 A/B drift, R3):
//   - a model flagged MODEL_TRANSPARENT in kRenderNormal renders as
//     kRenderTransAlpha (cutout glass-style brushes);
//   - studio-additive -> kRenderTransAdd (not a brush concern).
// MODEL_TRANSPARENT is model_s::flags bit 3 (engine ABI fact; this fork's
// com_model.h predates the constant, so it is named here at point of use).
const int kModelTransparent = ( 1 << 3 );

int ResolveBrushRenderMode( const cl_entity_t *ent )
{
	int mode = ent->curstate.rendermode;
	const EngModel *bmod = EngBsp( ent->model );

	if( mode == kRenderNormal && ( bmod->flags & kModelTransparent ) != 0 )
		return kRenderTransAlpha;

	return mode;
}

// Upper bound on transparent brush entities sorted in one frame; matches the
// composition root's FrameEntities::kMaxEntities cap (csz_renderer.h) without
// pulling that header into the geom layer.
const int kMaxBrushItems = 1024;

struct BrushDrawItem
{
	cl_entity_t *ent;
	float distSq;		// sort key: AABB-center dist^2, kRenderTransAlpha forced to 1e9 (drawn first)
};

int CompareBrushItems( const void *a, const void *b )
{
	const BrushDrawItem *ia = (const BrushDrawItem *)a;
	const BrushDrawItem *ib = (const BrushDrawItem *)b;

	// Back-to-front (descending distSq); blended brush composites correctly.
	if( ia->distSq > ib->distSq )
		return -1;
	if( ia->distSq < ib->distSq )
		return 1;
	return 0;
}

}

bool WorldRenderer::IsBuilt() const
{
	return s_world.built;
}

void WorldRenderer::MarkLightmapsDirty()
{
	s_world.lightmapsDirty = true;
}

void WorldRenderer::Destroy()
{
	// Generation rule (T1 calibration): names from an older GPU generation
	// must be forgotten, never deleted -- glDelete* on a fresh context could
	// hit foreign objects that reused the name.
	bool sameContext = ( s_world.gpuGeneration == GpuGeneration());

	if( s_world.vao != 0 || s_world.vbo != 0 )
	{
		if( sameContext )
		{
			if( s_world.vao != 0 )
				glDeleteVertexArrays( 1, &s_world.vao );
			if( s_world.vbo != 0 )
				glDeleteBuffers( 1, &s_world.vbo );
		}

		s_world.vao = 0;
		s_world.vbo = 0;
	}

	if( s_world.program.program != 0 )
	{
		if( sameContext )
			DestroyProgram( s_world.program );
		else
			s_world.program.program = 0;
	}

	if( s_world.litProgram.program != 0 )
	{
		if( sameContext )
			DestroyProgram( s_world.litProgram );
		else
			s_world.litProgram.program = 0;
	}

	if( s_world.depthProgram.program != 0 )
	{
		if( sameContext )
			DestroyProgram( s_world.depthProgram );
		else
			s_world.depthProgram.program = 0;
	}

	delete[] s_world.faces;
	delete[] s_world.opaque;
	delete[] s_world.visible;
	delete[] s_world.brushFaces;
	delete[] s_world.brushForGlobal;
	s_world.faces = NULL;
	s_world.opaque = NULL;
	s_world.visible = NULL;
	s_world.brushFaces = NULL;
	s_world.brushForGlobal = NULL;
	s_world.numFaces = s_world.numOpaque = 0;
	s_world.numBrushFaces = 0;
	s_world.model = NULL;
	s_world.bsp = NULL;
	s_world.built = false;
	s_world.lightmapsDirty = false;

	g_lightmaps.Reset();	// atlas slots are engine textures (context-safe free)
}

void WorldRenderer::EnsureBuilt( model_t *world )
{
	if( world == NULL )
		return;

	const EngModel *bsp = EngBsp( world );

	if( s_world.built && s_world.model == world &&
		s_world.gpuGeneration == GpuGeneration() &&
		strncmp( s_world.name, bsp->name, sizeof( s_world.name )) == 0 )
		return;

	Destroy();

	ValidateEngineAbi( bsp );

	s_world.model = world;
	s_world.bsp = bsp;
	s_world.gpuGeneration = GpuGeneration();
	memcpy( s_world.name, bsp->name, sizeof( s_world.name ));
	s_world.name[sizeof( s_world.name ) - 1] = '\0';

	s_world.whiteTexSlot = ( gRenderAPI.GL_FindTexture != NULL )
		? gRenderAPI.GL_FindTexture( "*white" ) : 0;

	int numFaces = bsp->nummodelsurfaces;
	s_world.numFaces = numFaces;
	s_world.faces = new( std::nothrow ) FaceRec[numFaces];
	s_world.opaque = new( std::nothrow ) int[numFaces];
	s_world.visible = new( std::nothrow ) unsigned char[numFaces];
	OpaqueSortKey *sortKeys = new( std::nothrow ) OpaqueSortKey[numFaces];

	if( s_world.faces == NULL || s_world.opaque == NULL || s_world.visible == NULL || sortKeys == NULL )
		CSZ_FatalInit( "world", "out of memory building world face tables" );

	// Brush submodels (E1): every BSP surface not owned by worldmodel
	// submodel 0. The worldmodel's own range is [firstmodelsurface,
	// +nummodelsurfaces); everything else belongs to inline submodels (*1..)
	// referenced by brush entities. Allocate worst-case (all are brush) and
	// fill densely.
	int totalSurfaces = bsp->numsurfaces;
	int worldFirst = bsp->firstmodelsurface;
	int worldLast = worldFirst + numFaces;	// exclusive
	int maxBrushFaces = ( totalSurfaces > numFaces ) ? ( totalSurfaces - numFaces ) : 0;

	s_world.brushForGlobal = new( std::nothrow ) int[totalSurfaces];
	s_world.brushFaces = ( maxBrushFaces > 0 ) ? new( std::nothrow ) FaceRec[maxBrushFaces] : NULL;

	if( s_world.brushForGlobal == NULL || ( maxBrushFaces > 0 && s_world.brushFaces == NULL ))
		CSZ_FatalInit( "world", "out of memory building brush face tables" );

	for( int i = 0; i < totalSurfaces; i++ )
		s_world.brushForGlobal[i] = -1;

	// First pass: count drawable vertices (sky/turb faces emit none) across
	// both the world range and every brush surface, so the shared VBO is sized
	// once.
	int totalVerts = 0;

	for( int g = 0; g < totalSurfaces; g++ )
	{
		const EngSurface &surf = bsp->surfaces[g];

		if(( surf.flags & ( kSurfDrawSky | kSurfDrawTurb )) == 0 )
			totalVerts += surf.numedges;
	}

	float *verts = new( std::nothrow ) float[(size_t)totalVerts * kVertexFloats];

	if( verts == NULL )
		CSZ_FatalInit( "world", "out of memory building world vertex buffer" );

	int vertCursor = 0;
	int badTexWarned = 0;
	int atlasFullWarned = 0;
	int maxPage = -1;

	s_world.numSky = s_world.numTurb = 0;
	s_world.numOpaque = 0;
	s_world.numBrushFaces = 0;

	// --- World pass (submodel 0): local-indexed faces[], camera visible[]. ---
	for( int i = 0; i < numFaces; i++ )
	{
		int globalIndex = worldFirst + i;
		const EngSurface &surf = bsp->surfaces[globalIndex];
		FaceRec &f = s_world.faces[i];

		if( !BuildFaceRec( bsp, globalIndex, i, f, &badTexWarned, &atlasFullWarned, &maxPage ))
		{
			if( surf.flags & kSurfDrawSky )
				s_world.numSky++;
			else if( surf.flags & kSurfDrawTurb )
				s_world.numTurb++;
			continue;
		}

		EmitFaceVerts( bsp, globalIndex, f, verts, vertCursor );
		vertCursor += surf.numedges;

		sortKeys[s_world.numOpaque].faceIndex = i;
		sortKeys[s_world.numOpaque].texSlot = f.texSlot;
		sortKeys[s_world.numOpaque].lmPage = f.lmPage;
		s_world.numOpaque++;
	}

	// Opaque list sorted by texture then lightmap page (bind-switch economy).
	qsort( sortKeys, (size_t)s_world.numOpaque, sizeof( OpaqueSortKey ), CompareOpaque );

	for( int i = 0; i < s_world.numOpaque; i++ )
		s_world.opaque[i] = sortKeys[i].faceIndex;

	delete[] sortKeys;

	// --- Brush pass: every surface outside the worldmodel range, built into
	// the SAME VBO. surfIndex holds the GLOBAL index; brushForGlobal maps it
	// back so a brush entity's contiguous global range -> contiguous run. Sky/
	// turb brush surfaces are skipped (no GL geometry), leaving a -1 hole that
	// the draw-time range scan tolerates. ---
	for( int g = 0; g < totalSurfaces; g++ )
	{
		if( g >= worldFirst && g < worldLast )
			continue;	// owned by submodel 0

		const EngSurface &surf = bsp->surfaces[g];
		FaceRec &f = s_world.brushFaces[s_world.numBrushFaces];

		if( !BuildFaceRec( bsp, g, g, f, &badTexWarned, &atlasFullWarned, &maxPage ))
			continue;	// sky/turb: emit nothing, leave brushForGlobal[g] = -1

		EmitFaceVerts( bsp, g, f, verts, vertCursor );
		vertCursor += surf.numedges;

		s_world.brushForGlobal[g] = s_world.numBrushFaces;
		s_world.numBrushFaces++;
	}

	// GPU objects. Build happens outside the takeover window (slot 6), so
	// leave VAO/VBO unbound for the engine afterwards.
	glGenVertexArrays( 1, &s_world.vao );
	BindVao( s_world.vao );
	glGenBuffers( 1, &s_world.vbo );
	glBindBuffer( GL_ARRAY_BUFFER, s_world.vbo );
	glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)( (size_t)totalVerts * kVertexStride ), verts, GL_STATIC_DRAW );

	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, kVertexStride, (const void *)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, kVertexStride, (const void *)( 3 * sizeof( float )));
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 2, GL_FLOAT, GL_FALSE, kVertexStride, (const void *)( 5 * sizeof( float )));
	glEnableVertexAttribArray( 3 );
	glVertexAttribPointer( 3, 3, GL_FLOAT, GL_FALSE, kVertexStride, (const void *)( 7 * sizeof( float )));

	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	delete[] verts;

	// World shader: init-time, so a compile failure is FATAL (spec 3.2).
	BuildProgram( "csz_world", kWorldVs, kWorldFs, true, s_world.program );
	s_world.uViewProj = UniformLoc( s_world.program, "u_viewProj" );
	s_world.uAlphaTest = UniformLoc( s_world.program, "u_alphaTest" );
	s_world.uModel = UniformLoc( s_world.program, "u_model" );
	s_world.uFog = UniformLoc( s_world.program, "u_fog" );
	s_world.uAmbTint = UniformLoc( s_world.program, "u_ambTint" );
	s_world.uSunDir = UniformLoc( s_world.program, "u_sunDir" );
	s_world.uSunColor = UniformLoc( s_world.program, "u_sunColor" );
	s_world.uBrushAlpha = UniformLoc( s_world.program, "u_brushAlpha" );

	UseProgram( s_world.program.program );
	glUniform1i( UniformLoc( s_world.program, "u_texDiffuse" ), 0 );
	glUniform1i( UniformLoc( s_world.program, "u_texLightmap" ), 1 );
	glUniform1f( s_world.uAlphaTest, 0.0f );
	glUniform1f( s_world.uBrushAlpha, 1.0f );	// opaque/world default; per-entity feed in DrawBrushTransparent

	Mat4 identity;
	Mat4Identity( identity );
	glUniformMatrix4fv( s_world.uModel, 1, GL_FALSE, identity.m );	// world stays identity (DrawOpaque re-pins)

	const float kFogOff[4] = { 0.0f, 0.0f, 0.0f, 0.0f };	// fog off until fed (DrawOpaque)
	const float kTintNeutral[3] = { 1.0f, 1.0f, 1.0f };	// neutral until fed (never tint-black)

	glUniform4fv( s_world.uFog, 1, kFogOff );
	glUniform3fv( s_world.uAmbTint, 1, kTintNeutral );
	UseProgram( 0 );

	// Lit-additive program (T6 spot pass); init-time, so failure is FATAL.
	BuildProgram( "csz_world_lit", kWorldLitVs, kWorldLitFs, true, s_world.litProgram );
	s_world.litUViewProj = UniformLoc( s_world.litProgram, "u_viewProj" );
	s_world.litUAlphaTest = UniformLoc( s_world.litProgram, "u_alphaTest" );
	s_world.litULightOrigin = UniformLoc( s_world.litProgram, "u_lightOrigin" );
	s_world.litULightDir = UniformLoc( s_world.litProgram, "u_lightDir" );
	s_world.litULightColor = UniformLoc( s_world.litProgram, "u_lightColor" );
	s_world.litULightRadius = UniformLoc( s_world.litProgram, "u_lightRadius" );
	s_world.litUCosInner = UniformLoc( s_world.litProgram, "u_cosInner" );
	s_world.litUCosOuter = UniformLoc( s_world.litProgram, "u_cosOuter" );
	s_world.litUMatShadow = UniformLoc( s_world.litProgram, "u_matShadow" );
	s_world.litUHasShadow = UniformLoc( s_world.litProgram, "u_hasShadow" );

	UseProgram( s_world.litProgram.program );
	glUniform1i( UniformLoc( s_world.litProgram, "u_texDiffuse" ), 0 );
	glUniform1i( UniformLoc( s_world.litProgram, "u_shadowMap" ), 2 );
	glUniform1f( s_world.litUAlphaTest, 0.0f );
	UseProgram( 0 );

	// Depth program (T7 shadow map pass); init-time, so failure is FATAL.
	BuildProgram( "csz_world_depth", kWorldDepthVs, kWorldDepthFs, true, s_world.depthProgram );
	s_world.depthUViewProj = UniformLoc( s_world.depthProgram, "u_viewProj" );

	int pages = maxPage + 1;

	s_world.built = true;
	s_world.lightmapsDirty = false;

	CSZ_LogInfo( "world", "built %s: %d world surfaces (%d sky, %d water skipped) + %d brush surfaces, %d verts, %d lightmap pages",
		s_world.name, numFaces, s_world.numSky, s_world.numTurb, s_world.numBrushFaces, totalVerts, pages );
}

void WorldRenderer::BuildVisibleSet( const ViewSetup &view )
{
	if( !s_world.built )
		return;

	memset( s_world.visible, 0, (size_t)s_world.numFaces );

	const EngModel *bsp = s_world.bsp;

	// Linear leaf scan instead of recursive node walk (notes-mechanisms b):
	// leafs[0] is the solid leaf, real leafs are 1..numleafs inclusive.
	for( int i = 1; i <= bsp->numleafs; i++ )
	{
		const EngLeaf &leaf = bsp->leafs[i];
		int cluster = leaf.cluster;

		if( cluster < 0 )
			continue;

		if( view.pvs != NULL && !( view.pvs[cluster >> 3] & ( 1 << ( cluster & 7 ))))
			continue;

		if( view.frustum.CullBox( &leaf.minmaxs[0], &leaf.minmaxs[3] ))
			continue;

		for( int j = 0; j < leaf.nummarksurfaces; j++ )
		{
			int local = (int)( leaf.firstmarksurface[j] - bsp->surfaces ) - bsp->firstmodelsurface;

			if( local >= 0 && local < s_world.numFaces )
				s_world.visible[local] = 1;
		}
	}
}

void WorldRenderer::DrawOpaque( const ViewSetup &view )
{
	if( !s_world.built )
		return;

	if( s_world.lightmapsDirty )
	{
		ReuploadLightmaps();
		s_world.lightmapsDirty = false;
	}

	UseProgram( s_world.program.program );
	glUniformMatrix4fv( s_world.uViewProj, 1, GL_FALSE, view.matViewProj.m );

	// World geometry is baked in world space: pin u_model to identity (the
	// brush passes set a per-entity matrix and may have left it dirty, E1).
	Mat4 identity;
	Mat4Identity( identity );
	glUniformMatrix4fv( s_world.uModel, 1, GL_FALSE, identity.m );

	// Ambience feed (M2a A1): server-authoritative snapshot rides in on the
	// view (plan 2.5 slot 7.2); base pass only, pitfall 23.
	const AmbienceParams &amb = view.ambience;
	const float fogVec[4] = { amb.fogColor[0], amb.fogColor[1], amb.fogColor[2], amb.fogDensity };

	glUniform4fv( s_world.uFog, 1, fogVec );
	glUniform3fv( s_world.uAmbTint, 1, amb.tint );
	glUniform3fv( s_world.uSunDir, 1, amb.moonlightDir );		// directional N.L (sky 档1), base pass only
	glUniform3fv( s_world.uSunColor, 1, amb.moonlightColor );	// (0,0,0) when the body light is off

	BindVao( s_world.vao );
	SetCull( false );	// BSP faces are culled per-face below (plan step 3)

	// The program object remembers the last frame's alpha-test value; pin a
	// known state so the per-face uniform dedup below stays truthful. Also pin
	// u_brushAlpha = 1.0: a prior DrawBrushTransparent may have left it at a
	// blended entity's renderamt (FIX 2), and the static world is always opaque.
	glUniform1f( s_world.uAlphaTest, 0.0f );
	glUniform1f( s_world.uBrushAlpha, 1.0f );

	int curTex = -1;
	int curPage = -1;
	float curAlpha = 0.0f;
	int drawn = 0;

	for( int i = 0; i < s_world.numOpaque; i++ )
	{
		const FaceRec &f = s_world.faces[s_world.opaque[i]];

		if( !s_world.visible[f.surfIndex] || f.firstVert < 0 )
			continue;

		// Plane-side backface cull (sign XOR SURF_PLANEBACK, plan step 3).
		float d = view.origin[0] * f.planeNormal[0] + view.origin[1] * f.planeNormal[1] +
			view.origin[2] * f.planeNormal[2] - f.planeDist;

		if( f.planeBack ? ( d > -kBackfaceEpsilon ) : ( d < kBackfaceEpsilon ))
			continue;

		if( f.texSlot != curTex )
		{
			BindTextureSlot( 0, f.texSlot );
			curTex = f.texSlot;
		}

		if( f.lmPage != curPage )
		{
			BindTextureSlot( 1, g_lightmaps.PageTexSlot(( f.lmPage >= 0 ) ? f.lmPage : 0 ));
			curPage = f.lmPage;
		}

		if( f.alphaTest != curAlpha )
		{
			glUniform1f( s_world.uAlphaTest, f.alphaTest );
			curAlpha = f.alphaTest;
		}

		glDrawArrays( GL_TRIANGLE_FAN, f.firstVert, f.numVerts );
		drawn++;
	}

	// Per-frame stats at Dev level with 1s self-throttle (R8).
	static float s_nextStatsTime;
	float now = ClientTime();

	if( now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "world", "drawn %d / %d opaque faces", drawn, s_world.numOpaque );
	}
}

namespace
{

// Draws every drawable face of one brush entity through the base program with
// the given model matrix already on u_model. Plane-side backface cull is done
// in MODEL space (the baked plane is local) by transforming the view origin
// through the inverse rigid transform, so rotating/moving brushes cull
// correctly. applyAlphaTest=false (transparent path) feeds u_alphaTest=0 so the
// full texture alpha blends instead of being cut out. curTex/curPage/curAlpha
// carry the bind/uniform dedup across entities in one pass. Returns faces drawn.
int DrawBrushEntityFaces( const cl_entity_t *ent, const ViewSetup &view, const Mat4 &model,
	bool applyAlphaTest, int &curTex, int &curPage, float &curAlpha )
{
	const EngModel *bmod = EngBsp( ent->model );
	int first = bmod->firstmodelsurface;
	int count = bmod->nummodelsurfaces;

	if( first < 0 || count <= 0 || first + count > s_world.bsp->numsurfaces )
		return 0;

	// View origin in the brush's local model space (rigid inverse:
	// localView = R^T * (worldView - origin); matrix basis columns are the
	// world-space axes, so the transpose rows are R^T).
	float rel[3] = {
		view.origin[0] - model.m[12],
		view.origin[1] - model.m[13],
		view.origin[2] - model.m[14],
	};
	float localView[3] = {
		rel[0] * model.m[0] + rel[1] * model.m[1] + rel[2] * model.m[2],
		rel[0] * model.m[4] + rel[1] * model.m[5] + rel[2] * model.m[6],
		rel[0] * model.m[8] + rel[1] * model.m[9] + rel[2] * model.m[10],
	};

	int drawn = 0;

	for( int g = first; g < first + count; g++ )
	{
		int slot = s_world.brushForGlobal[g];

		if( slot < 0 )
			continue;	// sky/turb surface: nothing emitted

		const FaceRec &f = s_world.brushFaces[slot];

		if( f.firstVert < 0 )
			continue;

		// Plane-side backface cull in model space (same sign rule as the world
		// opaque pass, plan step 3).
		float d = localView[0] * f.planeNormal[0] + localView[1] * f.planeNormal[1] +
			localView[2] * f.planeNormal[2] - f.planeDist;

		if( f.planeBack ? ( d > -kBackfaceEpsilon ) : ( d < kBackfaceEpsilon ))
			continue;

		if( f.texSlot != curTex )
		{
			BindTextureSlot( 0, f.texSlot );
			curTex = f.texSlot;
		}

		if( f.lmPage != curPage )
		{
			BindTextureSlot( 1, g_lightmaps.PageTexSlot(( f.lmPage >= 0 ) ? f.lmPage : 0 ));
			curPage = f.lmPage;
		}

		float wantAlpha = applyAlphaTest ? f.alphaTest : 0.0f;

		if( wantAlpha != curAlpha )
		{
			glUniform1f( s_world.uAlphaTest, wantAlpha );
			curAlpha = wantAlpha;
		}

		glDrawArrays( GL_TRIANGLE_FAN, f.firstVert, f.numVerts );
		drawn++;
	}

	return drawn;
}

// AABB center (world space) of a brush entity, used as the transparent sort
// key. mins/maxs are the model's local bounds; for transformed entities the
// center is rotated/translated by the model matrix.
void BrushWorldCenter( const cl_entity_t *ent, const Mat4 &model, float out[3] )
{
	const EngModel *bmod = EngBsp( ent->model );
	float lc[3] = {
		( bmod->mins[0] + bmod->maxs[0] ) * 0.5f,
		( bmod->mins[1] + bmod->maxs[1] ) * 0.5f,
		( bmod->mins[2] + bmod->maxs[2] ) * 0.5f,
	};

	out[0] = model.m[0] * lc[0] + model.m[4] * lc[1] + model.m[8] * lc[2] + model.m[12];
	out[1] = model.m[1] * lc[0] + model.m[5] * lc[1] + model.m[9] * lc[2] + model.m[13];
	out[2] = model.m[2] * lc[0] + model.m[6] * lc[1] + model.m[10] * lc[2] + model.m[14];
}

}

void WorldRenderer::DrawBrushOpaque( const ViewSetup &view, cl_entity_s *const *ents, int count )
{
	if( !s_world.built || count <= 0 )
		return;

	if( s_world.lightmapsDirty )
	{
		ReuploadLightmaps();
		s_world.lightmapsDirty = false;
	}

	// Same base program as the world opaque pass so brush surfaces eat the same
	// fog/night-tint (pitfall 23): reuse s_world.program + its u_fog/u_ambTint.
	UseProgram( s_world.program.program );
	glUniformMatrix4fv( s_world.uViewProj, 1, GL_FALSE, view.matViewProj.m );

	const AmbienceParams &amb = view.ambience;
	const float fogVec[4] = { amb.fogColor[0], amb.fogColor[1], amb.fogColor[2], amb.fogDensity };

	glUniform4fv( s_world.uFog, 1, fogVec );
	glUniform3fv( s_world.uAmbTint, 1, amb.tint );
	glUniform3fv( s_world.uSunDir, 1, amb.moonlightDir );		// directional N.L (sky 档1), base pass only
	glUniform3fv( s_world.uSunColor, 1, amb.moonlightColor );	// (0,0,0) when the body light is off

	BindVao( s_world.vao );
	SetCull( false );		// per-face plane-side cull (model space) below
	glUniform1f( s_world.uAlphaTest, 0.0f );
	glUniform1f( s_world.uBrushAlpha, 1.0f );	// opaque brush ents ignore renderamt (FIX 2)

	int curTex = -1;
	int curPage = -1;
	float curAlpha = 0.0f;
	int drawnEnts = 0;
	int drawnFaces = 0;

	for( int i = 0; i < count; i++ )
	{
		cl_entity_t *ent = ents[i];

		if( ent == NULL || ent->model == NULL || ent->model->type != mod_brush )
			continue;

		if( ResolveBrushRenderMode( ent ) != kRenderNormal )
			continue;	// transparent: handled in the trans domain

		Mat4 model;
		BuildBrushModelMatrix( ent, model );
		glUniformMatrix4fv( s_world.uModel, 1, GL_FALSE, model.m );

		int n = DrawBrushEntityFaces( ent, view, model, true, curTex, curPage, curAlpha );

		if( n > 0 )
		{
			drawnFaces += n;
			drawnEnts++;
		}
	}

	// Restore identity so later base-program users (DrawOpaque next frame, or a
	// re-entrant draw) never inherit a stale brush matrix.
	Mat4 identity;
	Mat4Identity( identity );
	glUniformMatrix4fv( s_world.uModel, 1, GL_FALSE, identity.m );

	static float s_nextStatsTime;
	float now = ClientTime();

	if( drawnEnts > 0 && now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "world", "drawn %d opaque brush ents (%d faces)", drawnEnts, drawnFaces );
	}
}

void WorldRenderer::DrawBrushTransparent( const ViewSetup &view, cl_entity_s *const *ents, int count )
{
	if( !s_world.built || count <= 0 )
		return;

	// Collect transparent brush entities + their sort keys (AABB-center
	// distance-squared to the view origin; brush+kRenderTransAlpha drawn first,
	// dist = 1e9). Bounded by the frame entity cap.
	static BrushDrawItem s_items[kMaxBrushItems];
	int numItems = 0;

	for( int i = 0; i < count && numItems < kMaxBrushItems; i++ )
	{
		cl_entity_t *ent = ents[i];

		if( ent == NULL || ent->model == NULL || ent->model->type != mod_brush )
			continue;

		int mode = ResolveBrushRenderMode( ent );

		if( mode == kRenderNormal )
			continue;	// opaque: handled in the opaque domain

		Mat4 model;
		BuildBrushModelMatrix( ent, model );

		float center[3];
		BrushWorldCenter( ent, model, center );

		float dx = center[0] - view.origin[0];
		float dy = center[1] - view.origin[1];
		float dz = center[2] - view.origin[2];
		float distSq = dx * dx + dy * dy + dz * dz;

		s_items[numItems].ent = ent;
		// kRenderTransAlpha brush sorts first (drawn before the alpha-blended
		// remainder): force it to the far end of the back-to-front order.
		s_items[numItems].distSq = ( mode == kRenderTransAlpha ) ? 1e9f : distSq;
		numItems++;
	}

	if( numItems == 0 )
		return;

	qsort( s_items, (size_t)numItems, sizeof( BrushDrawItem ), CompareBrushItems );

	// Shares the trans domain with sprites (slot 14). Base program for fog/tint
	// parity (pitfall 23). GL state is set per resolved rendermode below
	// (engine R_SetRenderMode, non-Quake GoldSrc default), NOT one blanket
	// alpha-blend: kRenderTransAlpha is a 1-bit cutout (alpha-test + depth-write,
	// opaque-style), only TransAdd/TransColor/TransTexture are genuinely blended.
	UseProgram( s_world.program.program );
	glUniformMatrix4fv( s_world.uViewProj, 1, GL_FALSE, view.matViewProj.m );

	const AmbienceParams &amb = view.ambience;
	const float fogVec[4] = { amb.fogColor[0], amb.fogColor[1], amb.fogColor[2], amb.fogDensity };
	const float fogOff[4] = { 0.0f, 0.0f, 0.0f, 0.0f };	// additive fades to black, not fog color

	glUniform3fv( s_world.uAmbTint, 1, amb.tint );
	glUniform4fv( s_world.uFog, 1, fogVec );	// fog on baseline (per-mode toggles off below)
	glUniform3fv( s_world.uSunDir, 1, amb.moonlightDir );		// directional N.L (sky 档1), base pass only
	glUniform3fv( s_world.uSunColor, 1, amb.moonlightColor );	// (0,0,0) when the body light is off

	BindVao( s_world.vao );
	SetCull( false );

	// Pin u_alphaTest to a known value so DrawBrushEntityFaces' curAlpha dedup
	// stays truthful regardless of what the prior pass left in the program.
	glUniform1f( s_world.uAlphaTest, 0.0f );

	int curTex = -1;
	int curPage = -1;
	float curAlpha = 0.0f;
	int drawnEnts = 0;
	int drawnFaces = 0;

	// State the per-item loop tracks so each GL/uniform switch fires only on a
	// real change (the sort groups kRenderTransAlpha cutouts at the front, then
	// the blended remainder back-to-front).
	int curMode = -1;		// resolved rendermode last applied
	bool fogIsOff = false;		// whether u_fog currently holds the off vector
	float curBrushAlpha = -1.0f;	// u_brushAlpha last uploaded

	for( int i = 0; i < numItems; i++ )
	{
		cl_entity_t *ent = s_items[i].ent;
		int mode = ResolveBrushRenderMode( ent );

		// --- Per-mode GL state (engine R_SetRenderMode parity) ---
		if( mode != curMode )
		{
			bool wantFogOff;
			float wantBrushAlpha;

			switch( mode )
			{
			case kRenderTransAdd:
				// Additive: GL_ONE,GL_ONE; depth-write off; alpha-test off; fog
				// off (additive content fades to black, never to fog color --
				// matches the additive sprite path, pitfall 23 family).
				SetBlend( kBlendAdditive );
				SetDepthWrite( false );
				wantFogOff = true;
				wantBrushAlpha = (float)ent->curstate.renderamt * ( 1.0f / 255.0f );
				break;
			case kRenderTransAlpha:
				// '{'-masked cutout (PT-01 primary target): hard 1-bit discard.
				// Alpha-test ON (FaceRec.alphaTest, set via applyAlphaTest=true
				// below), blend OFF, depth-write ON -- an opaque-style cutout
				// that DOES occlude. Fog on; brushAlpha 1 (cutout is not faded).
				SetBlend( kBlendNone );
				SetDepthWrite( true );
				wantFogOff = false;
				wantBrushAlpha = 1.0f;
				break;
			case kRenderTransColor:
			case kRenderTransTexture:
			default:
				// Alpha blend; depth-write off; alpha-test off; fog on. The
				// default lands here so any other resolved trans mode composites
				// (never silently becomes an opaque cutout).
				SetBlend( kBlendAlpha );
				SetDepthWrite( false );
				wantFogOff = false;
				wantBrushAlpha = (float)ent->curstate.renderamt * ( 1.0f / 255.0f );
				break;
			}

			if( wantFogOff != fogIsOff )
			{
				glUniform4fv( s_world.uFog, 1, wantFogOff ? fogOff : fogVec );
				fogIsOff = wantFogOff;
			}

			if( wantBrushAlpha != curBrushAlpha )
			{
				glUniform1f( s_world.uBrushAlpha, wantBrushAlpha );
				curBrushAlpha = wantBrushAlpha;
			}

			curMode = mode;
		}
		else if( mode != kRenderTransAlpha )
		{
			// Same mode as the previous item but a different entity: refresh the
			// per-entity renderamt (TransAlpha keeps brushAlpha pinned at 1).
			float wantBrushAlpha = (float)ent->curstate.renderamt * ( 1.0f / 255.0f );

			if( wantBrushAlpha != curBrushAlpha )
			{
				glUniform1f( s_world.uBrushAlpha, wantBrushAlpha );
				curBrushAlpha = wantBrushAlpha;
			}
		}

		Mat4 model;
		BuildBrushModelMatrix( ent, model );
		glUniformMatrix4fv( s_world.uModel, 1, GL_FALSE, model.m );

		// applyAlphaTest only for the cutout mode (feeds FaceRec.alphaTest=0.25);
		// the genuinely-blended modes pass alpha-test off so the full texture
		// alpha blends.
		int n = DrawBrushEntityFaces( ent, view, model, mode == kRenderTransAlpha,
			curTex, curPage, curAlpha );

		if( n > 0 )
		{
			drawnFaces += n;
			drawnEnts++;
		}
	}

	// Clean baseline for the next pass (viewmodel, slot 15) + identity model +
	// restored fog/brushAlpha so the next base-program user is unaffected.
	SetBlend( kBlendNone );
	SetDepthWrite( true );

	if( fogIsOff )
		glUniform4fv( s_world.uFog, 1, fogVec );
	glUniform1f( s_world.uBrushAlpha, 1.0f );

	Mat4 identity;
	Mat4Identity( identity );
	glUniformMatrix4fv( s_world.uModel, 1, GL_FALSE, identity.m );

	static float s_nextStatsTime;
	float now = ClientTime();

	if( drawnEnts > 0 && now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "world", "drawn %d transparent brush ents (%d faces)", drawnEnts, drawnFaces );
	}
}

void WorldRenderer::DrawDepth( const ViewSetup &lightView, const Frustum &lightCull )
{
	if( !s_world.built )
		return;

	UseProgram( s_world.depthProgram.program );
	glUniformMatrix4fv( s_world.depthUViewProj, 1, GL_FALSE, lightView.matViewProj.m );
	BindVao( s_world.vao );
	SetCull( false );	// plane-side selection below (triangle-fan winding is
				// not GL-cull reliable; same policy as DrawOpaque)

	// All-visible iteration on purpose (shadow PVS open, notes-mechanisms f-8):
	// s_world.visible is the CAMERA's set and must not gate shadow casters.
	int drawn = 0;

	for( int i = 0; i < s_world.numOpaque; i++ )
	{
		const FaceRec &f = s_world.faces[s_world.opaque[i]];

		if( f.firstVert < 0 )
			continue;

		// Far-side-only depth (the acne trick of notes-mechanisms e adapted to
		// plane-side culling): keep ONLY faces whose front side faces AWAY
		// from the light. Brushes are closed volumes, so every wall still
		// occludes through its far face, while lit (light-facing) faces are
		// never present to self-compare against.
		float dl = lightView.origin[0] * f.planeNormal[0] + lightView.origin[1] * f.planeNormal[1] +
			lightView.origin[2] * f.planeNormal[2] - f.planeDist;

		if( f.planeBack ? ( dl < kBackfaceEpsilon ) : ( dl > -kBackfaceEpsilon ))
			continue;

		if( lightCull.CullBox( f.mins, f.maxs ))
			continue;

		glDrawArrays( GL_TRIANGLE_FAN, f.firstVert, f.numVerts );
		drawn++;
	}

	// Per-frame stats at Dev level with 1s self-throttle (R8).
	static float s_nextStatsTime;
	float now = ClientTime();

	if( now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "world", "shadow depth: %d faces (light at %.0f %.0f %.0f)",
			drawn, lightView.origin[0], lightView.origin[1], lightView.origin[2] );
	}
}

void WorldRenderer::DrawLitAdditive( const ViewSetup &view, const SpotLightParams &light )
{
	if( !s_world.built )
		return;

	UseProgram( s_world.litProgram.program );
	glUniformMatrix4fv( s_world.litUViewProj, 1, GL_FALSE, view.matViewProj.m );
	glUniform3fv( s_world.litULightOrigin, 1, light.origin );
	glUniform3fv( s_world.litULightDir, 1, light.dir );
	glUniform3fv( s_world.litULightColor, 1, light.color );
	glUniform1f( s_world.litULightRadius, light.radius );
	glUniform1f( s_world.litUCosInner, light.cosInner );
	glUniform1f( s_world.litUCosOuter, light.cosOuter );
	glUniformMatrix4fv( s_world.litUMatShadow, 1, GL_FALSE, light.matShadow.m );
	glUniform1i( s_world.litUHasShadow, ( light.shadowTexSlot != 0 ) ? 1 : 0 );

	if( light.shadowTexSlot != 0 )
		BindTextureSlot( 2, light.shadowTexSlot );	// T7 depth map (T6: never taken)

	BindVao( s_world.vao );
	SetCull( false );
	// Equal-depth additive on top of the opaque pass: depth func stays
	// LEQUAL (EnterTakeover baseline), writes off so later passes are
	// unaffected by this one.
	SetBlend( kBlendAdditive );
	SetDepthWrite( false );

	glUniform1f( s_world.litUAlphaTest, 0.0f );

	int curTex = -1;
	float curAlpha = 0.0f;
	int drawn = 0;

	for( int i = 0; i < s_world.numOpaque; i++ )
	{
		const FaceRec &f = s_world.faces[s_world.opaque[i]];

		if( !s_world.visible[f.surfIndex] || f.firstVert < 0 )
			continue;

		// Same view-side plane cull as the opaque pass (invisible faces).
		float d = view.origin[0] * f.planeNormal[0] + view.origin[1] * f.planeNormal[1] +
			view.origin[2] * f.planeNormal[2] - f.planeDist;

		if( f.planeBack ? ( d > -kBackfaceEpsilon ) : ( d < kBackfaceEpsilon ))
			continue;

		// Light-side plane cull: a face whose front side faces away from the
		// light gets zero ndotl everywhere -- skip it entirely.
		float dl = light.origin[0] * f.planeNormal[0] + light.origin[1] * f.planeNormal[1] +
			light.origin[2] * f.planeNormal[2] - f.planeDist;

		if( f.planeBack ? ( dl > -kBackfaceEpsilon ) : ( dl < kBackfaceEpsilon ))
			continue;

		if( !SpotTouchesBox( light, f.mins, f.maxs ))
			continue;

		if( f.texSlot != curTex )
		{
			BindTextureSlot( 0, f.texSlot );
			curTex = f.texSlot;
		}

		if( f.alphaTest != curAlpha )
		{
			glUniform1f( s_world.litUAlphaTest, f.alphaTest );
			curAlpha = f.alphaTest;
		}

		glDrawArrays( GL_TRIANGLE_FAN, f.firstVert, f.numVerts );
		drawn++;
	}

	SetBlend( kBlendNone );
	SetDepthWrite( true );

	// Per-frame stats at Dev level with 1s self-throttle (R8).
	static float s_nextStatsTime;
	float now = ClientTime();

	if( now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "world", "lit %d faces (spot at %.0f %.0f %.0f)",
			drawn, light.origin[0], light.origin[1], light.origin[2] );
	}
}

}
