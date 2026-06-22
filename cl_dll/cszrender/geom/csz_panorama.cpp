/*
 * csz_panorama.cpp -- CSOZ renderer: sampled all-sky panorama backdrop (MW rework)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Replaces the per-pixel procedural
 * Milky Way (deleted from csz_stars) with a single sampled equirectangular sky
 * panorama: a static deep-space backdrop (NASA/GSFC SVS Deep Star Maps 2020 galactic
 * variant, Public Domain -- starless MW band + bright-removed dim starfield, baked
 * offline to the raw "CSZP" RGB8 mip container -- see bake/bake_panorama.py and
 * CREDITS-sky-assets.md) drawn additively into the HDR scene FBO, night-gated and
 * moon-occluded, behind the live bright-star twinkle layer and the moon. The moon
 * outer sky-glow (MoonSkyLum/MoonSkyGlow) was RELOCATED here out of the deleted
 * Milky Way shader so the moon halo survives the rewrite. Clean-room; no code or
 * shader is copied or translated from any license-tainted source. The panorama is
 * a separately-licensed DATA asset, not compiled into this GPL source.
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
// Composite point: a new PanoramaContribute(view) inserted in csz_renderer.cpp
// immediately BEFORE StarsContribute(view) (sky tier slot 10.5), so the backdrop
// sits behind the live twinkle stars and the moon. All three are additive into the
// RGBA16F HDR FBO (addition commutes, so order does not change summed radiance);
// the panorama draws first for conceptual clarity and consistent moon-occlusion.
#include "csz_sky_compose.h"
#include "csz_sky.h"          // g_sky.ComputePhase() (honors csz_sky_phase)
#include "csz_sky_math.h"     // skymath::SunDir
#include "csz_stars_math.h"   // starsmath::NightFactorFromSunElev (SHARED night curve)
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

#include "csz_panorama_shaders.inl"

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

float ReadCvar( cvar_t *cv, float fallback ) { return ( cv != NULL ) ? cv->value : fallback; }
float clampf( float v, float lo, float hi ) { return ( v < lo ) ? lo : ( v > hi ) ? hi : v; }

// --- cvars (registered eagerly at HUD init; read live each frame) ----------------
bool    s_cvarsReady = false;
cvar_t *s_cvarPano;        // csz_pano             "1":   master on/off for the sampled backdrop
cvar_t *s_cvarPanoInt;     // csz_pano_intensity   "1.0": single master radiance scalar
cvar_t *s_cvarPanoLon;     // csz_pano_lon_offset  "0.0": longitude alignment nudge (UV, wraps)
cvar_t *s_cvarPanoSat;     // csz_pano_saturation  "1.0": runtime post-saturation fine-tune
cvar_t *s_cvarPanoWarm;    // csz_pano_warm        "0.0": runtime warm(+)/cool(-) tint fine-tune
// Moon-wash cvars are OWNED by csz_stars (registered there at boot); we fetch the
// pointers so the relocated moon glow emits with the SAME live values the deleted
// Milky Way pass used -- numerically identical f(rho), no second source of truth.
cvar_t *s_cvMoonWash;      // csz_moon_wash
cvar_t *s_cvMoonWashEmit;  // csz_moon_wash_emit
cvar_t *s_cvMoonWashKa;    // csz_moon_wash_ka
cvar_t *s_cvMoonWashKm;    // csz_moon_wash_km
cvar_t *s_cvMoonWashKr;    // csz_moon_wash_kr
cvar_t *s_cvMoonWashRho0;  // csz_moon_wash_rho0
cvar_t *s_cvMoonWashMax;   // csz_moon_wash_max

// === FROZEN galactic-frame RAW constants -- MUST stay byte-identical to kMwRawPole/
// kMwRawCenter in csz_stars.cpp and RAW_POLE/RAW_CENTER in bake/bake_stars.py (the
// catalog's kStarGalPoleWorld/CenterWorld/YWorld are the orthonormalisation of these).
// They place the galactic band across the sky; the panorama must be authored in this
// galactic-equirect frame (band on v=0.5, centre at u=0.5). =========================
const float kMwRawPole[3]   = { 0.400f, -0.708f, 0.582f };
const float kMwRawCenter[3] = { 0.022f,  0.642f, 0.766f };

void GalacticBasis( float galPole[3], float galCenter[3], float galY[3] )
{
	float pn = sqrtf( kMwRawPole[0]*kMwRawPole[0] + kMwRawPole[1]*kMwRawPole[1] + kMwRawPole[2]*kMwRawPole[2] );
	if( pn < 1e-6f ) pn = 1.0f;
	galPole[0] = kMwRawPole[0]/pn; galPole[1] = kMwRawPole[1]/pn; galPole[2] = kMwRawPole[2]/pn;
	float d = kMwRawCenter[0]*galPole[0] + kMwRawCenter[1]*galPole[1] + kMwRawCenter[2]*galPole[2];
	float cx = kMwRawCenter[0] - galPole[0]*d, cy = kMwRawCenter[1] - galPole[1]*d, cz = kMwRawCenter[2] - galPole[2]*d;
	float cn = sqrtf( cx*cx + cy*cy + cz*cz );
	if( cn < 1e-6f ) cn = 1.0f;
	galCenter[0] = cx/cn; galCenter[1] = cy/cn; galCenter[2] = cz/cn;
	galY[0] = galPole[1]*galCenter[2] - galPole[2]*galCenter[1];
	galY[1] = galPole[2]*galCenter[0] - galPole[0]*galCenter[2];
	galY[2] = galPole[0]*galCenter[1] - galPole[1]*galCenter[0];
}

// --- GPU resources (generation-keyed; forget on a foreign context) ---------------
struct PanoramaGpu
{
	GLuint  panoVao;       // empty VAO for the attrib-less fullscreen triangle
	GLuint  texName;       // raw GL panorama texture (bound only via SkyComposeBindTex)
	bool    texTried;      // one-shot load guard (like s_gpu.moonTried)

	ShaderProgram prog;
	int uCamFwd, uCamRight, uCamUp;
	int uPanorama, uPanoValid, uPanoIntensity, uPanoLonOffset, uPanoSat, uPanoTint, uNight, uNightMoon;
	int uGalPole, uGalCenter, uGalY;
	int uMoonDir, uMoonAngR, uMoonSoft;
	int uMoonGlowL, uMoonGlowCoef, uMoonGlowMax, uMoonGlowEmit, uMoonGlowWarm, uMoonGlowCool;

	int  gpuGeneration;
	bool created;
	bool failedThisGen;
};
PanoramaGpu s_pano;

void RegisterCvars()
{
	if( s_cvarsReady )
		return;
	s_cvarPano     = gEngfuncs.pfnRegisterVariable( "csz_pano",            "1",   FCVAR_CLIENTDLL );
	s_cvarPanoInt  = gEngfuncs.pfnRegisterVariable( "csz_pano_intensity",  "1.0", FCVAR_CLIENTDLL );
	s_cvarPanoLon  = gEngfuncs.pfnRegisterVariable( "csz_pano_lon_offset", "0.0", FCVAR_CLIENTDLL );
	s_cvarPanoSat  = gEngfuncs.pfnRegisterVariable( "csz_pano_saturation", "1.0", FCVAR_CLIENTDLL );
	s_cvarPanoWarm = gEngfuncs.pfnRegisterVariable( "csz_pano_warm",       "0.0", FCVAR_CLIENTDLL );
	// Moon-wash cvars are registered by csz_stars at boot (OnHudInit, before any frame);
	// fetch the existing pointers so the relocated glow tracks the same live values.
	s_cvMoonWash     = gEngfuncs.pfnGetCvarPointer( "csz_moon_wash" );
	s_cvMoonWashEmit = gEngfuncs.pfnGetCvarPointer( "csz_moon_wash_emit" );
	s_cvMoonWashKa   = gEngfuncs.pfnGetCvarPointer( "csz_moon_wash_ka" );
	s_cvMoonWashKm   = gEngfuncs.pfnGetCvarPointer( "csz_moon_wash_km" );
	s_cvMoonWashKr   = gEngfuncs.pfnGetCvarPointer( "csz_moon_wash_kr" );
	s_cvMoonWashRho0 = gEngfuncs.pfnGetCvarPointer( "csz_moon_wash_rho0" );
	s_cvMoonWashMax  = gEngfuncs.pfnGetCvarPointer( "csz_moon_wash_max" );
	s_cvarsReady = true;
	CSZ_LogDev( "panorama", "cvars registered (csz_pano/csz_pano_intensity/csz_pano_lon_offset/csz_pano_saturation/csz_pano_warm)" );
}

void ForgetGpu()
{
	memset( &s_pano, 0, sizeof( s_pano ) );
}

void DestroyGpuSameContext()
{
	if( s_pano.texName )       glDeleteTextures( 1, &s_pano.texName );
	if( s_pano.panoVao )       glDeleteVertexArrays( 1, &s_pano.panoVao );
	if( s_pano.prog.program )  DestroyProgram( s_pano.prog );
	ForgetGpu();
}

// One-shot load of the offline-baked "CSZP" RGB8 mip chain onto sky unit 0 (abs unit
// kSkyTmuBase+0 = GL_TEXTURE4). VERBATIM copy of the EnsureMoonTexture() idiom
// (csz_sunmoon.cpp:221-285) with magic "CSZP" and the equirect sampler params. NO
// glGenerateMipmap (not wired) -- the mips are pre-baked. Bind/restore ONLY through
// the sky-TMU contract; never raw glActiveTexture/glBindTexture on a sky unit.
void EnsureTexture()
{
	if( s_pano.texTried )
		return;
	s_pano.texTried = true;
	s_pano.texName  = 0;

	int len = 0;
	byte *raw = gEngfuncs.COM_LoadFile( "gfx/csz/mw_panorama.bin", 5, &len );
	if( raw == NULL || len < 20 || memcmp( raw, "CSZP", 4 ) != 0 )
	{
		if( raw != NULL ) gEngfuncs.COM_FreeFile( raw );
		CSZ_LogWarn( "panorama", "gfx/csz/mw_panorama.bin missing/invalid -- backdrop disabled (moon glow still emits)" );
		return;
	}

	int hdr[4];
	memcpy( hdr, raw + 4, sizeof( hdr ) );
	int w = hdr[0], h = hdr[1], ch = hdr[2], nLev = hdr[3];
	if( w <= 0 || h <= 0 || ch != 3 || nLev <= 0 || nLev > 20 )
	{
		gEngfuncs.COM_FreeFile( raw );
		CSZ_LogWarn( "panorama", "mw_panorama.bin header invalid (%dx%d ch%d lev%d)", w, h, ch, nLev );
		return;
	}

	// GL_MAX_TEXTURE_SIZE guard (codex nit): the global caps floor is only 1024, but the 4K
	// panorama needs >= 4096. Do NOT raise the global FATAL floor (it would brick low-end GL3.3
	// parts that only need the moon); instead just disable the backdrop here if the base level
	// exceeds the limit -- the moon glow still emits, exactly like the missing-asset path.
	const int maxTex = Caps().maxTextureSize;
	if( w > maxTex || h > maxTex )
	{
		gEngfuncs.COM_FreeFile( raw );
		CSZ_LogWarn( "panorama", "GL_MAX_TEXTURE_SIZE %d < panorama %dx%d -- backdrop disabled (moon glow still emits)", maxTex, w, h );
		return;
	}

	const unsigned char *p   = (const unsigned char *)( raw + 20 );
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
		if( p + bytes > end ) { ok = false; break; }
		glTexImage2D( GL_TEXTURE_2D, lv, GL_RGB8, lw, lh, 0, GL_RGB, GL_UNSIGNED_BYTE, p );
		p += bytes;
		if( lw > 1 ) lw /= 2;
		if( lh > 1 ) lh /= 2;
	}
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );        // longitude seam wraps
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE ); // poles clamp
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, nLev - 1 );

	// POLE-PINCH FIX (anisotropic filtering) -- removes the radial "starburst" the USER flagged.
	// The equirect lat-long mapping (u = l/2pi+0.5, v = 0.5 - b/pi) is SINGULAR at the galactic
	// poles: a screen pixel near a pole spans a huge LONGITUDE (u) range but a tiny LATITUDE (v)
	// range -- an extremely anisotropic texture footprint. Plain isotropic trilinear must serve
	// both axes with ONE LOD; it picks the divergent azimuthal LOD and smears the surrounding
	// stars into RADIAL streaks emanating from the pole. Anisotropic filtering instead takes
	// several taps along the elongated (azimuthal) axis at the SHARP minor-axis LOD, dissolving
	// the smear while leaving mid-latitude (near-isotropic) sampling untouched -- the band/core/
	// arms are bit-identical. textureGrad's analytic gradients already supply the correct
	// footprint; AF just consumes it. One-shot, gracefully skipped when the extension is absent
	// (3.3-core-optional, core since 4.6): then the baked near-pole azimuthal band-limit in
	// bake/bake_panorama.py is the sole mitigation. Set via glTexParameteri / queried via
	// glGetIntegerv (the only TexParameter/Get entry points the loader binds; the int value is
	// validly converted to the float GL_TEXTURE_MAX_ANISOTROPY parameter).
	GLint maxAniso = 0, aniso = 0;
	while( glGetError() != GL_NO_ERROR ) { }   // drain so the probe's own enum error cannot leak out
	glGetIntegerv( GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso );
	bool haveAniso = ( glGetError() == GL_NO_ERROR ) && ( maxAniso > 1 );
	if( haveAniso )
	{
		aniso = ( maxAniso < 16 ) ? maxAniso : 16;
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso );
	}
	while( glGetError() != GL_NO_ERROR ) { }   // belt: start the per-frame glGetError users clean
	CSZ_LogInfo( "panorama", "anisotropic filtering %s (max %d, set %dx) -- pole-pinch fix",
		haveAniso ? "ENABLED" : "UNAVAILABLE; relying on baked pole band-limit only", maxAniso, aniso );

	SkyComposeRestoreTmus();
	gEngfuncs.COM_FreeFile( raw );

	if( ok )
	{
		s_pano.texName = tex;
		CSZ_LogInfo( "panorama", "panorama raw texture %dx%d (%d mips) uploaded (gl name %u)", w, h, nLev, (unsigned)tex );
	}
	else
	{
		glDeleteTextures( 1, &tex );
		CSZ_LogWarn( "panorama", "mw_panorama.bin truncated -- backdrop disabled" );
	}
}

bool EnsureCreated()
{
	if( s_pano.gpuGeneration != GpuGeneration() )
	{
		ForgetGpu();                       // foreign generation: forget, never glDelete
		s_pano.gpuGeneration = GpuGeneration();
	}
	if( s_pano.created )
		return true;
	if( s_pano.failedThisGen )
		return false;

	if( !BuildProgram( "csz_panorama", kPanoramaVs, kPanoramaFs, false, s_pano.prog ) )
	{
		s_pano.failedThisGen = true;
		CSZ_LogError( "panorama", "shader build failed; panorama disabled this generation" );
		return false;
	}

	s_pano.uCamFwd        = UniformLoc( s_pano.prog, "u_camFwd" );
	s_pano.uCamRight      = UniformLoc( s_pano.prog, "u_camRight" );
	s_pano.uCamUp         = UniformLoc( s_pano.prog, "u_camUp" );
	s_pano.uPanorama      = UniformLoc( s_pano.prog, "u_panorama" );
	s_pano.uPanoValid     = UniformLoc( s_pano.prog, "u_panoValid" );
	s_pano.uPanoIntensity = UniformLoc( s_pano.prog, "u_panoIntensity" );
	s_pano.uPanoLonOffset = UniformLoc( s_pano.prog, "u_panoLonOffset" );
	s_pano.uPanoSat       = UniformLoc( s_pano.prog, "u_panoSat" );
	s_pano.uPanoTint      = UniformLoc( s_pano.prog, "u_panoTint" );
	s_pano.uNight         = UniformLoc( s_pano.prog, "u_nightFactor" );
	s_pano.uNightMoon     = UniformLoc( s_pano.prog, "u_nightMoon" );
	s_pano.uGalPole       = UniformLoc( s_pano.prog, "u_galPole" );
	s_pano.uGalCenter     = UniformLoc( s_pano.prog, "u_galCenter" );
	s_pano.uGalY          = UniformLoc( s_pano.prog, "u_galY" );
	s_pano.uMoonDir       = UniformLoc( s_pano.prog, "u_moonDir" );
	s_pano.uMoonAngR      = UniformLoc( s_pano.prog, "u_moonAngR" );
	s_pano.uMoonSoft      = UniformLoc( s_pano.prog, "u_moonSoft" );
	s_pano.uMoonGlowL     = UniformLoc( s_pano.prog, "u_moonGlowL" );
	s_pano.uMoonGlowCoef  = UniformLoc( s_pano.prog, "u_moonGlowCoef" );
	s_pano.uMoonGlowMax   = UniformLoc( s_pano.prog, "u_moonGlowMax" );
	s_pano.uMoonGlowEmit  = UniformLoc( s_pano.prog, "u_moonGlowEmit" );
	s_pano.uMoonGlowWarm  = UniformLoc( s_pano.prog, "u_moonGlowWarm" );
	s_pano.uMoonGlowCool  = UniformLoc( s_pano.prog, "u_moonGlowCool" );

	glGenVertexArrays( 1, &s_pano.panoVao );

	s_pano.created = true;
	CSZ_LogDev( "panorama", "panorama pass created, gpu gen %d", s_pano.gpuGeneration );
	return true;
}

}  // anonymous namespace

// =============================================================================
// Boot-time cvar registration -- called from Renderer::OnHudInit alongside
// StarsRegisterCvars so the csz_pano* cvars exist before the first rendered frame.
// =============================================================================
void PanoramaRegisterCvars()
{
	RegisterCvars();
}

// =============================================================================
// PUBLIC: generation-safe shutdown -- called from Renderer::Shutdown while the
// owning GL context is still current.
// =============================================================================
void PanoramaShutdown()
{
	if( s_pano.created && s_pano.gpuGeneration == GpuGeneration() )
		DestroyGpuSameContext();
	else
		ForgetGpu();
}

// =============================================================================
// ABI SEAM: slot 10.5 additive backdrop into the bound HDR scene FBO, BEFORE the
// live twinkle stars. Night-gated on the SAME NightFactorFromSunElev curve as the
// stars (day/dawn/dusk unaffected). Emits the relocated moon background glow.
// =============================================================================
void PanoramaContribute( const ViewSetup &view )
{
	RegisterCvars();  // defensive: no-op after PanoramaRegisterCvars() at boot

	// csz_pano is the BACKDROP master toggle ONLY -- it must NOT gate the moon glow (codex
	// fix #7: the moon halo is independent of panorama availability). When it is 0 we still
	// run the pass so the relocated MoonSkyGlow emits; only the sampled backdrop is muted
	// (panoOn -> u_panoValid 0, and we skip the texture load so an off backdrop costs no VRAM).
	float panoOn = ( ReadCvar( s_cvarPano, 1.0f ) != 0.0f ) ? 1.0f : 0.0f;

	// Two INDEPENDENT night gates from the live sun elevation (honors csz_sky_phase):
	//  - nightBackdrop (CHANNEL 2): the baked field+MW backdrop fades in LATER, -9 -> -16
	//    deg (astronomical twilight -> night), so the dense field + Milky Way only emerge
	//    once the sky is genuinely dark -- AFTER the live bright stars (csz_stars, -2 -> -8).
	//  - nightMoon: the relocated MoonSkyGlow keeps its ORIGINAL 0 -> -7 curve so the moon's
	//    outer halo timing is UNCHANGED by the star/MW retiming (codex fix #7: the moon glow
	//    is independent of the backdrop). Decoupled into u_nightMoon so the later backdrop
	//    fade does not delay or suppress the moon halo in twilight.
	float phase = g_sky.ComputePhase();
	float sun[3];
	skymath::SunDir( phase, sun );
	float sz = ( sun[2] < -1.0f ) ? -1.0f : ( sun[2] > 1.0f ? 1.0f : sun[2] );
	float sunElevDeg = asinf( sz ) / kDegToRad;
	float nightBackdrop = starsmath::PanoramaNightFactor( sunElevDeg );
	float nightMoon     = starsmath::NightFactorFromSunElev( sunElevDeg );
	float nightFactor   = nightBackdrop;   // backdrop radiance gate (uploaded to u_nightFactor)
	if( nightBackdrop < 0.01f && nightMoon < 0.01f )
		return;   // daylight: nothing to add (backdrop + moon glow both fully faded out)

	if( !EnsureCreated() )
		return;
	if( panoOn != 0.0f )
		EnsureTexture();   // one-shot lazy load (texName stays 0 if absent / backdrop off)

	// Premultiplied additive into the HDR FBO; depth OFF (world overwrites by depth).
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendAddPremul );
	SetCull( false );

	// Camera basis (Quake Z-up); right/up pre-scaled by the half-FOV tangents so the
	// VS ray = fwd + right*ndc.x + up*ndc.y (IDENTICAL convention to the deleted MW pass).
	float fwd[3], right[3], up[3];
	AngleVectors( view.angles, fwd, right, up );
	float tanX = tanf( view.fovX * kDegToRad * 0.5f );
	float tanY = tanf( view.fovY * kDegToRad * 0.5f );
	float rightS[3] = { right[0] * tanX, right[1] * tanX, right[2] * tanX };
	float upS[3]    = { up[0] * tanY,    up[1] * tanY,    up[2] * tanY };

	// Galactic basis (orthonormalised from the frozen RAW constants -- the SAME frame
	// the baked catalog + the live star gold-core path use).
	float galPole[3], galCenter[3], galY[3];
	GalacticBasis( galPole, galCenter, galY );

	// Moon BODY occluder + Task D wash base luminance (same source the stars use).
	float moonOccDir[3] = { 0.0f, 0.0f, 1.0f };
	float moonOccAngR = -1.0f, moonOccSoft = 0.001f, moonGlowL = 0.0f;
	if( !MoonBodyOccluder( view, moonOccDir, moonOccAngR, moonOccSoft, moonGlowL ) )
	{
		moonOccDir[0] = 0.0f; moonOccDir[1] = 0.0f; moonOccDir[2] = 1.0f;
		moonOccAngR = -1.0f; moonOccSoft = 0.001f; moonGlowL = 0.0f;
	}
	// Moon wash parameters -- identical to the deleted MW pass (single-ownership f(rho)).
	moonGlowL *= ReadCvar( s_cvMoonWash, 1.0f );
	float moonGlowCoef[4] = {
		ReadCvar( s_cvMoonWashKa,   5.6f   ),
		ReadCvar( s_cvMoonWashKm,   0.42f  ),   // ring fix: fuller mid shoulder (was 0.3)
		ReadCvar( s_cvMoonWashKr,   0.377f ),
		clampf( ReadCvar( s_cvMoonWashRho0, 2.6f ), 0.05f, 30.0f )   // ring fix: softer aureole (was 1.5)
	};
	float moonGlowMax  = ReadCvar( s_cvMoonWashMax, 8.0f );
	float moonGlowEmit = ReadCvar( s_cvMoonWashEmit, 0.07f );
	const float moonGlowWarm[3] = { 1.00f, 0.96f, 0.90f };   // moon disc + tight aureole
	const float moonGlowCool[3] = { 0.70f, 0.80f, 1.00f };   // far Rayleigh pedestal (blue)

	UseProgram( s_pano.prog.program );
	BindVao( s_pano.panoVao );

	// Bind the panorama on sky unit 0 (abs unit GL_TEXTURE4); the sampler references it.
	// u_panoValid gates the sampled backdrop: 1 only when the texture loaded AND the master
	// csz_pano is on; the moon glow below ignores this (codex fix #7).
	bool haveTex = ( s_pano.texName != 0 );
	float panoValid = ( haveTex && panoOn != 0.0f ) ? 1.0f : 0.0f;
	if( haveTex )
		SkyComposeBindTex( 0, GL_TEXTURE_2D, s_pano.texName );
	// Runtime grade: post-saturation scalar + a warm/cool tint vec3 derived from csz_pano_warm
	// (positive = warmer: lift R, drop B; negative = cooler). Clamped so it stays a gentle nudge.
	float warm = clampf( ReadCvar( s_cvarPanoWarm, 0.0f ), -1.0f, 1.0f );
	float panoTint[3] = { 1.0f + 0.15f * warm, 1.0f, 1.0f - 0.15f * warm };
	if( s_pano.uPanorama >= 0 )      glUniform1i( s_pano.uPanorama, kSkyTmuBase + 0 );
	if( s_pano.uPanoValid >= 0 )     glUniform1f( s_pano.uPanoValid, panoValid );
	if( s_pano.uPanoIntensity >= 0 ) glUniform1f( s_pano.uPanoIntensity, ReadCvar( s_cvarPanoInt, 1.0f ) );
	if( s_pano.uPanoLonOffset >= 0 ) glUniform1f( s_pano.uPanoLonOffset, ReadCvar( s_cvarPanoLon, 0.0f ) );
	if( s_pano.uPanoSat >= 0 )       glUniform1f( s_pano.uPanoSat, clampf( ReadCvar( s_cvarPanoSat, 1.0f ), 0.0f, 3.0f ) );
	if( s_pano.uPanoTint >= 0 )      glUniform3fv( s_pano.uPanoTint, 1, panoTint );
	if( s_pano.uNight >= 0 )         glUniform1f( s_pano.uNight, nightFactor );   // CHANNEL 2 backdrop gate (-9 -> -16)
	if( s_pano.uNightMoon >= 0 )     glUniform1f( s_pano.uNightMoon, nightMoon ); // moon-glow gate (0 -> -7, UNCHANGED)
	if( s_pano.uCamFwd >= 0 )        glUniform3fv( s_pano.uCamFwd, 1, fwd );
	if( s_pano.uCamRight >= 0 )      glUniform3fv( s_pano.uCamRight, 1, rightS );
	if( s_pano.uCamUp >= 0 )         glUniform3fv( s_pano.uCamUp, 1, upS );
	if( s_pano.uGalPole >= 0 )       glUniform3fv( s_pano.uGalPole, 1, galPole );
	if( s_pano.uGalCenter >= 0 )     glUniform3fv( s_pano.uGalCenter, 1, galCenter );
	if( s_pano.uGalY >= 0 )          glUniform3fv( s_pano.uGalY, 1, galY );
	if( s_pano.uMoonDir >= 0 )       glUniform3fv( s_pano.uMoonDir, 1, moonOccDir );
	if( s_pano.uMoonAngR >= 0 )      glUniform1f( s_pano.uMoonAngR, moonOccAngR );
	if( s_pano.uMoonSoft >= 0 )      glUniform1f( s_pano.uMoonSoft, moonOccSoft );
	if( s_pano.uMoonGlowL >= 0 )     glUniform1f( s_pano.uMoonGlowL, moonGlowL );
	if( s_pano.uMoonGlowCoef >= 0 )  glUniform4fv( s_pano.uMoonGlowCoef, 1, moonGlowCoef );
	if( s_pano.uMoonGlowMax >= 0 )   glUniform1f( s_pano.uMoonGlowMax, moonGlowMax );
	if( s_pano.uMoonGlowEmit >= 0 )  glUniform1f( s_pano.uMoonGlowEmit, moonGlowEmit );
	if( s_pano.uMoonGlowWarm >= 0 )  glUniform3fv( s_pano.uMoonGlowWarm, 1, moonGlowWarm );
	if( s_pano.uMoonGlowCool >= 0 )  glUniform3fv( s_pano.uMoonGlowCool, 1, moonGlowCool );

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	// Restore the sky TMUs (whether or not we bound) before returning to engine GL_Bind,
	// then restore the takeover baseline so the world opaque pass starts clean.
	SkyComposeRestoreTmus();
	BindVao( 0 );
	UseProgram( 0 );
	SetBlend( kBlendNone );
	SetDepthTest( true );
	SetDepthWrite( true );
}

}  // namespace csz
