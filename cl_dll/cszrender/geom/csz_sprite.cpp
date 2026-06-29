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

#include "pm_defs.h"	// PM_WORLD_ONLY, pmtrace_t -- for kRenderGlow occlusion gate
#include "event_api.h"	// gEngfuncs.pEventAPI->EV_SetTraceHull / EV_PlayerTrace

#include <stdlib.h>
#include <string.h>
#include <math.h>

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

// msprite_t::type, angletype_t (engine sprite.h:54-61). M2c reproduces every
// orientation; the values are the classic Quake/GoldSrc angletype_t enum.
const int kSpriteMaxType = 4;		// SPR_VP_PARALLEL_ORIENTED

// angletype_t values (engine sprite.h). Names follow the GoldSrc enum so the
// orientation math below reads like the engine's R_GetSpriteAxes.
const int kSprVPParallelUpright = 0;	// faces the view plane, Z-locked upright
const int kSprFacingUpright = 1;	// upright, faces the viewer's origin
const int kSprVPParallel = 2;		// full view-plane billboard (default)
const int kSprOriented = 3;		// fixed world orientation from entity angles
const int kSprVPParallelOriented = 4;	// view-parallel, rolled by entity roll

const float kSpriteDegToRad = 3.14159265358979323846f / 180.0f;

// Normalize in place; returns false (leaving v untouched) for a ~zero vector.
bool SpriteNormalize( float v[3] )
{
	float len = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );

	if( len < 1e-6f )
		return false;

	float inv = 1.0f / len;
	v[0] *= inv; v[1] *= inv; v[2] *= inv;
	return true;
}

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
	float right[3];			// billboard basis, per-type (M2c orientations)
	float up[3];
	float color[4];			// rgb = rendercolor (zero promoted to white), a = blend
	float scale;
	int rendermode;
	int spriteType;			// effective angletype_t after the csz_sprite_orient gate
	bool used8way;			// directional frame was selected from an angled group
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
	int uFogParams, uCamPos;	// analytic base fog (fog M1 Step 2): height b/maxOpacity + ray origin
	int uAlphaTest;			// M2c kRenderNormal hard cutout threshold (0 = off)
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

// Per-feature observability cvars (default 1 = engine-parity behavior on;
// 0 = fall back to the M1 view-parallel / alpha-blend / dir-0 / world-only path).
cvar_t *s_cvOrient;	// csz_sprite_orient: non-parallel orientations
cvar_t *s_cvCutout;	// csz_sprite_cutout: kRenderNormal hard alpha-test
cvar_t *s_cv8way;	// csz_sprite_8way: directional frame selection
cvar_t *s_cvGlow;	// csz_glow: glow distance scaling + brush-ent occlusion

bool CvarOn( const cvar_t *c )
{
	return c != NULL && c->value != 0.0f;
}

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
// interval table. Angled groups select an 8-way directional frame from the
// camera yaw relative to the sprite yaw (csz_sprite_8way); *angledOut latches
// when a directional pick actually happened (observability).
const EngSpriteFrame *SelectFrame( const model_t *mod, const EngSprite *spr, int frame, float time,
	float viewYaw, float entYaw, bool use8way, bool *angledOut )
{
	if( angledOut != NULL )
		*angledOut = false;

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
		{
			if( !use8way )
				return group->frames[0];	// directional selection disabled

			// Engine angle->frame selection (Xash3D-FWGS R_GetSpriteFrame,
			// studied clean-room): map the camera yaw relative to the sprite
			// yaw onto 8 even buckets. Q_rint is round-half-up here; the &7
			// wraps the (rint - 4) bias into [0,7] (two's-complement safe).
			float rel = ( viewYaw - entYaw + 45.0f ) / 360.0f * 8.0f;
			int af = ( (int)floorf( rel + 0.5f ) - 4 ) & 7;

			if( af < 0 || af >= group->numframes )	// short groups (<8 dirs)
				af = 0;
			else if( angledOut != NULL )
				*angledOut = true;

			return group->frames[af];
		}

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
	s_sprite.uFogParams = UniformLoc( s_sprite.program, "u_fogParams" );
	s_sprite.uCamPos = UniformLoc( s_sprite.program, "u_camPos" );
	s_sprite.uAlphaTest = UniformLoc( s_sprite.program, "u_alphaTest" );

	UseProgram( s_sprite.program.program );
	glUniform1i( UniformLoc( s_sprite.program, "u_texDiffuse" ), 0 );
	// u_fog / u_fogParams / u_camPos / u_fogAdditive are fed per-frame by DrawSprites
	// (which forces u_fogAdditive on item 0 via curFogAdditive=-1) before the first
	// draw, so no init-time defaults are needed here.
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

// Squared distance from a world point to the view origin (sprite sort key).
float DistSqToView( const float p[3], const float viewOrigin[3] )
{
	float dx = p[0] - viewOrigin[0];
	float dy = p[1] - viewOrigin[1];
	float dz = p[2] - viewOrigin[2];
	return dx * dx + dy * dy + dz * dz;
}

// Builds the billboard basis (right/up world axes) for one sprite per its
// angletype_t, matching the Xash3D-FWGS R_GetSpriteAxes mechanism (studied
// clean-room). View axes come from the view-matrix rows: vright = row0,
// vup = row1, vforward = -row2 (GL eye looks down -Z). orientEnabled false
// forces classic view-parallel for every type (csz_sprite_orient 0). Returns
// false only for the degenerate near-vertical VP_PARALLEL_UPRIGHT view, where
// the engine drops the sprite; the caller then culls it.
bool ComputeSpriteAxes( const ViewSetup &view, int type, const float entAngles[3],
	const float origin[3], bool orientEnabled, float right[3], float up[3] )
{
	const float *m = view.matView.m;
	float vright[3]   = { m[0], m[4], m[8] };
	float vup[3]      = { m[1], m[5], m[9] };
	float vforward[3] = { -m[2], -m[6], -m[10] };

	if( !orientEnabled )
		type = kSprVPParallel;

	switch( type )
	{
	case kSprVPParallelUpright:	// 0: Z-locked upright, faces the view plane
		if( vforward[2] > 0.999848f || vforward[2] < -0.999848f )
			return false;		// looking ~straight up/down: engine skips it
		up[0] = 0.0f; up[1] = 0.0f; up[2] = 1.0f;
		right[0] = vforward[1]; right[1] = -vforward[0]; right[2] = 0.0f;
		SpriteNormalize( right );
		break;

	case kSprFacingUpright:		// 1: upright, faces the viewer's origin
		up[0] = 0.0f; up[1] = 0.0f; up[2] = 1.0f;
		right[0] = origin[1] - view.origin[1];
		right[1] = -( origin[0] - view.origin[0] );
		right[2] = 0.0f;
		if( !SpriteNormalize( right ))	// sprite directly over/under the eye
		{
			right[0] = vright[0]; right[1] = vright[1]; right[2] = 0.0f;
			SpriteNormalize( right );
		}
		break;

	case kSprOriented:		// 3: fixed world orientation from entity angles
		AngleVectors( entAngles, NULL, right, up );
		break;

	case kSprVPParallelOriented:	// 4: view-parallel, rolled by entity roll
	{
		float angle = entAngles[2] * kSpriteDegToRad;	// ROLL
		float sr = sinf( angle ), cr = cosf( angle );

		for( int i = 0; i < 3; i++ )
		{
			right[i] = vright[i] * cr + vup[i] * sr;
			up[i]    = vright[i] * -sr + vup[i] * cr;
		}
		break;
	}

	case kSprVPParallel:		// 2: full view-plane billboard (default)
	default:
		right[0] = vright[0]; right[1] = vright[1]; right[2] = vright[2];
		up[0] = vup[0]; up[1] = vup[1]; up[2] = vup[2];
		break;
	}

	return true;
}

// Resolves one entity into a draw item; returns false when culled/invalid.
bool BuildItem( const ViewSetup &view, cl_entity_s *ent, SpriteItem &out )
{
	model_t *mod = ent->model;
	const EngSprite *spr = (const EngSprite *)mod->cache.data;

	if( !ValidateSprite( mod, spr ))
		return false;

	bool use8way = CvarOn( s_cv8way );
	bool angled = false;
	const EngSpriteFrame *frame = SelectFrame( mod, spr, (int)ent->curstate.frame, ClientTime(),
		view.angles[1], ent->angles[1], use8way, &angled );

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

	// Distance-proportional glow SIZE scaling (engine parity): glow sprites keep
	// a roughly constant on-screen size as the camera recedes, so the world-space
	// quad grows with distance (~constant angular size). kGlowSizeCoeff = 1/200 is
	// the classic GoldSrc reference factor (unit scale at ~200 units). M1 used
	// curstate.scale alone, so distant glows shrank to specks. Applied BEFORE the
	// frustum cull so a grown glow is never culled early (safe error = keep it);
	// csz_glow off keeps the old curstate.scale-only behavior.
	if( rendermode == kRenderGlow && CvarOn( s_cvGlow ))
	{
		const float kGlowSizeCoeff = 1.0f / 200.0f;
		float gx = origin[0] - view.origin[0];
		float gy = origin[1] - view.origin[1];
		float gz = origin[2] - view.origin[2];
		float distScale = sqrtf( gx * gx + gy * gy + gz * gz ) * kGlowSizeCoeff;

		if( distScale < 1.0f )		// never shrink a near glow below its base size
			distScale = 1.0f;

		scale *= distScale;
	}

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

	// kRenderGlow world-visibility gate: depth test is off for glow (engine
	// parity, const.h "No Z buffer checks"), so we compensate with a world-only
	// CPU ray from the eye to the sprite.  Uses the event API + PM_WORLD_ONLY,
	// matching every other world-only trace in the client (environment.cpp,
	// entity.cpp); EV_PlayerTrace fills a caller-owned pmtrace_t (no NULL to
	// guard, unlike the static-storage PM_TraceLine).
	//
	// Brush-entity policy (deliberate): PM_WORLD_ONLY traces ONLY the static
	// worldspawn BSP, so func_wall / func_door / etc. do NOT occlude glows.
	// This is intentional -- the reported bug is glow bleeding through STATIC
	// walls, which PM_WORLD_ONLY fixes; brush-entity occlusion is an explicitly
	// deferred edge case, not an oversight.
	//
	// Robustness caveat (MEDIUM-5): a pmove-based trace clips against the
	// physent/world set built during client prediction.  With cl_predict 0, in
	// some spectator/replay paths, or before the first prediction frame, that
	// set can be stale/empty -> fraction == 1 -> no cull -> a glow may briefly
	// show through a wall.  The visual TEST pass probes these states.
	if( rendermode == kRenderGlow )
	{
		bool glowFull = CvarOn( s_cvGlow );
		float start[3] = { view.origin[0], view.origin[1], view.origin[2] };
		float end[3]   = { origin[0], origin[1], origin[2] };
		pmtrace_t tr;

		// Occluder set: csz_glow on -> trace world AND brush entities (func_wall/
		// func_door/...), ignoring studio models and glass-rendermode ents so a
		// player or a window never flickers a lamp glow.  This matches the engine's
		// R_GlowSightDistance trace (PM_STUDIO_IGNORE|PM_GLASS_IGNORE).  csz_glow
		// off restores the M1 world-only trace (brush ents do NOT occlude).
		int traceFlags = glowFull ? ( PM_STUDIO_IGNORE | PM_GLASS_IGNORE ) : PM_WORLD_ONLY;

		gEngfuncs.pEventAPI->EV_SetTraceHull( 2 );	// 2 = point hull
		gEngfuncs.pEventAPI->EV_PlayerTrace( start, end, traceFlags, -1, &tr );

		// Distance-proportional tolerance instead of a fixed fraction (HIGH-3):
		// fraction is normalized to ray length, so a constant 0.98 is a wide
		// world-space band on far sprites and a near-zero one on close ones.
		// Glow/halo sprites are routinely mounted flush on (or just inside)
		// wall/ceiling light fixtures, so cull ONLY when the occluder sits more
		// than kGlowOccludeSlop world units IN FRONT of the sprite -- this keeps
		// flush-mounted lamp glows while still culling glows genuinely behind a
		// wall.  The safe error direction is to keep a legitimate glow, never to
		// drop one.  Compared in squared form to avoid a sqrt.
		const float kGlowOccludeSlop = 16.0f;	// world units
		float ex = end[0] - start[0];
		float ey = end[1] - start[1];
		float ez = end[2] - start[2];
		float rayLenSq = ex * ex + ey * ey + ez * ez;
		float behind   = 1.0f - tr.fraction;	// ray fraction between the hit and the sprite

		if( behind > 0.0f && behind * behind * rayLenSq > kGlowOccludeSlop * kGlowOccludeSlop )
			return false;
	}

	// Per-type billboard basis (M2c). orientEnabled folds the type down to
	// view-parallel when csz_sprite_orient is 0.
	bool orientOn = CvarOn( s_cvOrient );

	if( !ComputeSpriteAxes( view, (int)spr->type, ent->angles, origin, orientOn, out.right, out.up ))
		return false;	// degenerate near-vertical upright view: engine drops it

	out.spriteType = orientOn ? (int)spr->type : kSprVPParallel;
	out.used8way = angled;

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

	out.distSq = DistSqToView( origin, view.origin );
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

	const EngSpriteFrame *frame = SelectFrame( s_test.model, spr, 0, ClientTime(), 0.0f, 0.0f, false, NULL );

	if( !ValidateFrame( s_test.model, frame ))
		return false;

	// Forward axis from the view matrix (column-major rows; GL eye looks
	// down -Z, so forward = -row2). The test quad is a plain view-parallel
	// billboard: right = row0, up = row1.
	const float *m = view.matView.m;
	float fwd[3] = { -m[2], -m[6], -m[10] };

	out.right[0] = m[0]; out.right[1] = m[4]; out.right[2] = m[8];
	out.up[0] = m[1]; out.up[1] = m[5]; out.up[2] = m[9];
	out.spriteType = kSprVPParallel;
	out.used8way = false;

	out.frame = frame;
	out.origin[0] = view.origin[0] + fwd[0] * 64.0f;
	out.origin[1] = view.origin[1] + fwd[1] * 64.0f;
	out.origin[2] = view.origin[2] + fwd[2] * 64.0f;
	out.color[0] = out.color[1] = out.color[2] = 1.0f;
	out.color[3] = 1.0f;
	out.scale = 1.0f;
	out.rendermode = kRenderTransAdd;	// depth test ON, depth write OFF

	out.distSq = DistSqToView( out.origin, view.origin );
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

	// Per-item billboard axes (M2c): each item carries its own right/up basis,
	// built in BuildItem per the sprite's angletype_t (view-parallel, upright,
	// facing-upright, oriented, parallel-oriented). The single shared view-row
	// basis of M1 is gone -- it could only express VP_PARALLEL.
	//
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

			v[0] = it.origin[0] + it.right[0] * r + it.up[0] * u;
			v[1] = it.origin[1] + it.right[1] * r + it.up[1] * u;
			v[2] = it.origin[2] + it.right[2] * r + it.up[2] * u;
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
	float fogVec[4], fogParams[4];
	CszFogUniformVecs( amb, fogVec, fogParams );	// analytic base fog (fog M1 Step 2): same form as world/studio

	glUniform4fv( s_sprite.uFog, 1, fogVec );
	glUniform4fv( s_sprite.uFogParams, 1, fogParams );
	glUniform3fv( s_sprite.uCamPos, 1, view.origin );	// ray origin for the height-fog integral

	BindVao( s_sprite.vao );

	glBindBuffer( GL_ARRAY_BUFFER, s_sprite.vbo );
	glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)( (size_t)numItems * kVertsPerQuad * kVertexFloats * sizeof( float )),
		s_verts, GL_STREAM_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );	// attribute bindings live in the VAO

	// SPR_ORIENTED quads can present either face to the camera, so culling stays
	// off for every sprite (the billboard/oriented winding is not guaranteed).
	SetCull( false );

	bool cutoutOn = CvarOn( s_cvCutout );
	int curFogAdditive = -1;	// force the first item to set it
	float curAlphaTest = -1.0f;	// force the first item to set it

	// Per-frame feature tallies ([CSZ:sprite] observability).
	int nOrient = 0, nCutout = 0, n8way = 0, nGlow = 0;

	for( int i = 0; i < numItems; i++ )
	{
		const SpriteItem &it = s_items[i];
		int fogAdditive = 0;
		float alphaTest = 0.0f;

		if( it.spriteType != kSprVPParallel )
			nOrient++;
		if( it.used8way )
			n8way++;
		if( it.rendermode == kRenderGlow )
			nGlow++;

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
			SetDepthTest( false );	// glow: no Z checks (engine parity); world occlusion gate applied in BuildItem
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
			if( cutoutOn )
			{
				// Engine-parity cutout: opaque draw + hard alpha-test discard
				// in the FS (GL3 core has no fixed-function alpha test), giving
				// crisp .spr edges with no soft halo and no black box.
				SetBlend( kBlendNone );
				SetDepthWrite( true );
				SetDepthTest( true );
				alphaTest = 0.5f;
				nCutout++;
			}
			else
			{
				// M1 fallback: no alpha test -> soft alpha blend (alpha = 1).
				SetBlend( kBlendAlpha );
				SetDepthWrite( true );
				SetDepthTest( true );
			}
			break;
		}

		if( fogAdditive != curFogAdditive )
		{
			glUniform1i( s_sprite.uFogAdditive, fogAdditive );
			curFogAdditive = fogAdditive;
		}

		if( alphaTest != curAlphaTest )
		{
			glUniform1f( s_sprite.uAlphaTest, alphaTest );
			curAlphaTest = alphaTest;
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
			CSZ_LogDev( "sprite", "[CSZ:sprite] drawn=%d orient=%d cutout=%d 8way=%d glow=%d",
				numItems, nOrient, nCutout, n8way, nGlow );
		}
	}
}

void RegisterSpriteCommands()
{
	gEngfuncs.pfnAddCommand( "csz_testsprite", TestSpriteCommand );

	// Per-feature cvars (M2c), default 1 = engine-parity behavior; 0 = M1 fallback.
	s_cvOrient = gEngfuncs.pfnRegisterVariable( "csz_sprite_orient", "1", FCVAR_CLIENTDLL );
	s_cvCutout = gEngfuncs.pfnRegisterVariable( "csz_sprite_cutout", "1", FCVAR_CLIENTDLL );
	s_cv8way   = gEngfuncs.pfnRegisterVariable( "csz_sprite_8way",   "1", FCVAR_CLIENTDLL );
	s_cvGlow   = gEngfuncs.pfnRegisterVariable( "csz_glow",          "1", FCVAR_CLIENTDLL );
}

}
