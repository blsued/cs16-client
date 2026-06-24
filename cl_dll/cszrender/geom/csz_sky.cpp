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
cvar_t *s_moonlightV2Cvar;	// csz_moonlight_v2 (L2): OPT-IN photometric phase response for the surface moonlight. 0 (default) = legacy LINEAR illuminated fraction (approved look, partial phases unchanged); >0 = exponent on the lit fraction so partial phases dim photometrically (e.g. 3.0 -> half-moon ~0.125 instead of 0.5). Full moon (frac==1) and legacy csz_moon_phase=-1 stay 1.0 at any exponent, so the APPROVED full-moon look is byte-identical regardless of this cvar.
// --- S2 physical night model knobs (REWORK-SPEC §S2). All registered in
// RegisterDevCvars (always, not dev-only); read once per frame in PublishLighting
// to fill the AmbienceParams night-transport slots. Defaults reproduce the approved
// 3fd8b7e look (csz_night_model 0 is the hard one-knob revert for A/B).
cvar_t *s_nightModelCvar;	// csz_night_model: 1 (default) = new physical model, 0 = pre-S2 tiled-multiplier night
cvar_t *s_nightRiseEndCvar;	// csz_night_rise_end: phase where nightness reaches 1 ramping up from sunset (codex P0 curve)
cvar_t *s_nightFallStartCvar;	// csz_night_fall_start: phase where nightness begins falling back toward dawn (codex P0 curve)
cvar_t *s_nightFallEndCvar;	// csz_night_fall_end: phase where nightness reaches 0 (approved dawn restored) (codex P0 curve)
cvar_t *s_nightKWorldCvar;	// csz_night_k_world: skyVis exponent k for the WORLD night ambient (higher = indoors darker faster)
cvar_t *s_nightKStudioCvar;	// csz_night_k_studio: skyVis exponent k for STUDIO (per-entity), calibrated separately (finding 9)
cvar_t *s_nightSkyWorldCvar;	// csz_night_sky_world: WORLD night sky-ambient intensity (scales the cool sky hue)
cvar_t *s_nightSkyStudioCvar;	// csz_night_sky_studio: STUDIO night sky-ambient intensity
cvar_t *s_nightFloorWorldCvar;	// csz_night_floor_world: WORLD competitive ambient floor intensity (prevents pure-black indoors)
cvar_t *s_nightFloorStudioCvar;	// csz_night_floor_studio: STUDIO ambient floor intensity (keeps enemy models readable)
cvar_t *s_nightMoonCvar;	// csz_night_moon: gain on the skyVis-GATED moon directional in physical night (the wallhack-fixed moonlight)
cvar_t *s_sunsetBrightCvar;	// csz_sunset_bright (S4): multiplier on the SUNSET keyframe RGB only (de-crush USER "夕阳整个地图很黑"). Scales luma, preserves warm hue. day/dawn/midnight keyframes untouched. Default 2.2 -> sunset luma ~0.20*2.2 ~= 0.43 ("暖黄昏 not 死黑"); USER tunes final.
// --- S3 analytic fog knobs (REWORK-SPEC §S3, findings 5/6/12). Registered in
// RegisterDevCvars; read once per frame in PublishLighting to fill the AmbienceParams
// S3 fog slots. These are the USER "口味" knobs (spec: all on cvars for real-machine
// tuning). Environmental fog is tied to nightness so the approved DAY look is unchanged
// (envExt = csz_fog_env * nightness -> 0 by day); the server black-fog path degrades the
// fancy features to a faithful blackout.
cvar_t *s_fogEnvCvar;		// csz_fog_env: client environmental extinction a (1/units) at full night; 0 = no client fog
cvar_t *s_fogHeightCvar;	// csz_fog_height: height falloff b (1/units) for client env fog (vertical gradient: thick low, thin high)
cvar_t *s_fogHgCvar;		// csz_fog_hg: Henyey-Greenstein asymmetry g for the directional in-scatter lobe (~0.7 forward)
cvar_t *s_fogStartCvar;		// csz_fog_start: in-scatter start distance (units) -- near field (weapon/skybox) stays crisp
cvar_t *s_fogCutoffCvar;	// csz_fog_cutoff: fog plateau distance (units); 0 = no far clamp
cvar_t *s_fogNoiseCvar;		// csz_fog_noise: 2D noise density modulation amplitude [0..1] (0 = flat slab)
cvar_t *s_fogNoiseScaleCvar;	// csz_fog_noise_scale: noise world-space frequency (1/units)
cvar_t *s_fogWindCvar;		// csz_fog_wind: noise drift speed (units/sec) = slow animation
cvar_t *s_fogExtBlueCvar;	// csz_fog_ext_blue: blue-channel extinction multiplier (>1 -> distance reads cooler; red fixed lower)
cvar_t *s_fogLitCvar;		// csz_fog_lit: toward-body (sun/moon) in-scatter glow brightness (the HG lobe color strength)

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

	// L2 OPT-IN photometric phase response (csz_moonlight_v2). Default "0" = OFF =
	// legacy linear illuminated fraction, so the approved look (full moon AND current
	// partial phases) is unchanged. Set >0 to make partial moons dim photometrically
	// (~lit^value). Registered like csz_moonlight; A/B with "1" vs "0".
	if( s_moonlightV2Cvar == NULL )
		s_moonlightV2Cvar = gEngfuncs.pfnRegisterVariable( "csz_moonlight_v2", "0", FCVAR_CLIENTDLL );

	// S2 physical night model (REWORK-SPEC §S2). Always registered (not dev-only),
	// same pattern as the cvars above. Defaults reproduce the approved 3fd8b7e look;
	// csz_night_model 0 is the hard A/B revert to the pre-S2 tiled-multiplier night.
	if( s_nightModelCvar == NULL )
		s_nightModelCvar = gEngfuncs.pfnRegisterVariable( "csz_night_model", "1", FCVAR_CLIENTDLL );
	if( s_nightRiseEndCvar == NULL )
		s_nightRiseEndCvar = gEngfuncs.pfnRegisterVariable( "csz_night_rise_end", "0.30", FCVAR_CLIENTDLL );
	if( s_nightFallStartCvar == NULL )
		s_nightFallStartCvar = gEngfuncs.pfnRegisterVariable( "csz_night_fall_start", "0.72", FCVAR_CLIENTDLL );
	if( s_nightFallEndCvar == NULL )
		s_nightFallEndCvar = gEngfuncs.pfnRegisterVariable( "csz_night_fall_end", "0.88", FCVAR_CLIENTDLL );
	if( s_nightKWorldCvar == NULL )
		s_nightKWorldCvar = gEngfuncs.pfnRegisterVariable( "csz_night_k_world", "0.7", FCVAR_CLIENTDLL );
	if( s_nightKStudioCvar == NULL )
		s_nightKStudioCvar = gEngfuncs.pfnRegisterVariable( "csz_night_k_studio", "0.7", FCVAR_CLIENTDLL );
	if( s_nightSkyWorldCvar == NULL )
		s_nightSkyWorldCvar = gEngfuncs.pfnRegisterVariable( "csz_night_sky_world", "0.16", FCVAR_CLIENTDLL );
	if( s_nightSkyStudioCvar == NULL )
		s_nightSkyStudioCvar = gEngfuncs.pfnRegisterVariable( "csz_night_sky_studio", "0.20", FCVAR_CLIENTDLL );
	if( s_nightFloorWorldCvar == NULL )
		s_nightFloorWorldCvar = gEngfuncs.pfnRegisterVariable( "csz_night_floor_world", "0.045", FCVAR_CLIENTDLL );	// v4: small readable-floor lift (unlit silhouette)
	if( s_nightFloorStudioCvar == NULL )
		s_nightFloorStudioCvar = gEngfuncs.pfnRegisterVariable( "csz_night_floor_studio", "0.06", FCVAR_CLIENTDLL );	// v4: unlit enemies keep a silhouette
	if( s_nightMoonCvar == NULL )
		s_nightMoonCvar = gEngfuncs.pfnRegisterVariable( "csz_night_moon", "1", FCVAR_CLIENTDLL );
	if( s_sunsetBrightCvar == NULL )
		s_sunsetBrightCvar = gEngfuncs.pfnRegisterVariable( "csz_sunset_bright", "2.2", FCVAR_CLIENTDLL );	// S4 sunset de-crush (USER tunes final)

	// S3 analytic fog knobs (REWORK-SPEC §S3). Defaults = a subtle cool night haze that
	// stays clear up close, cools/eats the far field, and drifts slowly. csz_fog_env 0
	// disables the client environmental fog (server fog still rendered through the new
	// equation). All FCVAR_CLIENTDLL so the USER can tune them live on the real machine.
	if( s_fogEnvCvar == NULL )
		s_fogEnvCvar = gEngfuncs.pfnRegisterVariable( "csz_fog_env", "0.0011", FCVAR_CLIENTDLL );
	if( s_fogHeightCvar == NULL )
		s_fogHeightCvar = gEngfuncs.pfnRegisterVariable( "csz_fog_height", "0.0019", FCVAR_CLIENTDLL );
	if( s_fogHgCvar == NULL )
		s_fogHgCvar = gEngfuncs.pfnRegisterVariable( "csz_fog_hg", "0.72", FCVAR_CLIENTDLL );
	if( s_fogStartCvar == NULL )
		s_fogStartCvar = gEngfuncs.pfnRegisterVariable( "csz_fog_start", "80", FCVAR_CLIENTDLL );
	if( s_fogCutoffCvar == NULL )
		s_fogCutoffCvar = gEngfuncs.pfnRegisterVariable( "csz_fog_cutoff", "0", FCVAR_CLIENTDLL );
	if( s_fogNoiseCvar == NULL )
		s_fogNoiseCvar = gEngfuncs.pfnRegisterVariable( "csz_fog_noise", "0.32", FCVAR_CLIENTDLL );
	if( s_fogNoiseScaleCvar == NULL )
		s_fogNoiseScaleCvar = gEngfuncs.pfnRegisterVariable( "csz_fog_noise_scale", "0.0016", FCVAR_CLIENTDLL );
	if( s_fogWindCvar == NULL )
		s_fogWindCvar = gEngfuncs.pfnRegisterVariable( "csz_fog_wind", "14", FCVAR_CLIENTDLL );
	if( s_fogExtBlueCvar == NULL )
		s_fogExtBlueCvar = gEngfuncs.pfnRegisterVariable( "csz_fog_ext_blue", "1.7", FCVAR_CLIENTDLL );
	if( s_fogLitCvar == NULL )
		s_fogLitCvar = gEngfuncs.pfnRegisterVariable( "csz_fog_lit", "0.55", FCVAR_CLIENTDLL );

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
	// S4 sunset de-crush (REWORK-SPEC §S4; USER complaint "夕阳的时候就整个地图很黑"). Scale the
	// SUNSET keyframe RGB uniformly by csz_sunset_bright -> luma rises (~0.20 -> ~0.43 at default
	// 2.2) while the warm R>G>B hue is preserved (uniform scale = pure brightness lift). ONLY the
	// sunset anchor is touched: the midnight/dawn/day keyframes and the whole phase>=0.5 path are
	// byte-identical, so this is a deliberate USER-requested change, NOT a regression. USER tunes
	// the final value on the real machine.
	{
		float sb = ( s_sunsetBrightCvar != NULL ) ? s_sunsetBrightCvar->value : 2.2f;
		if( sb < 0.5f ) sb = 0.5f;  if( sb > 6.0f ) sb = 6.0f;	// sane band (can dim below default too)
		sunsetR *= sb;  sunsetG *= sb;  sunsetB *= sb;
	}
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
	// L2 OPT-IN photometric phase response: by default (csz_moonlight_v2 0) the
	// surface moonlight uses the LINEAR illuminated fraction above (half-moon -> 0.5),
	// which is geometrically correct but photometrically too bright for partial phases
	// (a real half-moon delivers ~0.1 of full-moon ground illuminance). When the cvar
	// is >0 the fraction is raised to that exponent (~lit^p), so partial moons dim
	// realistically. Full moon (frac==1) and the legacy always-full moon (frac==1)
	// map to 1.0 for ANY exponent, so the APPROVED full-moon look is byte-identical;
	// only NON-default partial phases change, and only when the user opts in.
	{
		float v2 = ( s_moonlightV2Cvar != NULL ) ? s_moonlightV2Cvar->value : 0.0f;
		if( v2 > 0.0f )
		{
			if( v2 > 8.0f )  v2 = 8.0f;	// clamp to a sane band CPU-side
			moonLitFrac = powf( moonLitFrac, v2 );
		}
	}
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

	// L2 cloud-cover dimmer hook (for L3). amb.cloudDim is 1.0 on every L2 path
	// (AmbienceNeutral default, no driver yet) -> *1.0 is IEEE-exact identity, so the
	// approved look is byte-for-byte unchanged. L3 sets it <1.0 to attenuate moonlight
	// under cloud. Applied to moonLit ONLY (the moon directional + the L2 channels
	// derived from it below): the warm sun term, day-for-night tint, golden round-end
	// keyframe and indoor occlusion are all downstream/separate and untouched.
	float cloudDim = amb.cloudDim;
	if( cloudDim < 0.0f )  cloudDim = 0.0f;
	if( cloudDim > 1.0f )  cloudDim = 1.0f;
	moonLit *= cloudDim;

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

	// --- L2 exposed moon channels (sky-base D L2). The surface pass keeps consuming
	// the BLENDED moonlightColor above (no surface render change); these isolate the
	// distinct moon channels so L3 (cloud-dim) / L4 (light-shafts) drive each without
	// the "one scalar -> ground black, air bright" coupling. moonLit here already
	// carries the phase response (csz_moonlight_v2), csz_moonlight gain and cloudDim,
	// so a single knob upstream propagates coherently. NOT read by any current shader
	// -> pure exposure -> the approved look is byte-identical. ---
	amb.moonSurfaceDirect[0] = moonRGB[0] * moonLit;   // moon-only premul surface directional
	amb.moonSurfaceDirect[1] = moonRGB[1] * moonLit;
	amb.moonSurfaceDirect[2] = moonRGB[2] * moonLit;
	// S2 codex P2b: the WARM SUN-ONLY premul surface directional (sunRGB * the raw warm
	// sunLit, NO moonlight/cloud gains -- those are moon knobs). The physical-night shader
	// path lights the surface with this UNGATED (dusk/dawn warm sun, finding 2) plus the
	// skyVis-GATED moonSurfaceDirect, so the moon never leaks through walls at ANY nightness.
	// 0 when the sun is below the horizon. When the moon is down (sunset/day, the nightness=0
	// phases) this EQUALS the blended moonlightColor, so the shader stays byte-identical there.
	amb.sunSurfaceDirect[0] = sunRGB[0] * sunLit;
	amb.sunSurfaceDirect[1] = sunRGB[1] * sunLit;
	amb.sunSurfaceDirect[2] = sunRGB[2] * sunLit;
	amb.moonFogInScatter[0] = moonRGB[0];              // dedicated in-scatter color (unit-ish, NOT premul) for L4
	amb.moonFogInScatter[1] = moonRGB[1];
	amb.moonFogInScatter[2] = moonRGB[2];
	amb.moonFogInScatterIntensity = moonLit;           // strength; 0 unless the moon is meaningfully up

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

	// --- S2 physical night model transport (REWORK-SPEC §S2, findings 1,2,7,9). Fill
	// the AmbienceParams night slots from phase + cvars. The world/studio base passes
	// turn "go dark" from a tiled brightness multiplier into physical incident light
	// gated by the S1 geometric skyVis. Defaults reproduce the approved 3fd8b7e look:
	// nightness=0 (day/dusk) -> the shader's approvedDay branch is pixel-identical, and
	// csz_night_model 0 reverts the whole mechanism. The skyVis-GATED moon directional
	// in the shader reuses amb.moonDir + amb.moonSurfaceDirect (set above), so it carries
	// the full phase/cloud/csz_moonlight response already and is 0 unless the moon is up.
	amb.nightModel = ( s_nightModelCvar != NULL && s_nightModelCvar->value == 0.0f ) ? 0.0f : 1.0f;

	// Explicit phase gate (finding 7, codex P0): nightness is an INDEPENDENT PHASE CURVE,
	// NOT derived from u_ambTint.b-r. The old tint.b-r read mis-classified the cool dawn
	// keyframe (dawn B=0.264 > R=0.172 -> nightness~0.98) as full night, dragging the
	// approved dawn into the physical-night model and crushing the sunrise warm sun. Driving
	// nightness from phase keeps dawn approved: ramp up from sunset(0) to full night by
	// rise_end, hold through the dark hours, then fall back to 0 by fall_end (just before the
	// dawn keyframe at 0.86). sunset(0.0)=0, midnight(0.5)=1, dawn(0.86)~0, day(1.0)=0. NOTE (S4):
	// the active-path day-for-night COOL GRADE was retired to the unified compose post; the tint.b-r
	// (csz_night) signal still lives in the shader's approvedDay branch but now drives ONLY the indoor
	// ambient floor (csz_amb), independent of u_nightness -- so this nightness curve gates solely the
	// physical-night mix.
	float riseEnd   = ( s_nightRiseEndCvar   != NULL ) ? s_nightRiseEndCvar->value   : 0.30f;
	float fallStart = ( s_nightFallStartCvar != NULL ) ? s_nightFallStartCvar->value : 0.72f;
	float fallEnd   = ( s_nightFallEndCvar   != NULL ) ? s_nightFallEndCvar->value   : 0.88f;
	if( riseEnd < 1e-3f )        riseEnd = 1e-3f;                         // guard Smooth01 zero-width
	if( fallEnd <= fallStart )   fallEnd = fallStart + 1e-3f;
	float nightRise = skymath::Smooth01( 0.0f, riseEnd, phase );          // sunset 0 -> full night by rise_end
	float nightFall = 1.0f - skymath::Smooth01( fallStart, fallEnd, phase ); // full night -> 0 by fall_end (dawn restored)
	amb.nightness = ( nightRise < nightFall ) ? nightRise : nightFall;    // min: plateau at 1 through the dark hours

	// Per-domain night ambient (finding 9: world and studio are calibrated SEPARATELY).
	// Cool moonlit-ambient hues; cvar intensities scale them. skyHue drives the open-sky
	// micro-ambient (modulated by pow(skyVis,k)); floorHue is the competitive readable
	// floor that keeps indoors/缝隙 from going pure black (silhouettes still discernible).
	const float skyHue[3]   = { 0.62f, 0.74f, 1.00f };	// cool blue night sky ambient
	const float floorHue[3] = { 0.70f, 0.80f, 1.00f };	// slightly cool readable floor
	float wSky   = ( s_nightSkyWorldCvar    != NULL ) ? s_nightSkyWorldCvar->value    : 0.16f;
	float sSky   = ( s_nightSkyStudioCvar   != NULL ) ? s_nightSkyStudioCvar->value   : 0.20f;
	float wFloor = ( s_nightFloorWorldCvar  != NULL ) ? s_nightFloorWorldCvar->value  : 0.03f;
	float sFloor = ( s_nightFloorStudioCvar != NULL ) ? s_nightFloorStudioCvar->value : 0.05f;
	if( wSky < 0.0f ) wSky = 0.0f;  if( sSky < 0.0f ) sSky = 0.0f;
	if( wFloor < 0.0f ) wFloor = 0.0f;  if( sFloor < 0.0f ) sFloor = 0.0f;
	for( int c = 0; c < 3; c++ )
	{
		amb.nightSky[0][c]   = skyHue[c]   * wSky;	// world
		amb.nightSky[1][c]   = skyHue[c]   * sSky;	// studio
		amb.nightFloor[0][c] = floorHue[c] * wFloor;	// world
		amb.nightFloor[1][c] = floorHue[c] * sFloor;	// studio
	}
	amb.nightK[0] = ( s_nightKWorldCvar  != NULL ) ? s_nightKWorldCvar->value  : 0.7f;
	amb.nightK[1] = ( s_nightKStudioCvar != NULL ) ? s_nightKStudioCvar->value : 0.7f;
	if( amb.nightK[0] < 0.0f ) amb.nightK[0] = 0.0f;
	if( amb.nightK[1] < 0.0f ) amb.nightK[1] = 0.0f;

	amb.nightMoonGain = ( s_nightMoonCvar != NULL ) ? s_nightMoonCvar->value : 1.0f;
	if( amb.nightMoonGain < 0.0f ) amb.nightMoonGain = 0.0f;

	// --- S3 analytic fog (REWORK-SPEC §S3, findings 5/6/12). Replace the flat achromatic
	// in-scatter gray (CszApplyFogAmbient, RETIRED) with a sky/moon-coupled participating
	// medium. ONE owner (finding 5): the shader reads exactly the slots set here. moonRGB/
	// sunRGB and moonLit/sunLit (computed above) drive the directional "lit" end; nightSky
	// (above) is the cool dark back-light base. Environmental fog is gated on nightness so
	// the approved DAY look is byte-identical (envExt -> 0, fogDensity stays 0). The server
	// black-fog path keeps its authoritative color and degrades the fancy features. ---
	float envA   = ( s_fogEnvCvar        != NULL ) ? s_fogEnvCvar->value        : 0.0011f;
	float fogB   = ( s_fogHeightCvar     != NULL ) ? s_fogHeightCvar->value     : 0.0019f;
	float hgG    = ( s_fogHgCvar         != NULL ) ? s_fogHgCvar->value         : 0.72f;
	float fStart = ( s_fogStartCvar      != NULL ) ? s_fogStartCvar->value      : 80.0f;
	float fCut   = ( s_fogCutoffCvar     != NULL ) ? s_fogCutoffCvar->value     : 0.0f;
	float nAmp   = ( s_fogNoiseCvar      != NULL ) ? s_fogNoiseCvar->value      : 0.32f;
	float nScale = ( s_fogNoiseScaleCvar != NULL ) ? s_fogNoiseScaleCvar->value : 0.0016f;
	float fWind  = ( s_fogWindCvar       != NULL ) ? s_fogWindCvar->value       : 14.0f;
	float extB   = ( s_fogExtBlueCvar    != NULL ) ? s_fogExtBlueCvar->value    : 1.7f;
	float litI   = ( s_fogLitCvar        != NULL ) ? s_fogLitCvar->value        : 0.55f;
	if( hgG < 0.0f ) hgG = 0.0f;  if( hgG > 0.95f ) hgG = 0.95f;	// keep HG denominator well-conditioned
	if( fStart < 0.0f ) fStart = 0.0f;
	if( fCut < 0.0f ) fCut = 0.0f;
	if( nAmp < 0.0f ) nAmp = 0.0f;  if( nAmp > 1.0f ) nAmp = 1.0f;
	if( extB < 0.0f ) extB = 0.0f;
	if( litI < 0.0f ) litI = 0.0f;

	float nn = amb.nightness;
	bool serverFog = ( amb.fogDensity > 0.0f );	// server (or dev) supplied authoritative fog
	bool blackFog  = amb.fogBypassTint;		// server black gameplay fog (silhouettes/blackout)

	// Directional "lit" fog color: the body's light color, scaled by how high it is. Day =
	// warm sun; night = cool moon (spec: 夜用月方向、低强度). moonLit/sunLit are 0 unless the
	// body is meaningfully up, so the lobe only brightens toward a body that is actually there.
	float moonUp = skymath::clampf01( moonLit * 3.0f );
	float sunUp  = skymath::clampf01( sunLit  * 3.0f );
	float litDay[3], litNight[3], litCol[3];
	for( int c = 0; c < 3; c++ )
	{
		litDay[c]   = sunRGB[c]  * litI * sunUp;
		litNight[c] = moonRGB[c] * litI * moonUp;
		litCol[c]   = litDay[c] + ( litNight[c] - litDay[c] ) * nn;	// mix(day,night,nightness)
	}

	// Per-channel extinction tint (spec §S3.1: blue scatters most -> distance reads cooler).
	// Green = 1.0 (luma reference); red lower, blue higher. Achromatic for a faithful blackout.
	amb.fogExtTint[0] = blackFog ? 1.0f : 0.62f;
	amb.fogExtTint[1] = 1.0f;
	amb.fogExtTint[2] = blackFog ? 1.0f : extB;
	amb.fogHgG       = blackFog ? 0.0f : hgG;
	amb.fogStart     = blackFog ? 0.0f : fStart;
	amb.fogCutoff    = blackFog ? 0.0f : fCut;
	amb.fogNoiseAmp  = blackFog ? 0.0f : nAmp;
	amb.fogNoiseScale = nScale;
	amb.fogWind      = fWind;
	// fogLit: the toward-body color (HG lobe blends fog toward it). A blackout keeps it equal
	// to its near-black fog color (no directional brightening); else the body-light color above.
	amb.fogLit[0] = blackFog ? amb.fogColor[0] : litCol[0];
	amb.fogLit[1] = blackFog ? amb.fogColor[1] : litCol[1];
	amb.fogLit[2] = blackFog ? amb.fogColor[2] : litCol[2];

	// Base (back-light) fog color + density. When the server supplied fog, keep its color/
	// density (already phase-tinted above) and only enrich it. Otherwise synthesize the
	// client environmental haze: cool dark night sky color (nightSky, the same cool hue the
	// world night ambient uses), warm dim by day -- but density ramps with nightness so the
	// approved day is untouched. Height b gives the vertical gradient (thick low, thin high).
	if( !serverFog )
	{
		float envExt = envA * nn;	// 0 by day -> no client fog -> day byte-identical
		if( envExt > 0.0f )
		{
			float dayBase[3]   = { amb.tint[0] * 0.5f, amb.tint[1] * 0.5f, amb.tint[2] * 0.5f };
			float nightBase[3] = { amb.nightSky[0][0], amb.nightSky[0][1], amb.nightSky[0][2] };
			for( int c = 0; c < 3; c++ )
				amb.fogColor[c] = dayBase[c] + ( nightBase[c] - dayBase[c] ) * nn;	// cool dark at night
			amb.fogDensity = envExt / 0.6931471805599453f;	// shader extinction a = density*ln2 = envExt (FogExtinctionFromDensity inverse)
			amb.heightFalloff = fogB;		// client env height gradient
			// fogLit fell through above with the right night/day color; refresh against the
			// freshly-synthesized base so a moonless deep night still degrades to plain fog.
			if( !( amb.fogLit[0] > 0.0f || amb.fogLit[1] > 0.0f || amb.fogLit[2] > 0.0f ) )
			{
				amb.fogLit[0] = amb.fogColor[0];
				amb.fogLit[1] = amb.fogColor[1];
				amb.fogLit[2] = amb.fogColor[2];
			}
		}
	}
}

}
