/*
 * csz_triapi.cpp -- CSOZ renderer: self-drawn TriAPI emulation layer (decision B)
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
#include "csz_triapi.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"

#include <string.h>

namespace csz
{

#include "csz_triapi_shaders.inl"

namespace
{

// ---------------------------------------------------------------------------
// Minimal engine sprite ABI mirror, for SpriteTexture (frame -> gl_texturenum).
// Same facts as csz_sprite.cpp's EngSprite (the fork ships no in-memory sprite
// structs); kept private here so the two modules stay independent files.
// ---------------------------------------------------------------------------
const int kSpriteFrameSingle = 0;

struct EngSpriteFrame
{
	int width, height;
	float up, down, left, right;
	int gl_texturenum;		// ENGINE texture slot (GL_Bind), not a raw GL name
};

struct EngSpriteGroup
{
	int numframes;
	float *intervals;
	EngSpriteFrame *frames[1];
};

struct EngSpriteFrameDesc
{
	int type;
	EngSpriteFrame *frameptr;	// EngSpriteGroup* when type != single
};

struct EngSprite
{
	short type, texFormat;
	int maxwidth, maxheight, numframes, radius, facecull, synctype;
	EngSpriteFrameDesc frames[1];
};

// Vertex layout: pos3 + uv2 + color4.
const int kVertexFloats = 9;

// One Begin/End primitive cap. particleman emits one big TRI_QUADS with up to a
// few thousand verts (N particles x 4); 16384 covers heavy crossfire. Verts past
// the cap are dropped with a throttled warning (no silent truncation).
const int kMaxPrimVerts = 16384;
// QUADS expand 4 -> 6 verts; the expand buffer must hold that growth.
const int kMaxExpandVerts = kMaxPrimVerts / 4 * 6;

struct TriState
{
	bool shaderReady;
	int gpuGeneration;
	ShaderProgram program;
	int uViewProj, uTextured, uFog, uFogParams, uCamPos, uFogAdditive;
	unsigned int vao, vbo;

	triangleapi_t table;		// our function table (filled once)
	bool tableReady;
	triangleapi_t *engineTri;	// saved engine table, for query delegation

	// Per-dispatch frame constants.
	float viewProj[16];
	float camPos[3];
	float sceneFog[4], sceneFogParams[4];	// scene ambience fog (primary)
	bool modFogOn;				// mod TriAPI Fog() toggle (legacy cl_fog path)
	float modFogColor[3];
	float modFogDensity;

	// Current draw state (set between Begin/End by RenderMode/CullFace/...).
	int blendMode;		// csz::BlendMode
	bool fogAdditive;	// additive blend class -> fade-to-black fog
	bool depthWrite, depthTest;
	bool cullOn, cullFront;
	int texSlot;		// current bound sprite texture (0 = untextured)
	float curColor[4];
	float curUV[2];
	float brightness;

	// Current primitive accumulation.
	int prim;		// TRI_* or -1
	int vertCount;
	bool overflowed;	// throttle the cap warning
	float scratch[kMaxPrimVerts * kVertexFloats];
	float expand[kMaxExpandVerts * kVertexFloats];

	int trisThisDispatch;
	bool active;
};

TriState s_tri;
cvar_t *s_cvarTriApi;

void EnsureGpuObjects()
{
	if( s_tri.shaderReady && s_tri.gpuGeneration == GpuGeneration())
		return;

	if( s_tri.gpuGeneration != GpuGeneration())
	{
		s_tri.program.program = 0;
		s_tri.vao = s_tri.vbo = 0;
	}

	BuildProgram( "csz_triapi", kTriVs, kTriFs, true, s_tri.program );
	s_tri.uViewProj = UniformLoc( s_tri.program, "u_viewProj" );
	s_tri.uTextured = UniformLoc( s_tri.program, "u_textured" );
	s_tri.uFog = UniformLoc( s_tri.program, "u_fog" );
	s_tri.uFogParams = UniformLoc( s_tri.program, "u_fogParams" );
	s_tri.uCamPos = UniformLoc( s_tri.program, "u_camPos" );
	s_tri.uFogAdditive = UniformLoc( s_tri.program, "u_fogAdditive" );

	UseProgram( s_tri.program.program );
	glUniform1i( UniformLoc( s_tri.program, "u_texDiffuse" ), 0 );
	UseProgram( 0 );

	glGenVertexArrays( 1, &s_tri.vao );
	BindVao( s_tri.vao );
	glGenBuffers( 1, &s_tri.vbo );
	glBindBuffer( GL_ARRAY_BUFFER, s_tri.vbo );

	const int stride = kVertexFloats * (int)sizeof( float );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)( 3 * sizeof( float )));
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 4, GL_FLOAT, GL_FALSE, stride, (const void *)( 5 * sizeof( float )));

	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	s_tri.gpuGeneration = GpuGeneration();
	s_tri.shaderReady = true;
}

// ---- frame selection for SpriteTexture (engine-parity, single + group) ----
const EngSpriteFrame *SelectFrame( const EngSprite *spr, int frame )
{
	if( frame < 0 )
		frame = 0;
	else if( frame >= spr->numframes )
		frame = spr->numframes - 1;

	const EngSpriteFrameDesc &desc = spr->frames[frame];

	if( desc.type == kSpriteFrameSingle )
		return desc.frameptr;

	// Group / angled: M2c takes member 0 (particle sprites are single-frame in
	// practice; full time-based group selection is a sprite-pass concern).
	const EngSpriteGroup *group = (const EngSpriteGroup *)desc.frameptr;
	if( group == NULL || group->numframes < 1 || group->numframes > 4096 )
		return NULL;
	return group->frames[0];
}

// ---- expand the current primitive to a triangle list (QUADS only) ----
// Appends triangle (a,b,c) by copying three source verts.
void EmitTri( float *dst, int &n, const float *src, int ia, int ib, int ic )
{
	if( n + 3 > kMaxExpandVerts )
		return;
	memcpy( &dst[(size_t)( n + 0 ) * kVertexFloats], &src[(size_t)ia * kVertexFloats], kVertexFloats * sizeof( float ));
	memcpy( &dst[(size_t)( n + 1 ) * kVertexFloats], &src[(size_t)ib * kVertexFloats], kVertexFloats * sizeof( float ));
	memcpy( &dst[(size_t)( n + 2 ) * kVertexFloats], &src[(size_t)ic * kVertexFloats], kVertexFloats * sizeof( float ));
	n += 3;
}

// Resolve the effective fog for this flush. Scene ambience fog is primary (so
// TriAPI content matches the foggy world -- the G-P7穿帮 fix); when the scene has
// no fog but the mod turned its classic TriAPI fog on, fall back to that.
void ResolveFog( float fogVec[4], float fogParams[4] )
{
	if( s_tri.sceneFog[3] > 0.0f )
	{
		memcpy( fogVec, s_tri.sceneFog, 4 * sizeof( float ));
		memcpy( fogParams, s_tri.sceneFogParams, 4 * sizeof( float ));
	}
	else if( s_tri.modFogOn && s_tri.modFogDensity > 0.0f )
	{
		fogVec[0] = s_tri.modFogColor[0];
		fogVec[1] = s_tri.modFogColor[1];
		fogVec[2] = s_tri.modFogColor[2];
		fogVec[3] = s_tri.modFogDensity;	// classic uniform-density exp fog
		fogParams[0] = 0.0f; fogParams[1] = 0.0f; fogParams[2] = 1.0f; fogParams[3] = 0.0f;
	}
	else
	{
		fogVec[0] = fogVec[1] = fogVec[2] = 0.0f; fogVec[3] = 0.0f;	// off
		fogParams[0] = 0.0f; fogParams[1] = 0.0f; fogParams[2] = 1.0f; fogParams[3] = 0.0f;
	}
}

GLenum NativeMode( int prim )
{
	switch( prim )
	{
	case TRI_TRIANGLES:	return GL_TRIANGLES;
	case TRI_TRIANGLE_FAN:	return GL_TRIANGLE_FAN;
	case TRI_POLYGON:	return GL_TRIANGLE_FAN;		// convex polygon == fan
	case TRI_TRIANGLE_STRIP:return GL_TRIANGLE_STRIP;
	case TRI_QUAD_STRIP:	return GL_TRIANGLE_STRIP;	// same vertex layout tessellates the quads
	case TRI_LINES:		return GL_LINES;
	case TRI_POINTS:	return GL_POINTS;
	default:		return GL_TRIANGLES;
	}
}

void FlushPrimitive()
{
	int prim = s_tri.prim;
	int n = s_tri.vertCount;

	s_tri.prim = -1;
	s_tri.vertCount = 0;

	if( prim < 0 || n < 1 )
		return;

	EnsureGpuObjects();

	const float *data = s_tri.scratch;
	int drawCount = n;
	GLenum mode;

	if( prim == TRI_QUADS )
	{
		// Expand each 4-vert quad (a,b,c,d) -> (a,b,c)+(a,c,d).
		int en = 0;
		int quads = n / 4;
		for( int q = 0; q < quads; q++ )
		{
			int b = q * 4;
			EmitTri( s_tri.expand, en, s_tri.scratch, b + 0, b + 1, b + 2 );
			EmitTri( s_tri.expand, en, s_tri.scratch, b + 0, b + 2, b + 3 );
		}
		data = s_tri.expand;
		drawCount = en;
		mode = GL_TRIANGLES;
	}
	else
	{
		mode = NativeMode( prim );
	}

	if( drawCount < 1 )
		return;

	// --- uniforms ---
	UseProgram( s_tri.program.program );
	glUniformMatrix4fv( s_tri.uViewProj, 1, GL_FALSE, s_tri.viewProj );
	glUniform3fv( s_tri.uCamPos, 1, s_tri.camPos );
	glUniform1i( s_tri.uTextured, ( s_tri.texSlot > 0 ) ? 1 : 0 );
	glUniform1i( s_tri.uFogAdditive, s_tri.fogAdditive ? 1 : 0 );

	float fogVec[4], fogParams[4];
	ResolveFog( fogVec, fogParams );
	glUniform4fv( s_tri.uFog, 1, fogVec );
	glUniform4fv( s_tri.uFogParams, 1, fogParams );

	// --- GL state ---
	SetBlend( (BlendMode)s_tri.blendMode );
	SetDepthTest( s_tri.depthTest );
	SetDepthWrite( s_tri.depthWrite );
	SetCull( s_tri.cullOn );
	if( s_tri.cullOn )
		SetCullFront( s_tri.cullFront );

	if( s_tri.texSlot > 0 )
		BindTextureSlot( 0, s_tri.texSlot );

	BindVao( s_tri.vao );
	glBindBuffer( GL_ARRAY_BUFFER, s_tri.vbo );
	glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)( (size_t)drawCount * kVertexFloats * sizeof( float )),
		data, GL_STREAM_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	glDrawArrays( mode, 0, drawCount );

	// Triangle estimate for the dispatch log (lines/points count as their prim count).
	if( mode == GL_TRIANGLES )
		s_tri.trisThisDispatch += drawCount / 3;
	else if( mode == GL_TRIANGLE_FAN || mode == GL_TRIANGLE_STRIP )
		s_tri.trisThisDispatch += ( drawCount >= 3 ) ? ( drawCount - 2 ) : 0;
}

// =====================================================================
// triangleapi_t members. These run with gEngfuncs.pTriAPI pointed at our table.
// =====================================================================

void Tri_RenderMode( int mode )
{
	// Engine-parity rendermode -> blend/depth (mirrors csz_sprite's mapping).
	switch( mode )
	{
	case kRenderTransAdd:
		s_tri.blendMode = kBlendAdditive; s_tri.depthWrite = false; s_tri.depthTest = true; s_tri.fogAdditive = true;
		break;
	case kRenderGlow:
		s_tri.blendMode = kBlendAdditive; s_tri.depthWrite = false; s_tri.depthTest = false; s_tri.fogAdditive = true;
		break;
	case kRenderTransColor:
	case kRenderTransTexture:
		s_tri.blendMode = kBlendAlpha; s_tri.depthWrite = false; s_tri.depthTest = true; s_tri.fogAdditive = false;
		break;
	case kRenderTransAlpha:
		s_tri.blendMode = kBlendAlpha; s_tri.depthWrite = true; s_tri.depthTest = true; s_tri.fogAdditive = false;
		break;
	case kRenderNormal:
	default:
		s_tri.blendMode = kBlendNone; s_tri.depthWrite = true; s_tri.depthTest = true; s_tri.fogAdditive = false;
		break;
	}
}

void Tri_Begin( int primitiveCode )
{
	if( s_tri.prim >= 0 && s_tri.vertCount > 0 )
		FlushPrimitive();	// defensive: a missing End()
	s_tri.prim = primitiveCode;
	s_tri.vertCount = 0;
}

void Tri_End( void )
{
	FlushPrimitive();
}

void Tri_Color4f( float r, float g, float b, float a )
{
	s_tri.curColor[0] = r; s_tri.curColor[1] = g; s_tri.curColor[2] = b; s_tri.curColor[3] = a;
}

void Tri_Color4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a )
{
	const float k = 1.0f / 255.0f;
	s_tri.curColor[0] = r * k; s_tri.curColor[1] = g * k; s_tri.curColor[2] = b * k; s_tri.curColor[3] = a * k;
}

void Tri_Color4fRendermode( float r, float g, float b, float a, int rendermode )
{
	// Engine sets color (alpha pre-scaled in some modes) and forces a rendermode.
	Tri_RenderMode( rendermode );
	s_tri.curColor[0] = r; s_tri.curColor[1] = g; s_tri.curColor[2] = b; s_tri.curColor[3] = a;
}

void Tri_TexCoord2f( float u, float v )
{
	s_tri.curUV[0] = u; s_tri.curUV[1] = v;
}

void Tri_Brightness( float brightness )
{
	s_tri.brightness = brightness;
}

void PushVertex( float x, float y, float z )
{
	if( s_tri.prim < 0 )
		return;	// Vertex outside Begin/End: ignore

	if( s_tri.vertCount >= kMaxPrimVerts )
	{
		if( !s_tri.overflowed )
		{
			s_tri.overflowed = true;
			CSZ_LogWarn( "triapi", "primitive vertex cap (%d) hit; extra verts dropped this primitive", kMaxPrimVerts );
		}
		return;
	}

	float *out = &s_tri.scratch[(size_t)s_tri.vertCount * kVertexFloats];
	out[0] = x; out[1] = y; out[2] = z;
	out[3] = s_tri.curUV[0]; out[4] = s_tri.curUV[1];
	out[5] = s_tri.curColor[0] * s_tri.brightness;
	out[6] = s_tri.curColor[1] * s_tri.brightness;
	out[7] = s_tri.curColor[2] * s_tri.brightness;
	out[8] = s_tri.curColor[3];
	s_tri.vertCount++;
}

void Tri_Vertex3fv( float *worldPnt )
{
	if( worldPnt != NULL )
		PushVertex( worldPnt[0], worldPnt[1], worldPnt[2] );
}

void Tri_Vertex3f( float x, float y, float z )
{
	PushVertex( x, y, z );
}

void Tri_CullFace( TRICULLSTYLE style )
{
	if( style == TRI_NONE )
	{
		s_tri.cullOn = false;
	}
	else	// TRI_FRONT: cull front faces (engine TriAPI semantics)
	{
		s_tri.cullOn = true;
		s_tri.cullFront = true;
	}
}

int Tri_SpriteTexture( struct model_s *pSpriteModel, int frame )
{
	if( pSpriteModel == NULL || pSpriteModel->type != mod_sprite || pSpriteModel->cache.data == NULL )
	{
		s_tri.texSlot = 0;
		return 0;
	}

	const EngSprite *spr = (const EngSprite *)pSpriteModel->cache.data;
	if( spr->numframes < 1 || spr->numframes > 4096 )
	{
		s_tri.texSlot = 0;
		return 0;
	}

	const EngSpriteFrame *f = SelectFrame( spr, frame );
	if( f == NULL || f->gl_texturenum <= 0 )
	{
		s_tri.texSlot = 0;
		return 0;
	}

	s_tri.texSlot = f->gl_texturenum;
	return 1;
}

void Tri_Fog( float flFogColor[3], float flStart, float flEnd, int bOn )
{
	(void)flStart; (void)flEnd;	// classic linear params; we use density (FogParams) for the exp form
	s_tri.modFogOn = ( bOn != 0 );
	if( flFogColor != NULL )
	{
		const float k = 1.0f / 255.0f;	// engine passes 0..255 fog color bytes as floats
		s_tri.modFogColor[0] = flFogColor[0] * k;
		s_tri.modFogColor[1] = flFogColor[1] * k;
		s_tri.modFogColor[2] = flFogColor[2] * k;
	}
}

void Tri_FogParams( float flDensity, int iFogSkybox )
{
	(void)iFogSkybox;
	s_tri.modFogDensity = flDensity;
}

// ---- read-only query members: DELEGATE to the saved engine table ----
// They touch no GL draw state, so the engine's real implementations (which use
// the engine's current refdef/PVS) are both correct and free. Safe stub + a
// one-shot log if (defensively) no engine table was stashed.
void QueryUnavailable( const char *what )
{
	static bool s_logged;
	if( !s_logged )
	{
		s_logged = true;
		CSZ_LogWarn( "triapi", "engine query table unavailable; %s returns a safe default", what );
	}
}

int Tri_WorldToScreen( float *world, float *screen )
{
	if( s_tri.engineTri != NULL && s_tri.engineTri->WorldToScreen != NULL )
		return s_tri.engineTri->WorldToScreen( world, screen );
	QueryUnavailable( "WorldToScreen" );
	if( screen != NULL )
		screen[0] = screen[1] = screen[2] = 0.0f;
	return 1;	// report z-clipped (caller skips)
}

void Tri_ScreenToWorld( float *screen, float *world )
{
	if( s_tri.engineTri != NULL && s_tri.engineTri->ScreenToWorld != NULL )
	{
		s_tri.engineTri->ScreenToWorld( screen, world );
		return;
	}
	QueryUnavailable( "ScreenToWorld" );
	if( world != NULL )
		world[0] = world[1] = world[2] = 0.0f;
}

void Tri_GetMatrix( const int pname, float *matrix )
{
	if( s_tri.engineTri != NULL && s_tri.engineTri->GetMatrix != NULL )
	{
		s_tri.engineTri->GetMatrix( pname, matrix );
		return;
	}
	QueryUnavailable( "GetMatrix" );
}

int Tri_BoxInPVS( float *mins, float *maxs )
{
	if( s_tri.engineTri != NULL && s_tri.engineTri->BoxInPVS != NULL )
		return s_tri.engineTri->BoxInPVS( mins, maxs );
	QueryUnavailable( "BoxInPVS" );
	return 1;	// assume visible (fail-open; never hides content)
}

void Tri_LightAtPoint( float *pos, float *value )
{
	if( s_tri.engineTri != NULL && s_tri.engineTri->LightAtPoint != NULL )
	{
		s_tri.engineTri->LightAtPoint( pos, value );
		return;
	}
	QueryUnavailable( "LightAtPoint" );
	if( value != NULL )
		value[0] = value[1] = value[2] = 255.0f;	// fullbright fallback
}

void InitTable()
{
	if( s_tri.tableReady )
		return;

	triangleapi_t &t = s_tri.table;
	t.version = TRI_API_VERSION;
	t.RenderMode = Tri_RenderMode;
	t.Begin = Tri_Begin;
	t.End = Tri_End;
	t.Color4f = Tri_Color4f;
	t.Color4ub = Tri_Color4ub;
	t.TexCoord2f = Tri_TexCoord2f;
	t.Vertex3fv = Tri_Vertex3fv;
	t.Vertex3f = Tri_Vertex3f;
	t.Brightness = Tri_Brightness;
	t.CullFace = Tri_CullFace;
	t.SpriteTexture = Tri_SpriteTexture;
	t.WorldToScreen = Tri_WorldToScreen;
	t.Fog = Tri_Fog;
	t.ScreenToWorld = Tri_ScreenToWorld;
	t.GetMatrix = Tri_GetMatrix;
	t.BoxInPVS = Tri_BoxInPVS;
	t.LightAtPoint = Tri_LightAtPoint;
	t.Color4fRendermode = Tri_Color4fRendermode;
	t.FogParams = Tri_FogParams;

	s_tri.tableReady = true;
}

}

triangleapi_t *CszTriApiTable()
{
	InitTable();
	return &s_tri.table;
}

void CszTriApiBeginDispatch( const ViewSetup &view, triangleapi_t *engineTri )
{
	InitTable();

	s_tri.engineTri = engineTri;
	memcpy( s_tri.viewProj, view.matViewProj.m, 16 * sizeof( float ));
	s_tri.camPos[0] = view.origin[0]; s_tri.camPos[1] = view.origin[1]; s_tri.camPos[2] = view.origin[2];

	CszFogUniformVecs( view.ambience, s_tri.sceneFog, s_tri.sceneFogParams );	// scene black-fog (primary)
	s_tri.modFogOn = false;
	s_tri.modFogDensity = 0.0f;
	s_tri.modFogColor[0] = s_tri.modFogColor[1] = s_tri.modFogColor[2] = 0.0f;

	// Reset the TriAPI draw state to engine defaults.
	s_tri.blendMode = kBlendNone;
	s_tri.fogAdditive = false;
	s_tri.depthWrite = true;
	s_tri.depthTest = true;
	s_tri.cullOn = false;
	s_tri.cullFront = true;
	s_tri.texSlot = 0;
	s_tri.curColor[0] = s_tri.curColor[1] = s_tri.curColor[2] = s_tri.curColor[3] = 1.0f;
	s_tri.curUV[0] = s_tri.curUV[1] = 0.0f;
	s_tri.brightness = 1.0f;
	s_tri.prim = -1;
	s_tri.vertCount = 0;
	s_tri.overflowed = false;
	s_tri.trisThisDispatch = 0;
	s_tri.active = true;

	// Known GL baseline for the dispatched draws.
	SetBlend( kBlendNone );
	SetDepthTest( true );
	SetDepthWrite( true );
	SetCull( false );
}

void CszTriApiEndDispatch()
{
	if( s_tri.prim >= 0 )
		FlushPrimitive();	// defensive: a Begin without End

	// State hygiene (design §4): hand a clean baseline to the next CSZ pass.
	SetBlend( kBlendNone );
	SetDepthWrite( true );
	SetDepthTest( true );
	SetCull( false );
	BindVao( 0 );
	UseProgram( 0 );

	{
		static float s_nextLog;
		float now = ClientTime();
		if( now >= s_nextLog )
		{
			s_nextLog = now + 1.0f;
			CSZ_LogDev( "triapi", "tris=%d", s_tri.trisThisDispatch );
		}
	}

	s_tri.engineTri = NULL;
	s_tri.active = false;
}

bool CszTriApiEnabled()
{
	if( s_cvarTriApi == NULL )
		return true;
	return s_cvarTriApi->value != 0.0f;
}

void TriApiRegisterCvars()
{
	if( s_cvarTriApi == NULL )
		s_cvarTriApi = gEngfuncs.pfnRegisterVariable( "csz_triapi", "1", FCVAR_CLIENTDLL );
}

void TriApiShutdown()
{
	if( s_tri.shaderReady && s_tri.gpuGeneration == GpuGeneration())
	{
		if( s_tri.program.program != 0 )
			DestroyProgram( s_tri.program );
		if( s_tri.vbo != 0 )
			glDeleteBuffers( 1, &s_tri.vbo );
		if( s_tri.vao != 0 )
			glDeleteVertexArrays( 1, &s_tri.vao );
	}

	s_tri.shaderReady = false;
	s_tri.program.program = 0;
	s_tri.vao = s_tri.vbo = 0;
	s_tri.active = false;
	s_tri.engineTri = NULL;
}

}
