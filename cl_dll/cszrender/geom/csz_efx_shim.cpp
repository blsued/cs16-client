/*
 * csz_efx_shim.cpp -- CSOZ renderer: pEfxAPI interception table (M2c decision C, C-SHIM)
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
#include "csz_efx_shim.h"
#include "csz_beam.h"
#include "csz_particle.h"
#include "../core/csz_engine.h"
#include "../core/csz_log.h"

#include "r_efx.h"   // efx_api_t + BEAM (the emit table we wrap)

#include <string.h>

namespace csz
{

namespace
{

efx_api_t *s_realEfx;     // engine's real table (forward target for passthrough)
efx_api_t  s_shimEfx;     // our by-value copy with the redirects patched in
bool       s_installed;
cvar_t    *s_cvarShim;    // csz_efx_shim (default 1)

// one-shot OWED log latches for the un-redirected long tail (no silent drop)
bool s_owedLogged[16];
void LogOwedOnce( int id, const char *name )
{
	if( id < 0 || id >= (int)( sizeof( s_owedLogged ) / sizeof( s_owedLogged[0] ) ) )
		return;
	if( s_owedLogged[id] )
		return;
	s_owedLogged[id] = true;
	CSZ_LogDev( "efxshim", "OWED long-tail efx %s passed through to engine (not drawn under takeover)", name );
}

// ===== REDIRECTS: beam creators -> csz_beam pool =====
BEAM *Shim_R_BeamPoints( float *start, float *end, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	return BeamAllocPoints( start, end, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
}

BEAM *Shim_R_BeamEntPoint( int startEnt, float *end, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	return BeamAllocEntPoint( startEnt, end, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
}

// ===== REDIRECTS: first-batch particle / tracer creators -> csz_particle pool ====
void Shim_R_SparkEffect( float *pos, int count, int velocityMin, int velocityMax )
{
	ParticleEmitSparkEffect( pos, count, velocityMin, velocityMax );
}
void Shim_R_SparkShower( float *pos )
{
	ParticleEmitSparkShower( pos );
}
void Shim_R_SparkStreaks( float *pos, int count, int velocityMin, int velocityMax )
{
	ParticleEmitSparkStreaks( pos, count, velocityMin, velocityMax );
}
void Shim_R_StreakSplash( float *pos, float *dir, int color, int count, float speed, int velocityMin, int velocityMax )
{
	ParticleEmitStreakSplash( pos, dir, color, count, speed, velocityMin, velocityMax );
}
void Shim_R_BulletImpactParticles( float *pos )
{
	ParticleEmitBulletImpact( pos );
}
void Shim_R_RocketTrail( float *start, float *end, int type )
{
	ParticleEmitRocketTrail( start, end, type );
}
void Shim_R_TracerEffect( float *start, float *end )
{
	ParticleEmitTracer( start, end );
}

// ===== PASSTHROUGH + OWED LOG: un-redirected beam family / generic particle =====
BEAM *Shim_R_BeamEnts( int startEnt, int endEnt, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	LogOwedOnce( 0, "R_BeamEnts" );
	return s_realEfx->R_BeamEnts( startEnt, endEnt, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
}
BEAM *Shim_R_BeamFollow( int startEnt, int modelIndex, float life, float width, float r, float g, float b, float brightness )
{
	LogOwedOnce( 1, "R_BeamFollow" );
	return s_realEfx->R_BeamFollow( startEnt, modelIndex, life, width, r, g, b, brightness );
}
BEAM *Shim_R_BeamRing( int startEnt, int endEnt, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	LogOwedOnce( 2, "R_BeamRing" );
	return s_realEfx->R_BeamRing( startEnt, endEnt, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
}
BEAM *Shim_R_BeamCirclePoints( int type, float *start, float *end, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	LogOwedOnce( 3, "R_BeamCirclePoints" );
	return s_realEfx->R_BeamCirclePoints( type, start, end, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
}
BEAM *Shim_R_BeamLightning( float *start, float *end, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed )
{
	LogOwedOnce( 4, "R_BeamLightning" );
	return s_realEfx->R_BeamLightning( start, end, modelIndex, life, width, amplitude, brightness, speed );
}
void Shim_R_RunParticleEffect( float *org, float *dir, int color, int count )
{
	LogOwedOnce( 5, "R_RunParticleEffect" );
	s_realEfx->R_RunParticleEffect( org, dir, color, count );
}
void Shim_R_ParticleExplosion( float *org )
{
	LogOwedOnce( 6, "R_ParticleExplosion" );
	s_realEfx->R_ParticleExplosion( org );
}
void Shim_R_Blood( float *org, float *dir, int pcolor, int speed )
{
	LogOwedOnce( 7, "R_Blood" );
	s_realEfx->R_Blood( org, dir, pcolor, speed );
}
void Shim_R_LavaSplash( float *org )
{
	LogOwedOnce( 8, "R_LavaSplash" );
	s_realEfx->R_LavaSplash( org );
}
void Shim_R_TeleportSplash( float *org )
{
	LogOwedOnce( 9, "R_TeleportSplash" );
	s_realEfx->R_TeleportSplash( org );
}

void InstallShim()
{
	if( s_installed )
		return;

	s_realEfx = gEngfuncs.pEfxAPI;
	if( s_realEfx == NULL )
	{
		CSZ_LogWarn( "efxshim", "gEngfuncs.pEfxAPI is NULL; cannot install efx shim" );
		return;
	}

	s_shimEfx = *s_realEfx;   // transparent passthrough copy of the whole table

	// redirect (self-drawn pools)
	s_shimEfx.R_BeamPoints             = Shim_R_BeamPoints;
	s_shimEfx.R_BeamEntPoint           = Shim_R_BeamEntPoint;
	s_shimEfx.R_SparkEffect            = Shim_R_SparkEffect;
	s_shimEfx.R_SparkShower            = Shim_R_SparkShower;
	s_shimEfx.R_SparkStreaks           = Shim_R_SparkStreaks;
	s_shimEfx.R_StreakSplash           = Shim_R_StreakSplash;
	s_shimEfx.R_BulletImpactParticles  = Shim_R_BulletImpactParticles;
	s_shimEfx.R_RocketTrail            = Shim_R_RocketTrail;
	s_shimEfx.R_TracerEffect           = Shim_R_TracerEffect;

	// passthrough + OWED log (engine does not draw these under takeover)
	s_shimEfx.R_BeamEnts               = Shim_R_BeamEnts;
	s_shimEfx.R_BeamFollow             = Shim_R_BeamFollow;
	s_shimEfx.R_BeamRing               = Shim_R_BeamRing;
	s_shimEfx.R_BeamCirclePoints       = Shim_R_BeamCirclePoints;
	s_shimEfx.R_BeamLightning          = Shim_R_BeamLightning;
	s_shimEfx.R_RunParticleEffect      = Shim_R_RunParticleEffect;
	s_shimEfx.R_ParticleExplosion      = Shim_R_ParticleExplosion;
	s_shimEfx.R_Blood                  = Shim_R_Blood;
	s_shimEfx.R_LavaSplash             = Shim_R_LavaSplash;
	s_shimEfx.R_TeleportSplash         = Shim_R_TeleportSplash;

	// deliberately UNtouched (handled elsewhere, drawn correctly already):
	//   R_DecalShoot     -> engine decal clip path (decision A)
	//   R_MuzzleFlash    -> CL_AllocDlight light registry mirror
	//   R_Sprite_Trail   -> FTENT sprite tempents already drawn via HUD_AddEntity
	//   R_TempSprite / R_TempModel / CL_TempEntAlloc* / R_DecalSetupVerts ... -> as-is

	gEngfuncs.pEfxAPI = &s_shimEfx;
	s_installed = true;
	CSZ_LogInfo( "efxshim", "efx shim INSTALLED (real=%p shim=%p): beam+particle+tracer redirected, rest passthrough",
		(void *)s_realEfx, (void *)&s_shimEfx );
}

void UninstallShim()
{
	if( !s_installed )
		return;

	// only restore if the client is still pointing at us; never clobber a foreign re-swap
	if( gEngfuncs.pEfxAPI == &s_shimEfx )
		gEngfuncs.pEfxAPI = s_realEfx;
	else
		CSZ_LogWarn( "efxshim", "pEfxAPI moved out from under the shim; leaving it (no clobber)" );

	s_installed = false;
	CSZ_LogInfo( "efxshim", "efx shim RESTORED (engine real table)" );
}

}  // anonymous namespace

void EfxShimRegisterCvars()
{
	if( s_cvarShim == NULL )
		s_cvarShim = gEngfuncs.pfnRegisterVariable( "csz_efx_shim", "1", FCVAR_CLIENTDLL );
	CSZ_LogDev( "efxshim", "cvars registered (csz_efx_shim)" );
}

void EfxShimSetActive( bool takeoverActive )
{
	bool want = takeoverActive && ( ReadCvar( s_cvarShim, 1.0f ) >= 0.5f );
	if( want && !s_installed )
		InstallShim();
	else if( !want && s_installed )
		UninstallShim();
}

void EfxShimShutdown()
{
	UninstallShim();
}

}  // namespace csz
