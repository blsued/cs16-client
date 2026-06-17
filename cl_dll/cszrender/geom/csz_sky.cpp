/*
 * csz_sky.cpp -- CSOZ renderer: procedural day/night sky + celestial bodies
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
 * csoz docs/notes/primext-render-mechanisms-m2.md); implemented by an agent
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
#include "csz_sky.h"
#include "../core/csz_engine.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_log.h"
#include "../core/csz_math.h"
#include "../core/csz_shader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace csz
{

SkyRenderer g_sky;	// zero-initialized (static storage duration)

#include "csz_sky_shaders.inl"

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// One full sunset->midnight->dawn cycle, in seconds. Free-running off ClientTime
// (round-synced start is M5 gameplay wiring, deferred); ~300s lets a single round
// show a fuller arc. The dev cvar csz_sky_phase freezes it for checks.
const float kCycleSeconds = 300.0f;

// Angular radii (degrees) of the two discs. Real sun/moon are ~0.5deg; bumped
// a touch so the body reads at gameplay FOV without a telescope.
const float kSunSizeDeg = 3.0f;
const float kMoonSizeDeg = 3.5f;

struct SkyGpu
{
	ShaderProgram program;
	unsigned int vao;	// empty VAO: GL3.3 core forbids a vertexless draw with VAO 0
	int gpuGeneration;
	bool built;

	int uCamFwd, uCamRight, uCamUp;
	int uSunDir, uMoonDir, uSunColor, uMoonColor;
	int uPhase, uStarAmount, uBloodMoon;
	int uSunCosR, uMoonCosR, uMoonHalo, uFog;
};

SkyGpu s_sky;

cvar_t *s_phaseCvar;	// csz_sky_phase: < 0 = live off ClientTime, [0,1] = frozen

#if defined( CSZ_DEV_TOOLS )
cvar_t *s_fullscreenCvar;	// csz_sky_fullscreen: dev overlay, draw the sky over the whole frame
// csz_devsun <elev yaw r g b>: manual sun override, mirrors fog's csz_devmoon.
// Stored here and applied by DrawSky/PublishLighting when armed.
bool s_devSunOn;
float s_devSunElev, s_devSunYaw;
float s_devSunColor[3];
#endif

float clampf01( float v )
{
	return ( v < 0.0f ) ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

float Smooth01( float edge0, float edge1, float x )
{
	float t = clampf01( ( x - edge0 ) / ( edge1 - edge0 ) );
	return t * t * ( 3.0f - 2.0f * t );
}

// Elevation+yaw (degrees) -> unit world direction, FROM the scene TOWARD the
// body. Same convention as fog/csz_fog.cpp ElevYawToDir (Z up): the sky FS and
// PublishLighting must agree with the dominant-light channel, so both call this.
void ElevYawDir( float elevDeg, float yawDeg, float out[3] )
{
	float e = elevDeg * kDegToRad;
	float y = yawDeg * kDegToRad;

	out[0] = cosf( e ) * cosf( y );
	out[1] = cosf( e ) * sinf( y );
	out[2] = sinf( e );
}

// --- Phase -> body arc (shared by DrawSky and PublishLighting) -------------
// Round day-cycle: 0.00 = SUNSET (sun low and setting; round start), the moon
// rises as the sun goes down, 0.50 = MIDNIGHT (moon high, darkest), 0.80..1.00
// = DAWN (sun rises again; round ending). Azimuths drift so the disc moves.

float MoonElev( float ph )
{
	// Below the horizon at sunset, peaks (~74 deg) near midnight, sets by dawn.
	float t = sinf( 3.14159265f * clampf01( ( ph - 0.04f ) / 0.84f ) );
	return -10.0f + 84.0f * t;
}

float MoonYaw( float ph )
{
	return 210.0f + 70.0f * ph;	// slow drift across the night sky
}

float SunElev( float ph )
{
	// Sunset at round start (+13 -> below over the first 18%), down through the
	// night, then rising again at dawn (round ending).
	if( ph < 0.18f )
		return 13.0f - 30.0f * ( ph / 0.18f );			// +13 -> -17 (sun setting)
	if( ph < 0.80f )
		return -17.0f;						// below the horizon (night)
	return -17.0f + 62.0f * ( ( ph - 0.80f ) / 0.20f );		// -17 -> +45 (dawn)
}

float SunYaw( float ph )
{
	return ( ph < 0.5f ) ? 285.0f : 95.0f;	// sets in the west, dawns in the east
}

// Blood-moon factor: a triangular pulse centered TIGHTLY on midnight (0.5 +/-
// 0.08). Outside that the moon stays its normal cool white -- blood moon is a
// midnight-only event, it must not tint the ordinary night moon pink/red.
float BloodMoonFactor( float ph )
{
	float d = fabsf( ph - 0.5f );
	return clampf01( 1.0f - d * ( 1.0f / 0.08f ) );
}

#if defined( CSZ_DEV_TOOLS )
// csz_devsun <elev yaw r g b> | off: manual sun override (mirrors fog's
// DevMoonCommand). Applied by DrawSky/PublishLighting while armed.
void DevSunCommand()
{
	if( gEngfuncs.Cmd_Argc() >= 2 && strcmp( gEngfuncs.Cmd_Argv( 1 ), "off" ) == 0 )
	{
		s_devSunOn = false;
		CSZ_LogInfo( "sky", "csz_devsun off" );
		return;
	}

	if( gEngfuncs.Cmd_Argc() < 6 )
	{
		CSZ_LogInfo( "sky", "usage: csz_devsun <elev yaw r g b> | off  (degrees, bytes 0-255)" );
		return;
	}

	s_devSunOn = true;
	s_devSunElev = (float)atof( gEngfuncs.Cmd_Argv( 1 ));
	s_devSunYaw = (float)atof( gEngfuncs.Cmd_Argv( 2 ));
	s_devSunColor[0] = (float)( atoi( gEngfuncs.Cmd_Argv( 3 )) & 0xFF ) * ( 1.0f / 255.0f );
	s_devSunColor[1] = (float)( atoi( gEngfuncs.Cmd_Argv( 4 )) & 0xFF ) * ( 1.0f / 255.0f );
	s_devSunColor[2] = (float)( atoi( gEngfuncs.Cmd_Argv( 5 )) & 0xFF ) * ( 1.0f / 255.0f );
	CSZ_LogInfo( "sky", "csz_devsun on (elev=%.1f yaw=%.1f)", s_devSunElev, s_devSunYaw );
}
#endif

}

void SkyRenderer::RegisterDevCvars()
{
	// csz_sky_phase always registered (it is the phase override, not a dev-only
	// probe): default "-1" = live off ClientTime, [0,1] = freeze for G1/G2.
	if( s_phaseCvar == NULL )
		s_phaseCvar = gEngfuncs.pfnRegisterVariable( "csz_sky_phase", "-1", FCVAR_CLIENTDLL );

#if defined( CSZ_DEV_TOOLS )
	gEngfuncs.pfnAddCommand( "csz_devsun", DevSunCommand );	// mirror csz_devmoon (csz_fog.cpp)
	if( s_fullscreenCvar == NULL )
		s_fullscreenCvar = gEngfuncs.pfnRegisterVariable( "csz_sky_fullscreen", "0", FCVAR_CLIENTDLL );
	CSZ_LogDev( "sky", "dev sky commands registered (CSZ_DEV_TOOLS build)" );
#endif
}

float SkyRenderer::ComputePhase()
{
	// Dev override wins when in range; otherwise wrap ClientTime over the cycle.
	if( s_phaseCvar != NULL && s_phaseCvar->value >= 0.0f )
		return clampf01( s_phaseCvar->value );

	float t = ClientTime();
	float ph = t / kCycleSeconds - floorf( t / kCycleSeconds );
	return clampf01( ph );
}

void SkyRenderer::EnsureBuilt()
{
	if( s_sky.built && s_sky.gpuGeneration == GpuGeneration() )
		return;

	// GPU generation bumped (context loss / vid restart): forget the stale
	// names, never glDelete them (the context that owned them is gone).
	s_sky.gpuGeneration = GpuGeneration();

	// Empty VAO so a vertexless gl_VertexID draw is legal in GL3.3 core.
	glGenVertexArrays( 1, &s_sky.vao );

	// Init-time program: a compile failure is FATAL (matches world/studio).
	BuildProgram( "csz_sky", kSkyVs, kSkyFs, true, s_sky.program );

	s_sky.uCamFwd     = UniformLoc( s_sky.program, "u_camFwd" );
	s_sky.uCamRight   = UniformLoc( s_sky.program, "u_camRight" );
	s_sky.uCamUp      = UniformLoc( s_sky.program, "u_camUp" );
	s_sky.uSunDir     = UniformLoc( s_sky.program, "u_sunDir" );
	s_sky.uMoonDir    = UniformLoc( s_sky.program, "u_moonDir" );
	s_sky.uSunColor   = UniformLoc( s_sky.program, "u_sunColor" );
	s_sky.uMoonColor  = UniformLoc( s_sky.program, "u_moonColor" );
	s_sky.uPhase      = UniformLoc( s_sky.program, "u_phase" );
	s_sky.uStarAmount = UniformLoc( s_sky.program, "u_starAmount" );
	s_sky.uBloodMoon  = UniformLoc( s_sky.program, "u_bloodMoon" );
	s_sky.uSunCosR    = UniformLoc( s_sky.program, "u_sunCosR" );
	s_sky.uMoonCosR   = UniformLoc( s_sky.program, "u_moonCosR" );
	s_sky.uMoonHalo   = UniformLoc( s_sky.program, "u_moonHalo" );
	s_sky.uFog        = UniformLoc( s_sky.program, "u_fog" );

	s_sky.built = true;
	CSZ_LogDev( "sky", "sky program built (gpu gen %d)", s_sky.gpuGeneration );
}

void SkyRenderer::DrawSky( const ViewSetup &view )
{
	if( !s_sky.built )
		return;

	float phase = ComputePhase();

	// Body directions from phase (or the dev override). Kept in sync with the
	// dominant-light publish so the disc and the lit side agree.
	float sunDir[3], moonDir[3];

	ElevYawDir( SunElev( phase ), SunYaw( phase ), sunDir );
	ElevYawDir( MoonElev( phase ), MoonYaw( phase ), moonDir );

	float sunColor[3] = { 1.0f, 0.86f, 0.62f };	// warm sun
	float moonColor[3] = { 0.78f, 0.82f, 0.92f };	// cool-neutral moon (blue lives in tint)

#if defined( CSZ_DEV_TOOLS )
	if( s_devSunOn )
	{
		ElevYawDir( s_devSunElev, s_devSunYaw, sunDir );
		sunColor[0] = s_devSunColor[0];
		sunColor[1] = s_devSunColor[1];
		sunColor[2] = s_devSunColor[2];
	}
#endif

	// Camera basis (Quake world space, Z up). Pre-scale right/up by the
	// half-FOV tangents so the VS ray = fwd + right*ndc.x + up*ndc.y.
	float fwd[3], right[3], up[3];

	AngleVectors( view.angles, fwd, right, up );

	float tanX = tanf( view.fovX * kDegToRad * 0.5f );
	float tanY = tanf( view.fovY * kDegToRad * 0.5f );
	float rightS[3] = { right[0] * tanX, right[1] * tanX, right[2] * tanX };
	float upS[3]    = { up[0] * tanY,    up[1] * tanY,    up[2] * tanY };

	float starAmount = Smooth01( 0.0f, 0.30f, phase ) * ( 1.0f - Smooth01( 0.62f, 0.85f, phase ));
	float bloodMoon = BloodMoonFactor( phase );

	const AmbienceParams &amb = view.ambience;
	// Underwater (waterlevel >= 3): feed the sky zero fog density (water fog is
	// far denser than air; fogging the sky through it would break). PrimeXT left
	// this as dead code -- we make it active (pitfall 19). entity_state_s has no
	// waterlevel on the client, so probe the view point's contents (view.cpp parity).
	int viewContents = ( gEngfuncs.PM_PointContents != NULL )
		? gEngfuncs.PM_PointContents( (float *)view.origin, NULL ) : CONTENTS_EMPTY;
	bool underwater = ( viewContents <= CONTENTS_WATER );	// water/slime/lava (GoldSrc contents <= -3)
	float skyFogDensity = underwater ? 0.0f : amb.fogDensity;
	const float fogVec[4] = { amb.fogColor[0], amb.fogColor[1], amb.fogColor[2], skyFogDensity };

	// Depth OFF + write OFF + no blend: draw a full-framebuffer background that
	// the world overwrites by depth (plan option A); restore EnterTakeover
	// baseline after (the world pass expects depth test+write ON).
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendNone );
	SetCull( false );

	UseProgram( s_sky.program.program );
	BindVao( s_sky.vao );

	glUniform3fv( s_sky.uCamFwd, 1, fwd );
	glUniform3fv( s_sky.uCamRight, 1, rightS );
	glUniform3fv( s_sky.uCamUp, 1, upS );
	glUniform3fv( s_sky.uSunDir, 1, sunDir );
	glUniform3fv( s_sky.uMoonDir, 1, moonDir );
	glUniform3fv( s_sky.uSunColor, 1, sunColor );
	glUniform3fv( s_sky.uMoonColor, 1, moonColor );
	glUniform1f( s_sky.uPhase, phase );
	glUniform1f( s_sky.uStarAmount, starAmount );
	glUniform1f( s_sky.uBloodMoon, bloodMoon );
	glUniform1f( s_sky.uSunCosR, cosf( kSunSizeDeg * 0.5f * kDegToRad ));
	glUniform1f( s_sky.uMoonCosR, cosf( kMoonSizeDeg * 0.5f * kDegToRad ));
	glUniform1f( s_sky.uMoonHalo, 0.5f );
	glUniform4fv( s_sky.uFog, 1, fogVec );

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	BindVao( 0 );

	// Restore the takeover baseline for the world pass (depth test+write ON).
	SetDepthTest( true );
	SetDepthWrite( true );
}

void SkyRenderer::DrawDebugFullscreen( const ViewSetup &view )
{
#if defined( CSZ_DEV_TOOLS )
	// Dev proof/showcase: redraw the sky on top of the finished frame so it is
	// visible from any camera angle (normal play shows the sky only through the
	// map's sky surfaces). DrawSky runs with depth test/write OFF, so it paints
	// over everything already drawn this frame.
	if( s_fullscreenCvar != NULL && s_fullscreenCvar->value != 0.0f )
		DrawSky( view );
#else
	(void)view;
#endif
}

void SkyRenderer::PublishLighting( AmbienceParams &amb, float phase )
{
	// --- Ambient tint (multiplies the baked lightmap in the world/studio base
	// pass). The map's OVERALL brightness lives HERE: warm + bright at sunset
	// (round start), darkening to a cold, dim midnight, back to neutral by day.
	// Net brightness DECREASES from sunset to midnight -- as the moon rises the
	// map gets DARKER, not brighter (the moonlight below is only a faint accent). ---
	float sunsetR = 0.95f, sunsetG = 0.76f, sunsetB = 0.55f;	// sunset: warm, 2nd-brightest (below dawn)
	float midR = 0.07f, midG = 0.10f, midB = 0.19f;			// midnight: cold, very dark
	float dayR = 1.00f, dayG = 1.00f, dayB = 1.00f;			// daylight: neutral
	if( phase < 0.18f )		// sunset -> cool blue night (fast handoff; the dusk-lingers fix)
	{
		float t = Smooth01( 0.0f, 0.18f, phase );
		amb.tint[0] = sunsetR + ( 0.17f - sunsetR ) * t;
		amb.tint[1] = sunsetG + ( 0.23f - sunsetG ) * t;
		amb.tint[2] = sunsetB + ( 0.37f - sunsetB ) * t;
	}
	else if( phase < 0.5f )		// cool night -> midnight (stays blue, keeps darkening)
	{
		float t = Smooth01( 0.18f, 0.5f, phase );
		amb.tint[0] = 0.17f + ( midR - 0.17f ) * t;
		amb.tint[1] = 0.23f + ( midG - 0.23f ) * t;
		amb.tint[2] = 0.37f + ( midB - 0.37f ) * t;
	}
	else
	{
		float t = Smooth01( 0.5f, 1.0f, phase );		// midnight -> daylight
		amb.tint[0] = midR + ( dayR - midR ) * t;
		amb.tint[1] = midG + ( dayG - midG ) * t;
		amb.tint[2] = midB + ( dayB - midB ) * t;
	}

	// --- Dominant celestial directional light (CROSS-FILE contract: the world +
	// studio base passes add base*moonlightColor*max(N.L,0)). The setting/rising
	// SUN is warm and moderate; the MOON is cool and FAINT -- moonlight gives
	// directional SHAPE to a dark night, it must never brighten the scene past the
	// dimmed ambient (that was the round-start "moon makes the map brighter" bug). ---
	float sElev = SunElev( phase );
	float mElev = MoonElev( phase );
	float sunDir[3], moonDir[3];

	ElevYawDir( sElev, SunYaw( phase ), sunDir );
	ElevYawDir( mElev, MoonYaw( phase ), moonDir );

#if defined( CSZ_DEV_TOOLS )
	if( s_devSunOn )
		ElevYawDir( s_devSunElev, s_devSunYaw, sunDir );
#endif

	// Each light is gated by how far its body is ABOVE the horizon: the sun lights
	// the scene only at sunset/dawn, the moon only through the night. The moon cap
	// (0.10) is deliberately tiny -- a faint cool fill, not a second sun; night must
	// stay darker than both sunset and dawn.
	float sunLit = clampf01( ( sElev + 4.0f ) / 14.0f ) * 0.42f;	// warm sun (sunset/dawn)
	float moonLit = clampf01( mElev / 28.0f ) * 0.10f;		// faint cool moonlight

	const float moonRGB[3] = { 0.55f, 0.63f, 0.82f };	// cool (blue also lives in the tint)
	const float sunRGB[3] = { 1.00f, 0.78f, 0.50f };	// warm

	// Blend the two by intensity into the single published directional light.
	float wSum = sunLit + moonLit + 1e-5f;
	float lmDir[3];
	float lmColor[3];

	lmDir[0] = ( sunDir[0] * sunLit + moonDir[0] * moonLit ) / wSum;
	lmDir[1] = ( sunDir[1] * sunLit + moonDir[1] * moonLit ) / wSum;
	lmDir[2] = ( sunDir[2] * sunLit + moonDir[2] * moonLit ) / wSum;

	float len = sqrtf( lmDir[0] * lmDir[0] + lmDir[1] * lmDir[1] + lmDir[2] * lmDir[2] );
	if( len > 1e-5f )
	{
		float inv = 1.0f / len;
		lmDir[0] *= inv; lmDir[1] *= inv; lmDir[2] *= inv;
	}

	// Color premultiplied by intensity (world FS adds base*lmColor*N.L, no separate
	// strength term -- matches the AmbienceParams premul convention).
	for( int i = 0; i < 3; i++ )
		lmColor[i] = sunRGB[i] * sunLit + moonRGB[i] * moonLit;

	amb.moonlightEnabled = ( sunLit + moonLit ) > 0.001f;
	amb.moonlightDir[0] = lmDir[0];
	amb.moonlightDir[1] = lmDir[1];
	amb.moonlightDir[2] = lmDir[2];
	amb.moonlightColor[0] = lmColor[0];
	amb.moonlightColor[1] = lmColor[1];
	amb.moonlightColor[2] = lmColor[2];

	// --- Informational moon-object fields (the sky FS computes its own body
	// positions from phase, so these are parity only). Moon up = above horizon. ---
	amb.moonEnabled = mElev > -2.0f;
	amb.moonDir[0] = moonDir[0];
	amb.moonDir[1] = moonDir[1];
	amb.moonDir[2] = moonDir[2];
	amb.moonCosRadius = cosf( kMoonSizeDeg * 0.5f * kDegToRad );
	amb.moonColor[0] = moonRGB[0];
	amb.moonColor[1] = moonRGB[1];
	amb.moonColor[2] = moonRGB[2];
	amb.moonHalo = 0.5f;
}

}
