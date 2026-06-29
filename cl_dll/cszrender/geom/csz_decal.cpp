/*
 * csz_decal.cpp -- CSOZ renderer: world/brush BSP decals (self-draw, decision A)
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
#include "csz_decal.h"
#include "../core/csz_engine.h"
#include "../core/csz_engine_bsp.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"

#include <string.h>

namespace csz
{

#include "csz_decal_shaders.inl"

namespace
{

// ---------------------------------------------------------------------------
// Engine decal_t ABI mirror. The fork ships the OLD HLSDK common/com_model.h,
// whose decal_s uses `short dx, dy` -- but the PINNED FWGS engine lays decal_s
// out with `float dx, dy, scale` + a Xash3D tail (position/polys/reserved), so
// `texture` sits at a DIFFERENT offset (20, not 12). Reading decal->texture
// through the HLSDK struct returns garbage; this mirror has the engine layout
// (transcribed from the pinned engine common/com_model.h, the same facts the
// vendored 3rdparty/mainui_cpp/sdk_includes/common/com_model.h carries). Only
// pnext (offset 0) and psurface (offset 4) overlap the HLSDK layout -- the spike
// validated walking via pnext; this mirror additionally makes `texture` safe.
// Win32-only for M1; the hard size assert is gated on 32-bit. A runtime
// plausibility check (ValidateDecalAbi) confirms the layout before any draw.
// ---------------------------------------------------------------------------
struct EngDecal
{
	EngDecal	*pnext;			// linked list for each surface (offset 0)
	void		*psurface;		// msurface_t* (offset 4)
	float		dx;			// local texture coords (offset 8)
	float		dy;			// (offset 12)
	float		scale;			// pixel scale (offset 16)
	short		texture;		// decal gl ref texturenum (offset 20)
	short		flags;			// FDECAL_* (offset 22)
	short		entityIndex;		// attached entity (offset 24)
	// 2 bytes padding to align the vec3 (offset 26 -> 28)
	float		position[3];		// decal center, world space (offset 28)
	void		*polys;			// glpoly2_t* precomputed verts (offset 40)
	intptr_t	reserved[4];		// engine future-expansion tail (offset 44 -> 60)
};

static_assert( sizeof( void * ) != 4 || sizeof( EngDecal ) == 60,
	"EngDecal must match the pinned engine decal_s layout on 32-bit" );

// render_api PARM_TEX_FLAGS bit for a real alpha channel (render_api.h texFlags_t).
const int kTfHasAlpha = ( 1 << 16 );

// Vertex stream == R_DecalSetupVerts output: pos.xyz + base-uv + lightmap-uv.
const int kVertexFloats = 7;

// Per-class capacity (one VBO reused across both passes; the verts are kept on
// the CPU between DrawDecalsAlpha and the later DrawDecalsModulate because the
// engine's R_DecalSetupVerts buffer is static/transient). de_dust2 crossfire
// reaches a few hundred decal polys; 8192 verts / 4096 polys is generous.
const int kMaxClassVerts = 8192;
const int kMaxClassDraws = 4096;

// Runaway guard for a corrupt surf->pdecals chain.
const int kMaxChainPerSurf = 512;

struct DecalDraw
{
	int firstVtx;	// first vertex index in the class vert buffer
	int count;	// vertex count (convex polygon, drawn as a triangle fan)
	int texSlot;	// engine diffuse texture slot (GL_Bind)
	int lmSlot;	// engine lightmap texture slot (0 = unlit)
	int procedural;	// 1 = synthesize the decal (texture unusable in GL core; see 坑23)
};

struct DecalState
{
	bool shaderReady;
	int gpuGeneration;
	ShaderProgram program;
	int uViewProj, uHasLightmap, uModulate, uFog, uFogParams, uCamPos, uProcedural;
	unsigned int vao, vbo;

	// Per-frame CPU batches (filled in DrawDecalsAlpha's single BSP walk).
	float alphaVerts[kMaxClassVerts * kVertexFloats];
	int alphaVertCount;
	DecalDraw alphaDraws[kMaxClassDraws];
	int numAlphaDraws;

	float modVerts[kMaxClassVerts * kVertexFloats];
	int modVertCount;
	DecalDraw modDraws[kMaxClassDraws];
	int numModDraws;

	float buildTime;	// ClientTime() of the frame DrawDecalsAlpha last built
	bool built;		// modulate batch is valid for buildTime
	bool abiChecked;	// ValidateDecalAbi ran (once per map/build)
	bool abiOk;
};

DecalState s_decal;
cvar_t *s_cvarDecal;

bool DecalEnabled()
{
	if( s_cvarDecal == NULL )
		return true;
	return s_cvarDecal->value != 0.0f;
}

// Classification signal (M2c §3.1 / 坑21). The design frames the split literally
// as "decals WITH alpha (blood/scorch)" vs "classic NO-alpha bullet holes", so we
// read the decal texture's real alpha channel (PARM_TEX_FLAGS & TF_HAS_ALPHA).
// Isolated here so the visual A/B gate can swap it for the FDECAL_CUSTOM flag if a
// map's decal authoring tags bullet holes with an alpha channel too (recorded
// tuning point; see the integration spec).
bool DecalIsAlphaClass( int texSlot )
{
	if( gRenderAPI.RenderGetParm == NULL )
		return false;
	intptr_t flags = gRenderAPI.RenderGetParm( PARM_TEX_FLAGS, texSlot );
	return ( flags & kTfHasAlpha ) != 0;
}

// 坑23: the engine uploads GoldSrc gradient/masked decals ("{shot*", "{bigshot*",
// "{blood*", ...) as GL_LUMINANCE8_ALPHA8 -- a legacy fixed-function format that is
// REMOVED in the GL core profile this renderer requires. glTexImage2D rejects it
// (GL_INVALID_ENUM) so the GL texture is never populated and samples as a solid
// (0,0,0,1) -> a persistent OPAQUE BLACK SQUARE at every bullet impact. The pixels
// are unrecoverable on our side (GL_LoadTexture just returns the same cached LA
// slot), so when the bound decal texture is a non-RGB(A) format we synthesize the
// decal procedurally instead of sampling the dead texture. RGB(A) decals (if any)
// keep their real art.
bool DecalTexBroken( int texSlot )
{
	if( gRenderAPI.RenderGetParm == NULL )
		return false;
	intptr_t f = gRenderAPI.RenderGetParm( PARM_TEX_GLFORMAT, texSlot );
	switch( (unsigned)f )
	{
	case 0x1906:	// GL_ALPHA
	case 0x803C:	// GL_ALPHA8
	case 0x1909:	// GL_LUMINANCE
	case 0x8040:	// GL_LUMINANCE8
	case 0x190A:	// GL_LUMINANCE_ALPHA
	case 0x8045:	// GL_LUMINANCE8_ALPHA8
	case 0x8048:	// GL_LUMINANCE16_ALPHA16
	case 0x8049:	// GL_INTENSITY
		return true;
	default:
		return false;
	}
}

// Throttled (>= 1s) error dedup for per-frame failure paths.
void ThrottledError( const char *what )
{
	static float s_nextWarn;
	float now = ClientTime();
	if( now < s_nextWarn )
		return;
	s_nextWarn = now + 1.0f;
	CSZ_LogError( "decal", "%s", what );
}

void EnsureGpuObjects()
{
	if( s_decal.shaderReady && s_decal.gpuGeneration == GpuGeneration())
		return;

	if( s_decal.gpuGeneration != GpuGeneration())
	{
		// Stale generation: forget names, never delete (T1 rule).
		s_decal.program.program = 0;
		s_decal.vao = s_decal.vbo = 0;
	}

	BuildProgram( "csz_decal", kDecalVs, kDecalFs, true, s_decal.program );
	s_decal.uViewProj = UniformLoc( s_decal.program, "u_viewProj" );
	s_decal.uHasLightmap = UniformLoc( s_decal.program, "u_hasLightmap" );
	s_decal.uModulate = UniformLoc( s_decal.program, "u_modulate" );
	s_decal.uFog = UniformLoc( s_decal.program, "u_fog" );
	s_decal.uFogParams = UniformLoc( s_decal.program, "u_fogParams" );
	s_decal.uCamPos = UniformLoc( s_decal.program, "u_camPos" );
	s_decal.uProcedural = UniformLoc( s_decal.program, "u_procedural" );

	UseProgram( s_decal.program.program );
	glUniform1i( UniformLoc( s_decal.program, "u_texDiffuse" ), 0 );
	glUniform1i( UniformLoc( s_decal.program, "u_texLightmap" ), 1 );
	UseProgram( 0 );

	glGenVertexArrays( 1, &s_decal.vao );
	BindVao( s_decal.vao );
	glGenBuffers( 1, &s_decal.vbo );
	glBindBuffer( GL_ARRAY_BUFFER, s_decal.vbo );

	const int stride = kVertexFloats * (int)sizeof( float );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)( 3 * sizeof( float )));
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 2, GL_FLOAT, GL_FALSE, stride, (const void *)( 5 * sizeof( float )));

	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	s_decal.gpuGeneration = GpuGeneration();
	s_decal.shaderReady = true;
}

// One-shot runtime ABI self-check: the static_assert pins EngDecal's SIZE, but
// only a live decal proves the FIELD offsets. We confirm the psurface
// back-pointer of the first walkable decal points back at the surface we found
// it on (the same discipline csz_world uses with info->surf). Failure disables
// the pass (fail-safe: no decals beats garbage reads), logged once.
void ValidateDecalAbi( const EngModel *bsp, int first, int numFaces )
{
	s_decal.abiChecked = true;
	s_decal.abiOk = true;	// optimistic: no decals yet == nothing to disprove

	for( int local = 0; local < numFaces; local++ )
	{
		const EngSurface &surf = bsp->surfaces[first + local];
		const EngDecal *d = (const EngDecal *)surf.pdecals;
		if( d == NULL )
			continue;

		if( d->psurface != (const void *)&surf )
		{
			s_decal.abiOk = false;
			CSZ_LogError( "decal", "engine decal_t ABI mismatch (psurface back-pointer); decal pass disabled" );
		}
		else
		{
			CSZ_LogDev( "decal", "engine decal_t ABI self-check OK" );
		}
		return;
	}
}

// Appends one decal's clipped polygon (count verts, kVertexFloats each, already
// in the R_DecalSetupVerts layout) to a class batch. Returns false when the
// class is full (caller stops adding + warns once).
bool AppendDecal( float *verts, int &vertCount, DecalDraw *draws, int &numDraws,
	const float *src, int count, int texSlot, int lmSlot, int procedural )
{
	if( numDraws >= kMaxClassDraws || vertCount + count > kMaxClassVerts )
		return false;

	memcpy( &verts[(size_t)vertCount * kVertexFloats], src,
		(size_t)count * kVertexFloats * sizeof( float ));

	DecalDraw &d = draws[numDraws++];
	d.firstVtx = vertCount;
	d.count = count;
	d.texSlot = texSlot;
	d.lmSlot = lmSlot;
	d.procedural = procedural;

	vertCount += count;
	return true;
}

// Uploads one class batch and draws it. alphaClass selects the blend state and
// the shader's fog-fade target.
void DrawBatch( const ViewSetup &view, const float *verts, int vertCount,
	const DecalDraw *draws, int numDraws, bool alphaClass )
{
	if( numDraws == 0 || vertCount == 0 )
		return;

	EnsureGpuObjects();

	UseProgram( s_decal.program.program );
	glUniformMatrix4fv( s_decal.uViewProj, 1, GL_FALSE, view.matViewProj.m );

	float fogVec[4], fogParams[4];
	CszFogUniformVecs( view.ambience, fogVec, fogParams );	// same analytic base fog as world/sprite
	glUniform4fv( s_decal.uFog, 1, fogVec );
	glUniform4fv( s_decal.uFogParams, 1, fogParams );
	glUniform3fv( s_decal.uCamPos, 1, view.origin );
	glUniform1i( s_decal.uModulate, alphaClass ? 0 : 1 );

	BindVao( s_decal.vao );
	glBindBuffer( GL_ARRAY_BUFFER, s_decal.vbo );
	glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)( (size_t)vertCount * kVertexFloats * sizeof( float )),
		verts, GL_STREAM_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );	// attribute bindings live in the VAO

	// Decals never write depth (they hug a surface already in the buffer) and
	// must beat z-fight against it; polygon offset (-1,-1) per 坑21.
	SetPolygonOffset( true, -1.0f, -1.0f );
	SetDepthTest( true );
	SetDepthWrite( false );
	SetCull( false );
	SetBlend( alphaClass ? kBlendAlpha : kBlendModulate );

	int curLm = -2, curProc = -1;
	for( int i = 0; i < numDraws; i++ )
	{
		const DecalDraw &d = draws[i];
		int hasLm = ( d.lmSlot > 0 ) ? 1 : 0;

		if( hasLm != curLm )
		{
			glUniform1i( s_decal.uHasLightmap, hasLm );
			curLm = hasLm;
		}

		if( d.procedural != curProc )
		{
			glUniform1i( s_decal.uProcedural, d.procedural );
			curProc = d.procedural;
		}

		// A procedural decal ignores the (dead) diffuse texture, but unit 0 must
		// still reference a complete texture for the sampler -- the engine slot is
		// harmless to bind even when empty, so keep the normal bind either way.
		BindTextureSlot( 0, d.texSlot );
		if( hasLm )
			BindTextureSlot( 1, d.lmSlot );

		glDrawArrays( GL_TRIANGLE_FAN, d.firstVtx, d.count );
	}

	// Hand a clean baseline to the next pass.
	SetPolygonOffset( false, 0.0f, 0.0f );
	SetBlend( kBlendNone );
	SetDepthWrite( true );
	BindVao( 0 );
}

void LogCount( int alpha, int mod )
{
	static float s_nextLog;
	float now = ClientTime();
	if( now < s_nextLog )
		return;
	s_nextLog = now + 1.0f;
	CSZ_LogDev( "decal", "count=%d (alpha=%d modulate=%d)", alpha + mod, alpha, mod );
}

}

void DrawDecalsAlpha( const ViewSetup &view, const unsigned char *visibleFaces, int numFaces )
{
	s_decal.built = false;
	s_decal.alphaVertCount = s_decal.numAlphaDraws = 0;
	s_decal.modVertCount = s_decal.numModDraws = 0;

	if( !DecalEnabled() || visibleFaces == NULL || numFaces <= 0 )
		return;

	if( gRenderAPI.R_DecalSetupVerts == NULL )
		return;	// handshake audit FATALs on null; defensive no-op regardless

	model_t *world = WorldModel();
	if( world == NULL )
		return;

	const EngModel *bsp = EngBsp( world );
	int first = bsp->firstmodelsurface;

	// Clamp to the world's own surface range (visibleFaces length == nummodelsurfaces).
	if( numFaces > bsp->nummodelsurfaces )
		numFaces = bsp->nummodelsurfaces;

	if( !s_decal.abiChecked )
		ValidateDecalAbi( bsp, first, numFaces );
	if( !s_decal.abiOk )
		return;

	bool alphaFull = false, modFull = false;

	for( int local = 0; local < numFaces; local++ )
	{
		if( !visibleFaces[local] )
			continue;

		const EngSurface &surf = bsp->surfaces[first + local];
		if( surf.pdecals == NULL )
			continue;

		// Engine lightmap page for this face (one lookup per surface). The
		// lm-uv R_DecalSetupVerts returns indexes THIS engine atlas page.
		int lmSlot = 0;
		if( surf.lightmaptexturenum >= 0 && gRenderAPI.RenderGetParm != NULL )
			lmSlot = (int)gRenderAPI.RenderGetParm( PARM_TEX_LIGHTMAP, surf.lightmaptexturenum );

		int chain = 0;
		for( EngDecal *d = (EngDecal *)surf.pdecals; d != NULL && chain < kMaxChainPerSurf; d = d->pnext, chain++ )
		{
			int tex = d->texture;
			if( tex <= 0 )
				continue;

			int count = 0;
			// Pass the decal's OWN texture so the engine scales the base-uv to it.
			float *v = gRenderAPI.R_DecalSetupVerts( (struct decal_s *)d,
				(struct msurface_s *)const_cast<EngSurface *>( &surf ), tex, &count );

			if( v == NULL || count < 3 || count > kMaxClassVerts )
				continue;

			// 坑23: decal textures the engine uploaded in a core-invalid luminance/
			// alpha format are unsamplable -> draw them procedurally (see DecalTexBroken).
			int procedural = DecalTexBroken( tex ) ? 1 : 0;

			if( DecalIsAlphaClass( tex ))
			{
				if( !AppendDecal( s_decal.alphaVerts, s_decal.alphaVertCount,
					s_decal.alphaDraws, s_decal.numAlphaDraws, v, count, tex, lmSlot, procedural ))
					alphaFull = true;
			}
			else
			{
				if( !AppendDecal( s_decal.modVerts, s_decal.modVertCount,
					s_decal.modDraws, s_decal.numModDraws, v, count, tex, lmSlot, procedural ))
					modFull = true;
			}
		}
	}

	if( alphaFull || modFull )
		ThrottledError( "decal batch full; some decals skipped this frame" );

	s_decal.buildTime = ClientTime();
	s_decal.built = true;

	LogCount( s_decal.numAlphaDraws, s_decal.numModDraws );

	// Slot 11.6: draw the alpha (blood/scorch) class BEFORE the additive light
	// pass so it gets lit (坑21). The modulate class is held for slot 13.x.
	DrawBatch( view, s_decal.alphaVerts, s_decal.alphaVertCount,
		s_decal.alphaDraws, s_decal.numAlphaDraws, true );
}

void DrawDecalsModulate( const ViewSetup &view )
{
	// Only draw the batch THIS frame's DrawDecalsAlpha built (the walk that
	// produced it is the single source of truth; ClientTime is frame-stable).
	if( !DecalEnabled() || !s_decal.built || s_decal.buildTime != ClientTime())
		return;

	// Slot 13.x: classic no-alpha bullet holes AFTER the additive light pass with
	// DST_COLOR x SRC_COLOR modulate (the engine's 2x decal blend), else the
	// additive light over-brightens them (坑21).
	DrawBatch( view, s_decal.modVerts, s_decal.modVertCount,
		s_decal.modDraws, s_decal.numModDraws, false );
}

void DecalRegisterCvars()
{
	if( s_cvarDecal == NULL )
		s_cvarDecal = gEngfuncs.pfnRegisterVariable( "csz_decal", "1", FCVAR_CLIENTDLL );
}

void DecalShutdown()
{
	// HUD_Shutdown: the owning GL context is still current, so delete the objects
	// when the generation matches (csz_dust precedent). On a generation mismatch
	// the names belong to a dead context -> FORGET them, never glDelete (T1 rule).
	if( s_decal.shaderReady && s_decal.gpuGeneration == GpuGeneration())
	{
		if( s_decal.program.program != 0 )
			DestroyProgram( s_decal.program );
		if( s_decal.vbo != 0 )
			glDeleteBuffers( 1, &s_decal.vbo );
		if( s_decal.vao != 0 )
			glDeleteVertexArrays( 1, &s_decal.vao );
	}

	s_decal.shaderReady = false;
	s_decal.program.program = 0;
	s_decal.vao = s_decal.vbo = 0;
	s_decal.abiChecked = false;
	s_decal.built = false;
}

}
