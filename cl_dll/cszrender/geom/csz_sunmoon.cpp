/*
 * csz_sunmoon.cpp -- CSOZ renderer: physically-based sun/moon bodies (C3)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). CLEAN-ROOM implementation: the
 * lunar disc/phase/limb and halo/aureole rendering is written from first-
 * principles geometry and published, non-copyrightable atmospheric math. No
 * code/shader source is copied or translated from PrimeXT, Paranoia, Trinity,
 * Unreal's EULA tree, sebh/UnrealEngineSkyAtmosphere, Bruneton's repo, retail/
 * leaked sources, or any other license-tainted source (see csoz
 * docs/provenance.md). The moon SURFACE imagery is a SEPARATE public-domain NASA
 * asset loaded at runtime (NOT compiled into this GPL source); see the CREDITS /
 * ATTRIBUTIONS manifest for the required NASA credit + no-endorsement line.
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
// C3. Implements the ABI seam SunMoonContribute(view): draws the moon (NASA
// surface texture + sun-driven phase + cold/blood-moon tint; the moon's outer glow is
// now MoonSkyLum in csz_stars.cpp, Task D -- no standalone halo here) and the sun
// (warm disc + Mie forward-scatter aureole) into the bound HDR scene target at slot
// 10.5, ADDITIVELY, depth off. Atmospheric extinction near the
// horizon comes from the C2 transmittance LUT (SkyComposeAtmosResources). All
// texture binds go through SkyComposeBindTex / SkyComposeRestoreTmus (never raw
// glActiveTexture), no glEnable(GL_TEXTURE_*) on a sky unit, every GL name keyed
// on GpuGeneration() (forget -- never glDelete -- on a foreign context).
#include "csz_sky_compose.h"
#include "csz_sunmoon.h"        // CszGodraySrc / CszGodraySource (FOG Step 4 god-ray source)
#include "csz_sky.h"            // g_sky.ComputePhase() (honors csz_sky_phase)
#include "csz_sky_math.h"       // skymath::SunDir / MoonDir (frozen day-arc)
#include "csz_sunmoon_math.h"   // pure CPU body geometry
#include "../core/csz_engine.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_log.h"
#include "../core/csz_math.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"

#include <math.h>
#include <string.h>

namespace csz
{

#include "csz_sunmoon_shaders.inl"

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// Ground radius (km) the C2 transmittance LUT was built for. MUST match
// csz_atmos.cpp (kRg 6360 + kViewHeightKm 0.5) and the shader Rg constant.
const float kViewR = 6360.5f;

// --- cvars (lazy-registered on first use; read live) --------------------------
// csz_moon / csz_sun are VISUAL-ONLY body toggles: they enable/disable drawing the
// moon/sun BODY here (disc + halo/aureole). They do NOT gate the published GROUND
// light -- csz_sky.cpp PublishLighting still emits the celestial ground light when
// these are 0 (the scene stays lit by the body that is up, just with no disc drawn).
// Use csz_sky_phase / the lighting cvars to change the ground light.
cvar_t *s_cvMoon;        // csz_moon        enable moon BODY rendering (visual-only)
cvar_t *s_cvSun;         // csz_sun         enable sun  BODY rendering (visual-only)
cvar_t *s_cvMoonGain;    // csz_moon_gain   disc radiance scale
cvar_t *s_cvMoonSize;    // csz_moon_size   angular DIAMETER (deg)
cvar_t *s_cvSunGain;     // csz_sun_gain
cvar_t *s_cvSunSize;     // csz_sun_size    angular DIAMETER (deg)
cvar_t *s_cvSunAureole;  // csz_sun_aureole aureole gain
cvar_t *s_cvAureoleK;    // csz_sun_aureole_k power-law exponent
cvar_t *s_cvMoonPhase;   // csz_moon_phase  -1 = legacy full moon, [0,1] = 1 full / 0.5 quarter / 0 new
cvar_t *s_cvMoonPhaseExp;// csz_moon_phase_exp  Task D moonlight-wash phase non-linearity p (I_moon=lit^p)
cvar_t *s_cvMoonGodray;  // csz_moon_godray  moon god-ray SOURCE scale (the moon is a far weaker shaft source than the sun)
cvar_t *s_cvDebug;       // csz_sunmoon_debug  0 off / 1 face moon (full) / 2 face sun
cvar_t *s_cvBloodMoon;   // csz_bloodmoon (registered by csz_sky); looked up
bool    s_cvarsReady;
bool    s_bloodLookedUp;

void EnsureCvars()
{
	if( s_cvarsReady )
		return;
	s_cvarsReady = true;
	s_cvMoon        = gEngfuncs.pfnRegisterVariable( "csz_moon", "1", FCVAR_CLIENTDLL );
	s_cvSun         = gEngfuncs.pfnRegisterVariable( "csz_sun", "1", FCVAR_CLIENTDLL );
	// USER target 2026-06-21 "月亮更大更亮" (USER-TARGET moon+milkyway ref): the disc
	// read DIM (bare peak ~60/255, godray-on ~80/255) at gain 0.9. Bumped 0.9 -> 1.8 to
	// brighten the disc toward the reference's prominent bright (but not blown-out) moon.
	// NOTE this also scales the moon god-ray SOURCE (moonColor*moonGain*csz_moon_godray).
	// Tuned by GL4.6 capture A/B against the USER-TARGET ref: gain 2.2/1.7 blew the disc
	// CORE to pure white (2620 / 495 clipped px, maria lost); 1.4 keeps the disc bright
	// but the whole maria-detailed core stays UNDER clip (measured disc p99 ~208/255, only
	// ~2 clipped px). Landed 0.9 -> 1.4. csz_moon_godray default LEFT at 0.05 (untouched).
	// cvar-tunable [0,100].
	s_cvMoonGain    = gEngfuncs.pfnRegisterVariable( "csz_moon_gain", "1.4", FCVAR_CLIENTDLL );
	// USER real-machine feedback 2026-06-19 "月亮感觉有点小了": disc angular DIAMETER
	// bumped 2.0 -> 3.0 deg. SKY-REWORK-SPEC v3 Task C "大小：默认放大成焦点": bumped
	// again 3.0 -> 4.0 deg so the moon is a clear focal disc (对标 TARGET; ~8x the real
	// 0.5deg, well under the 30deg clamp). Phase / blood / limb-darkening / maria texture
	// are all scale-invariant, and the C4 T_moonBody occlusion mask reads this SAME size
	// (via MoonBodyOccluder) so the disc footprint and the star/MW cutout stay locked.
	// cvar-tunable for the USER's visual A/B sign-off.
	// USER target 2026-06-21 "月亮更大更亮": disc angular DIAMETER bumped 4.0 -> 5.5 deg so
	// the moon reads as the large prominent focal disc in the USER-TARGET ref (moon ~13%
	// of frame width); maria/limb/phase are scale-invariant and the C4 occluder mask
	// reads this SAME size so the star/MW cutout stays locked. Well under the 30deg clamp.
	// Capture A/B landed 6.5 deg: at the proven moon framing (Ang -30 90) the disc reads
	// as the reference's clear focal moon; 5.5 was a touch small, 6.5 matched prominence.
	s_cvMoonSize    = gEngfuncs.pfnRegisterVariable( "csz_moon_size", "8.5", FCVAR_CLIENTDLL );  // regrade G5: codex re-gate "moon too small" -> 6.5->8.5 deg (radiance size-invariant; occluder reads same size; <30deg clamp)
	// SKY-REWORK-SPEC v3 Task D: the standalone moon HALO cvars (csz_moon_halo /
	// csz_moon_halo_tau) were REMOVED. The moon's outer glow is now the rho->0 inner
	// segment of MoonSkyLum (csz_stars.cpp, single ownership). Tune it via the
	// csz_moon_wash* cvars (registered in csz_stars.cpp) -- NOT here.
	// Sun retuned to kill the "white blob": gain down (less R/G double-saturation),
	// aureole gain WAY down + exponent steeper so it is a tight halo, not a ~34deg smear.
	s_cvSunGain     = gEngfuncs.pfnRegisterVariable( "csz_sun_gain", "1.0", FCVAR_CLIENTDLL );
	s_cvSunSize     = gEngfuncs.pfnRegisterVariable( "csz_sun_size", "1.2", FCVAR_CLIENTDLL );
	s_cvSunAureole  = gEngfuncs.pfnRegisterVariable( "csz_sun_aureole", "0.22", FCVAR_CLIENTDLL );
	s_cvAureoleK    = gEngfuncs.pfnRegisterVariable( "csz_sun_aureole_k", "1.4", FCVAR_CLIENTDLL );
	// Real moon phases: -1 keeps the legacy always-full moon (default, opt-in safe);
	// [0,1] => 1 full / 0.5 quarter / 0 new. Single source of truth shared with the
	// PublishLighting phase-scaled moonlight (csz_sky.cpp, Option II).
	s_cvMoonPhase   = gEngfuncs.pfnRegisterVariable( "csz_moon_phase", "-1", FCVAR_CLIENTDLL );
	// Task D moonlight-wash phase non-linearity (opposition surge): I_moon = lit^p.
	// p~2.7 makes a quarter moon (lit 0.5) ~0.15 of full -- the strongly non-linear,
	// non-area-proportional brightness the research requires. Feeds MoonBodyOccluder's
	// outGlowL (the single source of the wash base luminance L_moon).
	s_cvMoonPhaseExp = gEngfuncs.pfnRegisterVariable( "csz_moon_phase_exp", "2.7", FCVAR_CLIENTDLL );
	// Moon god-ray SOURCE scale. The screen-space god-ray pass (fog/csz_fog_godrays)
	// reuses ONE sun-tuned scatter (sourceIntensity 1 * exposure 0.30 over a 49-tap
	// decay-0.95/weight-0.45 march => ~2.65x the reported source color AT the body
	// centre). The SUN can carry that; the MOON cannot -- at the moon's reported
	// source (moonColor*moonGain ~0.99) it accumulates ~2.6 linear over the disc,
	// hard-clipping the disc + halo to a white blob that erases the maria (measured
	// disc region 254,255,255 vs the bare disc's ~0.24). The moon is a far weaker
	// shaft source than the sun, so scale ITS god-ray source down here (disc/phase/
	// limb/maria/blood-moon are all untouched -- this ONLY feeds the god-ray pass via
	// CszGodraySource, not DrawBodies). 0 = no moon shafts; ~0.05 = a soft controlled
	// cool glow. cvar-tunable for the USER's visual A/B.
	s_cvMoonGodray  = gEngfuncs.pfnRegisterVariable( "csz_moon_godray", "0.05", FCVAR_CLIENTDLL );
	s_cvDebug       = gEngfuncs.pfnRegisterVariable( "csz_sunmoon_debug", "0", FCVAR_CLIENTDLL );
	CSZ_LogDev( "sunmoon", "cvars registered (csz_moon/sun + gain/size/aureole/phase/phase_exp/godray/debug)" );
}

float BloodMoonValue()
{
	if( !s_bloodLookedUp )
	{
		s_bloodLookedUp = true;
		s_cvBloodMoon = gEngfuncs.pfnGetCvarPointer( "csz_bloodmoon" );
	}
	return ReadCvar( s_cvBloodMoon, 0.0f );
}

// --- GPU resources (generation-keyed; forget on foreign context) --------------
struct SunMoonGpu
{
	GLuint vao;
	ShaderProgram prog;

	int uCamFwd, uCamRight, uCamUp;
	int uMoonDir, uMoonRight, uMoonUp, uSunDir;
	int uMoonLightDir;
	int uMoonExtinctDir, uSunExtinctDir;
	int uMoonAngR, uSunAngR;
	int uMoonColor, uSunColor, uMoonGain, uSunGain;
	int uSunAureoleGain, uAureoleK;
	int uBloodMoon, uMoonVis, uSunVis;
	int uTransLut, uHasTrans, uFallbackTransmit, uViewR;
	int uMoonTex, uHasMoonTex;

	GLuint moonGlName;     // raw GL name of the engine-loaded moon texture (0 = none)
	bool   moonTried;      // attempted load this generation

	int  gpuGeneration;
	bool created;
	bool failedThisGen;
};
SunMoonGpu s_gpu;

void ForgetGpu()
{
	memset( &s_gpu, 0, sizeof( s_gpu ) );
}

// NOTE: the C3 body pass had a GL_TIME_ELAPSED GPU-timer ring here. It was
// REMOVED (A2 hardening): the body pass runs at slot 10.5 INSIDE the HDR compose
// GL_TIME_ELAPSED span (csz_sky_compose.cpp BeginScene..Resolve), and GL forbids
// nesting two queries of the same target -- the nested glEndQuery closed the
// compose query early and raised GL_INVALID_OPERATION. It was dev-only
// instrumentation, so it is gone rather than re-plumbed; the compose timer
// already measures the whole HDR scene span (which includes the bodies).

// Load the NASA moon surface texture as our OWN raw GL texture on a sky unit
// (the sanctioned sky-subsystem pattern -- glGenTextures + SkyComposeBindTex,
// never the engine texture manager). The asset is a raw "CSZM" RGB8 mip chain
// produced offline from the public-domain NASA SVS CGI Moon Kit equirect; we
// upload each level so trilinear minification stays crisp on the small disc.
void EnsureMoonTexture()
{
	if( s_gpu.moonTried )
		return;
	s_gpu.moonTried = true;
	s_gpu.moonGlName = 0;

	int len = 0;
	byte *raw = gEngfuncs.COM_LoadFile( "gfx/csz/moon_color.bin", 5, &len );
	if( raw == NULL || len < 20 || memcmp( raw, "CSZM", 4 ) != 0 )
	{
		if( raw != NULL ) gEngfuncs.COM_FreeFile( raw );
		CSZ_LogWarn( "sunmoon", "gfx/csz/moon_color.bin missing/invalid -- using procedural fallback disc" );
		return;
	}

	int hdr[4];
	memcpy( hdr, raw + 4, sizeof( hdr ) );
	int w = hdr[0], h = hdr[1], ch = hdr[2], nLev = hdr[3];
	if( w <= 0 || h <= 0 || ch != 3 || nLev <= 0 || nLev > 20 )
	{
		gEngfuncs.COM_FreeFile( raw );
		CSZ_LogWarn( "sunmoon", "moon_color.bin header invalid (%dx%d ch%d lev%d)", w, h, ch, nLev );
		return;
	}

	// GL_MAX_TEXTURE_SIZE guard (mirrors the panorama loader): reject an over-large
	// base level rather than feeding it to glTexImage2D. Bounds the size math below;
	// the legit small moon disc is far under the limit, so this is behavior-neutral.
	const int maxTex = Caps().maxTextureSize;
	if( w > maxTex || h > maxTex )
	{
		gEngfuncs.COM_FreeFile( raw );
		CSZ_LogWarn( "sunmoon", "GL_MAX_TEXTURE_SIZE %d < moon %dx%d -- using procedural fallback disc", maxTex, w, h );
		return;
	}

	const unsigned char *p = (const unsigned char *)( raw + 20 );
	const unsigned char *end = (const unsigned char *)raw + len;

	GLuint tex = 0;
	glGenTextures( 1, &tex );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, tex );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );

	bool ok = true;
	int lw = w, lh = h;
	for( int lv = 0; lv < nLev; lv++ )
	{
		size_t bytes = (size_t)lw * (size_t)lh * 3;
		if( bytes > (size_t)( end - p ) ) { ok = false; break; }	// overflow-safe (no pointer-past-end UB)
		glTexImage2D( GL_TEXTURE_2D, lv, GL_RGB8, lw, lh, 0, GL_RGB, GL_UNSIGNED_BYTE, p );
		p += bytes;
		if( lw > 1 ) lw /= 2;
		if( lh > 1 ) lh /= 2;
	}
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );        // lon wraps
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, nLev - 1 );
	SkyComposeRestoreTmus();
	gEngfuncs.COM_FreeFile( raw );

	if( ok )
	{
		s_gpu.moonGlName = tex;
		CSZ_LogInfo( "sunmoon", "moon raw texture %dx%d (%d mips) uploaded (gl name %u)", w, h, nLev, (unsigned)tex );
	}
	else
	{
		glDeleteTextures( 1, &tex );
		CSZ_LogWarn( "sunmoon", "moon_color.bin truncated -- using procedural fallback disc" );
	}
}

bool EnsureCreated()
{
	if( s_gpu.gpuGeneration != GpuGeneration() )
	{
		ForgetGpu();                 // foreign generation: forget, never glDelete
		s_gpu.gpuGeneration = GpuGeneration();
	}
	if( s_gpu.created )
		return true;
	if( s_gpu.failedThisGen )
		return false;

	glGenVertexArrays( 1, &s_gpu.vao );

	if( !BuildProgram( "csz_sunmoon", kSunMoonVs, kSunMoonFs, false, s_gpu.prog ) )
	{
		s_gpu.failedThisGen = true;
		CSZ_LogError( "sunmoon", "shader build failed; sun/moon disabled this generation" );
		return false;
	}

	s_gpu.uCamFwd          = UniformLoc( s_gpu.prog, "u_camFwd" );
	s_gpu.uCamRight        = UniformLoc( s_gpu.prog, "u_camRight" );
	s_gpu.uCamUp           = UniformLoc( s_gpu.prog, "u_camUp" );
	s_gpu.uMoonDir         = UniformLoc( s_gpu.prog, "u_moonDir" );
	s_gpu.uMoonRight       = UniformLoc( s_gpu.prog, "u_moonRight" );
	s_gpu.uMoonUp          = UniformLoc( s_gpu.prog, "u_moonUp" );
	s_gpu.uSunDir          = UniformLoc( s_gpu.prog, "u_sunDir" );
	s_gpu.uMoonLightDir    = UniformLoc( s_gpu.prog, "u_moonLightDir" );
	s_gpu.uMoonExtinctDir  = UniformLoc( s_gpu.prog, "u_moonExtinctDir" );
	s_gpu.uSunExtinctDir   = UniformLoc( s_gpu.prog, "u_sunExtinctDir" );
	s_gpu.uMoonAngR        = UniformLoc( s_gpu.prog, "u_moonAngR" );
	s_gpu.uSunAngR         = UniformLoc( s_gpu.prog, "u_sunAngR" );
	s_gpu.uMoonColor       = UniformLoc( s_gpu.prog, "u_moonColor" );
	s_gpu.uSunColor        = UniformLoc( s_gpu.prog, "u_sunColor" );
	s_gpu.uMoonGain        = UniformLoc( s_gpu.prog, "u_moonGain" );
	s_gpu.uSunGain         = UniformLoc( s_gpu.prog, "u_sunGain" );
	s_gpu.uSunAureoleGain  = UniformLoc( s_gpu.prog, "u_sunAureoleGain" );
	s_gpu.uAureoleK        = UniformLoc( s_gpu.prog, "u_aureoleK" );
	s_gpu.uBloodMoon       = UniformLoc( s_gpu.prog, "u_bloodMoon" );
	s_gpu.uMoonVis         = UniformLoc( s_gpu.prog, "u_moonVis" );
	s_gpu.uSunVis          = UniformLoc( s_gpu.prog, "u_sunVis" );
	s_gpu.uTransLut        = UniformLoc( s_gpu.prog, "u_transLut" );
	s_gpu.uHasTrans        = UniformLoc( s_gpu.prog, "u_hasTrans" );
	s_gpu.uFallbackTransmit= UniformLoc( s_gpu.prog, "u_fallbackTransmit" );
	s_gpu.uViewR           = UniformLoc( s_gpu.prog, "u_viewR" );
	s_gpu.uMoonTex         = UniformLoc( s_gpu.prog, "u_moonTex" );
	s_gpu.uHasMoonTex      = UniformLoc( s_gpu.prog, "u_hasMoonTex" );

	s_gpu.moonTried = false;
	EnsureMoonTexture();

	s_gpu.created = true;
	CSZ_LogDev( "sunmoon", "GL resources created (gpu gen %d)", s_gpu.gpuGeneration );
	return true;
}

// Core body-draw shared by the slot-10.5 contribution and the dev fullscreen
// showcase. Assumes the HDR scene target is bound; draws additively, depth off,
// then restores the takeover baseline.
void DrawBodies( const ViewSetup &view )
{
	EnsureCvars();
	if( ReadCvar( s_cvMoon, 1.0f ) == 0.0f && ReadCvar( s_cvSun, 1.0f ) == 0.0f )
		return;
	if( !EnsureCreated() )
		return;

	// Phase + body world directions (honors csz_sky_phase; moon = antipodal sun).
	// The REAL celestial dirs drive atmospheric extinction (so a high midnight
	// moon stays bright even when the dev override re-aims the disc at the camera).
	float phase = g_sky.ComputePhase();
	float realSunDir[3], realMoonDir[3];
	skymath::SunDir( phase, realSunDir );
	realMoonDir[0] = -realSunDir[0]; realMoonDir[1] = -realSunDir[1]; realMoonDir[2] = -realSunDir[2];
	float sunDir[3]  = { realSunDir[0], realSunDir[1], realSunDir[2] };
	float moonDir[3] = { realMoonDir[0], realMoonDir[1], realMoonDir[2] };

	// Camera basis (Quake world, Z up); right/up pre-scaled by the half-FOV
	// tangents so the VS ray = fwd + right*ndc.x + up*ndc.y.
	float fwd[3], right[3], up[3];
	AngleVectors( view.angles, fwd, right, up );
	float tanX = tanf( view.fovX * kDegToRad * 0.5f );
	float tanY = tanf( view.fovY * kDegToRad * 0.5f );
	float rightS[3] = { right[0] * tanX, right[1] * tanX, right[2] * tanX };
	float upS[3]    = { up[0] * tanY,    up[1] * tanY,    up[2] * tanY };

	// Dev capture-aim override: face a body at the camera so a headless capture
	// frames it deterministically regardless of map spawn angle (analogue of
	// csz_devsun). 1 = full moon centered (sun antipodal); 2 = sun centered.
	int dbg = (int)ReadCvar( s_cvDebug, 0.0f );
	float moonVisOverride = -1.0f, sunVisOverride = -1.0f;
	if( dbg == 1 )
	{
		moonDir[0] = fwd[0]; moonDir[1] = fwd[1]; moonDir[2] = fwd[2];
		sunDir[0] = -fwd[0]; sunDir[1] = -fwd[1]; sunDir[2] = -fwd[2];  // full moon
		moonVisOverride = 1.0f; sunVisOverride = 0.0f;
	}
	else if( dbg == 2 )
	{
		sunDir[0] = fwd[0]; sunDir[1] = fwd[1]; sunDir[2] = fwd[2];
		sunVisOverride = 1.0f; moonVisOverride = 0.0f;
	}

	float moonRight[3], moonUp[3];
	sunmoon::BodyBasis( moonDir, moonRight, moonUp );

	// Phase-light direction for the moon disc (REAL phases, position stable: the disc
	// stays at moonDir == -sunDir; only the LIGHTING direction changes). csz_moon_phase
	// < 0 keeps the legacy always-full moon (light from the sun). Otherwise the light
	// rotates in the (-moonDir, moonRight) plane by the phase angle a = (1-phase)*PI:
	// full(a=0) -> -moonDir(==sunDir), quarter(a=PI/2) -> moonRight (vertical terminator),
	// new(a=PI) -> moonDir (dark near side). Computed after the dev override + BodyBasis
	// so the headless capture (dbg==1) also frames a centred crescent/gibbous.
	float moonPhase = ReadCvar( s_cvMoonPhase, -1.0f );
	float moonLightDir[3];
	if( moonPhase < 0.0f )
	{
		moonLightDir[0] = sunDir[0]; moonLightDir[1] = sunDir[1]; moonLightDir[2] = sunDir[2];
	}
	else
	{
		float p  = sunmoon::Clampf( moonPhase, 0.0f, 1.0f );
		float a  = ( 1.0f - p ) * 3.14159265358979323846f;
		float ca = cosf( a ), sa = sinf( a );
		moonLightDir[0] = ca * ( -moonDir[0] ) + sa * moonRight[0];
		moonLightDir[1] = ca * ( -moonDir[1] ) + sa * moonRight[1];
		moonLightDir[2] = ca * ( -moonDir[2] ) + sa * moonRight[2];
		sunmoon::Normalize3( moonLightDir );
		// (The disc HALO lit-fraction scaling was REMOVED with the standalone halo,
		// Task D; the wash phase term I_moon now lives in MoonBodyOccluder's outGlowL.)
	}

	// Visibility (horizon fade) + cvar enable.
	float moonVis = ( moonVisOverride >= 0.0f ) ? moonVisOverride : sunmoon::HorizonVis( moonDir );
	float sunVis  = ( sunVisOverride  >= 0.0f ) ? sunVisOverride  : sunmoon::HorizonVis( sunDir );
	if( ReadCvar( s_cvMoon, 1.0f ) == 0.0f ) moonVis = 0.0f;
	if( ReadCvar( s_cvSun, 1.0f ) == 0.0f )  sunVis = 0.0f;
	if( moonVis <= 0.0f && sunVis <= 0.0f )
		return;

	// Atmospheric extinction source (C2): bind the transmittance LUT if ready.
	const SkyAtmosResources &atmos = SkyComposeAtmosResources();
	bool transReady = atmos.ready
	                  && atmos.transmittanceLut.name != 0
	                  && atmos.transmittanceLut.gpuGeneration == GpuGeneration();
	float fallbackT[3] = { atmos.fallbackTransmittance[0], atmos.fallbackTransmittance[1], atmos.fallbackTransmittance[2] };
	if( fallbackT[0] <= 0.0f && fallbackT[1] <= 0.0f && fallbackT[2] <= 0.0f )
	{ fallbackT[0] = fallbackT[1] = fallbackT[2] = 1.0f; }   // no-extinction safe default

	// Clamp every cvar that feeds shader math to a sane range CPU-side: a negative
	// angular size -> pow() on a negative base (NaN), a negative gain -> the
	// soft-knee denominator (1+sunRad) can hit 0 (Inf), a negative tau/exponent ->
	// degenerate falloff. Diameters [0.05,30] deg; the rest non-negative / bounded.
	float moonAngR = sunmoon::Clampf( ReadCvar( s_cvMoonSize, 2.0f ), 0.05f, 30.0f ) * 0.5f * kDegToRad;
	float sunAngR  = sunmoon::Clampf( ReadCvar( s_cvSunSize, 1.2f ), 0.05f, 30.0f ) * 0.5f * kDegToRad;
	float moonGain     = sunmoon::Clampf( ReadCvar( s_cvMoonGain, 0.9f ),    0.0f, 100.0f );
	float sunGain      = sunmoon::Clampf( ReadCvar( s_cvSunGain, 1.0f ),     0.0f, 100.0f );
	float sunAureole   = sunmoon::Clampf( ReadCvar( s_cvSunAureole, 0.22f ), 0.0f, 100.0f );
	float aureoleK     = sunmoon::Clampf( ReadCvar( s_cvAureoleK, 1.4f ),    0.05f, 16.0f );

	// Blood moon: feed C3 the SAME tight-midnight pulse the legacy ground tint uses
	// (csz_sky.cpp BloodMoonFactor: triangular pulse, half-width 0.08 around midnight
	// ph=0.5), gated by csz_bloodmoon -- not a binary all-night red. KEEP THIS FORMULA
	// IN SYNC with csz_sky.cpp BloodMoonFactor() (one intended behaviour, two subsystems).
	float bloodFactor = 0.0f;
	if( BloodMoonValue() != 0.0f )
		bloodFactor = sunmoon::Clampf( 1.0f - fabsf( phase - 0.5f ) * ( 1.0f / 0.08f ), 0.0f, 1.0f );

	// Cool tint that neutralises the warm lunar albedo + horizon extinction so the
	// DISC reads near-neutral (D5: surface sat <~0.07, R~G~B) while the additive
	// HALO carries the cold blue-white night cast (D5: halo B>=R).
	const float moonColor[3] = { 0.78f, 0.88f, 1.10f };   // cold day-for-night DISC tint
	// (The standalone moon-halo tint was REMOVED with the halo, Task D. The wash glow's
	// warm-inner -> cool-blue colour now lives in u_moonGlowWarm/u_moonGlowCool, published
	// by csz_stars.cpp into the MoonSkyLum emit path.)
	const float sunColor[3]  = { 1.00f, 0.80f, 0.55f };   // warm

	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendAdditive );
	SetCull( false );

	UseProgram( s_gpu.prog.program );
	BindVao( s_gpu.vao );

	// Sky unit 0 = moon texture, sky unit 1 = C2 transmittance LUT.
	bool hasMoonTex = ( s_gpu.moonGlName != 0 );
	if( hasMoonTex )
		SkyComposeBindTex( 0, GL_TEXTURE_2D, s_gpu.moonGlName );
	if( transReady )
		SkyComposeBindTex( 1, GL_TEXTURE_2D, atmos.transmittanceLut.name );

	if( s_gpu.uCamFwd >= 0 )    glUniform3fv( s_gpu.uCamFwd, 1, fwd );
	if( s_gpu.uCamRight >= 0 )  glUniform3fv( s_gpu.uCamRight, 1, rightS );
	if( s_gpu.uCamUp >= 0 )     glUniform3fv( s_gpu.uCamUp, 1, upS );
	if( s_gpu.uMoonDir >= 0 )   glUniform3fv( s_gpu.uMoonDir, 1, moonDir );
	if( s_gpu.uMoonRight >= 0 ) glUniform3fv( s_gpu.uMoonRight, 1, moonRight );
	if( s_gpu.uMoonUp >= 0 )    glUniform3fv( s_gpu.uMoonUp, 1, moonUp );
	if( s_gpu.uSunDir >= 0 )    glUniform3fv( s_gpu.uSunDir, 1, sunDir );
	if( s_gpu.uMoonLightDir >= 0 ) glUniform3fv( s_gpu.uMoonLightDir, 1, moonLightDir );
	if( s_gpu.uMoonExtinctDir >= 0 ) glUniform3fv( s_gpu.uMoonExtinctDir, 1, realMoonDir );
	if( s_gpu.uSunExtinctDir >= 0 )  glUniform3fv( s_gpu.uSunExtinctDir, 1, realSunDir );
	if( s_gpu.uMoonAngR >= 0 )  glUniform1f( s_gpu.uMoonAngR, moonAngR );
	if( s_gpu.uSunAngR >= 0 )   glUniform1f( s_gpu.uSunAngR, sunAngR );
	if( s_gpu.uMoonColor >= 0 ) glUniform3fv( s_gpu.uMoonColor, 1, moonColor );
	if( s_gpu.uSunColor >= 0 )  glUniform3fv( s_gpu.uSunColor, 1, sunColor );
	if( s_gpu.uMoonGain >= 0 )  glUniform1f( s_gpu.uMoonGain, moonGain );
	if( s_gpu.uSunGain >= 0 )   glUniform1f( s_gpu.uSunGain, sunGain );
	if( s_gpu.uSunAureoleGain >= 0 ) glUniform1f( s_gpu.uSunAureoleGain, sunAureole );
	if( s_gpu.uAureoleK >= 0 )  glUniform1f( s_gpu.uAureoleK, aureoleK );
	if( s_gpu.uBloodMoon >= 0 ) glUniform1f( s_gpu.uBloodMoon, bloodFactor );
	if( s_gpu.uMoonVis >= 0 )   glUniform1f( s_gpu.uMoonVis, moonVis );
	if( s_gpu.uSunVis >= 0 )    glUniform1f( s_gpu.uSunVis, sunVis );
	if( s_gpu.uTransLut >= 0 )  glUniform1i( s_gpu.uTransLut, kSkyTmuBase + 1 );
	if( s_gpu.uHasTrans >= 0 )  glUniform1i( s_gpu.uHasTrans, transReady ? 1 : 0 );
	if( s_gpu.uFallbackTransmit >= 0 ) glUniform3fv( s_gpu.uFallbackTransmit, 1, fallbackT );
	if( s_gpu.uViewR >= 0 )     glUniform1f( s_gpu.uViewR, kViewR );
	if( s_gpu.uMoonTex >= 0 )   glUniform1i( s_gpu.uMoonTex, kSkyTmuBase + 0 );
	if( s_gpu.uHasMoonTex >= 0 ) glUniform1i( s_gpu.uHasMoonTex, hasMoonTex ? 1 : 0 );

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	BindVao( 0 );
	SkyComposeRestoreTmus();
	SetBlend( kBlendNone );
	SetDepthTest( true );
	SetDepthWrite( true );
}

cvar_t *s_cvFullscreen;
bool s_fullscreenLookedUp;

// True when the dev fullscreen overlay (csz_sky_fullscreen) is armed. Used to draw
// the bodies EXACTLY ONCE per frame: when the overlay repaints them over the
// finished frame, the slot-10.5 production pass must skip, else the ADDITIVE bodies
// stack/double (the dev double-draw the FIX-PLAN flags). Default csz_sky_fullscreen
// 0 (registered CSZ_DEV_TOOLS-only) -> normal slot-10.5 path, overlay no-op.
bool FullscreenActive()
{
	if( !s_fullscreenLookedUp )
	{
		s_fullscreenLookedUp = true;
		s_cvFullscreen = gEngfuncs.pfnGetCvarPointer( "csz_sky_fullscreen" );
	}
	return s_cvFullscreen != NULL && s_cvFullscreen->value != 0.0f;
}

}  // anonymous namespace

// =============================================================================
// PUBLIC: SKY-REWORK-SPEC v3 Task C data contract (moon -> stars/MW). Reports the
// moon BODY occluder disc geometry for the analytic T_moonBody mask the C4 star +
// Milky Way shaders apply. The disc placement (phase->dir, dev-aim override),
// angular RADIUS (csz_moon_size) and horizon visibility are computed IDENTICALLY to
// DrawBodies() so the cutout in layer1 lands EXACTLY under the drawn disc. The mask
// is PHASE-INDEPENDENT (an opaque geometric body -> the dark side occludes too): we
// report only dir/angR/softness, never the lit fraction. Returns false when the
// moon body is not contributing (below the horizon or csz_moon 0) -> the caller
// leaves T_moonBody == 1 everywhere (no occlusion).
// =============================================================================
bool MoonBodyOccluder( const ViewSetup &view, float outDir[3], float &outAngR, float &outSoft, float &outGlowL )
{
	outGlowL = 0.0f;
	EnsureCvars();
	if( ReadCvar( s_cvMoon, 1.0f ) == 0.0f )
		return false;   // moon body disabled -> no geometric occlusion / no wash

	// Moon disc placement = antipodal sun (same as DrawBodies), honoring csz_sky_phase.
	float phase = g_sky.ComputePhase();
	float realSunDir[3];
	skymath::SunDir( phase, realSunDir );
	float moonDir[3] = { -realSunDir[0], -realSunDir[1], -realSunDir[2] };

	// Dev capture-aim override (mirror DrawBodies): 1 = moon faced at the camera
	// (full-moon capture), 2 = sun faced (moon hidden). MUST match so a headless
	// capture's framed disc occludes the stars under it.
	int dbg = (int)ReadCvar( s_cvDebug, 0.0f );
	float moonVis;
	if( dbg == 1 )
	{
		float fwd[3];
		AngleVectors( view.angles, fwd, NULL, NULL );	// only fwd consumed here
		moonDir[0] = fwd[0]; moonDir[1] = fwd[1]; moonDir[2] = fwd[2];
		moonVis = 1.0f;
	}
	else if( dbg == 2 )
	{
		moonVis = 0.0f;
	}
	else
	{
		moonVis = sunmoon::HorizonVis( moonDir );
	}
	if( moonVis <= 0.0f )
		return false;   // moon at/below the horizon -> no disc drawn -> no occlusion

	// Angular RADIUS from csz_moon_size (DIAMETER deg) -- SAME clamp as DrawBodies.
	float moonAngR = sunmoon::Clampf( ReadCvar( s_cvMoonSize, 4.0f ), 0.05f, 30.0f ) * 0.5f * kDegToRad;

	outDir[0] = moonDir[0]; outDir[1] = moonDir[1]; outDir[2] = moonDir[2];
	outAngR = moonAngR;
	// Limb-AA feather just OUTSIDE the disc (~6% of the radius, floored ~0.05deg so it
	// is ~1-2 px at 1080p): the mask is T=0 across the WHOLE disc (ang<=angR) and
	// feathers to 1 over [angR, angR+soft] -> the entire footprint occludes layer1 and
	// the limb does not alias against the star field.
	outSoft = moonAngR * 0.06f;
	if( outSoft < 0.0009f )
		outSoft = 0.0009f;

	// --- Task D moonlight-wash base luminance L_moon = I_moon(phase) * moonAltFactor ---
	// I_moon: strongly non-linear in phase (opposition surge) -- I_moon = litFrac^p, so a
	// quarter moon (lit 0.5) is only ~0.15 of full. moonAltFactor: a high moon washes the
	// most (smaller airmass, lights more sky) -> max(0,sin(moonAlt))^0.5 from the REAL
	// celestial altitude (NOT the dev-aimed disc dir). dbg==1 capture = high full moon so a
	// headless V4 grab shows a strong wash regardless of map phase. Below the horizon the
	// non-dbg path already returned false above, so altF>0 here in normal play.
	{
		float litFrac, altF;
		if( dbg == 1 )
		{
			litFrac = 1.0f; altF = 1.0f;            // capture: high full moon
		}
		else
		{
			float mp = ReadCvar( s_cvMoonPhase, -1.0f );
			if( mp < 0.0f )
				litFrac = 1.0f;                     // legacy always-full moon
			else
				litFrac = skymath::MoonLitFractionFromPhase( mp );
			float mz = -realSunDir[2];              // sin(moon altitude) == moonDir.z
			altF = ( mz > 0.0f ) ? sqrtf( mz ) : 0.0f;
		}
		float pexp  = sunmoon::Clampf( ReadCvar( s_cvMoonPhaseExp, 2.7f ), 1.0f, 6.0f );
		float Imoon = powf( sunmoon::Clampf( litFrac, 0.0f, 1.0f ), pexp );
		outGlowL = Imoon * altF;
	}
	return true;
}

// =============================================================================
// PUBLIC: FOG Step 4 god-ray SOURCE (FOG-STEP4-GODRAYS-SPEC.md §-BodyContract).
// Resolves the single dominant active body the screen-space radial god-ray pass
// scatters from. Mirrors DrawBodies()'s EXACT visibility/phase/color terms -- the
// SAME g_sky.ComputePhase / skymath::SunDir / sunmoon::HorizonVis primitives, the
// SAME csz_sunmoon_debug capture-aim override, the SAME disc-shader phase-vis
// (litFrac->smoothstep, csz_sunmoon_shaders.inl:249-250), and the SAME disc base
// colors/gains -- so the shaft origin/fade/tint track the drawn disc. KEEP IN SYNC
// with DrawBodies() (same independent-re-derivation pattern as MoonBodyOccluder()).
// Does NOT use amb.moonlightColor (that is the GROUND directional light).
// =============================================================================
CszGodraySrc CszGodraySource( const ViewSetup &view )
{
	CszGodraySrc src;
	src.present = false;
	src.worldDir[0] = 0.0f; src.worldDir[1] = 0.0f; src.worldDir[2] = 1.0f;
	src.vis = 0.0f;
	src.color[0] = src.color[1] = src.color[2] = 0.0f;

	EnsureCvars();

	// Phase + body world directions (mirror DrawBodies: moon = antipodal sun).
	float phase = g_sky.ComputePhase();
	float realSunDir[3];
	skymath::SunDir( phase, realSunDir );
	float sunDir[3]  = { realSunDir[0], realSunDir[1], realSunDir[2] };
	float moonDir[3] = { -realSunDir[0], -realSunDir[1], -realSunDir[2] };

	// Dev capture-aim override (mirror DrawBodies): 1 = full moon faced at the
	// camera (sun antipodal), 2 = sun faced. So a headless capture frames the body
	// the god rays then originate from.
	int dbg = (int)ReadCvar( s_cvDebug, 0.0f );
	float moonVisOverride = -1.0f, sunVisOverride = -1.0f;
	if( dbg == 1 )
	{
		float fwd[3];
		AngleVectors( view.angles, fwd, NULL, NULL );	// only fwd consumed here
		moonDir[0] = fwd[0];  moonDir[1] = fwd[1];  moonDir[2] = fwd[2];
		sunDir[0]  = -fwd[0]; sunDir[1]  = -fwd[1]; sunDir[2]  = -fwd[2];
		moonVisOverride = 1.0f; sunVisOverride = 0.0f;
	}
	else if( dbg == 2 )
	{
		float fwd[3];
		AngleVectors( view.angles, fwd, NULL, NULL );	// only fwd consumed here
		sunDir[0] = fwd[0]; sunDir[1] = fwd[1]; sunDir[2] = fwd[2];
		sunVisOverride = 1.0f; moonVisOverride = 0.0f;
	}

	// HorizonVis fade + cvar enable gate (mirror DrawBodies lines 392-395).
	float moonVis = ( moonVisOverride >= 0.0f ) ? moonVisOverride : sunmoon::HorizonVis( moonDir );
	float sunVis  = ( sunVisOverride  >= 0.0f ) ? sunVisOverride  : sunmoon::HorizonVis( sunDir );
	if( ReadCvar( s_cvMoon, 1.0f ) == 0.0f ) moonVis = 0.0f;
	if( ReadCvar( s_cvSun, 1.0f ) == 0.0f )  sunVis = 0.0f;

	// Moon phase-illumination factor = the disc shader's bodyVis = smoothstep(0,0.02,
	// litFrac), litFrac = 0.5*(1+cos((1-phase)*PI)) (csz_sunmoon_shaders.inl:249-250
	// and the MoonBodyOccluder host litFrac). New moon (phase 0) -> 0 -> no rays;
	// legacy always-full (csz_moon_phase < 0) -> 1. Sun phaseFactor = 1.
	float moonPhase = ReadCvar( s_cvMoonPhase, -1.0f );
	float litFrac;
	if( moonPhase < 0.0f )
		litFrac = 1.0f;
	else
	{
		float p = sunmoon::Clampf( moonPhase, 0.0f, 1.0f );
		float a = ( 1.0f - p ) * 3.14159265358979323846f;
		litFrac = 0.5f * ( 1.0f + cosf( a ) );
	}
	// GLSL smoothstep(0,0.02,litFrac).
	float te = sunmoon::Clampf( litFrac / 0.02f, 0.0f, 1.0f );
	float phaseFactor = te * te * ( 3.0f - 2.0f * te );

	float sunEff  = sunVis;                 // sun phaseFactor = 1
	float moonEff = moonVis * phaseFactor;

	// present = max(sunEff, moonEff) > 1e-3; active = argmax; exact tie -> sun.
	if( sunEff <= 1e-3f && moonEff <= 1e-3f )
		return src;

	if( sunEff >= moonEff )
	{
		float sunGain = sunmoon::Clampf( ReadCvar( s_cvSunGain, 1.0f ), 0.0f, 100.0f );
		const float sunColor[3] = { 1.00f, 0.80f, 0.55f };   // warm (mirror DrawBodies)
		src.present = true;
		src.worldDir[0] = sunDir[0]; src.worldDir[1] = sunDir[1]; src.worldDir[2] = sunDir[2];
		src.vis = sunEff;
		src.color[0] = sunColor[0] * sunGain;
		src.color[1] = sunColor[1] * sunGain;
		src.color[2] = sunColor[2] * sunGain;
	}
	else
	{
		float moonGain = sunmoon::Clampf( ReadCvar( s_cvMoonGain, 0.9f ), 0.0f, 100.0f );
		// Moon god-ray SOURCE scale: the moon is a far weaker shaft source than the sun,
		// so attenuate the reported source color (the sun-tuned scatter pass amplifies it
		// ~2.65x at the body centre and would otherwise blow the moon disc to white). This
		// scopes ONLY the god-ray source -- the disc (DrawBodies), phase, limb, maria and
		// blood-moon are untouched. csz_moon_godray 0 = no moon shafts; ~0.05 = soft glow.
		float moonGodray = sunmoon::Clampf( ReadCvar( s_cvMoonGodray, 0.05f ), 0.0f, 4.0f );
		const float moonColor[3] = { 0.78f, 0.88f, 1.10f };   // cold (mirror DrawBodies)
		src.present = true;
		src.worldDir[0] = moonDir[0]; src.worldDir[1] = moonDir[1]; src.worldDir[2] = moonDir[2];
		src.vis = moonEff;
		src.color[0] = moonColor[0] * moonGain * moonGodray;
		src.color[1] = moonColor[1] * moonGain * moonGodray;
		src.color[2] = moonColor[2] * moonGain * moonGodray;
	}
	return src;
}

// =============================================================================
// PUBLIC: ABI seam -- slot 10.5 body contribution into the bound HDR FBO.
// =============================================================================
void SunMoonContribute( const ViewSetup &view )
{
	// One body draw per frame: skip the slot-10.5 production pass when the dev
	// fullscreen overlay is armed (it repaints the bodies over the finished frame),
	// so the additive bodies never stack twice. Normal play (csz_sky_fullscreen 0)
	// draws here.
	if( FullscreenActive() )
		return;
	DrawBodies( view );
}

// Register the C3 cvars at HUD init so configs can set them before the first
// frame (avoids the "Unknown command" warning from lazy registration). Safe to
// call repeatedly. Not in the frozen sky ABI -> forward-declared at the call site.
void SunMoonRegisterCvars()
{
	EnsureCvars();
}

// Dev proof/showcase: repaint the bodies over the whole finished frame when
// csz_sky_fullscreen != 0 (mirrors AtmosDrawDebugFullscreen), so a headless
// capture shows the moon/sun regardless of map geometry / camera aim. No-op when
// the cvar is absent/0.
void SunMoonDrawDebugFullscreen( const ViewSetup &view )
{
	if( !FullscreenActive() )
		return;
	DrawBodies( view );
}

}  // namespace csz
