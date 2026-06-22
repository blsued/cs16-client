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
#include "csz_sky_math.h"	// pure phase + celestial-body geometry (single source of truth)
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

// Angular radius (degrees) of the moon. Real moon is ~0.5deg; bumped a touch so
// the body reads at gameplay FOV. Used only for the informational
// amb.moonCosRadius parity field (PublishLighting); the legacy sun radius
// kSunSizeDeg was removed with the retired disc (DEAD-1, infra pass 2).
const float kMoonSizeDeg = 2.9f;

struct SkyGpu
{
	ShaderProgram program;
	unsigned int vao;	// empty VAO: GL3.3 core forbids a vertexless draw with VAO 0
	int gpuGeneration;
	bool built;

	int uCamFwd, uCamRight, uCamUp;
	int uSunDir, uMoonDir, uMoonColor;
	int uPhase, uStarAmount, uFog;
	int uFogParams;		// analytic base fog (fog M1 Step 2): maxOpacity floor for the horizon band
};

SkyGpu s_sky;

cvar_t *s_phaseCvar;	// csz_sky_phase: < 0 = live off ClientTime, [0,1] = frozen
cvar_t *s_bloodMoonCvar;	// csz_bloodmoon: 0 = OFF (default), != 0 = enable the midnight blood-moon tint (future M5 blood-moon server mode)
cvar_t *s_skyDebugCvar;	// csz_sky_debug: 0 = OFF (default); != 0 = csz_renderer dumps per-phase ambience once/sec (disposable observability hook)
cvar_t *s_moonPhaseCvar;	// csz_moon_phase (registered by C3 csz_sunmoon.cpp): looked up here for Option II phase-scaled moonlight; single source of truth shared with the disc shader
cvar_t *s_moonlightCvar;	// csz_moonlight: USER-tunable multiplier on the NIGHT moonlight intensity ONLY (default 1.3 = "one notch" stronger per 2026-06-19 real-machine feedback). Scales moonLit, which is already 0 except mid-night, so day/dusk/round-start identity is preserved.

#if defined( CSZ_DEV_TOOLS )
cvar_t *s_fullscreenCvar;	// csz_sky_fullscreen: dev overlay, draw the sky over the whole frame
// csz_devsun <elev yaw r g b>: manual sun override, mirrors fog's csz_devmoon.
// Stored here and applied by DrawSky/PublishLighting when armed.
bool s_devSunOn;
float s_devSunElev, s_devSunYaw;
float s_devSunColor[3];
#endif

// Sun/moon geometry + the round->phase mapping now live in csz_sky_math.h
// (csz::skymath::), the single source of truth shared with the unit test. Sun &
// moon ride ONE fixed great circle, antipodal (MoonDir = -SunDir), so there is
// no separate MoonElev/MoonYaw any more. clampf01/Smooth01/ElevYawDir/SunDir/
// SunElevDeg are referenced via skymath:: below.

// Legacy-disc blood-moon factor (a tight midnight triangular pulse) was REMOVED
// with the retired disc (DEAD-1, infra pass 2 2026-06-18): the legacy sky FS no
// longer has a u_bloodMoon path. The LIVE physically-based blood-moon pulse is
// owned by C3 (geom/csz_sunmoon.cpp), which replicates the same formula and is
// gated by the csz_bloodmoon cvar this file still registers.

// Illuminated fraction of the moon disc for the current csz_moon_phase (Option II).
// The cvar is the single source of truth shared with the C3 disc shader's
// u_moonLightDir, so the published moonlight INTENSITY tracks the visible phase.
// Phase angle a = (1-p)*PI, fraction k = (1+cos a)/2: full(p=1)->1, quarter(p=0.5)
// ->0.5, new(p=0)->0. Returns 1.0 for the legacy full moon (csz_moon_phase < 0).
float MoonLitFraction()
{
	if( s_moonPhaseCvar == NULL )
		s_moonPhaseCvar = gEngfuncs.pfnGetCvarPointer( "csz_moon_phase" );
	if( s_moonPhaseCvar == NULL || s_moonPhaseCvar->value < 0.0f )
		return 1.0f;	// legacy always-full moon
	float p = skymath::clampf01( s_moonPhaseCvar->value );
	float a = ( 1.0f - p ) * 3.14159265358979323846f;
	return 0.5f * ( 1.0f + cosf( a ) );
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

	// Blood-moon tint is DISABLED by default (csz_bloodmoon 0). The legacy-sky
	// blood-moon path was removed (DEAD-1); this cvar is still registered here as
	// the single public toggle and is now consumed by C3 (geom/csz_sunmoon.cpp)
	// for the physically-based body. A future M5 server mode sets "csz_bloodmoon 1".
	// Always registered (not dev-only), alongside csz_sky_phase.
	if( s_bloodMoonCvar == NULL )
		s_bloodMoonCvar = gEngfuncs.pfnRegisterVariable( "csz_bloodmoon", "0", FCVAR_CLIENTDLL );

	// Disposable runtime-observability hook: default OFF, ships harmlessly. When
	// set != 0, csz_renderer.cpp dumps the published per-phase ambience (tint/fog)
	// once per second so a test agent can read the ground truth. Always registered
	// (not dev-only), same pattern as csz_sky_phase above.
	if( s_skyDebugCvar == NULL )
		s_skyDebugCvar = gEngfuncs.pfnRegisterVariable( "csz_sky_debug", "0", FCVAR_CLIENTDLL );

	// USER-tunable NIGHT moonlight strength (real-machine feedback 2026-06-19 "月辉
	// ... 感觉不到"): default 1.3 strengthens the world moonlight one notch so the
	// night reads as moonlit, WITHOUT touching day/dusk (it scales moonLit only, and
	// moonLit is 0 unless the moon is meaningfully above the horizon -> mid-night).
	// Always registered (not dev-only), same pattern as csz_sky_phase above.
	if( s_moonlightCvar == NULL )
		s_moonlightCvar = gEngfuncs.pfnRegisterVariable( "csz_moonlight", "1.3", FCVAR_CLIENTDLL );

#if defined( CSZ_DEV_TOOLS )
	gEngfuncs.pfnAddCommand( "csz_devsun", DevSunCommand );	// mirror csz_devmoon (csz_fog.cpp)
	if( s_fullscreenCvar == NULL )
		s_fullscreenCvar = gEngfuncs.pfnRegisterVariable( "csz_sky_fullscreen", "0", FCVAR_CLIENTDLL );
	CSZ_LogDev( "sky", "dev sky commands registered (CSZ_DEV_TOOLS build)" );
#endif
}

float SkyRenderer::ComputePhase()
{
	if( s_phaseCvar != NULL && s_phaseCvar->value >= 0.0f )		// dev freeze wins (csz_sky_phase)
		return skymath::clampf01( s_phaseCvar->value );

	float dur, rem;
	if( RoundTiming( dur, rem ) && dur > 1.0f )			// round-driven: start=sunset, last 30s=dawn
		return skymath::RoundPhase( dur - rem, dur );		// RoundPhase clamps elapsed internally

	float t = ClientTime();						// no active round (idle/showcase): free-run, preserves prior behavior
	return skymath::clampf01( t / kCycleSeconds - floorf( t / kCycleSeconds ));
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
	s_sky.uMoonColor  = UniformLoc( s_sky.program, "u_moonColor" );
	s_sky.uPhase      = UniformLoc( s_sky.program, "u_phase" );
	s_sky.uStarAmount = UniformLoc( s_sky.program, "u_starAmount" );
	s_sky.uFog        = UniformLoc( s_sky.program, "u_fog" );
	s_sky.uFogParams  = UniformLoc( s_sky.program, "u_fogParams" );

	s_sky.built = true;
	CSZ_LogDev( "sky", "sky program built (gpu gen %d)", s_sky.gpuGeneration );
}

void SkyRenderer::DrawSky( const ViewSetup &view )
{
	if( !s_sky.built )
		return;

	float phase = ComputePhase();

	// Body directions from phase (or the dev override). The moon is the exact
	// antipode of the sun (computed AFTER the dev override), so the disc and the
	// lit side always agree and the two stay 180 deg opposite.
	float sunDir[3], moonDir[3];

	skymath::SunDir( phase, sunDir );
	skymath::MoonDir( phase, moonDir );

	float moonColor[3] = { 0.90f, 0.93f, 1.00f };	// moonlit-cloud tint (near-white, faint cool edge)

#if defined( CSZ_DEV_TOOLS )
	if( s_devSunOn )
		skymath::ElevYawDir( s_devSunElev, s_devSunYaw, sunDir );	// sunColor override removed with the disc (DEAD-1)
#endif

	// Moon = -sun (antipodal even under the dev sun override).
	moonDir[0] = -sunDir[0];
	moonDir[1] = -sunDir[1];
	moonDir[2] = -sunDir[2];

	// Camera basis (Quake world space, Z up). Pre-scale right/up by the
	// half-FOV tangents so the VS ray = fwd + right*ndc.x + up*ndc.y.
	float fwd[3], right[3], up[3];

	AngleVectors( view.angles, fwd, right, up );

	float tanX = tanf( view.fovX * kDegToRad * 0.5f );
	float tanY = tanf( view.fovY * kDegToRad * 0.5f );
	float rightS[3] = { right[0] * tanX, right[1] * tanX, right[2] * tanX };
	float upS[3]    = { up[0] * tanY,    up[1] * tanY,    up[2] * tanY };

	// Stars ramp in after sunset and persist through the long dark night,
	// fading out only in the final dawn window (matches the new phase mapping).
	float starAmount = skymath::Smooth01( 0.0f, 0.30f, phase ) * ( 1.0f - skymath::Smooth01( 0.86f, 1.0f, phase ));
	// (legacy bloodMoon disc factor removed -- DEAD-1; C3 owns the live blood moon)

	const AmbienceParams &amb = view.ambience;
	// Underwater (waterlevel >= 3): feed the sky zero fog density (water fog is
	// far denser than air; fogging the sky through it would break). PrimeXT left
	// this as dead code -- we make it active (pitfall 19). entity_state_s has no
	// waterlevel on the client, so probe the view point's contents (view.cpp parity).
	int viewContents = ( gEngfuncs.PM_PointContents != NULL )
		? gEngfuncs.PM_PointContents( (float *)view.origin, NULL ) : CONTENTS_EMPTY;
	bool underwater = ( viewContents <= CONTENTS_WATER );	// water/slime/lava (GoldSrc contents <= -3)
	float skyFogDensity = underwater ? 0.0f : amb.fogDensity;
	// Analytic base fog (fog M1 Step 2): the sky horizon band uses the same
	// natural-exp extinction as world/studio so e^(-a*..) matches the old
	// exp2(-density*..) look and there is no seam/double horizon fog.
	const float fogVec[4] = { amb.fogColor[0], amb.fogColor[1], amb.fogColor[2], FogExtinctionFromDensity( skyFogDensity ) };
	const float fogParams[4] = { 0.0f, 0.0f, amb.maxOpacity > 0.0f ? amb.maxOpacity : 1.0f, 0.0f };	// z = maxOpacity floor

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
	glUniform3fv( s_sky.uMoonColor, 1, moonColor );
	glUniform1f( s_sky.uPhase, phase );
	glUniform1f( s_sky.uStarAmount, starAmount );
	glUniform4fv( s_sky.uFog, 1, fogVec );
	glUniform4fv( s_sky.uFogParams, 1, fogParams );

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
	// pass). The map's OVERALL brightness lives HERE: a warm but DIM sunset
	// (round start), darkening to a cold, dim midnight, back to neutral by day.
	// Net brightness DECREASES from sunset to midnight -- as the moon rises the
	// map gets DARKER, not brighter (the moonlight below is only a faint accent).
	// At round-end (phase 1.0) the ground re-warms to a GOLDEN daylight (not neutral
	// white) so the world tint matches the warm golden-hour sky overhead. ---
	// Four keyframes on the absolute-time schedule's phase anchors (sunset 0.0,
	// midnight 0.5, dawn 0.86, day 1.0), Smooth01-interpolated per channel.
	// Brightness RE-GROUNDED in real illuminance: lux -> Stevens perceptual power
	// law (gamma 1/3), dayLux=100,000, playability floor 0.10. sunset and dawn are
	// EQUAL brightness (luma ~0.20, real sun-at-horizon ~759 lux) -- they differ
	// ONLY in hue (warm amber dusk vs cool pinkish-blue pre-dawn); midnight sits at
	// the 0.10 floor; full daylight is reached only in the final ramp.
	float sunsetR = 0.241f, sunsetG = 0.189f, sunsetB = 0.149f;	// sunset @0.00: luma 0.20, warm amber dusk (R>G>B)
	float midR = 0.120f, midG = 0.165f, midB = 0.300f;		// midnight @0.50: lifted readability floor, cold moonlit blue (B>G>R)
	float dawnR = 0.172f, dawnG = 0.202f, dawnB = 0.264f;		// dawn @0.86: luma 0.20 (== sunset), cool pinkish-blue (B>G>R)
	float dayR = 1.00f, dayG = 0.82f, dayB = 0.62f;			// round-end @1.00: warm golden daylight to MATCH the golden-hour sky (R>G>B amber, luma ~0.84, reached only in the last 5s)
	if( phase < 0.5f )		// sunset -> midnight (DIM dusk, darkening to deep night)
	{
		float t = skymath::Smooth01( 0.0f, 0.5f, phase );
		amb.tint[0] = sunsetR + ( midR - sunsetR ) * t;
		amb.tint[1] = sunsetG + ( midG - sunsetG ) * t;
		amb.tint[2] = sunsetB + ( midB - sunsetB ) * t;
	}
	else if( phase < 0.86f )	// midnight -> dim cool pre-dawn (slowly lightening, still dim)
	{
		float t = skymath::Smooth01( 0.5f, 0.86f, phase );
		amb.tint[0] = midR + ( dawnR - midR ) * t;
		amb.tint[1] = midG + ( dawnG - midG ) * t;
		amb.tint[2] = midB + ( dawnB - midB ) * t;
	}
	else				// dim pre-dawn -> full daylight (final ramp, the last 5s)
	{
		float t = skymath::Smooth01( 0.86f, 1.0f, phase );
		amb.tint[0] = dawnR + ( dayR - dawnR ) * t;
		amb.tint[1] = dawnG + ( dayG - dawnG ) * t;
		amb.tint[2] = dawnB + ( dayB - dawnB ) * t;
	}

	// Phase-consistent fog: the night tint also tints the (server-supplied) fog so a
	// stale warm sunset fog cools with the sky instead of locking the world warm all
	// night. Phase tint is the BASE; server fog MODULATES. No-op when fog is off
	// (fogColor/density 0). Fixes "moon up but map still sunset-warm-yellow".
	// Black gameplay fog BYPASSES this multiply (fog M1 spec 3.6): a blackout must
	// stay the server's exact (near-black) color, not get re-tinted by the sky
	// phase. Environmental fog still participates (gate false by default).
	if( !amb.fogBypassTint )
	{
		amb.fogColor[0] *= amb.tint[0];
		amb.fogColor[1] *= amb.tint[1];
		amb.fogColor[2] *= amb.tint[2];
	}

	// --- Dominant celestial directional light (CROSS-FILE contract: the world +
	// studio base passes add base*moonlightColor*max(N.L,0)). The setting/rising
	// SUN is warm and moderate; the MOON is cool and FAINT -- moonlight gives
	// directional SHAPE to a dark night, it must never brighten the scene past the
	// dimmed ambient (that was the round-start "moon makes the map brighter" bug). ---
	float sElev = skymath::SunElevDeg( phase );
	float sunDir[3], moonDir[3];

	skymath::SunDir( phase, sunDir );

#if defined( CSZ_DEV_TOOLS )
	if( s_devSunOn )
	{
		skymath::ElevYawDir( s_devSunElev, s_devSunYaw, sunDir );
		sElev = s_devSunElev;
	}
#endif

	// Moon = exact antipode of the sun (180 deg opposite, mirrored elevation), so
	// when the sun is up the moon is down and vice-versa -- one body lights the scene.
	moonDir[0] = -sunDir[0];
	moonDir[1] = -sunDir[1];
	moonDir[2] = -sunDir[2];
	float mElev = -sElev;

	// Each light is gated by how far its body is ABOVE the horizon: the sun lights
	// the scene only at sunset/dawn, the moon only through the night. The moon cap
	// (0.10) is deliberately tiny -- a faint cool fill, not a second sun; night must
	// stay darker than both sunset and dawn.
	float sunLit = skymath::clampf01( ( sElev + 4.0f ) / 14.0f ) * 0.42f;	// warm sun (sunset/dawn)
	float moonLit = skymath::clampf01( mElev / 28.0f ) * 0.90f;		// cool moonlight: enough to shape the scene (surfaces facing the moon clearly brighter)

	// Option II (user-locked): scale the published moonlight INTENSITY by the moon's
	// lit fraction, with a playability FLOOR so a new-moon night stays minimally
	// playable (full moon -> x1.0, new moon -> x0.25). Default csz_moon_phase -1 ->
	// MoonLitFraction()==1 -> IDENTICAL to the pre-fix full-moon behaviour. The C3
	// disc shows the matching phase from the same cvar; Chunk C layers indoor
	// occlusion on top of this published moonlight.
	float moonLitFrac = MoonLitFraction();
	if( moonLitFrac < 0.25f )
		moonLitFrac = 0.25f;
	moonLit *= moonLitFrac;

	// USER real-machine feedback 2026-06-19 "月辉不是很明显": strengthen the WORLD
	// moonlight one notch via csz_moonlight (default 1.3), tunable. Applied to moonLit
	// ONLY, so it cannot touch the warm sun term (sunLit), the day-for-night ambient
	// tint, the golden round-end keyframe (phase>=0.86), or the Chunk-C indoor sky-
	// access occlusion (all downstream/separate). moonLit is already 0 unless the moon
	// is above the horizon, so day/dusk/round-start brightness is provably unchanged.
	// Clamped CPU-side to a sane band so a stray value can't blow out the night.
	float moonlightMul = ( s_moonlightCvar != NULL ) ? s_moonlightCvar->value : 1.3f;
	if( moonlightMul < 0.0f )  moonlightMul = 0.0f;
	if( moonlightMul > 4.0f )  moonlightMul = 4.0f;
	moonLit *= moonlightMul;

	const float moonRGB[3] = { 0.54f, 0.64f, 0.95f };	// cool (slightly brightened; blue also lives in the tint)
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
	amb.moonHalo = 0.9f;
}

}
