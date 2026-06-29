/*
 * csz_stars.cpp -- CSOZ renderer: star field + Milky Way (C4)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). CLEAN-ROOM implementation of a
 * night-sky star field (BSC5 Yale Bright Star Catalogue numeric facts --
 * positions / magnitudes / B-V colour indices -- baked into
 * csz_stars_catalog.inl; see CREDITS-stars.md for the CDS/VizieR
 * acknowledgement) and a structured procedural Milky Way band. No code/shader/art
 * is copied or translated from any license-tainted source (see csoz
 * docs/provenance.md). Mechanism for the GL takeover / TMU / generation
 * discipline studied from PrimeXT (see csoz docs/notes/primext-render-
 * mechanisms-m2.md); implemented by an agent that has not read that source.
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
// C4. Fills the ABI seam StarsContribute(view): draws the Milky Way band then the
// star points into the bound HDR scene FBO at slot 10.5 (between AtmosDrawSky and
// SunMoonContribute). Both deposit PREMULTIPLIED linear-HDR radiance with alpha 0
// via SetBlend(kBlendAddPremul) (glBlendFunc(GL_ONE, GL_ONE)), depth test/write
// OFF, so the world overwrites by depth afterwards and the stars fade out under
// daylight (an explicit sun-elevation night factor zeroes the contribution above
// the horizon). All GL names are keyed on GpuGeneration() (forget -- never
// glDelete -- on a foreign context); StarsShutdown() deletes them on the owning
// context. Cvars are registered eagerly at HUD init via StarsRegisterCvars().
//
// PURKINJE NOTE (honest deferral): the resolve purkinje() hook lives in the
// FROZEN csz_sky_compose.cpp and the ABI publishes no channel for C4 to feed a
// low-luminance term into it without editing a frozen file. The scotopic blue
// shift is therefore NOT wired here; it is deferred until an ABI seam exists.
#include "csz_sky_compose.h"
#include "csz_sky.h"          // g_sky.ComputePhase() (honors csz_sky_phase)
#include "csz_sky_math.h"     // skymath::SunDir
#include "csz_stars_math.h"   // starsmath::NightFactorFromSunElev
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
#include <stdlib.h>

namespace csz
{

#include "csz_stars_shaders.inl"
#include "csz_stars_catalog.inl"

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;
const int   kFloatsPerStar = 5;   // {dirX,dirY,dirZ, bv, vmag}
const int   kVertsPerStar  = 6;   // expanded quad corners

// --- cvars (registered eagerly at HUD init, read live each frame) -------------
bool    s_cvarsReady = false;
cvar_t *s_cvarStars;       // csz_stars              "1": master on/off
cvar_t *s_cvarIntensity;   // csz_stars_intensity    "1.0": star master flux scale
cvar_t *s_cvarSize;        // csz_stars_size         "1.0": star spot-size scale (look only)
cvar_t *s_cvarColorSat;    // csz_stars_color_sat    "1.0": star chroma strength
cvar_t *s_cvarTwinkle;     // csz_stars_twinkle      "1.0": master twinkle amplitude scale (instant-off at 0)
cvar_t *s_cvarTwSpeed;     // csz_stars_twinkle_speed      "1.0": Hz multiplier on the per-star 1.5-6 Hz base
cvar_t *s_cvarTwChroma;    // csz_stars_twinkle_chroma     "0.12": luminance-preserving colour-flash strength (horizon + bright)
cvar_t *s_cvarTwElev;      // csz_stars_twinkle_elev       "0.875": airmass exponent (ampAir = X^elev; lower = zenith twinkles more)
cvar_t *s_cvarTwBright;    // csz_stars_twinkle_brightbias "0.15": faint-star amplitude floor brightW=mix(this,1,brightT)
cvar_t *s_cvarTwClampLo;   // csz_stars_twinkle_clamplo    "0.10": lower multiplier strobe-guard (never-extinguish floor)
cvar_t *s_cvarTwClampHi;   // csz_stars_twinkle_clamphi    "2.0": upper multiplier strobe-guard
cvar_t *s_cvarMagLimit;    // csz_pano_twinkle_maglimit "3.8": live twinkle layer renders ONLY stars with vmag <= this; dimmer stars live in the sampled panorama. REAL live cutoff (codex fix #8) -- the star VS culls per-frame on this uniform, so the cvar is never ignored by a build-time VBO.
cvar_t *s_cvarDiag;        // csz_stars_diag         "0": dev preflight probe (default OFF)
// --- SKY-REWORK-SPEC v3 Task D moonlight wash (MoonSkyLum) cvars ---------------
cvar_t *s_cvarMoonWash;     // csz_moon_wash       "1.0":  master wash strength (scales L_moon)
cvar_t *s_cvarMoonWashEmit; // csz_moon_wash_emit  "0.07": background-glow emit gain (f(rho) sky-lum -> HDR radiance)
cvar_t *s_cvarMoonWashKa;   // csz_moon_wash_ka    "5.6":  aureole coeff kA  (bounded-core peak)
cvar_t *s_cvarMoonWashKm;   // csz_moon_wash_km    "0.42": Mie mid-angle coeff kM (ring fix: was 0.3)
cvar_t *s_cvarMoonWashKr;   // csz_moon_wash_kr    "0.377":Rayleigh all-sky pedestal coeff kR
cvar_t *s_cvarMoonWashRho0; // csz_moon_wash_rho0  "2.6":  aureole bounded-core angle rho0 (deg) (ring fix: was 1.5)
cvar_t *s_cvarMoonWashMax;  // csz_moon_wash_max   "8.0":  GLOW_MAX cap on the f(rho) sum
cvar_t *s_cvarMoonWashC;    // csz_moon_wash_dimc  "1.0":  star-dimming lower-edge factor c (threshold=moonLum*c)
cvar_t *s_cvarMoonWashK;    // csz_moon_wash_dimk  "2.5":  star-dimming width factor k (upper edge=moonLum*c*k)


// --- GPU resources (generation-keyed; forget on foreign context) --------------
struct StarsGpu
{
	GLuint starsVao;
	GLuint starsVbo;
	GLuint diagVao;      // empty VAO for the preflight probe

	ShaderProgram progStars;
	ShaderProgram progDiag;

	// star program uniforms (live, cvar-driven; physics constants are baked into the GLSL)
	int uS_viewProj, uS_viewportPx, uS_time, uS_night;
	int uS_intensity, uS_sizeMul, uS_colorSat, uS_twAmp, uS_resScale;
	int uS_twSpeed, uS_twChroma, uS_twElev, uS_twBright, uS_twClampLo, uS_twClampHi;
	int uS_magLimit;                            // csz_pano_twinkle_maglimit live cutoff (codex fix #8)
	int uS_moonDir, uS_moonAngR, uS_moonSoft;   // Task C T_moonBody occlusion (star points)
	int uS_moonGlowL, uS_moonGlowCoef, uS_moonGlowMax, uS_starDim;   // Task D MoonSkyLum dimming (star points)
	// preflight probe uniform
	int uD_viewportPx;

	int  gpuGeneration;
	bool created;
	bool failedThisGen;
};
StarsGpu s_gpu;

// NOTE on GPU timing: we do NOT open our own GL_TIME_ELAPSED query here -- the HDR
// compose layer already has one ACTIVE across the whole scene span, and slot 10.5
// runs inside it. Nested GL_TIME_ELAPSED queries raise GL_INVALID_OPERATION, so
// the stars' GPU cost is measured as the delta of the compose scene span
// (csz_hdr_timing 1) with csz_stars 1 vs 0.

void DbgErr( const char *where )
{
	DbgGlError( s_cvarDiag, "stars", where );
}

void RegisterCvars()
{
	if( s_cvarsReady )
		return;
	s_cvarStars       = gEngfuncs.pfnRegisterVariable( "csz_stars", "1", FCVAR_CLIENTDLL );
	// Default 2.0 (was 1.0): USER ran the deployed build, set star brightness to ~2x
	// and signed off on it (2026-06-19 real-machine feedback). Baked in as the default.
	// Pure linear flux scale (u_intensity) -- the log-magnitude DISTRIBUTION SHAPE is
	// unchanged, so it cannot reintroduce the old clamp "TV snow"; it only lifts the
	// overall level the user already approved.
	s_cvarIntensity   = gEngfuncs.pfnRegisterVariable( "csz_stars_intensity", "2.0", FCVAR_CLIENTDLL );
	s_cvarSize        = gEngfuncs.pfnRegisterVariable( "csz_stars_size", "1.0", FCVAR_CLIENTDLL );
	s_cvarColorSat    = gEngfuncs.pfnRegisterVariable( "csz_stars_color_sat", "1.0", FCVAR_CLIENTDLL );
	s_cvarTwinkle     = gEngfuncs.pfnRegisterVariable( "csz_stars_twinkle", "1.0", FCVAR_CLIENTDLL );
	// Twinkle (atmospheric scintillation) -- REWRITTEN 2026-06-20 to the SKY-REWORK-SPEC
	// v3 Task B frozen contract: per-star hash-driven phase + 1.5-6 Hz frequency + right-
	// skewed amplitude under the X^0.875 airmass law, symmetric (energy-neutral) boil.
	// Defaults below are the frozen-contract numbers; csz_stars_twinkle is the master amp
	// scale (0 = instant OFF -- accessibility for photosensitive/competitive play). _speed
	// is a Hz multiplier (1.0 = native 1.5-6 Hz), _elev the airmass exponent (0.875),
	// _brightbias the faint-star amplitude floor (0.15), _clamplo/_clamphi the anti-strobe
	// clamps (0.10 / 2.0), _chroma the luminance-preserving colour-flash strength.
	s_cvarTwSpeed     = gEngfuncs.pfnRegisterVariable( "csz_stars_twinkle_speed",      "1.0",   FCVAR_CLIENTDLL );
	s_cvarTwChroma    = gEngfuncs.pfnRegisterVariable( "csz_stars_twinkle_chroma",     "0.12",  FCVAR_CLIENTDLL );
	s_cvarTwElev      = gEngfuncs.pfnRegisterVariable( "csz_stars_twinkle_elev",       "0.875", FCVAR_CLIENTDLL );
	s_cvarTwBright    = gEngfuncs.pfnRegisterVariable( "csz_stars_twinkle_brightbias", "0.15",  FCVAR_CLIENTDLL );
	s_cvarTwClampLo   = gEngfuncs.pfnRegisterVariable( "csz_stars_twinkle_clamplo",    "0.10",  FCVAR_CLIENTDLL );
	s_cvarTwClampHi   = gEngfuncs.pfnRegisterVariable( "csz_stars_twinkle_clamphi",    "2.0",   FCVAR_CLIENTDLL );
	// MW REWORK (2026-06-21): all procedural Milky Way cvars (csz_stars_milkyway,
	// csz_stars_mw_intensity/warmth/dust/starboost/nebula/corechroma/bright) were DELETED --
	// the band is now the sampled equirect panorama (csz_pano* cvars live in csz_panorama.cpp).
	// The ONLY new cvar here is the live twinkle-layer magnitude cutoff. Default 3.8 keeps the
	// live set to the brightest ~few-hundred BSC5 stars (those worth twinkling); the dim
	// remainder is supplied by the panorama (no double-count). The star VS honors this PER
	// FRAME (codex fix #8): the VBO holds all BSC5 stars, so raising it reveals more live stars
	// without a VBO rebuild. RAW values 0..7.5; clamped in StarsContribute before upload.
	s_cvarMagLimit    = gEngfuncs.pfnRegisterVariable( "csz_pano_twinkle_maglimit", "3.8", FCVAR_CLIENTDLL );
	s_cvarDiag        = gEngfuncs.pfnRegisterVariable( "csz_stars_diag", "0", FCVAR_CLIENTDLL );
	// --- SKY-REWORK-SPEC v3 Task D moonlight wash (MoonSkyLum) -----------------
	// Single-ownership f(rho) wash: drives BOTH the per-star photometric dimming
	// (star points) AND the emitted background glow (Milky Way pass; the moon's outer
	// halo is this glow's rho->0 inner segment -- the standalone C3 halo was removed).
	// Defaults are physically-reasoned (K&S aureole + Rayleigh pedestal) calibrated so
	// the near-moon limiting magnitude ~2.5 and the far full-moon global ~5 vs a dark-
	// sky field LEFT UNCHANGED (the dimming is moon-relative -> no wash, no change). All
	// are live cvars so the capture/USER A/B can trim near/far balance + halo radius.
	s_cvarMoonWash     = gEngfuncs.pfnRegisterVariable( "csz_moon_wash",      "1.0",   FCVAR_CLIENTDLL );
	s_cvarMoonWashEmit = gEngfuncs.pfnRegisterVariable( "csz_moon_wash_emit", "0.07",  FCVAR_CLIENTDLL );
	s_cvarMoonWashKa   = gEngfuncs.pfnRegisterVariable( "csz_moon_wash_ka",   "5.6",   FCVAR_CLIENTDLL );
	// MOON DARK-RING FIX (2026-06-21): the additive moon sky-glow had a too-spiky inner
	// aureole (rho0 1.5) + thin mid-angle fill (km 0.3). Empirically (toggle-isolation
	// radial profile) that bright tight skirt, sitting over a dim panorama patch, read as
	// a dark *contrast* annulus just outside it (NOT a negative/subtractive dip -- the glow
	// is strictly additive+monotonic). Softening the aureole core (rho0 1.5->2.6) + fuller
	// Mie shoulder (km 0.3->0.42) makes the halo a smooth monotonic falloff with no harsh
	// skirt, so there is no dark ring. Shared f(rho) (single ownership); the star-dimming
	// side is negligible (diagnosis A/B: wash on vs off pixel-identical outside the disc).
	s_cvarMoonWashKm   = gEngfuncs.pfnRegisterVariable( "csz_moon_wash_km",   "0.42",  FCVAR_CLIENTDLL );
	s_cvarMoonWashKr   = gEngfuncs.pfnRegisterVariable( "csz_moon_wash_kr",   "0.377", FCVAR_CLIENTDLL );
	s_cvarMoonWashRho0 = gEngfuncs.pfnRegisterVariable( "csz_moon_wash_rho0", "2.6",   FCVAR_CLIENTDLL );
	s_cvarMoonWashMax  = gEngfuncs.pfnRegisterVariable( "csz_moon_wash_max",  "8.0",   FCVAR_CLIENTDLL );
	s_cvarMoonWashC    = gEngfuncs.pfnRegisterVariable( "csz_moon_wash_dimc", "1.0",   FCVAR_CLIENTDLL );
	s_cvarMoonWashK    = gEngfuncs.pfnRegisterVariable( "csz_moon_wash_dimk", "2.5",   FCVAR_CLIENTDLL );
	s_cvarsReady = true;
	CSZ_LogDev( "stars", "cvars registered (csz_stars/intensity/size/color_sat/twinkle[+_speed/_chroma/_elev/_brightbias/_clamplo/_clamphi]/pano_twinkle_maglimit/diag/moon_wash[+_emit/_ka/_km/_kr/_rho0/_max/_dimc/_dimk])" );
}

void ForgetGpu()
{
	memset( &s_gpu, 0, sizeof( s_gpu ) );
}

void DestroyGpuSameContext()
{
	if( s_gpu.starsVbo ) glDeleteBuffers( 1, &s_gpu.starsVbo );
	if( s_gpu.starsVao ) glDeleteVertexArrays( 1, &s_gpu.starsVao );
	if( s_gpu.diagVao )  glDeleteVertexArrays( 1, &s_gpu.diagVao );
	if( s_gpu.progStars.program ) DestroyProgram( s_gpu.progStars );
	if( s_gpu.progDiag.program )  DestroyProgram( s_gpu.progDiag );
	ForgetGpu();
}

bool EnsureCreated()
{
	if( s_gpu.gpuGeneration != GpuGeneration() )
	{
		ForgetGpu();                       // foreign generation: forget, never glDelete
		s_gpu.gpuGeneration = GpuGeneration();
	}
	if( s_gpu.created )
		return true;
	if( s_gpu.failedThisGen )
		return false;

	BuildProgram( "csz_stars_points", kStarsVs, kStarsFs, true, s_gpu.progStars );
	BuildProgram( "csz_stars_diag", kStarsDiagVs, kStarsDiagFs, true, s_gpu.progDiag );

	s_gpu.uS_viewProj   = UniformLoc( s_gpu.progStars, "u_viewProj" );
	s_gpu.uS_viewportPx = UniformLoc( s_gpu.progStars, "u_viewportPx" );
	s_gpu.uS_time       = UniformLoc( s_gpu.progStars, "u_time" );
	s_gpu.uS_night      = UniformLoc( s_gpu.progStars, "u_nightFactor" );
	s_gpu.uS_intensity  = UniformLoc( s_gpu.progStars, "u_intensity" );
	s_gpu.uS_sizeMul    = UniformLoc( s_gpu.progStars, "u_sizeMul" );
	s_gpu.uS_colorSat   = UniformLoc( s_gpu.progStars, "u_colorSat" );
	s_gpu.uS_twAmp      = UniformLoc( s_gpu.progStars, "u_twAmp" );
	s_gpu.uS_twSpeed    = UniformLoc( s_gpu.progStars, "u_twSpeed" );
	s_gpu.uS_twChroma   = UniformLoc( s_gpu.progStars, "u_twChroma" );
	s_gpu.uS_twElev     = UniformLoc( s_gpu.progStars, "u_twElev" );
	s_gpu.uS_twBright   = UniformLoc( s_gpu.progStars, "u_twBrightBias" );
	s_gpu.uS_twClampLo  = UniformLoc( s_gpu.progStars, "u_twClampLo" );
	s_gpu.uS_twClampHi  = UniformLoc( s_gpu.progStars, "u_twClampHi" );
	s_gpu.uS_resScale   = UniformLoc( s_gpu.progStars, "u_resScale" );
	s_gpu.uS_magLimit   = UniformLoc( s_gpu.progStars, "u_magLimit" );   // live cutoff (codex fix #8)
	s_gpu.uS_moonDir    = UniformLoc( s_gpu.progStars, "u_moonDir" );
	s_gpu.uS_moonAngR   = UniformLoc( s_gpu.progStars, "u_moonAngR" );
	s_gpu.uS_moonSoft   = UniformLoc( s_gpu.progStars, "u_moonSoft" );
	s_gpu.uS_moonGlowL    = UniformLoc( s_gpu.progStars, "u_moonGlowL" );
	s_gpu.uS_moonGlowCoef = UniformLoc( s_gpu.progStars, "u_moonGlowCoef" );
	s_gpu.uS_moonGlowMax  = UniformLoc( s_gpu.progStars, "u_moonGlowMax" );
	s_gpu.uS_starDim      = UniformLoc( s_gpu.progStars, "u_starDim" );

	s_gpu.uD_viewportPx = UniformLoc( s_gpu.progDiag, "u_viewportPx" );

	// (The procedural Milky Way program uniform fetches were DELETED with the MW pass.)

	// Star VBO: each star {dir(3), bv(1), vmag(1)} = 5 floats is EXPANDED to its 6
	// quad-corner vertices (the corner comes from gl_VertexID % 6 in the VS), so a
	// plain non-instanced glDrawArrays(GL_TRIANGLES, 0, kStarCount*6) renders one
	// screen-aligned quad per star -- using only GL entry points present in the
	// renderer's loader table (no glDrawArraysInstanced / glVertexAttribDivisor).
	// STATIC, built once per generation; ~3.4 MB for 28k stars -- negligible.
	const size_t expandFloats = (size_t)kStarCount * kVertsPerStar * kFloatsPerStar;
	float *expand = (float *)malloc( expandFloats * sizeof( float ) );
	if( expand == NULL )
	{
		s_gpu.failedThisGen = true;
		CSZ_LogDev( "stars", "FATAL: out of memory expanding star VBO (%u floats)", (unsigned)expandFloats );
		return false;
	}
	for( int i = 0; i < kStarCount; ++i )
	{
		const float *src = &kStarData[(size_t)i * kFloatsPerStar];
		for( int v = 0; v < kVertsPerStar; ++v )
		{
			float *dst = &expand[((size_t)i * kVertsPerStar + v) * kFloatsPerStar];
			memcpy( dst, src, kFloatsPerStar * sizeof( float ) );
		}
	}

	glGenVertexArrays( 1, &s_gpu.starsVao );
	BindVao( s_gpu.starsVao );
	glGenBuffers( 1, &s_gpu.starsVbo );
	glBindBuffer( GL_ARRAY_BUFFER, s_gpu.starsVbo );
	glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)( expandFloats * sizeof( float ) ), expand, GL_STATIC_DRAW );
	free( expand );
	const int stride = kFloatsPerStar * (int)sizeof( float );
	glEnableVertexAttribArray( 0 );   // a_dir  (vec3)
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)0 );
	glEnableVertexAttribArray( 1 );   // a_bv   (float)
	glVertexAttribPointer( 1, 1, GL_FLOAT, GL_FALSE, stride, (const void *)( 3 * sizeof( float ) ) );
	glEnableVertexAttribArray( 2 );   // a_vmag (float)
	glVertexAttribPointer( 2, 1, GL_FLOAT, GL_FALSE, stride, (const void *)( 4 * sizeof( float ) ) );

	// Preflight probe: attrib-less draw via gl_VertexID; own empty VAO.
	glGenVertexArrays( 1, &s_gpu.diagVao );

	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	s_gpu.created = true;
	CSZ_LogDev( "stars", "star field created: %d BSC5 stars (live twinkle = brightest vmag<=cutoff), gpu gen %d",
		kStarCount, s_gpu.gpuGeneration );
	return true;
}

}  // anonymous namespace

// =============================================================================
// Boot-time cvar registration -- called from Renderer::OnHudInit() alongside
// SkyCompose/Atmos/SunMoon so the csz_stars_* cvars exist before the first
// rendered frame (no "Unknown command" on console queries at map load).
// Idempotent: the guard inside RegisterCvars() makes repeated calls harmless.
// =============================================================================
void StarsRegisterCvars()
{
	RegisterCvars();
}

// =============================================================================
// PUBLIC: generation-safe shutdown -- called from Renderer::Shutdown() while the
// owning GL context is still current. Deletes our GL objects on the owning
// context; if the generation has rolled (foreign context) we only forget.
// =============================================================================
void StarsShutdown()
{
	if( s_gpu.created && s_gpu.gpuGeneration == GpuGeneration() )
		DestroyGpuSameContext();
	else
		ForgetGpu();
}

// =============================================================================
// ABI SEAM: slot 10.5 additive contribution into the bound HDR scene FBO.
// =============================================================================
void StarsContribute( const ViewSetup &view )
{
	RegisterCvars();  // defensive: no-op after StarsRegisterCvars() at boot

	if( ReadCvar( s_cvarStars, 1.0f ) == 0.0f )
		return;

	// Night-visibility factor from the live sun elevation (honors csz_sky_phase).
	// CHANNEL 1 = live BRIGHT stars: fade in -2 -> -8 deg (civil/nautical twilight) so
	// the bright stars appear FIRST, well before the baked panorama field+MW backdrop
	// (which fades in later, -9 -> -16, in PanoramaContribute). Pure function of sun
	// elevation -> dusk/dawn symmetric for free (bright stars linger last at dawn).
	float phase = g_sky.ComputePhase();
	float sun[3];
	skymath::SunDir( phase, sun );
	float sz = ( sun[2] < -1.0f ) ? -1.0f : ( sun[2] > 1.0f ? 1.0f : sun[2] );
	float sunElevDeg = asinf( sz ) / kDegToRad;
	float nightFactor = starsmath::BrightStarNightFactor( sunElevDeg );
	if( nightFactor < 0.01f )
		return;   // daylight / before bright-star onset: nothing to add

	if( !EnsureCreated() )
		return;
	DbgErr( "ensure-created" );

	// Premultiplied additive into the HDR FBO; depth OFF (world overwrites by depth).
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendAddPremul );
	SetCull( false );

	// ---- Moon BODY occluder (SKY-REWORK-SPEC v3 Task C: T_moonBody) ----
	// Query the SAME disc geometry the C3 sun/moon pass draws (MoonBodyOccluder mirrors
	// DrawBodies: phase->dir, dev-aim override, csz_moon_size, horizon vis) so the
	// analytic mask in the star + MW shaders occludes layer1 EXACTLY under the drawn
	// disc. Inactive (moon down / csz_moon 0) -> angR < 0 -> the shader mask is T==1.
	float moonOccDir[3] = { 0.0f, 0.0f, 1.0f };
	float moonOccAngR = -1.0f, moonOccSoft = 0.001f;
	float moonGlowL = 0.0f;   // Task D: L_moon = I_moon(phase)*moonAltFactor (0 = no wash)
	if( !MoonBodyOccluder( view, moonOccDir, moonOccAngR, moonOccSoft, moonGlowL ) )
	{
		moonOccDir[0] = 0.0f; moonOccDir[1] = 0.0f; moonOccDir[2] = 1.0f;
		moonOccAngR = -1.0f; moonOccSoft = 0.001f;
		moonGlowL = 0.0f;
	}

	// ---- Task D MoonSkyLum wash parameters (per-star photometric DIMMING only) ----
	// moonGlowL is scaled by the master csz_moon_wash; the coefficients are the shared
	// f(rho) shape. The star-points pass reads MoonSkyLum to DIM each star near the moon;
	// it does NOT emit the wash radiance. The visible moon outer glow (emit + warm/cool
	// tint) is now emitted ONCE by the panorama pass (csz_panorama.cpp), which owns the
	// relocated MoonSkyGlow -- so the single-ownership f(rho) stays numerically identical.
	moonGlowL *= ReadCvar( s_cvarMoonWash, 1.0f );
	float moonGlowCoef[4] = {
		ReadCvar( s_cvarMoonWashKa,   5.6f   ),    // kA aureole
		ReadCvar( s_cvarMoonWashKm,   0.42f  ),    // kM mie (ring fix: fuller mid shoulder, was 0.3)
		ReadCvar( s_cvarMoonWashKr,   0.377f ),    // kR rayleigh pedestal
		clampf( ReadCvar( s_cvarMoonWashRho0, 2.6f ), 0.05f, 30.0f )  // rho0 bounded-core (deg) (ring fix: softer aureole, was 1.5)
	};
	float moonGlowMax  = ReadCvar( s_cvarMoonWashMax, 8.0f );
	float starDim[2]   = {
		ReadCvar( s_cvarMoonWashC, 1.0f ),                          // c
		clampf( ReadCvar( s_cvarMoonWashK, 2.5f ), 1.05f, 8.0f )    // k (>1 so smoothstep edge1>edge0)
	};

	// ---- Star points: conserved-flux, energy-normalised Gaussian quad PSF ----
	// (The procedural Milky Way band that used to draw here FIRST was DELETED in the MW
	// rewrite -- the band is now the sampled equirect panorama drawn by PanoramaContribute,
	// inserted in csz_renderer.cpp immediately BEFORE this StarsContribute. The galactic
	// basis + camera-basis + MW colour/uniform setup went with it; the panorama pass keeps
	// its own copy of the galactic frame, so band/stars/moon stay mutually aligned.)
	//
	// Viewport pixel size (with the headless 1920x1080 fallback), shared by the
	// star-points upload here and the diag-probe upload below (view.viewport is not
	// mutated between them).
	float vpW = ( view.viewport[2] > 0 ) ? (float)view.viewport[2] : 1920.0f;
	float vpH = ( view.viewport[3] > 0 ) ? (float)view.viewport[3] : 1080.0f;
	{
		float resScale = vpH / 1080.0f;
		if( resScale < 0.5f ) resScale = 0.5f;

		UseProgram( s_gpu.progStars.program );
		BindVao( s_gpu.starsVao );
		float vpVec[3] = { vpW, vpH, 0.0f };
		if( s_gpu.uS_viewProj >= 0 )   glUniformMatrix4fv( s_gpu.uS_viewProj, 1, GL_FALSE, view.matViewProj.m );
		if( s_gpu.uS_viewportPx >= 0 ) glUniform3fv( s_gpu.uS_viewportPx, 1, vpVec );
		// Wrap CPU-side before upload: the twinkle boil raises frequencies ~6x, and
		// sin(largeTime*freq) loses float-mantissa bits after a long map -> the high
		// octaves stair-step / drift toward sync. mod 3600 s is large enough that the
		// once-an-hour global wrap is imperceptible against the per-star boil (every
		// star's phase is a fixed offset). Do the wrap here, NEVER in-shader.
		// Verification infra: csz_verify_freeze pins the twinkle/flicker clock to a constant so the
		// per-star boil cannot drift between two captures of the SAME fixed instant (byte-stable A/B).
		if( s_gpu.uS_time >= 0 )       glUniform1f( s_gpu.uS_time,
			SkyComposeVerifyFreeze() ? 0.0f : fmodf( ClientTime(), 3600.0f ) );
		if( s_gpu.uS_night >= 0 )      glUniform1f( s_gpu.uS_night, nightFactor );
		if( s_gpu.uS_intensity >= 0 )  glUniform1f( s_gpu.uS_intensity, ReadCvar( s_cvarIntensity, 1.0f ) );
		// Clamp the look-only spot-size cvar to a safe range: sigmaPx is divided by sigma^2
		// in the energy norm, so a 0 / negative / tiny value would blow the centre peak up
		// to inf/NaN. Default 1.0 is unaffected.
		float sizeMul = ReadCvar( s_cvarSize, 1.0f );
		if( sizeMul < 0.25f ) sizeMul = 0.25f;
		else if( sizeMul > 4.0f ) sizeMul = 4.0f;
		if( s_gpu.uS_sizeMul >= 0 )    glUniform1f( s_gpu.uS_sizeMul, sizeMul );
		if( s_gpu.uS_colorSat >= 0 )   glUniform1f( s_gpu.uS_colorSat, ReadCvar( s_cvarColorSat, 1.0f ) );
		// TWINKLE REWRITE (2026-06-20, SKY-REWORK-SPEC v3 Task B frozen contract).
		// AMPLITUDE CALIBRATION (2026-06-20, mechanism frozen, amplitude-only retune):
		// the v3 rewrite shipped too weak -- GL4.6 timeseries measured low-sky "obviously
		// twinkling" share ~0.3% / zenith 0% vs the spec Section 2 V3 target of 20-40%.
		// u_twAmp is the master amplitude fed into amp_i = clamp(u_twAmp * skew_i *
		// ampAir * brightW, 0, cap). Base raised 0.12 -> 0.35 (~2.9x): measured low-sky
		// max single-star RMS was 13.4% @ 0.12, so 0.35 linear-extrapolates the brightest
		// low star (ampAir~2.41, alt~21deg) to amp~0.84 -> RMS ~39%, and combined with the
		// softened s^1.5 skew (in the VS, was s*s) lifts the mid-skew majority ~4-5x,
		// pulling the obviously-twinkling share into the 20-40% band. Zenith stays gentle
		// (airmass law unchanged): max RMS ~3.5% -> ~10%. The skew is RIGHT-skewed still,
		// so most stars stay calm and only a low-sky minority "boils". This is the single
		// calibration knob the capture/quantitative gate (spec Section 2) trims.
		// csz_stars_twinkle is the master scale (0 = instant off), kept user-dialable.
		if( s_gpu.uS_twAmp >= 0 )      glUniform1f( s_gpu.uS_twAmp, 0.35f * ReadCvar( s_cvarTwinkle, 1.0f ) );
		// Twinkle knobs, clamped to safe ranges (guards against blow-ups / seizure band).
		// _speed is now a Hz MULTIPLIER on the per-star 1.5-6 Hz base (default 1.0);
		// _elev is the airmass exponent (default 0.875); _brightbias is the faint-star
		// amplitude FLOOR brightW = mix(_brightbias, 1, brightT) (default 0.15);
		// _clamplo/_clamphi are the symmetric anti-strobe clamps (default 0.10 / 2.0).
		if( s_gpu.uS_twSpeed   >= 0 ) glUniform1f( s_gpu.uS_twSpeed,   clampf( ReadCvar( s_cvarTwSpeed,   1.0f  ), 0.25f, 3.0f  ) );
		if( s_gpu.uS_twChroma  >= 0 ) glUniform1f( s_gpu.uS_twChroma,  clampf( ReadCvar( s_cvarTwChroma,  0.12f ), 0.0f,  0.30f ) );
		if( s_gpu.uS_twElev    >= 0 ) glUniform1f( s_gpu.uS_twElev,    clampf( ReadCvar( s_cvarTwElev,    0.875f ), 0.0f,  1.5f ) );
		if( s_gpu.uS_twBright  >= 0 ) glUniform1f( s_gpu.uS_twBright,  clampf( ReadCvar( s_cvarTwBright,  0.15f ), 0.0f,  1.0f ) );
		if( s_gpu.uS_twClampLo >= 0 ) glUniform1f( s_gpu.uS_twClampLo, clampf( ReadCvar( s_cvarTwClampLo, 0.10f ), 0.05f, 0.90f ) );
		if( s_gpu.uS_twClampHi >= 0 ) glUniform1f( s_gpu.uS_twClampHi, clampf( ReadCvar( s_cvarTwClampHi, 2.0f  ), 1.10f, 2.20f ) );
		if( s_gpu.uS_resScale >= 0 )   glUniform1f( s_gpu.uS_resScale, resScale );
		// Live magnitude cutoff (codex fix #8): the VS culls any star with vmag > this each
		// frame, so the live twinkle layer renders only the brightest ~few-hundred BSC5 stars
		// and the dim remainder is left to the sampled panorama (no double-count). Clamped to
		// the catalogue's vmag span so a stray cvar can't blank the whole field or run unbounded.
		if( s_gpu.uS_magLimit >= 0 )   glUniform1f( s_gpu.uS_magLimit, clampf( ReadCvar( s_cvarMagLimit, 3.8f ), -2.0f, 7.5f ) );
		// Task C T_moonBody: same occluder geometry as the panorama backdrop (and the C3 disc).
		if( s_gpu.uS_moonDir >= 0 )   glUniform3fv( s_gpu.uS_moonDir, 1, moonOccDir );
		if( s_gpu.uS_moonAngR >= 0 )  glUniform1f( s_gpu.uS_moonAngR, moonOccAngR );
		if( s_gpu.uS_moonSoft >= 0 )  glUniform1f( s_gpu.uS_moonSoft, moonOccSoft );
		// Task D MoonSkyLum: shared f(rho) + per-star dimming calibration (this pass does
		// NOT emit the wash radiance -- it only reads MoonSkyLum to threshold each star).
		if( s_gpu.uS_moonGlowL >= 0 )    glUniform1f( s_gpu.uS_moonGlowL, moonGlowL );
		if( s_gpu.uS_moonGlowCoef >= 0 ) glUniform4fv( s_gpu.uS_moonGlowCoef, 1, moonGlowCoef );
		if( s_gpu.uS_moonGlowMax >= 0 )  glUniform1f( s_gpu.uS_moonGlowMax, moonGlowMax );
		if( s_gpu.uS_starDim >= 0 )      glUniform2fv( s_gpu.uS_starDim, 1, starDim );
		glDrawArrays( GL_TRIANGLES, 0, kStarCount * kVertsPerStar );
		DbgErr( "points" );
	}

	// ---- DIAG PREFLIGHT (csz_stars_diag 1 only) ----
	// One fixed 16x16 pure-white quad at the viewport centre, REPLACE blend (off),
	// into the same scene HDR FBO. If it shows in a capture the whole
	// StarsContribute -> FBO -> resolve -> screenshot chain is sound. NEVER valid as
	// success evidence for the real star field.
	if( ReadCvar( s_cvarDiag, 0.0f ) != 0.0f )
	{
		float vpVec[3] = { vpW, vpH, 0.0f };
		SetBlend( kBlendNone );          // replace -- guaranteed coverage
		UseProgram( s_gpu.progDiag.program );
		BindVao( s_gpu.diagVao );
		if( s_gpu.uD_viewportPx >= 0 ) glUniform3fv( s_gpu.uD_viewportPx, 1, vpVec );
		glDrawArrays( GL_TRIANGLES, 0, 6 );
		DbgErr( "diag-probe" );
	}

	// Restore the takeover baseline.
	BindVao( 0 );
	UseProgram( 0 );
	SetBlend( kBlendNone );
	SetDepthTest( true );
	SetDepthWrite( true );
}

}  // namespace csz
