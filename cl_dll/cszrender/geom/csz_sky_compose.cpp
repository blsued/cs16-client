/*
 * csz_sky_compose.cpp -- CSOZ renderer: HDR scene target + resolve + sky ABI impl
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
#include "csz_sky_compose.h"
#include "../core/csz_engine.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_log.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"

#include <string.h>

namespace csz
{

#include "csz_sky_compose_shaders.inl"

namespace
{

// --- cvars (registered in SkyComposeRegisterCvars, read live each frame) ------
cvar_t *s_cvarHdr;        // csz_hdr        default "1": 1 = HDR FBO + resolve, 0 = straight-to-backbuffer baseline
cvar_t *s_cvarExposure;   // csz_exposure   default "1.0"
cvar_t *s_cvarTonemap;    // csz_tonemap    default "0" (identity); 1 = ACES filmic
cvar_t *s_cvarEncode;     // csz_encode     default "0" (no OETF / display passthrough); 1 = sRGB OETF
cvar_t *s_cvarDither;     // csz_dither     default "0" (byte-clean A/B); 1 = TPDF dither
cvar_t *s_cvarHlRolloff;  // csz_highlight_rolloff default "1": identity-path overbright shoulder strength (0 = legacy/off A/B)
cvar_t *s_cvarHlKnee;     // csz_highlight_knee   default "0.95": shoulder onset (maxRGB <= knee unchanged per-pixel)
cvar_t *s_cvarTiming;     // csz_hdr_timing default "0"; 1 = log the GPU timer ms (Dev level)
cvar_t *s_cvarPerfDump;   // csz_perf_dump  default "0" (L0 observability); 1 = orchestrator emits the [csz_perf] line AND forces the in-scene GPU timer query (passive: no draw change)
// S4 night grade (REWORK-SPEC §S4). All read live each frame; the whole grade is gated by
// the published nightness so DAY is bit-identical (nightness 0 -> shader skips the block).
cvar_t *s_cvarNightExposure;  // csz_night_exposure   default "1.0" (no darkening; <1 = darker night mood)
cvar_t *s_cvarNightToe;       // csz_night_toe        default "0.35" (shadow-lift strength for readability)
cvar_t *s_cvarToeGamma;       // csz_night_toe_gamma  default "1.5" (>1 lifts shadows/midtones)
cvar_t *s_cvarPurkinje;       // csz_purkinje         default "1.0" (scotopic desaturation + cool shift)
cvar_t *s_cvarPurkinjeKnee;   // csz_purkinje_knee    default "0.35" (luma above which chroma is preserved)

// S4: nightness published by the renderer each frame (after PublishLighting), consumed by the
// resolve so the night grade engages on the phase curve. 0 (day) -> the shader block is skipped.
float s_publishedNightness = 0.0f;

// --- HDR scene target (red-team fix #9: real color draw buffer; fix #10: depth
//     renderbuffer, no stencil in the CSZ takeover scene target) ---------------
struct HdrTarget
{
	GLuint fbo;
	GLuint colorTex;   // RGBA16F, LINEAR/CLAMP (filterable for future use; resolve uses texelFetch)
	GLuint depthTex;   // GL_DEPTH_COMPONENT24 TEXTURE (sampleable; raw depth, compare-mode NONE) -- fog Step 1
	int    width, height;
	int    gpuGeneration;
	bool   valid;
	bool   failedThisGen;   // creation failed on this generation: do not spam-retry, fall back to baseline
};

HdrTarget s_hdr;

// --- resolve program ----------------------------------------------------------
struct ResolveGpu
{
	ShaderProgram program;
	GLuint vao;        // empty VAO for the VAO-less fullscreen triangle
	int    gpuGeneration;
	bool   built;

	int uHdr, uViewOrigin, uExposure, uTonemap, uEncode, uDither;
	int uHlRolloff, uHlKnee;
	int uNightness, uNightExposure, uNightToe, uToeGamma, uPurkinje, uPurkinjeKnee;	// S4 night grade
};

ResolveGpu s_resolve;

// --- GPU timer ring (red-team fix #11: 3-deep + GL_QUERY_RESULT_AVAILABLE) -----
const int kQueryRing = 3;

struct GpuTimer
{
	GLuint  query[kQueryRing];
	bool    issued[kQueryRing];   // a glBeginQuery/EndQuery pair was recorded into this slot
	int     writeIndex;           // slot we begin into THIS frame
	int     gpuGeneration;
	bool    created;
	bool    inSpan;               // a Begin without its End (defensive)
	double  lastMs;               // most recent AVAILABLE result in ms, or -1
};

GpuTimer s_timer;

// --- sky TMU bookkeeping (red-team fix #1) ------------------------------------
struct SkyTmuRec
{
	bool   bound;
	GLenum target;
};

SkyTmuRec s_skyTmu[kSkyTmuCount];

// The real GL active texture unit (a GL_TEXTURE* enum) captured ONCE at the start
// of each bind/restore cycle, before our first raw glActiveTexture desyncs it. -1
// = not captured this cycle. RestoreTmus restores the real unit to this value so
// the engine's tracked glState.activeTMU stays coherent without a GL_SelectTexture
// round-trip (which raises 0x502 on the core profile -- see SkyComposeRestoreTmus).
int s_savedActiveTex = -1;

// --- atmos resources singleton (default-with-fallbacks until C2 publishes) ----
SkyAtmosResources s_atmos;
bool s_atmosInit;

void EnsureAtmosDefault()
{
	if( s_atmosInit )
		return;

	memset( &s_atmos, 0, sizeof( s_atmos ));
	s_atmos.version = 2;   // C2: extended with multiple-scattering + sky-view LUTs
	s_atmos.ready = false;

	// Default samplers (documented in the header): linear filter, clamp.
	const SkySamplerSpec lin = { GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE };
	s_atmos.transmittanceSampler = lin;
	s_atmos.horizonSampler = lin;
	s_atmos.multiScatteringSampler = lin;
	// Sky-view azimuth wraps at the sun meridian -> REPEAT on S (no seam).
	const SkySamplerSpec skyv = { GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE };
	s_atmos.skyViewSampler = skyv;

	// LINEAR-space fallbacks: no extinction, no horizon contribution (additive
	// identity) -- safe until C2 lands.
	s_atmos.fallbackTransmittance[0] = 1.0f;
	s_atmos.fallbackTransmittance[1] = 1.0f;
	s_atmos.fallbackTransmittance[2] = 1.0f;
	s_atmos.fallbackHorizonColor[0] = 0.0f;
	s_atmos.fallbackHorizonColor[1] = 0.0f;
	s_atmos.fallbackHorizonColor[2] = 0.0f;

	// C2 additive fields: neutral defaults until C2 publishes real LUTs.
	s_atmos.horizonColor[0] = 0.0f;
	s_atmos.horizonColor[1] = 0.0f;
	s_atmos.horizonColor[2] = 0.0f;
	s_atmos.sunDirection[0] = 0.0f;
	s_atmos.sunDirection[1] = 0.0f;
	s_atmos.sunDirection[2] = 1.0f;
	s_atmos.sunTransmittance[0] = 1.0f;
	s_atmos.sunTransmittance[1] = 1.0f;
	s_atmos.sunTransmittance[2] = 1.0f;

	s_atmosInit = true;
}


// Dev GL-error probe under csz_sky_glcheck (mirrors csz_renderer.cpp SkyGlCheck):
// drains + reports the error queue at a named checkpoint. Used to ATTRIBUTE a
// residual to our own resolve GL vs errors that arrived earlier -- the engine's
// own gRenderAPI.GL_Bind calls in the world/studio/lights/sprites/viewmodel passes
// that run between the renderer's sunmoon probe and its resolve probe each trip the
// same core-profile compat 0x502 (pure engine, not ours). Draining at resolve ENTRY
// lets the renderer's "compose resolve" probe show only resolve-GENERATED errors.
// Ships harmless (default off), exactly like the renderer's bisection probe.
void ComposeGlCheck( const char *tag )
{
	static cvar_t *s_cv;
	static bool s_looked;

	if( !s_looked )
	{
		s_looked = true;
		s_cv = gEngfuncs.pfnGetCvarPointer( "csz_sky_glcheck" );
	}

	if( s_cv == NULL || s_cv->value == 0.0f )
		return;

	GLenum e;
	while( ( e = glGetError()) != GL_NO_ERROR )
		CSZ_LogError( "skyglcheck", "GL error 0x%x after %s", (unsigned int)e, tag );
}

// One-shot capability log (assert FRAMEBUFFER_SRGB disabled + report formats).
void LogCapsOnce()
{
	static bool s_logged;

	if( s_logged )
		return;

	s_logged = true;

	GLboolean srgb = glIsEnabled( GL_FRAMEBUFFER_SRGB );

	if( srgb == GL_TRUE )
	{
		// We never want implicit sRGB encode under the display-space pipeline.
		glDisable( GL_FRAMEBUFFER_SRGB );
		CSZ_LogWarn( "compose", "GL_FRAMEBUFFER_SRGB was ENABLED at HDR init; forced OFF (display-space pipeline)" );
	}

	CSZ_LogInfo( "compose", "HDR scene target: color=GL_RGBA16F depth=GL_DEPTH_COMPONENT24 (sampleable texture); "
		"FRAMEBUFFER_SRGB=%s; backbuffer assumed non-sRGB RGBA8",
		( srgb == GL_TRUE ) ? "was-on-forced-off" : "off" );
}

// Forget stale GL names from a foreign context (never glDelete them).
void ForgetHdr()
{
	s_hdr.fbo = 0;
	s_hdr.colorTex = 0;
	s_hdr.depthTex = 0;
	s_hdr.width = 0;
	s_hdr.height = 0;
	s_hdr.valid = false;
	s_hdr.failedThisGen = false;
}

void DestroyHdrSameContext()
{
	// Caller guarantees same live context.
	if( s_hdr.fbo != 0 )
		glDeleteFramebuffers( 1, &s_hdr.fbo );
	if( s_hdr.colorTex != 0 )
		glDeleteTextures( 1, &s_hdr.colorTex );
	if( s_hdr.depthTex != 0 )
		glDeleteTextures( 1, &s_hdr.depthTex );

	ForgetHdr();
}

void BuildResolveProgram()
{
	if( s_resolve.built && s_resolve.gpuGeneration == GpuGeneration())
		return;

	// Stale generation: forget names (the owning context is gone).
	s_resolve.gpuGeneration = GpuGeneration();

	glGenVertexArrays( 1, &s_resolve.vao );

	// Init-time program: compile failure is FATAL (matches sky/world/studio).
	BuildProgram( "csz_hdr_resolve", kComposeVs, kComposeFs, true, s_resolve.program );

	s_resolve.uHdr        = UniformLoc( s_resolve.program, "u_hdr" );
	s_resolve.uViewOrigin = UniformLoc( s_resolve.program, "u_viewOrigin" );
	s_resolve.uExposure   = UniformLoc( s_resolve.program, "u_exposure" );
	s_resolve.uTonemap    = UniformLoc( s_resolve.program, "u_tonemap" );
	s_resolve.uEncode     = UniformLoc( s_resolve.program, "u_encode" );
	s_resolve.uDither     = UniformLoc( s_resolve.program, "u_dither" );
	s_resolve.uHlRolloff  = UniformLoc( s_resolve.program, "u_hlRolloff" );
	s_resolve.uHlKnee     = UniformLoc( s_resolve.program, "u_hlKnee" );
	s_resolve.uNightness    = UniformLoc( s_resolve.program, "u_nightness" );		// S4
	s_resolve.uNightExposure= UniformLoc( s_resolve.program, "u_nightExposure" );	// S4
	s_resolve.uNightToe     = UniformLoc( s_resolve.program, "u_nightToe" );		// S4
	s_resolve.uToeGamma     = UniformLoc( s_resolve.program, "u_toeGamma" );		// S4
	s_resolve.uPurkinje     = UniformLoc( s_resolve.program, "u_purkinje" );		// S4
	s_resolve.uPurkinjeKnee = UniformLoc( s_resolve.program, "u_purkinjeKnee" );	// S4

	s_resolve.built = true;
	CSZ_LogDev( "compose", "HDR resolve program built (gpu gen %d)", s_resolve.gpuGeneration );
}

// Ensure the HDR FBO exists at (w,h) on the live generation. Mirrors
// csz_shadowmap.cpp: generation rule + completeness check + B-class degrade.
bool EnsureHdrTarget( int w, int h )
{
	if( w < 1 ) w = 1;
	if( h < 1 ) h = 1;

	// Foreign-generation names: forget (never delete on a dead context).
	if( s_hdr.gpuGeneration != GpuGeneration())
	{
		ForgetHdr();
		s_hdr.gpuGeneration = GpuGeneration();
	}

	if( s_hdr.valid && s_hdr.width == w && s_hdr.height == h )
		return true;

	if( s_hdr.failedThisGen )
		return false;   // already failed on this generation; do not retry (logged once)

	// Resize on a LIVE context: delete the old names first.
	if( s_hdr.fbo != 0 || s_hdr.colorTex != 0 || s_hdr.depthTex != 0 )
		DestroyHdrSameContext();
	s_hdr.gpuGeneration = GpuGeneration();

	// Color: RGBA16F, LINEAR + CLAMP_TO_EDGE (filterable for future use; the
	// resolve samples via texelFetch for an exact copy). Bound on a SKY unit so
	// we never disturb the engine-tracked units 0..3.
	glGenTextures( 1, &s_hdr.colorTex );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_hdr.colorTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	// Depth: DEPTH_COMPONENT24 *texture* (sampleable), replacing the old
	// renderbuffer (fog Step 1, spec FOG-MILESTONE1 §4.2). Same internal format
	// and size as the renderbuffer it replaces -- rasterization / depth-test /
	// clear behaviour is bit-identical; only that the depth is now sampleable.
	// Sampler state for the fog read: NEAREST (raw, no filtering of non-linear
	// depth), CLAMP_TO_EDGE, and crucially TEXTURE_COMPARE_MODE = GL_NONE so a
	// later fog sampler reads the *raw* window-space z (distinct from the shadow
	// map's GL_COMPARE_R_TO_TEXTURE mode). No stencil (red-team fix #10). Bound on
	// a sky unit for the param setup, then restored with the color tex below.
	glGenTextures( 1, &s_hdr.depthTex );
	SkyComposeBindTex( 1, GL_TEXTURE_2D, s_hdr.depthTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE );
	SkyComposeRestoreTmus();   // resync engine TMU tracker before any GL_Bind

	glGenFramebuffers( 1, &s_hdr.fbo );
	BindFbo( s_hdr.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_hdr.colorTex, 0 );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_hdr.depthTex, 0 );

	// Color FBO: draw buffer MUST be COLOR_ATTACHMENT0 (red-team fix #9; do NOT
	// copy the shadow FBO's GL_NONE, which would give no color writes / black).
	GLenum drawBuf = GL_COLOR_ATTACHMENT0;
	glDrawBuffers( 1, &drawBuf );
	glReadBuffer( GL_COLOR_ATTACHMENT0 );

	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	BindFbo( 0 );

	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		DestroyHdrSameContext();
		s_hdr.failedThisGen = true;
		CSZ_LogError( "compose", "HDR FBO incomplete (status 0x%x); HDR path disabled this generation (baseline render)",
			(unsigned int)status );
		return false;
	}

	s_hdr.width = w;
	s_hdr.height = h;
	s_hdr.valid = true;
	CSZ_LogInfo( "compose", "HDR scene target ready (%dx%d RGBA16F + D24 depth-tex, gpu gen %d)", w, h, s_hdr.gpuGeneration );
	return true;
}

// GL-ERR-2: live "should the GPU timer run this frame?" gate -- the capability
// latch AND the csz_hdr_timing cvar. When false, NO query code executes.
bool ComposeTimingActive()
{
	// Issue the (single, non-nestable) in-scene GPU timer when EITHER the HDR
	// timing log OR the L0 perf dump asks for it. Both only READ the query result;
	// neither alters any draw, so the rendered frame is byte-identical whether or
	// not the query runs.
	return HaveTimerQuery()
		&& ( ReadCvar( s_cvarTiming, 0.0f ) != 0.0f || ReadCvar( s_cvarPerfDump, 0.0f ) != 0.0f );
}

void EnsureTimer()
{
	// GL-ERR-2: never touch timer-query objects without the capability latch.
	if( !HaveTimerQuery())
	{
		s_timer.created = false;
		return;
	}

	if( s_timer.created && s_timer.gpuGeneration == GpuGeneration())
		return;

	// Foreign generation: forget (queries died with the context).
	for( int i = 0; i < kQueryRing; i++ )
		s_timer.issued[i] = false;

	s_timer.gpuGeneration = GpuGeneration();
	s_timer.writeIndex = 0;
	s_timer.inSpan = false;
	s_timer.lastMs = -1.0;
	s_timer.created = false;

	glGenQueries( kQueryRing, s_timer.query );

	// glGenQueries names are not real query objects until first use; that is
	// fine -- issued[] guards reads until a slot has actually been recorded.
	s_timer.created = true;
}

}  // anonymous namespace

// =============================================================================
// PUBLIC: safe sky-unit TMU bind / restore (red-team fix #1,#2,#3)
// =============================================================================
void SkyComposeBindTex( int skyUnitIndex, GLenum target, GLuint rawGlName )
{
	if( skyUnitIndex < 0 || skyUnitIndex >= kSkyTmuCount )
	{
		CSZ_LogError( "compose", "SkyComposeBindTex: sky unit %d out of range [0,%d)", skyUnitIndex, kSkyTmuCount );
		return;
	}

	// Capture the engine-coherent active unit ONCE, before the first raw
	// glActiveTexture of this cycle desyncs it. Everything up to here drove the
	// active unit through the engine wrappers, so the real unit == the engine's
	// tracked glState.activeTMU; saving it lets RestoreTmus put the real unit back
	// WITHOUT a GL_SelectTexture call (red-team fix #1 without the 0x502).
	if( s_savedActiveTex < 0 )
	{
		GLint cur = GL_TEXTURE0;
		glGetIntegerv( GL_ACTIVE_TEXTURE, &cur );
		s_savedActiveTex = (int)cur;
	}

	glActiveTexture( GL_TEXTURE0 + kSkyTmuBase + skyUnitIndex );
	glBindTexture( target, rawGlName );

	s_skyTmu[skyUnitIndex].bound = ( rawGlName != 0 );
	s_skyTmu[skyUnitIndex].target = target;
}

void SkyComposeRestoreTmus()
{
	// Unbind every sky unit we touched, on its own raw active unit.
	for( int i = 0; i < kSkyTmuCount; i++ )
	{
		if( !s_skyTmu[i].bound )
			continue;

		glActiveTexture( GL_TEXTURE0 + kSkyTmuBase + i );
		glBindTexture( s_skyTmu[i].target, 0 );
		s_skyTmu[i].bound = false;
	}

	// Return the REAL active unit to exactly what it was before this bind/restore
	// cycle (the value SkyComposeBindTex captured). We only ever touched the active
	// unit with RAW glActiveTexture on sky units 4..7 -- the engine's tracked
	// glState.activeTMU was NEVER changed -- so restoring the real unit to the saved
	// value leaves real == tracker coherent, which is all the engine needs (red-team
	// fix #1: a later gRenderAPI.GL_Bind hits the right unit; LeaveTakeover's
	// down-walk cleanup stays coherent because real matches the tracker it walks).
	//
	// ROOT CAUSE of the old SkyComposeRestoreTmus 0x502 (infra pass 2, 2026-06-18):
	// the previous code resynced by calling gRenderAPI.GL_SelectTexture(3) then (0).
	// The engine renderer runs on a GL 3.3 CORE profile context (harness launches
	// with `-glcore`; glcaps logs profile=0x1). The engine's GL_SelectTexture, when
	// it ACTUALLY changes the active unit, issues a compatibility-profile-only TMU
	// call (client-active-texture / fixed-function texture-unit state) that raises
	// GL_INVALID_OPERATION (0x502) on a core context. So EVERY sky pass that ended in
	// RestoreTmus left a fresh 0x502 -- the residual the runtime bisection localized
	// "after SkyComposeRestoreTmus". A single GL_SelectTexture(0) fixed the slot-10.5
	// passes (atmos dome, sun/moon), where the tracker was already 0 (guard skips the
	// wrapped call), but NOT the resolve pass: it runs at slot 15.5 AFTER the world/
	// studio passes have GL_Bind'd a non-zero unit, so (0) genuinely changed the unit
	// and the engine still raised 0x502. Restoring the saved unit instead makes the
	// resync a pure raw glActiveTexture -- NO engine wrapper, NO compat call, NO error
	// -- for ALL THREE sky passes regardless of the entry unit. We do NOT swallow the
	// error; we stop GENERATING it. The same engine-internal 0x502 still occurs inside
	// LeaveTakeover's own GL_SelectTexture and the engine's 2D/HUD path (csz_glstate.cpp,
	// shared infra; not ours to change) -- that pure-engine residual surfaces only at
	// the "pre-sky" drain, which the bisection tool labels "engine / prior passes".
	// If no SkyComposeBindTex ran this cycle (s_savedActiveTex < 0, defensive
	// standalone call), the active unit was never desynced -- leave it untouched.
	if( s_savedActiveTex >= 0 )
	{
		glActiveTexture( (GLenum)s_savedActiveTex );
		s_savedActiveTex = -1;
	}
}

// =============================================================================
// PUBLIC: typed atmos resource ABI (C2 fills it later)
// =============================================================================
const SkyAtmosResources &SkyComposeAtmosResources()
{
	EnsureAtmosDefault();
	return s_atmos;
}

void SkyComposePublishAtmosResources( const SkyAtmosResources &res )
{
	EnsureAtmosDefault();
	s_atmos = res;
}

// =============================================================================
// PUBLIC: stub seams (pure no-ops/identity in C1)
// =============================================================================
// AtmosBuildLuts() is now IMPLEMENTED by C2 in geom/csz_atmos.cpp (the C1 no-op
// stub that lived here has moved there). SunMoonContribute() is now IMPLEMENTED
// by C3 in geom/csz_sunmoon.cpp (its C1 no-op stub has moved there).
// StarsContribute() is now IMPLEMENTED by C4 in geom/csz_stars.cpp (its C1 no-op
// stub has moved there).

// =============================================================================
// PUBLIC: cvars + activity query
// =============================================================================
void SkyComposeRegisterCvars()
{
	if( s_cvarHdr == NULL )
		s_cvarHdr = gEngfuncs.pfnRegisterVariable( "csz_hdr", "1", FCVAR_CLIENTDLL );
	if( s_cvarExposure == NULL )
		s_cvarExposure = gEngfuncs.pfnRegisterVariable( "csz_exposure", "1.0", FCVAR_CLIENTDLL );
	if( s_cvarTonemap == NULL )
		s_cvarTonemap = gEngfuncs.pfnRegisterVariable( "csz_tonemap", "0", FCVAR_CLIENTDLL );
	if( s_cvarEncode == NULL )
		s_cvarEncode = gEngfuncs.pfnRegisterVariable( "csz_encode", "0", FCVAR_CLIENTDLL );
	if( s_cvarDither == NULL )
		s_cvarDither = gEngfuncs.pfnRegisterVariable( "csz_dither", "0", FCVAR_CLIENTDLL );
	if( s_cvarHlRolloff == NULL )
		// L-polish A: identity-path overbright shoulder. Default 1 (fix ON); set 0 for
		// the legacy hard-clip A/B (every pixel returned exactly as pre-L-polish).
		s_cvarHlRolloff = gEngfuncs.pfnRegisterVariable( "csz_highlight_rolloff", "1", FCVAR_CLIENTDLL );
	if( s_cvarHlKnee == NULL )
		s_cvarHlKnee = gEngfuncs.pfnRegisterVariable( "csz_highlight_knee", "0.95", FCVAR_CLIENTDLL );
	if( s_cvarTiming == NULL )
		s_cvarTiming = gEngfuncs.pfnRegisterVariable( "csz_hdr_timing", "0", FCVAR_CLIENTDLL );
	if( s_cvarPerfDump == NULL )
		s_cvarPerfDump = gEngfuncs.pfnRegisterVariable( "csz_perf_dump", "0", FCVAR_CLIENTDLL );
	// S4 night grade knobs (REWORK-SPEC §S4). USER real-machine "口味" knobs; all no-op by day
	// (gated on nightness). csz_night_exposure default 1.0 = no darkening (USER dials down for a
	// darker mood); the readability/cool defaults (toe + purkinje) are ON.
	if( s_cvarNightExposure == NULL )
		s_cvarNightExposure = gEngfuncs.pfnRegisterVariable( "csz_night_exposure", "1.0", FCVAR_CLIENTDLL );
	if( s_cvarNightToe == NULL )
		s_cvarNightToe = gEngfuncs.pfnRegisterVariable( "csz_night_toe", "0.35", FCVAR_CLIENTDLL );
	if( s_cvarToeGamma == NULL )
		s_cvarToeGamma = gEngfuncs.pfnRegisterVariable( "csz_night_toe_gamma", "1.5", FCVAR_CLIENTDLL );
	if( s_cvarPurkinje == NULL )
		s_cvarPurkinje = gEngfuncs.pfnRegisterVariable( "csz_purkinje", "1.0", FCVAR_CLIENTDLL );
	if( s_cvarPurkinjeKnee == NULL )
		s_cvarPurkinjeKnee = gEngfuncs.pfnRegisterVariable( "csz_purkinje_knee", "0.35", FCVAR_CLIENTDLL );

	CSZ_LogDev( "compose", "HDR cvars registered (csz_hdr/exposure/tonemap/encode/dither/hdr_timing/perf_dump; "
		"S4 night_exposure/night_toe/night_toe_gamma/purkinje/purkinje_knee)" );
}

// S4: the renderer pushes the live phase nightness here once per frame (after PublishLighting,
// before the resolve) so the resolve's night grade follows the phase curve. Day (nightness 0)
// -> the shader skips the entire grade -> bit-identical resolve.
void SkyComposePublishNight( float nightness )
{
	if( nightness < 0.0f ) nightness = 0.0f;
	if( nightness > 1.0f ) nightness = 1.0f;
	s_publishedNightness = nightness;
}

bool SkyComposeActive()
{
	// Read live so an in-session A/B toggle takes effect next frame.
	return ReadCvar( s_cvarHdr, 1.0f ) != 0.0f;
}

// Raw GL name of the sampleable scene depth texture (GL_DEPTH_COMPONENT24,
// compare-mode NONE), or 0 when the HDR target is not valid this frame. The fog
// pass (Step 2+) binds this via SkyComposeBindTex to reconstruct view/world
// position from depth. Returns 0 in the baseline (csz_hdr 0) path. (fog Step 1)
GLuint SkyComposeDepthTex()
{
	return s_hdr.valid ? s_hdr.depthTex : 0;
}

// Raw GL name of the HDR scene FBO (RGBA16F color + sampleable D24 depth), or 0
// when the HDR target is not valid this frame. The fog volume pass (Step 3) binds
// it to ADDITIVELY composite its in-scatter into the scene at the kTmVolume seam,
// then restores it as the bound target for the following transparent/viewmodel
// passes. (fog Step 3)
GLuint SkyComposeHdrFbo()
{
	return s_hdr.valid ? s_hdr.fbo : 0;
}

// =============================================================================
// PUBLIC: scene begin (slot 10) -- bind HDR FBO + clear, or passthrough baseline
// =============================================================================
void SkyComposeBeginScene( const struct ref_viewpass_s *rvp, const float clearRgba[4] )
{
	const ref_viewpass_t *vp = (const ref_viewpass_t *)rvp;

	if( !SkyComposeActive())
	{
		// Baseline path: identical to ApplyMainViewport (FBO 0 + viewport +
		// clear). csz_hdr 0 reproduces the C0 baseline exactly.
		ApplyMainViewport( rvp, clearRgba );
		return;
	}

	LogCapsOnce();
	BuildResolveProgram();

	// GL-ERR-2: only create/run the GPU timer when the capability latch AND
	// csz_hdr_timing are on. Off-by-default play touches no query code at all.
	bool timing = ComposeTimingActive();
	if( timing )
		EnsureTimer();

	// Size the FBO to the FULL extent (origin + size): the scene renders with
	// glViewport(v0,v1,w,h), so a non-(0,0) origin lands at texels [v0,v0+w) x
	// [v1,v1+h) in the FBO. Resolve on backbuffer 0 uses the SAME viewport, so
	// gl_FragCoord maps 1:1 to those texels and u_viewOrigin = (0,0). This is
	// robust to any viewport origin (GoldSrc main view is (0,0) in practice).
	int w = vp->viewport[0] + vp->viewport[2];
	int h = vp->viewport[1] + vp->viewport[3];

	if( !EnsureHdrTarget( w, h ))
	{
		// B-class degrade: HDR target unavailable this generation. Fall back to
		// the baseline straight-to-backbuffer path so the frame still renders.
		ApplyMainViewport( rvp, clearRgba );
		return;
	}

	// Begin the GPU timer span around the HDR-active region (BeginScene clear
	// .. Resolve). Reuse the slot we are about to write; mark it issued so the
	// matching EndQuery in Resolve closes it.
	if( timing && s_timer.created )
	{
		glBeginQuery( GL_TIME_ELAPSED, s_timer.query[s_timer.writeIndex] );
		s_timer.inSpan = true;
	}

	// Bind the HDR FBO; viewport matches rvp; clear color+depth with the SAME
	// clearColor the engine would use (identity).
	BindFbo( s_hdr.fbo );
	glViewport( vp->viewport[0], vp->viewport[1], vp->viewport[2], vp->viewport[3] );
	glClearColor( clearRgba[0], clearRgba[1], clearRgba[2], clearRgba[3] );
	glClearDepth( 1.0 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
}

// =============================================================================
// PUBLIC: resolve (before slot 16) -- fullscreen HDR -> backbuffer, or no-op
// =============================================================================
void SkyComposeResolve( const struct ref_viewpass_s *rvp, const float clearRgba[4] )
{
	const ref_viewpass_t *vp = (const ref_viewpass_t *)rvp;

	if( !SkyComposeActive() || !s_hdr.valid )
		return;   // baseline path: scene already on FBO 0, nothing to resolve

	// Drain + attribute any error that arrived BEFORE the resolve (the world/studio/
	// lights/sprites/viewmodel passes between the renderer's sunmoon probe and its
	// resolve probe each trip the engine's own core-profile GL_Bind 0x502). After
	// this, the renderer's "compose resolve" probe shows only resolve-GENERATED GL.
	ComposeGlCheck( "resolve-ENTRY (pre-resolve: engine GL_Bind in world/studio/etc)" );

	bool timing = ComposeTimingActive();   // GL-ERR-2: latch + csz_hdr_timing

	// Close the GPU timer span opened in BeginScene. Unconditional on inSpan (not
	// `timing`) so a span opened while timing was on still closes if the cvar was
	// toggled off mid-frame -- otherwise the GL_TIME_ELAPSED query stays active.
	if( s_timer.created && s_timer.inSpan )
	{
		glEndQuery( GL_TIME_ELAPSED );
		s_timer.inSpan = false;
		s_timer.issued[s_timer.writeIndex] = true;
	}

	// --- bind backbuffer 0 + clean fullscreen state (red-team fix #4) ---------
	BindFbo( 0 );
	glViewport( vp->viewport[0], vp->viewport[1], vp->viewport[2], vp->viewport[3] );

	// The cached glstate wrappers are valid inside the takeover window; use them
	// where they exist, raw GL otherwise. Depth/blend/cull/scissor/poly-offset
	// OFF; color mask all-true; opaque alpha is written by the FS (1.0).
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendNone );
	SetCull( false );
	SetPolygonOffset( false, 0.0f, 0.0f );
	glDisable( GL_SCISSOR_TEST );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

	// FBO 0 is no longer cleared by the HDR path (red-team fix #5): baseline
	// ApplyMainViewport cleared FBO 0; now only the HDR FBO was cleared. Clear
	// FBO 0 color+depth with the same clearColor so letterbox borders + default
	// depth are not stale. (Done before the resolve draw, which is opaque and
	// covers the viewport, but the clear also covers any non-viewport area.)
	glClearColor( clearRgba[0], clearRgba[1], clearRgba[2], clearRgba[3] );
	glClearDepth( 1.0 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	// Belt-and-suspenders: ensure no implicit sRGB encode on the backbuffer.
	if( glIsEnabled( GL_FRAMEBUFFER_SRGB ) == GL_TRUE )
		glDisable( GL_FRAMEBUFFER_SRGB );

	// --- resolve draw ---------------------------------------------------------
	UseProgram( s_resolve.program.program );
	BindVao( s_resolve.vao );

	// Bind the HDR color texture via the SAFE sky-unit bind (red-team fix #2):
	// do NOT raw-bind it on unit 0 (that corrupts the engine-tracked unit-0
	// binding). u_hdr references the ABSOLUTE sky unit.
	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_hdr.colorTex );

	if( s_resolve.uHdr >= 0 )
		glUniform1i( s_resolve.uHdr, kSkyTmuBase + 0 );
	// Viewport origin for the exact 1:1 texelFetch (red-team fix #8): the HDR
	// texture is the full FBO so a non-(0,0) viewport origin must be subtracted
	// from gl_FragCoord to land on texel 0.
	if( s_resolve.uViewOrigin >= 0 )
		glUniform2i( s_resolve.uViewOrigin, 0, 0 );   // FBO sized to full extent: 1:1 with gl_FragCoord
	if( s_resolve.uExposure >= 0 )
		glUniform1f( s_resolve.uExposure, ReadCvar( s_cvarExposure, 1.0f ));
	if( s_resolve.uTonemap >= 0 )
		glUniform1i( s_resolve.uTonemap, ( ReadCvar( s_cvarTonemap, 0.0f ) != 0.0f ) ? 1 : 0 );
	if( s_resolve.uEncode >= 0 )
		glUniform1i( s_resolve.uEncode, ( ReadCvar( s_cvarEncode, 0.0f ) != 0.0f ) ? 1 : 0 );
	if( s_resolve.uDither >= 0 )
		glUniform1i( s_resolve.uDither, ( ReadCvar( s_cvarDither, 0.0f ) != 0.0f ) ? 1 : 0 );
	// L-polish A: identity-path overbright shoulder. Clamp rolloff to [0,1] (blend
	// factor) and knee to (0,1) so a stray cvar value cannot break the shoulder math.
	if( s_resolve.uHlRolloff >= 0 )
	{
		float roll = ReadCvar( s_cvarHlRolloff, 1.0f );
		if( roll < 0.0f ) roll = 0.0f;
		if( roll > 1.0f ) roll = 1.0f;
		glUniform1f( s_resolve.uHlRolloff, roll );
	}
	if( s_resolve.uHlKnee >= 0 )
	{
		float knee = ReadCvar( s_cvarHlKnee, 0.95f );
		if( knee < 0.0f ) knee = 0.0f;
		if( knee > 0.999f ) knee = 0.999f;
		glUniform1f( s_resolve.uHlKnee, knee );
	}
	// S4 night grade (REWORK-SPEC §S4). Feed the live phase nightness (published by the renderer)
	// + the USER tuning cvars, all CPU-clamped to sane bands so a stray value cannot break the
	// resolve. At nightness 0 (day) the shader skips the whole block -> bit-identical.
	if( s_resolve.uNightness >= 0 )
		glUniform1f( s_resolve.uNightness, s_publishedNightness );
	if( s_resolve.uNightExposure >= 0 )
	{
		float e = ReadCvar( s_cvarNightExposure, 1.0f );
		if( e < 0.0f ) e = 0.0f;
		if( e > 4.0f ) e = 4.0f;
		glUniform1f( s_resolve.uNightExposure, e );
	}
	if( s_resolve.uNightToe >= 0 )
	{
		float toe = ReadCvar( s_cvarNightToe, 0.35f );
		if( toe < 0.0f ) toe = 0.0f;
		if( toe > 1.0f ) toe = 1.0f;
		glUniform1f( s_resolve.uNightToe, toe );
	}
	if( s_resolve.uToeGamma >= 0 )
	{
		float g = ReadCvar( s_cvarToeGamma, 1.5f );
		if( g < 0.1f ) g = 0.1f;
		if( g > 4.0f ) g = 4.0f;
		glUniform1f( s_resolve.uToeGamma, g );
	}
	if( s_resolve.uPurkinje >= 0 )
	{
		float p = ReadCvar( s_cvarPurkinje, 1.0f );
		if( p < 0.0f ) p = 0.0f;
		if( p > 2.0f ) p = 2.0f;
		glUniform1f( s_resolve.uPurkinje, p );
	}
	if( s_resolve.uPurkinjeKnee >= 0 )
	{
		float k = ReadCvar( s_cvarPurkinjeKnee, 0.35f );
		if( k < 0.01f ) k = 0.01f;
		if( k > 1.0f ) k = 1.0f;
		glUniform1f( s_resolve.uPurkinjeKnee, k );
	}

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	BindVao( 0 );

	// Restore the takeover baseline for whatever (dev overlay re-run) might
	// follow, and unbind + resync the sky TMU (red-team fix #1) BEFORE returning
	// to LeaveTakeover (which uses gRenderAPI wrappers).
	SetDepthTest( true );
	SetDepthWrite( true );
	SkyComposeRestoreTmus();

	// --- read the GPU timer ring (non-stalling; red-team fix #11) -------------
	// Read the OLDEST issued slot only if its result is AVAILABLE. Advance the
	// write index for next frame. GL-ERR-2: only when timing is active.
	if( timing && s_timer.created )
	{
		int readIndex = ( s_timer.writeIndex + 1 ) % kQueryRing;   // oldest of the ring

		if( s_timer.issued[readIndex] )
		{
			GLint available = 0;
			glGetQueryObjectiv( s_timer.query[readIndex], GL_QUERY_RESULT_AVAILABLE, &available );

			if( available == GL_TRUE )
			{
				GLuint64 ns = 0;
				glGetQueryObjectui64v( s_timer.query[readIndex], GL_QUERY_RESULT, &ns );
				s_timer.lastMs = (double)ns / 1.0e6;
				s_timer.issued[readIndex] = false;

				if( ReadCvar( s_cvarTiming, 0.0f ) != 0.0f )
					CSZ_LogDev( "compose", "HDR GPU span (BeginScene..Resolve) = %.3f ms "
						"(~whole CSZ scene; true HDR overhead = whole-frame csz_hdr 1 vs 0 delta)",
						s_timer.lastMs );
			}
		}

		s_timer.writeIndex = ( s_timer.writeIndex + 1 ) % kQueryRing;
	}

	// Attribute any error GENERATED by the resolve itself (entry was drained above).
	ComposeGlCheck( "resolve-EXIT (our HDR resolve draw + RestoreTmus)" );
}

double SkyComposeLastGpuMs()
{
	return s_timer.lastMs;
}

// L0 observability: is csz_perf_dump armed this frame? Read live so an in-session
// toggle takes effect next frame, matching the rest of the cvar reads here.
bool SkyComposePerfDumpEnabled()
{
	return ReadCvar( s_cvarPerfDump, 0.0f ) != 0.0f;
}

// =============================================================================
// PUBLIC: shutdown (generation-safe teardown)
// =============================================================================
void SkyComposeShutdown()
{
	bool sameContext = ( s_hdr.gpuGeneration == GpuGeneration());

	if( sameContext )
		DestroyHdrSameContext();
	else
		ForgetHdr();

	if( s_resolve.built && s_resolve.gpuGeneration == GpuGeneration())
	{
		if( s_resolve.vao != 0 )
			glDeleteVertexArrays( 1, &s_resolve.vao );
		DestroyProgram( s_resolve.program );
	}
	s_resolve.vao = 0;
	s_resolve.built = false;

	if( s_timer.created && s_timer.gpuGeneration == GpuGeneration())
		glDeleteQueries( kQueryRing, s_timer.query );
	s_timer.created = false;
	s_timer.inSpan = false;
	for( int i = 0; i < kQueryRing; i++ )
		s_timer.issued[i] = false;
}

}  // namespace csz
