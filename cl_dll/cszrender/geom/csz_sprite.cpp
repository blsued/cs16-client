/*
 * csz_sprite.cpp -- CSOZ renderer: minimal parallel sprite pass
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
#include "csz_sprite.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"

#include <stdlib.h>
#include <string.h>

namespace csz
{

#include "csz_sprite_shaders.inl"

namespace
{

// ---------------------------------------------------------------------------
// Engine sprite ABI mirror (csz_engine_bsp.h precedent): the fork ships the
// OLD HLSDK common/com_model.h which has NO in-memory sprite structures at
// all. The structs below mirror the PINNED engine's layout (interoperability
// facts, transcribed from engine common/com_model.h:456-490 and the enums
// from engine sprite.h:39-67). model_t itself is offset-compatible between
// both headers on 32-bit (asserted in csz_engine_bsp.h), so model->cache.data
// is safe to read. Win32-only for M1; hard size asserts gated on 32-bit.
// A runtime plausibility self-check (ValidateSprite below) runs before any
// frame data is trusted; failure skips the sprite with a deduplicated Error.
// ---------------------------------------------------------------------------

// spriteframetype_t (engine com_model.h:456)
const int kSpriteFrameSingle = 0;
const int kSpriteFrameGroup = 1;
const int kSpriteFrameAngled = 2;	// Xash3D ext: 8-way; M1 draws direction 0

// msprite_t::type, angletype_t (engine sprite.h:54-61). M1 draws EVERY type
// as view-parallel (plan section 8: parallel/billboard only; known gap for
// SPR_ORIENTED / upright variants, listed for the T8 report).
const int kSpriteMaxType = 4;		// SPR_FWD_PARALLEL_ORIENTED

struct EngSpriteFrame			// engine mspriteframe_t
{
	int width;
	int height;
	float up, down, left, right;	// world-unit quad extents around the origin
	int gl_texturenum;		// ENGINE (ref) texture slot for GL_Bind, NOT a raw GL name
};

struct EngSpriteGroup			// engine mspritegroup_t
{
	int numframes;
	float *intervals;		// cumulative seconds, loader-guaranteed positive
	EngSpriteFrame *frames[1];
};

struct EngSpriteFrameDesc		// engine mspriteframedesc_t
{
	int type;			// spriteframetype_t
	EngSpriteFrame *frameptr;	// EngSpriteGroup* when type != single
};

struct EngSprite			// engine msprite_t (model->cache.data)
{
	short type;			// angletype_t
	short texFormat;		// drawtype_t (loader hint; rendermode wins at draw)
	int maxwidth, maxheight;
	int numframes;
	int radius;
	int facecull;
	int synctype;
	EngSpriteFrameDesc frames[1];
};

// Hard size asserts, gated on 32-bit like csz_engine_bsp.h (M1 is Win32-only).
static_assert( sizeof( void * ) != 4 || sizeof( EngSpriteFrame ) == 28, "EngSpriteFrame ABI drift" );
static_assert( sizeof( void * ) != 4 || sizeof( EngSpriteFrameDesc ) == 8, "EngSpriteFrameDesc ABI drift" );
static_assert( sizeof( void * ) != 4 || sizeof( EngSpriteGroup ) == 12, "EngSpriteGroup ABI drift" );
static_assert( sizeof( void * ) != 4 || sizeof( EngSprite ) == 36, "EngSprite ABI drift" );

// One renderable quad, fully resolved on the CPU.
struct SpriteItem
{
	const EngSpriteFrame *frame;
	float origin[3];
	float color[4];			// rgb = rendercolor (zero promoted to white), a = blend
	float scale;
	int rendermode;
	float distSq;			// to view origin, for far-to-near sorting
};

// Capacity: FrameEntities::kMaxEntities (1024, plan 2.2 contract; geom must
// not include the composition root, so the value is restated here) + 1 for
// the csz_testsprite quad.
const int kMaxItems = 1024 + 1;

// Vertex layout (plan 2.4 sprite contract): pos3 + uv2 + color4 = 9 floats.
const int kVertexFloats = 9;
const int kVertsPerQuad = 4;
const int kIndicesPerQuad = 6;

struct SpriteState
{
	bool shaderReady;
	int gpuGeneration;
	ShaderProgram program;
	int uViewProj;
	int uFog, uFogAdditive;		// M2a fog (no tint: sprites are emitters, plan 2.6)
	unsigned int vao, vbo, ibo;
};

struct TestSpriteState
{
	bool enabled;
	bool attempted;			// one load attempt per map (failure latched)
	model_t *model;			// lazily (re)loaded; keyed by world identity
	model_t *world;			// world the cached model pointer belongs to
};

SpriteState s_sprite;
TestSpriteState s_test;

SpriteItem s_items[kMaxItems];
float s_verts[kMaxItems * kVertsPerQuad * kVertexFloats];

const char kTestSpriteName[] = "sprites/glow01.spr";

// Throttled (>= 1s) Error dedup for per-frame failure paths.
void ThrottledError( const char *what, const char *modelName )
{
	static float s_nextWarn;
	float now = ClientTime();

	if( now < s_nextWarn )
		return;

	s_nextWarn = now + 1.0f;
	CSZ_LogError( "sprite", "%s (%s); sprite skipped", what, modelName );
}

// Runtime ABI plausibility self-check before trusting mirrored reads
// (discipline: ABI mirror + runtime self-check, csz_engine_bsp.h precedent).
bool ValidateSprite( const model_t *mod, const EngSprite *spr )
{
	if( spr == NULL )
		return false;

	if( spr->numframes < 1 || spr->numframes > 4096 )
	{
		ThrottledError( "implausible sprite numframes", mod->name );
		return false;
	}

	if( spr->type < 0 || spr->type > kSpriteMaxType )
	{
		ThrottledError( "implausible sprite type", mod->name );
		return false;
	}

	return true;
}

bool ValidateFrame( const model_t *mod, const EngSpriteFrame *frame )
{
	if( frame == NULL )
	{
		ThrottledError( "NULL sprite frame", mod->name );
		return false;
	}

	if( frame->width <= 0 || frame->width > 8192 || frame->height <= 0 || frame->height > 8192 ||
		frame->gl_texturenum <= 0 )
	{
		ThrottledError( "implausible sprite frame data", mod->name );
		return false;
	}

	return true;
}

// Engine-parity frame selection (pinned engine cl_sprite.c R_GetSpriteFrame):
// clamp index, single frames direct, group frames by time in the cumulative
// interval table. Angled groups take direction 0 (M1, plan section 8).
const EngSpriteFrame *SelectFrame( const model_t *mod, const EngSprite *spr, int frame, float time )
{
	if( frame < 0 )
		frame = 0;
	else if( frame >= spr->numframes )
		frame = spr->numframes - 1;

	const EngSpriteFrameDesc &desc = spr->frames[frame];

	if( desc.type == kSpriteFrameSingle )
		return desc.frameptr;

	if( desc.type == kSpriteFrameGroup || desc.type == kSpriteFrameAngled )
	{
		const EngSpriteGroup *group = (const EngSpriteGroup *)desc.frameptr;

		if( group == NULL || group->numframes < 1 || group->numframes > 4096 )
		{
			ThrottledError( "implausible sprite group", mod->name );
			return NULL;
		}

		if( desc.type == kSpriteFrameAngled )
			return group->frames[0];	// M1: no 8-way selection

		float fullinterval = group->intervals[group->numframes - 1];

		if( fullinterval <= 0.0f )
			return group->frames[0];

		// Loader guarantees positive, ascending intervals (engine fact).
		float targettime = time - (float)(int)( time / fullinterval ) * fullinterval;
		int i;

		for( i = 0; i < group->numframes - 1; i++ )
		{
			if( group->intervals[i] > targettime )
				break;
		}

		return group->frames[i];
	}

	ThrottledError( "unknown sprite frame type", mod->name );
	return NULL;
}

int CompareItems( const void *a, const void *b )
{
	const SpriteItem *ia = (const SpriteItem *)a;
	const SpriteItem *ib = (const SpriteItem *)b;

	// Far to near: blended quads must composite back-to-front.
	if( ia->distSq > ib->distSq )
		return -1;
	if( ia->distSq < ib->distSq )
		return 1;
	return 0;
}

void EnsureGpuObjects()
{
	if( s_sprite.shaderReady && s_sprite.gpuGeneration == GpuGeneration())
		return;

	if( s_sprite.gpuGeneration != GpuGeneration())
	{
		// Stale generation: forget names, never delete (T1 rule).
		s_sprite.program.program = 0;
		s_sprite.vao = s_sprite.vbo = s_sprite.ibo = 0;
	}

	// Init-time shader: compile failure is FATAL (spec 3.2).
	BuildProgram( "csz_sprite", kSpriteVs, kSpriteFs, true, s_sprite.program );
	s_sprite.uViewProj = UniformLoc( s_sprite.program, "u_viewProj" );
	s_sprite.uFog = UniformLoc( s_sprite.program, "u_fog" );
	s_sprite.uFogAdditive = UniformLoc( s_sprite.program, "u_fogAdditive" );

	UseProgram( s_sprite.program.program );
	glUniform1i( UniformLoc( s_sprite.program, "u_texDiffuse" ), 0 );

	const float kFogOff[4] = { 0.0f, 0.0f, 0.0f, 0.0f };	// fog off until fed (DrawSprites)

	glUniform4fv( s_sprite.uFog, 1, kFogOff );
	glUniform1i( s_sprite.uFogAdditive, 0 );
	UseProgram( 0 );

	glGenVertexArrays( 1, &s_sprite.vao );
	BindVao( s_sprite.vao );
	glGenBuffers( 1, &s_sprite.vbo );
	glBindBuffer( GL_ARRAY_BUFFER, s_sprite.vbo );

	const int stride = kVertexFloats * (int)sizeof( float );

	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)( 3 * sizeof( float )));
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 4, GL_FLOAT, GL_FALSE, stride, (const void *)( 5 * sizeof( float )));

	// Fixed index pattern for every possible quad, built once (STATIC):
	// 4 verts / 6 indices per sprite (plan section 8 step 1).
	unsigned short indices[kMaxItems * kIndicesPerQuad];

	for( int q = 0; q < kMaxItems; q++ )
	{
		unsigned short base = (unsigned short)( q * kVertsPerQuad );

		indices[q * 6 + 0] = base;
		indices[q * 6 + 1] = (unsigned short)( base + 1 );
		indices[q * 6 + 2] = (unsigned short)( base + 2 );
		indices[q * 6 + 3] = base;
		indices[q * 6 + 4] = (unsigned short)( base + 2 );
		indices[q * 6 + 5] = (unsigned short)( base + 3 );
	}

	glGenBuffers( 1, &s_sprite.ibo );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, s_sprite.ibo );	// binding recorded in the VAO
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof( indices ), indices, GL_STATIC_DRAW );

	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	s_sprite.gpuGeneration = GpuGeneration();
	s_sprite.shaderReady = true;
}

// Resolves one entity into a draw item; returns false when culled/invalid.
bool BuildItem( const ViewSetup &view, cl_entity_s *ent, SpriteItem &out )
{
	model_t *mod = ent->model;
	const EngSprite *spr = (const EngSprite *)mod->cache.data;

	if( !ValidateSprite( mod, spr ))
		return false;

	const EngSpriteFrame *frame = SelectFrame( mod, spr, (int)ent->curstate.frame, ClientTime());

	if( !ValidateFrame( mod, frame ))
		return false;

	float origin[3] = { ent->origin[0], ent->origin[1], ent->origin[2] };

	// movetype FOLLOW: ride the parent entity (studio attachment point when
	// curstate.body selects one) -- engine-parity position resolution.
	if( ent->curstate.movetype == MOVETYPE_FOLLOW && ent->curstate.aiment > 0 )
	{
		cl_entity_t *parent = gEngfuncs.GetEntityByIndex( ent->curstate.aiment );

		if( parent != NULL && parent->model != NULL )
		{
			if( parent->model->type == mod_studio && ent->curstate.body > 0 )
			{
				int num = ent->curstate.body;

				if( num > 4 )	// cl_entity_t carries 4 attachment slots
					num = 4;

				origin[0] = parent->attachment[num - 1][0];
				origin[1] = parent->attachment[num - 1][1];
				origin[2] = parent->attachment[num - 1][2];
			}
			else
			{
				origin[0] = parent->origin[0];
				origin[1] = parent->origin[1];
				origin[2] = parent->origin[2];
			}
		}
	}

	int rendermode = ent->curstate.rendermode;
	float alpha = ( rendermode == kRenderNormal ) ? 1.0f : (float)ent->curstate.renderamt * ( 1.0f / 255.0f );

	if( alpha <= 0.0f )
		return false;	// fully faded: engine skips these too

	float scale = ent->curstate.scale;

	if( scale <= 0.0f )
		scale = 1.0f;

	// Frustum cull on the scaled model bbox (engine-parity; sprites never
	// rotate, so translate + scale is exact).
	float mins[3], maxs[3];

	for( int i = 0; i < 3; i++ )
	{
		mins[i] = origin[i] + mod->mins[i] * scale;
		maxs[i] = origin[i] + mod->maxs[i] * scale;
	}

	if( view.frustum.CullBox( mins, maxs ))
		return false;

	out.frame = frame;
	out.origin[0] = origin[0];
	out.origin[1] = origin[1];
	out.origin[2] = origin[2];

	// rendercolor 0,0,0 is promoted to white (notes-mechanisms f-16).
	if( ent->curstate.rendercolor.r || ent->curstate.rendercolor.g || ent->curstate.rendercolor.b )
	{
		out.color[0] = (float)ent->curstate.rendercolor.r * ( 1.0f / 255.0f );
		out.color[1] = (float)ent->curstate.rendercolor.g * ( 1.0f / 255.0f );
		out.color[2] = (float)ent->curstate.rendercolor.b * ( 1.0f / 255.0f );
	}
	else
	{
		out.color[0] = out.color[1] = out.color[2] = 1.0f;
	}

	out.color[3] = alpha;
	out.scale = scale;
	out.rendermode = rendermode;

	float dx = origin[0] - view.origin[0];
	float dy = origin[1] - view.origin[1];
	float dz = origin[2] - view.origin[2];

	out.distSq = dx * dx + dy * dy + dz * dz;
	return true;
}

// Deterministic acceptance object (plan section 8): an additive glow quad
// 64 units ahead of the camera, available on maps with no sprite entities.
bool BuildTestItem( const ViewSetup &view, SpriteItem &out )
{
	model_t *world = WorldModel();

	if( !s_test.attempted || s_test.world != world )
	{
		// One attempt per map; the old model pointer is dead on map change.
		// CL_LoadModel only resolves SERVER-precached models (pinned engine
		// cl_game.c:2624 CL_FindModelIndex), so the client-side HUD sprite
		// path is the reliable loader here: SPR_Load forces the load and
		// GetSpritePointer hands back the model_t.
		HSPRITE hspr = gEngfuncs.pfnSPR_Load( kTestSpriteName );

		s_test.model = ( hspr != 0 ) ? (model_t *)gEngfuncs.GetSpritePointer( hspr ) : NULL;
		s_test.world = world;
		s_test.attempted = true;

		if( s_test.model == NULL || s_test.model->type != mod_sprite )
		{
			s_test.model = NULL;	// failure latched until next map
			CSZ_LogError( "sprite", "test sprite %s failed to load (logged once per map)", kTestSpriteName );
		}
	}

	if( s_test.model == NULL )
		return false;

	const EngSprite *spr = (const EngSprite *)s_test.model->cache.data;

	if( !ValidateSprite( s_test.model, spr ))
		return false;

	const EngSpriteFrame *frame = SelectFrame( s_test.model, spr, 0, ClientTime());

	if( !ValidateFrame( s_test.model, frame ))
		return false;

	// Forward axis from the view matrix (column-major rows; GL eye looks
	// down -Z, so forward = -row2).
	const float *m = view.matView.m;
	float fwd[3] = { -m[2], -m[6], -m[10] };

	out.frame = frame;
	out.origin[0] = view.origin[0] + fwd[0] * 64.0f;
	out.origin[1] = view.origin[1] + fwd[1] * 64.0f;
	out.origin[2] = view.origin[2] + fwd[2] * 64.0f;
	out.color[0] = out.color[1] = out.color[2] = 1.0f;
	out.color[3] = 1.0f;
	out.scale = 1.0f;
	out.rendermode = kRenderTransAdd;	// depth test ON, depth write OFF

	float dx = out.origin[0] - view.origin[0];
	float dy = out.origin[1] - view.origin[1];
	float dz = out.origin[2] - view.origin[2];

	out.distSq = dx * dx + dy * dy + dz * dz;
	return true;
}

void TestSpriteCommand()
{
	s_test.enabled = !s_test.enabled;

	if( s_test.enabled )
		CSZ_LogInfo( "sprite", "test sprite enabled (%s)", kTestSpriteName );
	else
		CSZ_LogInfo( "sprite", "test sprite disabled" );
}

}

void DrawSprites( const ViewSetup &view, cl_entity_s *const *ents, int count )
{
	int numItems = 0;

	for( int i = 0; i < count && numItems < kMaxItems - 1; i++ )
	{
		cl_entity_s *ent = ents[i];

		if( ent == NULL || ent->model == NULL || ent->model->type != mod_sprite || ent->model->cache.data == NULL )
			continue;

		if( BuildItem( view, ent, s_items[numItems] ))
			numItems++;
	}

	if( s_test.enabled && BuildTestItem( view, s_items[numItems] ))
		numItems++;

	if( numItems == 0 )
		return;	// zero-cost pass on sprite-free frames

	qsort( s_items, (size_t)numItems, sizeof( SpriteItem ), CompareItems );

	EnsureGpuObjects();

	// Billboard axes from the view matrix rows (plan section 8: parallel
	// orientation only; column-major element (row, col) = m[col*4+row]).
	const float *m = view.matView.m;
	float right[3] = { m[0], m[4], m[8] };
	float up[3] = { m[1], m[5], m[9] };

	// Engine-parity quad corners (pinned ref gl_sprite.c R_DrawSpriteQuad):
	// (left,down) uv(0,1) -> (left,up) uv(0,0) -> (right,up) uv(1,0) ->
	// (right,down) uv(1,1); left/down are negative extents in the frame data.
	for( int i = 0; i < numItems; i++ )
	{
		const SpriteItem &it = s_items[i];
		float *v = &s_verts[i * kVertsPerQuad * kVertexFloats];
		const float corner[4][2] = {
			{ it.frame->left, it.frame->down },
			{ it.frame->left, it.frame->up },
			{ it.frame->right, it.frame->up },
			{ it.frame->right, it.frame->down },
		};
		const float uv[4][2] = {{ 0.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }};

		for( int c = 0; c < 4; c++ )
		{
			float r = corner[c][0] * it.scale;
			float u = corner[c][1] * it.scale;

			v[0] = it.origin[0] + right[0] * r + up[0] * u;
			v[1] = it.origin[1] + right[1] * r + up[1] * u;
			v[2] = it.origin[2] + right[2] * r + up[2] * u;
			v[3] = uv[c][0];
			v[4] = uv[c][1];
			v[5] = it.color[0];
			v[6] = it.color[1];
			v[7] = it.color[2];
			v[8] = it.color[3];
			v += kVertexFloats;
		}
	}

	UseProgram( s_sprite.program.program );
	glUniformMatrix4fv( s_sprite.uViewProj, 1, GL_FALSE, view.matViewProj.m );

	// Ambience feed (M2a A1): fog only -- sprites are emitters, so no night
	// tint (plan 2.6). The additive/alpha fog form is selected per item below.
	const AmbienceParams &amb = view.ambience;
	const float fogVec[4] = { amb.fogColor[0], amb.fogColor[1], amb.fogColor[2], amb.fogDensity };

	glUniform4fv( s_sprite.uFog, 1, fogVec );

	BindVao( s_sprite.vao );

	glBindBuffer( GL_ARRAY_BUFFER, s_sprite.vbo );
	glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)( (size_t)numItems * kVertsPerQuad * kVertexFloats * sizeof( float )),
		s_verts, GL_STREAM_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );	// attribute bindings live in the VAO

	SetCull( false );	// billboards always face the camera; no winding games

	int curFogAdditive = -1;	// force the first item to set it

	for( int i = 0; i < numItems; i++ )
	{
		const SpriteItem &it = s_items[i];
		int fogAdditive = 0;

		// Blend state by rendermode (engine-parity, pinned gl_sprite.c
		// R_DrawSpriteModel:404; plan section 8 step 1). Depth TEST stays on
		// for everything except glow; depth WRITE is off for every blended
		// mode so sprites never occlude later passes. Additive modes take the
		// fade-to-black fog form (u_fogAdditive, plan 2.6).
		switch( it.rendermode )
		{
		case kRenderGlow:
			SetBlend( kBlendAdditive );
			SetDepthWrite( false );
			SetDepthTest( false );	// glow: no Z checks (engine parity)
			fogAdditive = 1;
			break;
		case kRenderTransAdd:
			SetBlend( kBlendAdditive );
			SetDepthWrite( false );
			SetDepthTest( true );
			fogAdditive = 1;
			break;
		case kRenderTransAlpha:
		case kRenderTransTexture:
		case kRenderTransColor:
			SetBlend( kBlendAlpha );
			SetDepthWrite( false );
			SetDepthTest( true );
			break;
		case kRenderNormal:
		default:
			// M1 approximation: the GL3 path has no fixed-function alpha
			// test, so cutout-format sprites in normal mode go through
			// alpha blend (alpha = 1) instead of opaque + alphatest.
			SetBlend( kBlendAlpha );
			SetDepthWrite( true );
			SetDepthTest( true );
			break;
		}

		if( fogAdditive != curFogAdditive )
		{
			glUniform1i( s_sprite.uFogAdditive, fogAdditive );
			curFogAdditive = fogAdditive;
		}

		BindTextureSlot( 0, it.frame->gl_texturenum );
		glDrawElements( GL_TRIANGLES, kIndicesPerQuad, GL_UNSIGNED_SHORT,
			(const void *)( (size_t)i * kIndicesPerQuad * sizeof( unsigned short )));
	}

	// Hand a clean baseline to the next pass (viewmodel, slot 15).
	SetBlend( kBlendNone );
	SetDepthWrite( true );
	SetDepthTest( true );
	BindVao( 0 );

	// Per-frame Dev log, self-throttled >= 1s (logging rule R8).
	{
		static float s_nextLog;
		float now = ClientTime();

		if( now >= s_nextLog )
		{
			s_nextLog = now + 1.0f;
			CSZ_LogDev( "sprite", "drawn %d sprite quads", numItems );
		}
	}
}

void RegisterSpriteCommands()
{
	gEngfuncs.pfnAddCommand( "csz_testsprite", TestSpriteCommand );
}

}
