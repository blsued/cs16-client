/*
 * csz_spike.cpp -- CSOZ renderer: M2c de-risk spikes (S1/S2/S3), cvar-gated probe
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
#include "csz_spike.h"
#include "../core/csz_engine.h"
#include "../core/csz_engine_bsp.h"
#include "../core/csz_log.h"

#include "r_efx.h"		// efx_api_t + BEAM (beam/particle/tracer emit table to shim)
#include "triangleapi.h"	// triangleapi_t (S2 member-presence probe)

#include <stdio.h>
#include <string.h>

namespace csz
{

namespace
{

// ---------------------------------------------------------------------------
// efx emit table shim (S1 hit proof + S3 frequency calibration).
//
// The client emits ALL beam/particle/tracer effects through gEngfuncs.pEfxAPI
// (ev_hldm.cpp / events/ev_cs16.cpp / entity.cpp). When csz_spike != 0 we copy
// the engine's whole efx_api_t by value, redirect the beam/spark/particle/tracer
// creators to thin wrappers that COUNT the call and FORWARD to the real engine
// function (so behaviour is byte-identical -- the engine still simulates them),
// then point gEngfuncs.pEfxAPI at our copy. Every other member passes straight
// through (untouched copy). This is the exact C-shim mechanism the M2c plan
// (decision C) needs to de-risk: one chokepoint, A/B by toggling the cvar,
// zero edits at the emit sites. We do NOT redirect R_DecalShoot (decision A
// keeps the engine decal path) -- it is wrapped only to COUNT, still forwarding.
// ---------------------------------------------------------------------------
enum EfxId
{
	EFX_BeamPoints, EFX_BeamEntPoint, EFX_BeamEnts, EFX_BeamFollow, EFX_BeamRing,
	EFX_BeamCirclePoints, EFX_BeamLightning,
	EFX_SparkEffect, EFX_SparkShower, EFX_SparkStreaks, EFX_StreakSplash,
	EFX_BulletImpactParticles, EFX_RocketTrail, EFX_Sprite_Trail,
	EFX_TracerEffect, EFX_RunParticleEffect, EFX_MuzzleFlash, EFX_DecalShoot,
	EFX_COUNT
};

const char *const kEfxNames[EFX_COUNT] =
{
	"R_BeamPoints", "R_BeamEntPoint", "R_BeamEnts", "R_BeamFollow", "R_BeamRing",
	"R_BeamCirclePoints", "R_BeamLightning",
	"R_SparkEffect", "R_SparkShower", "R_SparkStreaks", "R_StreakSplash",
	"R_BulletImpactParticles", "R_RocketTrail", "R_Sprite_Trail",
	"R_TracerEffect", "R_RunParticleEffect", "R_MuzzleFlash", "R_DecalShoot"
};

efx_api_t *s_realEfx;		// the engine's real table (forward target)
efx_api_t  s_shimEfx;		// our by-value copy with the wrappers patched in
bool       s_installed;

unsigned s_efxCount[EFX_COUNT];	// reset every ~1s dump (per-second rate)
bool     s_efxSeen[EFX_COUNT];	// first-hit latch (proves the swap intercepted a live emit)

inline void BumpEfx( int id )
{
	s_efxCount[id]++;

	if( !s_efxSeen[id] )
	{
		s_efxSeen[id] = true;
		// FIRST-HIT: the client emitted through OUR table -> the pEfxAPI swap works.
		CSZ_LogInfo( "spike", "efx FIRST-HIT: %s (shim intercepted a live emitter)", kEfxNames[id] );
	}
}

// --- beam wrappers (return BEAM*) --------------------------------------------
BEAM *Shim_R_BeamPoints( float *start, float *end, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	BumpEfx( EFX_BeamPoints );
	return s_realEfx->R_BeamPoints( start, end, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
}

BEAM *Shim_R_BeamEntPoint( int startEnt, float *end, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	BumpEfx( EFX_BeamEntPoint );
	return s_realEfx->R_BeamEntPoint( startEnt, end, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
}

BEAM *Shim_R_BeamEnts( int startEnt, int endEnt, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	BumpEfx( EFX_BeamEnts );
	return s_realEfx->R_BeamEnts( startEnt, endEnt, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
}

BEAM *Shim_R_BeamFollow( int startEnt, int modelIndex, float life, float width, float r, float g, float b, float brightness )
{
	BumpEfx( EFX_BeamFollow );
	return s_realEfx->R_BeamFollow( startEnt, modelIndex, life, width, r, g, b, brightness );
}

BEAM *Shim_R_BeamRing( int startEnt, int endEnt, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	BumpEfx( EFX_BeamRing );
	return s_realEfx->R_BeamRing( startEnt, endEnt, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
}

BEAM *Shim_R_BeamCirclePoints( int type, float *start, float *end, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	BumpEfx( EFX_BeamCirclePoints );
	return s_realEfx->R_BeamCirclePoints( type, start, end, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
}

BEAM *Shim_R_BeamLightning( float *start, float *end, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed )
{
	BumpEfx( EFX_BeamLightning );
	return s_realEfx->R_BeamLightning( start, end, modelIndex, life, width, amplitude, brightness, speed );
}

// --- particle / spark / tracer wrappers (void) -------------------------------
void Shim_R_SparkEffect( float *pos, int count, int velocityMin, int velocityMax )
{
	BumpEfx( EFX_SparkEffect );
	s_realEfx->R_SparkEffect( pos, count, velocityMin, velocityMax );
}

void Shim_R_SparkShower( float *pos )
{
	BumpEfx( EFX_SparkShower );
	s_realEfx->R_SparkShower( pos );
}

void Shim_R_SparkStreaks( float *pos, int count, int velocityMin, int velocityMax )
{
	BumpEfx( EFX_SparkStreaks );
	s_realEfx->R_SparkStreaks( pos, count, velocityMin, velocityMax );
}

void Shim_R_StreakSplash( float *pos, float *dir, int color, int count, float speed, int velocityMin, int velocityMax )
{
	BumpEfx( EFX_StreakSplash );
	s_realEfx->R_StreakSplash( pos, dir, color, count, speed, velocityMin, velocityMax );
}

void Shim_R_BulletImpactParticles( float *pos )
{
	BumpEfx( EFX_BulletImpactParticles );
	s_realEfx->R_BulletImpactParticles( pos );
}

void Shim_R_RocketTrail( float *start, float *end, int type )
{
	BumpEfx( EFX_RocketTrail );
	s_realEfx->R_RocketTrail( start, end, type );
}

void Shim_R_Sprite_Trail( int type, float *start, float *end, int modelIndex, int count, float life,
	float size, float amplitude, int renderamt, float speed )
{
	BumpEfx( EFX_Sprite_Trail );
	s_realEfx->R_Sprite_Trail( type, start, end, modelIndex, count, life, size, amplitude, renderamt, speed );
}

void Shim_R_TracerEffect( float *start, float *end )
{
	BumpEfx( EFX_TracerEffect );
	s_realEfx->R_TracerEffect( start, end );
}

void Shim_R_RunParticleEffect( float *org, float *dir, int color, int count )
{
	BumpEfx( EFX_RunParticleEffect );
	s_realEfx->R_RunParticleEffect( org, dir, color, count );
}

void Shim_R_MuzzleFlash( float *pos1, int type )
{
	BumpEfx( EFX_MuzzleFlash );
	s_realEfx->R_MuzzleFlash( pos1, type );
}

void Shim_R_DecalShoot( int textureIndex, int entity, int modelIndex, float *position, int flags )
{
	// COUNT only -- decision A keeps the engine decal path (R_DecalShoot stays
	// the real engine function); we just observe how often decals are created.
	BumpEfx( EFX_DecalShoot );
	s_realEfx->R_DecalShoot( textureIndex, entity, modelIndex, position, flags );
}

void InstallEfxShim()
{
	if( s_installed )
		return;

	s_realEfx = gEngfuncs.pEfxAPI;

	if( s_realEfx == NULL )
	{
		CSZ_LogWarn( "spike", "gEngfuncs.pEfxAPI is NULL; cannot install efx shim (S1 emit-intercept proof BLOCKED)" );
		return;
	}

	s_shimEfx = *s_realEfx;	// transparent passthrough copy of the whole table

	s_shimEfx.R_BeamPoints = Shim_R_BeamPoints;
	s_shimEfx.R_BeamEntPoint = Shim_R_BeamEntPoint;
	s_shimEfx.R_BeamEnts = Shim_R_BeamEnts;
	s_shimEfx.R_BeamFollow = Shim_R_BeamFollow;
	s_shimEfx.R_BeamRing = Shim_R_BeamRing;
	s_shimEfx.R_BeamCirclePoints = Shim_R_BeamCirclePoints;
	s_shimEfx.R_BeamLightning = Shim_R_BeamLightning;
	s_shimEfx.R_SparkEffect = Shim_R_SparkEffect;
	s_shimEfx.R_SparkShower = Shim_R_SparkShower;
	s_shimEfx.R_SparkStreaks = Shim_R_SparkStreaks;
	s_shimEfx.R_StreakSplash = Shim_R_StreakSplash;
	s_shimEfx.R_BulletImpactParticles = Shim_R_BulletImpactParticles;
	s_shimEfx.R_RocketTrail = Shim_R_RocketTrail;
	s_shimEfx.R_Sprite_Trail = Shim_R_Sprite_Trail;
	s_shimEfx.R_TracerEffect = Shim_R_TracerEffect;
	s_shimEfx.R_RunParticleEffect = Shim_R_RunParticleEffect;
	s_shimEfx.R_MuzzleFlash = Shim_R_MuzzleFlash;
	s_shimEfx.R_DecalShoot = Shim_R_DecalShoot;

	gEngfuncs.pEfxAPI = &s_shimEfx;
	s_installed = true;

	CSZ_LogInfo( "spike", "efx shim INSTALLED (real=%p shim=%p); wrapped %d emitters, rest passthrough",
		(void *)s_realEfx, (void *)&s_shimEfx, (int)EFX_COUNT );
}

void RestoreEfxShim()
{
	if( !s_installed )
		return;

	// Only restore if the client is still pointing at us; if something else
	// re-swapped pEfxAPI after us, leave it (we never silently clobber).
	if( gEngfuncs.pEfxAPI == &s_shimEfx )
		gEngfuncs.pEfxAPI = s_realEfx;
	else
		CSZ_LogWarn( "spike", "pEfxAPI moved out from under the shim; leaving it (no clobber)" );

	s_installed = false;
	CSZ_LogInfo( "spike", "efx shim RESTORED" );
}

void DumpEfxCounts()
{
	char buf[640];
	int n = 0;
	bool any = false;

	buf[0] = '\0';

	for( int i = 0; i < EFX_COUNT; i++ )
	{
		if( s_efxCount[i] == 0 )
			continue;

		any = true;

		int w = snprintf( buf + n, sizeof( buf ) - (size_t)n, "%s=%u ", kEfxNames[i], s_efxCount[i] );
		if( w > 0 )
			n += w;

		s_efxCount[i] = 0;

		if( n >= (int)sizeof( buf ) - 1 )
			break;
	}

	if( any )
		CSZ_LogInfo( "spike", "efx ~1s rate: %s", buf );
}

// ---------------------------------------------------------------------------
// S2 runtime sanity: which triangleapi_t members the engine actually fills.
// The source-level 3D-world vs 2D-HUD split (which consumers our dispatch
// owns) lives in the spike report doc; here we only prove the members the
// csz_triapi mirror must implement are present in the live table.
// ---------------------------------------------------------------------------
void TriApiProbe()
{
	static bool s_done;

	if( s_done )
		return;

	s_done = true;

	triangleapi_t *t = gEngfuncs.pTriAPI;

	if( t == NULL )
	{
		CSZ_LogWarn( "spike", "gEngfuncs.pTriAPI is NULL (S2 mirror target missing)" );
		return;
	}

#define NB( x ) ( (x) != NULL ? "y" : "n" )
	CSZ_LogInfo( "spike", "pTriAPI ver=%d members: RenderMode=%s Begin=%s End=%s Color4f=%s Color4ub=%s "
		"TexCoord2f=%s Vertex3fv=%s Vertex3f=%s Brightness=%s CullFace=%s SpriteTexture=%s "
		"WorldToScreen=%s Fog=%s ScreenToWorld=%s GetMatrix=%s BoxInPVS=%s LightAtPoint=%s "
		"Color4fRendermode=%s FogParams=%s",
		t->version, NB( t->RenderMode ), NB( t->Begin ), NB( t->End ), NB( t->Color4f ), NB( t->Color4ub ),
		NB( t->TexCoord2f ), NB( t->Vertex3fv ), NB( t->Vertex3f ), NB( t->Brightness ), NB( t->CullFace ), NB( t->SpriteTexture ),
		NB( t->WorldToScreen ), NB( t->Fog ), NB( t->ScreenToWorld ), NB( t->GetMatrix ), NB( t->BoxInPVS ), NB( t->LightAtPoint ),
		NB( t->Color4fRendermode ), NB( t->FogParams ) );
#undef NB
}

// ---------------------------------------------------------------------------
// S1 interface proof: gRenderAPI.R_DecalSetupVerts non-null, surf->pdecals
// walkable, and a real call returns a sane vertex array.
//
// pdecals is the engine decal_t* chain (EngSurface mirror, csz_engine_bsp.h:159).
// We only follow `pnext` (the chain link), which is the FIRST member of the
// engine decal_t (ABI fact, same offset 0 as the HLSDK decal_s) -- so this tiny
// link view is layout-safe regardless of the rest of the engine struct. For the
// R_DecalSetupVerts `texture` argument we pass the SURFACE's gl texture slot
// (a valid ref texturenum: the engine only reads its width/height to scale decal
// UVs). That keeps the call crash-safe and the returned vertex COUNT + position
// layout correct -- which is exactly what the spike must confirm; only the decal
// UV scale would differ from the real decal texture (logged as a caveat, not a
// concern for the layout question).
// ---------------------------------------------------------------------------
struct SpikeDecalLink	// view of the engine decal_t's first field only (pnext @ offset 0)
{
	const void *pnext;
};

void DecalProbe()
{
	bool have = ( gRenderAPI.R_DecalSetupVerts != NULL );

	static bool s_loggedIface;
	if( !s_loggedIface )
	{
		s_loggedIface = true;
		CSZ_LogInfo( "spike", "render_api: R_DecalSetupVerts=%s DrawSingleDecal=%s R_EntityRemoveDecals=%s",
			have ? "NON-NULL (decal self-draw VIABLE)" : "NULL (decal self-draw BLOCKED -> plan-B CPU clip)",
			gRenderAPI.DrawSingleDecal != NULL ? "non-null" : "null",
			gRenderAPI.R_EntityRemoveDecals != NULL ? "non-null" : "null" );
	}

	model_t *world = WorldModel();
	if( world == NULL )
		return;

	const EngModel *bsp = EngBsp( world );
	if( bsp->numsurfaces <= 0 || bsp->surfaces == NULL )
		return;

	int surfWithDecals = 0;
	int totalDecals = 0;
	const EngSurface *firstSurf = NULL;
	const void *firstDecal = NULL;

	for( int i = 0; i < bsp->numsurfaces; i++ )
	{
		const EngSurface &s = bsp->surfaces[i];

		if( s.pdecals == NULL )
			continue;

		surfWithDecals++;

		for( const void *p = s.pdecals; p != NULL; )
		{
			totalDecals++;

			if( firstDecal == NULL )
			{
				firstDecal = p;
				firstSurf = &s;
			}

			p = reinterpret_cast<const SpikeDecalLink *>( p )->pnext;

			if( totalDecals > 20000 )	// runaway/garbage-chain guard
				break;
		}
	}

	CSZ_LogInfo( "spike", "decal scan: surfaces=%d surfWithDecals=%d totalDecals=%d",
		bsp->numsurfaces, surfWithDecals, totalDecals );

	if( firstDecal == NULL || !have )
		return;

	int texnum = ( firstSurf->texinfo != NULL && firstSurf->texinfo->texture != NULL )
		? firstSurf->texinfo->texture->gl_texturenum : 0;

	int count = -1;
	float *v = gRenderAPI.R_DecalSetupVerts(
		(struct decal_s *)const_cast<void *>( firstDecal ),
		(struct msurface_s *)const_cast<EngSurface *>( firstSurf ),
		texnum, &count );

	if( v == NULL || count <= 0 )
	{
		CSZ_LogWarn( "spike", "R_DecalSetupVerts returned v=%p count=%d (surf texSlot=%d) -- unexpected; revisit before C-DEC",
			(void *)v, count, texnum );
		return;
	}

	// GoldSrc/Xash decal verts are interleaved float[VERTEXSIZE] with VERTEXSIZE=7:
	// pos.xyz, base s/t, lightmap s/t. We log the count + dump the first verts under
	// THAT hypothesis; if the dumped numbers look insane (e.g. lm uv way outside
	// [0,1] or pos not on the surface) the stride assumption is wrong and C-DEC must
	// re-derive it. Reading 7 floats per vert for k<count is in-bounds.
	CSZ_LogInfo( "spike", "R_DecalSetupVerts OK: count=%d (hypothesis stride=7 floats: pos.xyz/base-uv/lm-uv; surf texSlot=%d used for UV scale)",
		count, texnum );

	int dump = ( count < 3 ) ? count : 3;
	for( int k = 0; k < dump; k++ )
	{
		const float *vert = &v[k * 7];
		CSZ_LogInfo( "spike", "  decal v[%d] pos=(%.2f %.2f %.2f) base-uv=(%.4f %.4f) lm-uv=(%.4f %.4f)",
			k, vert[0], vert[1], vert[2], vert[3], vert[4], vert[5], vert[6] );
	}
}

cvar_t *s_cvSpike;

}	// anonymous namespace

void SpikeRegisterCvars()
{
	if( s_cvSpike == NULL )
		s_cvSpike = gEngfuncs.pfnRegisterVariable( "csz_spike", "0", FCVAR_CLIENTDLL );
}

void SpikeFrame()
{
	bool on = ( s_cvSpike != NULL && s_cvSpike->value != 0.0f );

	// Manage the emit shim every frame so toggling csz_spike live is a clean A/B.
	if( on && !s_installed )
		InstallEfxShim();
	else if( !on && s_installed )
		RestoreEfxShim();

	if( !on )
		return;

	// Throttle the probes to ~1/s (the efx counters still accumulate every emit
	// in between, so the dumped numbers are a true per-second rate). ClientTime
	// is the engine wall clock; never per-frame logging (log throttling R8).
	static float s_next;
	float now = ClientTime();

	if( now < s_next )
		return;

	s_next = now + 1.0f;

	TriApiProbe();
	DecalProbe();
	DumpEfxCounts();
}

void SpikeShutdown()
{
	RestoreEfxShim();
}

}	// namespace csz
