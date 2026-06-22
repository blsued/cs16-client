/*
 * csz_clouds.cpp -- CSOZ renderer: drifting local night cloud dome (sky-base D L3a)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original work written for
 * CSOZ; no code in this file is copied or translated from PrimeXT, Paranoia,
 * Trinity, retail/leaked sources, or any other license-tainted source (see csoz
 * docs/provenance.md, section 6). The cloud FBM, Beer-Lambert transmittance and
 * Henyey-Greenstein phase are published physical/empirical FORMULAS (facts);
 * re-typed clean-room. Clean-room implementation; implemented by an agent that
 * has not read any license-tainted source.
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
// L3a render order: inserted in csz_renderer.cpp between StarsContribute(view)
// and SunMoonContribute(view) so the clouds occlude the panorama backdrop AND
// the live stars (alpha-over), while the moon disc still draws crisply on top.
#include "csz_clouds.h"
#include "csz_sky.h"          // g_sky.ComputePhase() (honors csz_sky_phase)
#include "csz_sky_math.h"     // skymath::SunDir / MoonDir / SunElevDeg
#include "csz_stars_math.h"   // starsmath::NightFactorFromSunElev (SHARED night curve)
#include "csz_sky_compose.h"  // kSkyTmuBase, SkyComposeBindTex / RestoreTmus
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
#include <vector>

namespace csz
{

#include "csz_clouds_shaders.inl"

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

float ReadCvar( cvar_t *cv, float fallback ) { return ( cv != NULL ) ? cv->value : fallback; }
float clampf( float v, float lo, float hi ) { return ( v < lo ) ? lo : ( v > hi ) ? hi : v; }

// --- cvars (registered eagerly at HUD init; read live each frame) ----------------
bool    s_cvarsReady = false;
cvar_t *s_cvarClouds;      // csz_clouds       "1": master on/off (0 = clear sky = current/AB-off)
cvar_t *s_cvarCover;       // csz_cloud_cover  "0.45": 0..1 cloud amount
cvar_t *s_cvarDump;        // csz_clouds_dump  "0": throttled scalar dump for self-test

// --- GPU resources (generation-keyed; forget on a foreign context) ---------------
const int kNoiseSize = 256;   // tiling 2D noise edge (seamless, wrap-repeat)

struct CloudsGpu
{
	GLuint vao;            // empty VAO for the attrib-less fullscreen triangle
	GLuint noiseTex;       // raw GL tiling-noise texture (bound only via SkyComposeBindTex)

	ShaderProgram prog;
	int uCamFwd, uCamRight, uCamUp;
	int uNoise, uMoonDir, uMoonColor, uMoonLitFrac, uNight, uCover, uTime;

	int  gpuGeneration;
	bool created;
	bool failedThisGen;
};
CloudsGpu s_gpu;

void RegisterCvarsImpl()
{
	if( s_cvarsReady )
		return;
	// csz_clouds default "1" = the USER-desired light drifting clouds; "0" = clear
	// sky = current look = the A/B-off contract (Contribute early-outs).
	s_cvarClouds = gEngfuncs.pfnRegisterVariable( "csz_clouds",      "1",    FCVAR_CLIENTDLL );
	// csz_cloud_cover 0..1 cloud amount; default 0.45 keeps the Milky Way mostly visible.
	s_cvarCover  = gEngfuncs.pfnRegisterVariable( "csz_cloud_cover", "0.45", FCVAR_CLIENTDLL );
	// Disposable observability: when != 0, UpdateScalars dumps the 3 owned scalars
	// + cover once/sec so self-test can confirm sane values. Ships harmlessly.
	s_cvarDump   = gEngfuncs.pfnRegisterVariable( "csz_clouds_dump", "0",    FCVAR_CLIENTDLL );
	s_cvarsReady = true;
	CSZ_LogDev( "clouds", "cvars registered (csz_clouds/csz_cloud_cover/csz_clouds_dump)" );
}

void ForgetGpu()
{
	memset( &s_gpu, 0, sizeof( s_gpu ) );
}

void DestroyGpuSameContext()
{
	if( s_gpu.noiseTex )      glDeleteTextures( 1, &s_gpu.noiseTex );
	if( s_gpu.vao )           glDeleteVertexArrays( 1, &s_gpu.vao );
	if( s_gpu.prog.program )  DestroyProgram( s_gpu.prog );
	ForgetGpu();
}

// 2D integer hash -> [0,1). Public-domain integer-hash style.
float Hash2( int x, int y )
{
	unsigned int h = (unsigned int)( x * 374761393 + y * 668265263 );
	h = ( h ^ ( h >> 13 ) ) * 1274126177u;
	h = h ^ ( h >> 16 );
	return (float)h * ( 1.0f / 4294967296.0f );
}

// Value noise on a periodic lattice (period = wrap) so the baked texture tiles
// seamlessly when sampled with GL_REPEAT.
float ValueNoiseTiling( float fx, float fy, int wrap )
{
	int x0 = (int)floorf( fx ), y0 = (int)floorf( fy );
	float tx = fx - (float)x0, ty = fy - (float)y0;
	int xi0 = ( ( x0 % wrap ) + wrap ) % wrap;
	int yi0 = ( ( y0 % wrap ) + wrap ) % wrap;
	int xi1 = ( xi0 + 1 ) % wrap;
	int yi1 = ( yi0 + 1 ) % wrap;
	float sx = tx * tx * ( 3.0f - 2.0f * tx );
	float sy = ty * ty * ( 3.0f - 2.0f * ty );
	float n00 = Hash2( xi0, yi0 );
	float n10 = Hash2( xi1, yi0 );
	float n01 = Hash2( xi0, yi1 );
	float n11 = Hash2( xi1, yi1 );
	float nx0 = n00 + ( n10 - n00 ) * sx;
	float nx1 = n01 + ( n11 - n01 ) * sx;
	return nx0 + ( nx1 - nx0 ) * sy;
}

// Multi-octave tiling FBM in [0,1] (each octave's lattice period scales with the
// frequency so the whole stack stays seamless).
float FbmTiling( float fx, float fy, int baseWrap, int octaves )
{
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	int wrap = baseWrap;
	for( int o = 0; o < octaves; o++ )
	{
		sum  += amp * ValueNoiseTiling( fx * (float)( 1 << o ), fy * (float)( 1 << o ), wrap );
		norm += amp;
		amp  *= 0.5f;
		wrap *= 2;
	}
	return sum / norm;
}

// Build the CPU pre-baked tiling noise texture and upload it on sky unit 0.
// R = detail FBM seed (higher freq base), G = coverage FBM seed (lower freq).
void BuildNoiseTexture()
{
	const int N = kNoiseSize;
	std::vector<unsigned char> px( (size_t)N * N * 2 );   // RG8
	const int baseWrapR = 8;    // detail base lattice cells across the tile
	const int baseWrapG = 4;    // coverage base lattice cells across the tile (lower freq)
	for( int y = 0; y < N; y++ )
	{
		for( int x = 0; x < N; x++ )
		{
			float u = (float)x / (float)N;
			float v = (float)y / (float)N;
			float r = FbmTiling( u * (float)baseWrapR, v * (float)baseWrapR, baseWrapR, 4 );
			float g = FbmTiling( u * (float)baseWrapG, v * (float)baseWrapG, baseWrapG, 3 );
			size_t idx = ( (size_t)y * N + x ) * 2;
			px[idx + 0] = (unsigned char)clampf( r * 255.0f, 0.0f, 255.0f );
			px[idx + 1] = (unsigned char)clampf( g * 255.0f, 0.0f, 255.0f );
		}
	}

	GLuint tex = 0;
	glGenTextures( 1, &tex );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, tex );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RG8, N, N, 0, GL_RG, GL_UNSIGNED_BYTE, &px[0] );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
	SkyComposeRestoreTmus();

	s_gpu.noiseTex = tex;
	CSZ_LogInfo( "clouds", "tiling noise %dx%d RG8 baked + uploaded (gl name %u)", N, N, (unsigned)tex );
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

	if( !BuildProgram( "csz_clouds", kCloudsVs, kCloudsFs, false, s_gpu.prog ) )
	{
		s_gpu.failedThisGen = true;
		CSZ_LogError( "clouds", "shader build failed; clouds disabled this generation" );
		return false;
	}

	s_gpu.uCamFwd      = UniformLoc( s_gpu.prog, "u_camFwd" );
	s_gpu.uCamRight    = UniformLoc( s_gpu.prog, "u_camRight" );
	s_gpu.uCamUp       = UniformLoc( s_gpu.prog, "u_camUp" );
	s_gpu.uNoise       = UniformLoc( s_gpu.prog, "u_noise" );
	s_gpu.uMoonDir     = UniformLoc( s_gpu.prog, "u_moonDir" );
	s_gpu.uMoonColor   = UniformLoc( s_gpu.prog, "u_moonColor" );
	s_gpu.uMoonLitFrac = UniformLoc( s_gpu.prog, "u_moonLitFrac" );
	s_gpu.uNight       = UniformLoc( s_gpu.prog, "u_night" );
	s_gpu.uCover       = UniformLoc( s_gpu.prog, "u_cover" );
	s_gpu.uTime        = UniformLoc( s_gpu.prog, "u_time" );

	glGenVertexArrays( 1, &s_gpu.vao );

	// For the record (L3a research note): query GL_MAX_3D_TEXTURE_SIZE -- that enum
	// is valid even though glTexImage3D is NOT in the loaded GL func table -- and log
	// that the 3D froxel path is deferred in favor of the 2D tiling-noise fallback.
	{
		GLint max3d = 0;
		while( glGetError() != GL_NO_ERROR ) { }
		glGetIntegerv( GL_MAX_3D_TEXTURE_SIZE, &max3d );
		while( glGetError() != GL_NO_ERROR ) { }
		CSZ_LogInfo( "clouds", "GL_MAX_3D_TEXTURE_SIZE=%d -- 3D path deferred (glTexImage3D not in loaded func table) -> using 2D tiling noise", (int)max3d );
	}

	BuildNoiseTexture();

	s_gpu.created = true;
	CSZ_LogDev( "clouds", "cloud pass created, gpu gen %d", s_gpu.gpuGeneration );
	return true;
}

// Shared night gate: identical curve the bright stars use (sun elevation ->
// 0..1), so day/dawn/dusk stay clean exactly like the stars/panorama passes.
float CloudNightFactor( float phase )
{
	float sun[3];
	skymath::SunDir( phase, sun );
	float sz = ( sun[2] < -1.0f ) ? -1.0f : ( sun[2] > 1.0f ? 1.0f : sun[2] );
	float sunElevDeg = asinf( sz ) / kDegToRad;
	return starsmath::NightFactorFromSunElev( sunElevDeg );
}

}  // anonymous namespace

CloudRenderer g_clouds;

// =============================================================================
// Boot-time cvar registration -- called alongside g_sky.RegisterDevCvars().
// =============================================================================
void CloudRenderer::RegisterCvars()
{
	RegisterCvarsImpl();
}

void CloudRenderer::EnsureBuilt()
{
	EnsureCreated();
}

void CloudRenderer::Shutdown()
{
	if( s_gpu.created && s_gpu.gpuGeneration == GpuGeneration() )
		DestroyGpuSameContext();
	else
		ForgetGpu();
}

// =============================================================================
// RENDER: alpha-over cloud pass into the HDR scene FBO, drawn AFTER the panorama
// + stars and BEFORE the moon disc so clouds occlude the Milky Way / stars.
// csz_clouds 0 early-outs -> sky identical to current (A/B-off contract).
// =============================================================================
void CloudRenderer::Contribute( const ViewSetup &view )
{
	RegisterCvarsImpl();  // defensive: no-op after boot registration

	// A/B-off contract: clear sky = current look. MUST hold exactly.
	if( ReadCvar( s_cvarClouds, 1.0f ) < 0.5f )
		return;

	// Night gate (same curve as the bright stars): clouds invisible by day.
	float phase = g_sky.ComputePhase();
	float night = CloudNightFactor( phase );
	if( night < 0.01f )
		return;

	if( !EnsureCreated() )
		return;
	if( s_gpu.noiseTex == 0 )
		return;

	// Body directions from phase (moon = exact antipode of the sun, like the sky).
	float sunDir[3], moonDir[3];
	skymath::SunDir( phase, sunDir );
	moonDir[0] = -sunDir[0];
	moonDir[1] = -sunDir[1];
	moonDir[2] = -sunDir[2];
	const float moonColor[3] = { 0.90f, 0.93f, 1.00f };   // moonlit-cloud tint (matches DrawSky)
	// MoonLitFraction lives in csz_sky.cpp (file-local); re-derive from csz_moon_phase
	// here so the cloud glow tracks the same visible phase (single cvar source).
	float moonLitFrac;
	{
		cvar_t *mp = gEngfuncs.pfnGetCvarPointer( "csz_moon_phase" );
		if( mp == NULL || mp->value < 0.0f )
			moonLitFrac = 1.0f;                              // legacy always-full moon
		else
		{
			float p = clampf( mp->value, 0.0f, 1.0f );
			float a = ( 1.0f - p ) * 3.14159265358979323846f;
			moonLitFrac = 0.5f * ( 1.0f + cosf( a ) );
		}
	}

	float cover = clampf( ReadCvar( s_cvarCover, 0.45f ), 0.0f, 1.0f );

	// Camera basis (Quake Z-up); right/up pre-scaled by the half-FOV tangents so
	// the VS ray = fwd + right*ndc.x + up*ndc.y (IDENTICAL to sky/panorama/stars).
	float fwd[3], right[3], up[3];
	AngleVectors( view.angles, fwd, right, up );
	float tanX = tanf( view.fovX * kDegToRad * 0.5f );
	float tanY = tanf( view.fovY * kDegToRad * 0.5f );
	float rightS[3] = { right[0] * tanX, right[1] * tanX, right[2] * tanX };
	float upS[3]    = { up[0] * tanY,    up[1] * tanY,    up[2] * tanY };

	// Alpha-over blend into the HDR FBO; depth OFF, write OFF, no cull (mirror the
	// sky pass GL-state save/restore). cloudRadiance is linear HDR (pre-tonemap).
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendAlpha );
	SetCull( false );

	UseProgram( s_gpu.prog.program );
	BindVao( s_gpu.vao );

	// Bind the tiling noise on sky unit 0 (abs unit GL_TEXTURE4); sampler refs it.
	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_gpu.noiseTex );

	float t = fmodf( ClientTime(), 3600.0f );   // bounded for FP precision in the shader
	if( s_gpu.uNoise >= 0 )       glUniform1i( s_gpu.uNoise, kSkyTmuBase + 0 );
	if( s_gpu.uCamFwd >= 0 )      glUniform3fv( s_gpu.uCamFwd, 1, fwd );
	if( s_gpu.uCamRight >= 0 )    glUniform3fv( s_gpu.uCamRight, 1, rightS );
	if( s_gpu.uCamUp >= 0 )       glUniform3fv( s_gpu.uCamUp, 1, upS );
	if( s_gpu.uMoonDir >= 0 )     glUniform3fv( s_gpu.uMoonDir, 1, moonDir );
	if( s_gpu.uMoonColor >= 0 )   glUniform3fv( s_gpu.uMoonColor, 1, moonColor );
	if( s_gpu.uMoonLitFrac >= 0 ) glUniform1f( s_gpu.uMoonLitFrac, moonLitFrac );
	if( s_gpu.uNight >= 0 )       glUniform1f( s_gpu.uNight, night );
	if( s_gpu.uCover >= 0 )       glUniform1f( s_gpu.uCover, cover );
	if( s_gpu.uTime >= 0 )        glUniform1f( s_gpu.uTime, t );

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	// Restore the sky TMUs before returning to engine GL_Bind, then the takeover
	// baseline so the world opaque pass starts clean.
	SkyComposeRestoreTmus();
	BindVao( 0 );
	UseProgram( 0 );
	SetBlend( kBlendNone );
	SetDepthTest( true );
	SetDepthWrite( true );
}

// =============================================================================
// 3 OWNED OUTPUT SCALARS (CPU). Coarse GLOBAL average-cloud-state hints for L3b/
// L4 (per-pixel cloud lives in the shader). L3a ONLY computes & stores these; it
// MUST NOT read them back to dim the map/world/moon. Do not touch PublishLighting
// darkening math (cloudDim stays as-is).
// =============================================================================
void CloudRenderer::UpdateScalars( AmbienceParams &amb, float phase )
{
	RegisterCvarsImpl();

	float cloudsOn = ( ReadCvar( s_cvarClouds, 1.0f ) >= 0.5f ) ? 1.0f : 0.0f;
	float cover    = clampf( ReadCvar( s_cvarCover, 0.45f ), 0.0f, 1.0f );
	float night    = CloudNightFactor( phase );

	// Effective average cloud state: amount * night gate, zeroed when clouds off.
	float c = cloudsOn * cover * night;

	const float k_d = 2.0f;   // moonlight direct-transmission falloff
	const float k_a = 0.2f;   // sky-ambient gentle dimming

	float directTransmittance = expf( -k_d * c );
	float skyAmbientScale     = 1.0f - k_a * c;
	if( skyAmbientScale < 0.6f ) skyAmbientScale = 0.6f;          // ambient dims only gently
	float shaftMask           = 1.0f - skymath::Smooth01( 0.4f, 0.9f, c );  // gaps->1, heavy cover->0

	// L3a: computed for L3b/L4 consumption; NOT applied here.
	amb.directTransmittance = directTransmittance;
	amb.skyAmbientScale     = skyAmbientScale;
	amb.shaftMask           = shaftMask;

	// Throttled scalar dump (csz_clouds_dump != 0) so self-test can confirm sane
	// values. Once/sec off ClientTime, never per-frame.
	if( s_cvarDump != NULL && s_cvarDump->value != 0.0f )
	{
		static float s_next = 0.0f;
		float now = ClientTime();
		if( now >= s_next )
		{
			s_next = now + 1.0f;
			CSZ_LogInfo( "clouds", "scalars: cover=%.3f night=%.3f c=%.3f | directT=%.3f skyAmb=%.3f shaft=%.3f",
				cover, night, c, directTransmittance, skyAmbientScale, shaftMask );
		}
	}
}

}  // namespace csz
