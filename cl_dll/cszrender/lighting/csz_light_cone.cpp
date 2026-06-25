/*
 * csz_light_cone.cpp -- CSOZ renderer: world-space visible flashlight beam volume (L6a / slot 13.4)
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
// Slot 13.4 render order (csz_renderer.cpp): AFTER RunLightPasses (spot DIRECT
// additive world+studio lighting + the re-enabled non-local ground pool, slot 13)
// and BEFORE the Step-3 first-person fog march (slot 13.5).
//
// DESIGN-SPEC §V2 rebuild (post-red-team): the third-person (NON-LOCAL) air cone is a
// TWO-PASS half-res path. Pass 1 accumulates every visible non-local cone (the
// budgeter selects <=12) into a SEPARATE half-res RGBA16F buffer with an
// energy-conserving Beer-Lambert single-scatter march. Pass 2 upsamples that buffer to
// full-res with a depth-aware bilateral filter (ported from the first-person fog
// volume), applies an order-independent soft-knee compression ON THE VOLUME BUFFER
// ONLY, and ADDS the result into the HDR scene. The LOCAL first-person beam is excluded
// here (slot 13.5 owns it) so there is no double-draw / double-energy.
#include "csz_light_cone.h"
#include "csz_light_budget.h"
#include "csz_light_registry.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_math.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"
#include "../core/csz_light_types.h"
#include "../geom/csz_sky_compose.h"

#include <math.h>
#include <string>

namespace csz
{

#include "../fog/csz_fog_shaders.inl"   // kFogDepthReconstructGlsl (Step 1 helpers: linViewZ/worldPosFromDepth)
#include "csz_light_cone_shaders.inl"   // kConeVs / kConeFsBody / kConeUpVs / kConeUpFsBody

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// Radial wedge count of the procedural cone (smooth rim at any near distance; a
// flashlight cone is small on screen so 64 is plenty). MUST match the draw count.
const int kConeSegments = 64;

// Bounded march samples through the cone volume (full tier). Only cone-silhouette
// pixels of the HALF-RES buffer run this, so the cost is local + quartered vs the old
// full-res path; the energy-conserving slice + static jitter + bilateral upsample keep
// it smooth at this count.
const int kConeSteps = 16;


// --- cvars (read live each frame) --------------------------------------------
cvar_t *s_cvarTp;          // csz_flashlight_tp           default "1": world beam visible (0 = off, A/B)
cvar_t *s_cvarTpIntensity; // csz_flashlight_tp_intensity default "3.0": beam brightness (dev tuning)
cvar_t *s_cvarNlSurfFade;  // csz_flashlight_nl_surffade  default "1.0": non-local surface-fade band scale (>0)
cvar_t *s_cvarRange;       // csz_flashlight_range (owned by FogVolume); caps beam length. Fetched lazily.
bool    s_lookedRange;
cvar_t *s_cvarV3;          // csz_flashlight_v3 (owned by light_pass); L5R master A/B. Fetched lazily.
cvar_t *s_cvarTpG;         // csz_flashlight_tp_g         default "0.7": world-cone HG forward g (clamp 0.6..0.8)
cvar_t *s_cvarTpSteps;     // csz_flashlight_tp_steps     default "16": full-tier march samples (clamp 8..32)
cvar_t *s_cvarTpFogCouple; // csz_flashlight_tp_fogcouple default "0": density-couple amount 0..1 (optional)
// §V2 new cvars (energy-conserving rebuild; sensible defaults, live-adjustable):
cvar_t *s_cvarTpSigmaS;    // csz_flashlight_tp_sigmaS    default "0.05": scattering coefficient
cvar_t *s_cvarTpSigmaE;    // csz_flashlight_tp_sigmaE    default "0.05": extinction coefficient (albedo 1 by default)
cvar_t *s_cvarTpCap;       // csz_flashlight_tp_cap       default "2.5": per-light radiance cap
cvar_t *s_cvarTpKnee;      // csz_flashlight_tp_knee      default "0.8": volume-buffer soft-knee compression knee
cvar_t *s_cvarTpHalo;      // csz_flashlight_tp_halo      default "0.3": two-lobe halo weight w1 (0..1)
cvar_t *s_cvarTpHero;      // csz_flashlight_tp_heroshadows default "0": hero shadow-map count (v1: NOT implemented)
bool    s_lookedV3;

// --- half-res in-scatter target (RGBA16F, no depth) ---------------------------
// Mirrors the first-person fog volume's VolTarget: a separate accumulation buffer the
// cones add into, then a bilateral upsample composites it into the HDR scene.
struct VolTarget
{
	GLuint fbo;
	GLuint colorTex;        // RGBA16F: rgb = accumulated in-scatter (a unused)
	int    width, height;
	int    gpuGeneration;
	bool   valid;
	bool   failedThisGen;
};
VolTarget s_vol;

// --- GPU resources (generation-keyed; forget on a foreign context) -----------
struct ConeGpu
{
	ShaderProgram march;     // cone-mesh half-res accumulation program
	ShaderProgram up;        // fullscreen bilateral upsample + soft-knee + composite
	GLuint vao;
	int    gpuGeneration;
	bool   built;
	bool   failedThisGen;

	// march uniforms
	int mMatViewProj, mApex, mAxis, mRight, mUp, mSegments;
	int mDepthTex, mTargetSize, mCamPos, mAxisDir, mLen;
	int mCosInner, mCosOuter, mColor, mIntensity, mHgG, mHalo, mSigmaS, mSigmaE, mCap, mSteps, mSurfFade;
	int mZNear, mZFar, mInvViewProj;

	// upsample uniforms
	int uInscatter, uDepthTex, uFullSize, uHalfSize, uSmoothSigma, uKnee, uZNear, uZFar;
};
ConeGpu s_gpu;

float ReadCvar( cvar_t *cv, float fallback )
{
	return ( cv != NULL ) ? cv->value : fallback;
}

void ForgetGpu()
{
	s_gpu.vao = 0;
	s_gpu.march.program = 0;
	s_gpu.up.program = 0;
	s_gpu.built = false;
	s_gpu.failedThisGen = false;
}

void DestroyGpuSameContext()
{
	if( s_gpu.vao != 0 )
		glDeleteVertexArrays( 1, &s_gpu.vao );
	if( s_gpu.march.program != 0 )
		DestroyProgram( s_gpu.march );
	if( s_gpu.up.program != 0 )
		DestroyProgram( s_gpu.up );
	ForgetGpu();
}

void ForgetVol()
{
	s_vol.fbo = 0;
	s_vol.colorTex = 0;
	s_vol.width = 0;
	s_vol.height = 0;
	s_vol.valid = false;
	s_vol.failedThisGen = false;
}

void DestroyVolSameContext()
{
	if( s_vol.fbo != 0 )
		glDeleteFramebuffers( 1, &s_vol.fbo );
	if( s_vol.colorTex != 0 )
		glDeleteTextures( 1, &s_vol.colorTex );
	ForgetVol();
}

// Ensure the half-res accumulation FBO at (w,h) on the live generation. Mirrors the
// generation rule + completeness check + B-class degrade of the fog volume target.
bool EnsureVolTarget( int w, int h )
{
	if( w < 1 ) w = 1;
	if( h < 1 ) h = 1;

	if( s_vol.gpuGeneration != GpuGeneration() )
	{
		ForgetVol();
		s_vol.gpuGeneration = GpuGeneration();
	}

	if( s_vol.valid && s_vol.width == w && s_vol.height == h )
		return true;

	if( s_vol.failedThisGen )
		return false;

	if( s_vol.fbo != 0 || s_vol.colorTex != 0 )
		DestroyVolSameContext();
	s_vol.gpuGeneration = GpuGeneration();

	// RGBA16F, LINEAR (the bilateral upsample samples it with texture()), CLAMP.
	// Bound on a sky unit for setup so the engine-tracked units 0..3 are untouched.
	glGenTextures( 1, &s_vol.colorTex );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_vol.colorTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	SkyComposeRestoreTmus();

	glGenFramebuffers( 1, &s_vol.fbo );
	BindFbo( s_vol.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_vol.colorTex, 0 );

	GLenum drawBuf = GL_COLOR_ATTACHMENT0;
	glDrawBuffers( 1, &drawBuf );
	glReadBuffer( GL_COLOR_ATTACHMENT0 );

	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	BindFbo( 0 );

	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		DestroyVolSameContext();
		s_vol.failedThisGen = true;
		CSZ_LogError( "lightcone", "half-res FBO incomplete (status 0x%x); world beam disabled this generation",
			(unsigned int)status );
		return false;
	}

	s_vol.width = w;
	s_vol.height = h;
	s_vol.valid = true;
	CSZ_LogInfo( "lightcone", "half-res in-scatter target ready (%dx%d RGBA16F, gpu gen %d)", w, h, s_vol.gpuGeneration );
	return true;
}

bool EnsureBuilt()
{
	if( s_gpu.gpuGeneration != GpuGeneration() )
	{
		ForgetGpu();                       // foreign generation: forget, never glDelete
		s_gpu.gpuGeneration = GpuGeneration();
	}
	if( s_gpu.built )
		return true;
	if( s_gpu.failedThisGen )
		return false;

	glGenVertexArrays( 1, &s_gpu.vao );

	// Both final FS = "#version 330 core" + shared Step-1 reconstruct helpers + body.
	std::string marchFs = std::string( "#version 330 core\n" ) + kFogDepthReconstructGlsl + kConeFsBody;
	std::string upFs    = std::string( "#version 330 core\n" ) + kFogDepthReconstructGlsl + kConeUpFsBody;

	if( !BuildProgram( "csz_light_cone_march", kConeVs, marchFs.c_str(), false, s_gpu.march ) ||
	    !BuildProgram( "csz_light_cone_up", kConeUpVs, upFs.c_str(), false, s_gpu.up ) )
	{
		if( s_gpu.march.program != 0 ) DestroyProgram( s_gpu.march );
		if( s_gpu.up.program != 0 )    DestroyProgram( s_gpu.up );
		s_gpu.march.program = 0;
		s_gpu.up.program = 0;
		s_gpu.failedThisGen = true;
		CSZ_LogError( "lightcone", "shader build failed; world beam disabled this generation" );
		return false;
	}

	s_gpu.mMatViewProj = UniformLoc( s_gpu.march, "u_matViewProj" );
	s_gpu.mApex        = UniformLoc( s_gpu.march, "u_apex" );
	s_gpu.mAxis        = UniformLoc( s_gpu.march, "u_axis" );
	s_gpu.mRight       = UniformLoc( s_gpu.march, "u_right" );
	s_gpu.mUp          = UniformLoc( s_gpu.march, "u_up" );
	s_gpu.mSegments    = UniformLoc( s_gpu.march, "u_segments" );
	s_gpu.mDepthTex    = UniformLoc( s_gpu.march, "u_depthTex" );
	s_gpu.mTargetSize  = UniformLoc( s_gpu.march, "u_targetSize" );
	s_gpu.mCamPos      = UniformLoc( s_gpu.march, "u_camPos" );
	s_gpu.mAxisDir     = UniformLoc( s_gpu.march, "u_axisDir" );
	s_gpu.mLen         = UniformLoc( s_gpu.march, "u_len" );
	s_gpu.mCosInner    = UniformLoc( s_gpu.march, "u_cosInner" );
	s_gpu.mCosOuter    = UniformLoc( s_gpu.march, "u_cosOuter" );
	s_gpu.mColor       = UniformLoc( s_gpu.march, "u_color" );
	s_gpu.mIntensity   = UniformLoc( s_gpu.march, "u_intensity" );
	s_gpu.mHgG         = UniformLoc( s_gpu.march, "u_hgG" );
	s_gpu.mHalo        = UniformLoc( s_gpu.march, "u_halo" );
	s_gpu.mSigmaS      = UniformLoc( s_gpu.march, "u_sigmaS" );
	s_gpu.mSigmaE      = UniformLoc( s_gpu.march, "u_sigmaE" );
	s_gpu.mCap         = UniformLoc( s_gpu.march, "u_cap" );
	s_gpu.mSteps       = UniformLoc( s_gpu.march, "u_steps" );
	s_gpu.mSurfFade    = UniformLoc( s_gpu.march, "u_surfFade" );
	s_gpu.mZNear       = UniformLoc( s_gpu.march, "u_zNear" );
	s_gpu.mZFar        = UniformLoc( s_gpu.march, "u_zFar" );
	s_gpu.mInvViewProj = UniformLoc( s_gpu.march, "u_invViewProj" );

	s_gpu.uInscatter   = UniformLoc( s_gpu.up, "u_inscatter" );
	s_gpu.uDepthTex    = UniformLoc( s_gpu.up, "u_depthTex" );
	s_gpu.uFullSize    = UniformLoc( s_gpu.up, "u_fullSize" );
	s_gpu.uHalfSize    = UniformLoc( s_gpu.up, "u_halfSize" );
	s_gpu.uSmoothSigma = UniformLoc( s_gpu.up, "u_smoothSigma" );
	s_gpu.uKnee        = UniformLoc( s_gpu.up, "u_knee" );
	s_gpu.uZNear       = UniformLoc( s_gpu.up, "u_zNear" );
	s_gpu.uZFar        = UniformLoc( s_gpu.up, "u_zFar" );

	s_gpu.built = true;
	CSZ_LogDev( "lightcone", "world beam programs built (gpu gen %d)", s_gpu.gpuGeneration );
	return true;
}

// Orthonormal basis perpendicular to the cone axis (any consistent choice; the
// beam is rotationally symmetric so the seam location is irrelevant).
void BasisFromAxis( const float d[3], float right[3], float up[3] )
{
	float ref[3] = { 0.0f, 0.0f, 1.0f };
	if( fabsf( d[2] ) > 0.99f )            // axis near-vertical: pick a different ref
	{
		ref[0] = 1.0f; ref[1] = 0.0f; ref[2] = 0.0f;
	}
	// right = normalize( cross( ref, d ) )
	right[0] = ref[1] * d[2] - ref[2] * d[1];
	right[1] = ref[2] * d[0] - ref[0] * d[2];
	right[2] = ref[0] * d[1] - ref[1] * d[0];
	float rl = sqrtf( right[0] * right[0] + right[1] * right[1] + right[2] * right[2] );
	if( rl < 1e-6f ) rl = 1e-6f;
	right[0] /= rl; right[1] /= rl; right[2] /= rl;
	// up = cross( d, right )
	up[0] = d[1] * right[2] - d[2] * right[1];
	up[1] = d[2] * right[0] - d[0] * right[2];
	up[2] = d[0] * right[1] - d[1] * right[0];
}

// Draw ONE spot's cone mesh into the bound half-res buffer. Shared state (FBO/
// viewport/blend/program/VAO/depth tex + the frame-constant uniforms) is set by the
// caller; this pushes per-spot uniforms + draws.
void DrawConeForSpot( const ViewSetup &view, const SpotLightParams &spot,
                      float range, int steps, float surfFade )
{
	float len = spot.radius;
	if( range > 0.0f && range < len )
		len = range;                       // csz_flashlight_range caps the beam length
	if( len < 1.0f )
		return;

	// Outer half-angle from cosOuter -> tan for the rim radius at the base plane.
	float cosOuter = spot.cosOuter;
	if( cosOuter > 0.9999f ) cosOuter = 0.9999f;
	if( cosOuter < 0.02f )   cosOuter = 0.02f;
	float halfOuter = acosf( cosOuter );
	float tanHalf = tanf( halfOuter );
	if( tanHalf < 1e-3f ) tanHalf = 1e-3f;

	float right[3], up[3];
	BasisFromAxis( spot.dir, right, up );

	float rimR = tanHalf * len;
	float axis[3]  = { spot.dir[0] * len, spot.dir[1] * len, spot.dir[2] * len };
	float rightS[3] = { right[0] * rimR, right[1] * rimR, right[2] * rimR };
	float upS[3]    = { up[0] * rimR,    up[1] * rimR,    up[2] * rimR };

	if( s_gpu.mMatViewProj >= 0 ) glUniformMatrix4fv( s_gpu.mMatViewProj, 1, GL_FALSE, view.matViewProj.m );
	if( s_gpu.mApex >= 0 )        glUniform3fv( s_gpu.mApex, 1, spot.origin );
	if( s_gpu.mAxis >= 0 )        glUniform3fv( s_gpu.mAxis, 1, axis );
	if( s_gpu.mRight >= 0 )       glUniform3fv( s_gpu.mRight, 1, rightS );
	if( s_gpu.mUp >= 0 )          glUniform3fv( s_gpu.mUp, 1, upS );
	if( s_gpu.mSegments >= 0 )    glUniform1f( s_gpu.mSegments, (float)kConeSegments );

	if( s_gpu.mAxisDir >= 0 )     glUniform3fv( s_gpu.mAxisDir, 1, spot.dir );
	if( s_gpu.mLen >= 0 )         glUniform1f( s_gpu.mLen, len );
	if( s_gpu.mCosInner >= 0 )    glUniform1f( s_gpu.mCosInner, spot.cosInner );
	if( s_gpu.mCosOuter >= 0 )    glUniform1f( s_gpu.mCosOuter, spot.cosOuter );
	if( s_gpu.mColor >= 0 )       glUniform3fv( s_gpu.mColor, 1, spot.color );
	// Per-spot march steps: full tier = fullSteps, cheap tier = reduced (budget).
	if( s_gpu.mSteps >= 0 )       glUniform1i( s_gpu.mSteps, steps );
	if( s_gpu.mSurfFade >= 0 )    glUniform1f( s_gpu.mSurfFade, surfFade );

	glDrawArrays( GL_TRIANGLES, 0, 3 * kConeSegments );
}

}  // anonymous namespace

void LightConeRegisterCvars()
{
	if( s_cvarTp == NULL )
		// v5: DEFAULT 0 -- the non-local third-person world light-cone (volumetric air
		// shaft) is disabled. It was repeatedly rejected/reverted; the third-person tell is
		// now a faint warm lantern dlight on the holder (csz_tpdl_*, csz_light_pass.cpp).
		// Set csz_flashlight_tp 1 to re-enable the legacy world cone for A/B only.
		s_cvarTp = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp", "0", FCVAR_CLIENTDLL );
	if( s_cvarTpIntensity == NULL )
		// Linear-HDR additive radiance scale on the energy-conserving in-scatter. With
		// the §V2 rebuild the integral is bounded by the Beer-Lambert slice + per-light
		// cap, so this is a straight brightness knob; 3.0 makes the side-on air shaft
		// clearly visible without the old white-out. Dev-tunable.
		s_cvarTpIntensity = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_intensity", "3.0", FCVAR_CLIENTDLL );
	if( s_cvarNlSurfFade == NULL )
		// Non-local (other players') beam surface-fade band scale. The air shaft tapers
		// out over the last stretch before the marched surface so it melts into the
		// separate full-res ground pool instead of piling a bright floor-dome shell.
		// 1.0 = tuned default; larger fades earlier; small reaches closer to the floor.
		s_cvarNlSurfFade = gEngfuncs.pfnRegisterVariable( "csz_flashlight_nl_surffade", "1.0", FCVAR_CLIENTDLL );
	if( s_cvarTpG == NULL )
		// World-cone HG forward anisotropy. Clamped 0.6..0.8 below (pitfall #6: g>=0.95
		// is invisible side-on -- the common third-person angle -- and blinding head-on).
		s_cvarTpG = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_g", "0.7", FCVAR_CLIENTDLL );
	if( s_cvarTpFogCouple == NULL )
		// Optional density coupling (default OFF): brighten the world cone with the
		// scene fog density instead of the cvar sigmaS/sigmaE alone. 0 = legacy.
		s_cvarTpFogCouple = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_fogcouple", "0", FCVAR_CLIENTDLL );
	if( s_cvarTpSteps == NULL )
		// Full-tier march samples. Clamp 8..32. Half-res + static jitter + the 5x5
		// bilateral upsample keep 16 smooth; bump if any residual grain remains.
		s_cvarTpSteps = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_steps", "16", FCVAR_CLIENTDLL );

	// === §V2 energy-conserving rebuild cvars ===
	if( s_cvarTpSigmaS == NULL )
		// Scattering coefficient sigmaS of the beam medium (per world unit). Larger =
		// denser-looking, brighter shaft. Paired with sigmaE (albedo = sigmaS/sigmaE).
		s_cvarTpSigmaS = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_sigmaS", "0.05", FCVAR_CLIENTDLL );
	if( s_cvarTpSigmaE == NULL )
		// Extinction coefficient sigmaE (per world unit). Bounds the Beer-Lambert slice
		// (sigmaS/sigmaE)(1-exp(-sigmaE*dt)); default == sigmaS gives albedo 1.
		s_cvarTpSigmaE = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_sigmaE", "0.05", FCVAR_CLIENTDLL );
	if( s_cvarTpCap == NULL )
		// Per-light radiance cap: bounds a SINGLE cone's accumulated in-scatter peak so
		// one near beam can never blow out alone (artistic clamp). Cross-cone overlap is
		// handled separately by the volume-buffer soft-knee.
		s_cvarTpCap = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_cap", "2.5", FCVAR_CLIENTDLL );
	if( s_cvarTpKnee == NULL )
		// Volume-buffer soft-knee compression knee (0..1). Applied to the upsampled
		// VOLUME radiance ONLY (never the HDR scene) to roll off N-cone overlap before
		// composite. Lower = compresses sooner. Artistic, not physical.
		s_cvarTpKnee = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_knee", "0.8", FCVAR_CLIENTDLL );
	if( s_cvarTpHalo == NULL )
		// Two-lobe phase halo weight w1 (0..1): forward lobe gets w0 = 1-w1, halo lobe
		// (g*0.5) gets w1. Broadens the glow around the bright core; weights stay
		// normalized (w0+w1<=1) so energy is not double-counted.
		s_cvarTpHalo = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_halo", "0.3", FCVAR_CLIENTDLL );
	if( s_cvarTpHero == NULL )
		// Hero shadow-map count. v1 does NOT implement per-cone hero shadow maps (the
		// single 1024 spot map is owned by the first-person path; a multi-hero atlas is
		// out of scope, see DESIGN-SPEC §V2 #5). Registered at 0 as a documented stub;
		// residual distant wall-bleed from the light side is an accepted v1 limitation.
		s_cvarTpHero = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_heroshadows", "0", FCVAR_CLIENTDLL );

	CSZ_LogDev( "lightcone", "cvars registered (tp/_intensity/_nl_surffade/_g/_fogcouple/_steps/_sigmaS/_sigmaE/_cap/_knee/_halo/_heroshadows)" );
}

void LightConeRender( const ViewSetup &view )
{
	// Gate 1: master switch (A/B-off contract: the world keeps the spot direct lit
	// pool, only the air volume disappears).
	if( ReadCvar( s_cvarTp, 1.0f ) < 0.5f )
		return;

	// Gate 2: needs the HDR path (a linear RGBA16F target to add into) + the
	// sampleable scene depth texture for camera-side occlusion.
	if( !SkyComposeActive() )
		return;
	GLuint depthTex = SkyComposeDepthTex();
	GLuint hdrFbo   = SkyComposeHdrFbo();
	if( depthTex == 0 || hdrFbo == 0 )
		return;

	// World-space reconstruction matrix for the per-fragment depth->world bound.
	Mat4 invViewProj;
	if( !Mat4Inverse( view.matViewProj, invViewProj ) )
		return;

	// L5R master split (csz_flashlight_v3, default 1). Under v3 the LOCAL first-person
	// beam's air volume is owned by the slot-13.5 fog march, so it is EXCLUDED here (no
	// double-draw). Non-local (third-person) cones are this pass's job.
	if( !s_lookedV3 )
	{
		s_lookedV3 = true;
		s_cvarV3 = gEngfuncs.pfnGetCvarPointer( "csz_flashlight_v3" );
	}
	bool v3 = ( ReadCvar( s_cvarV3, 1.0f ) >= 0.5f );

	// Count eligible non-local cones first: if none, do not touch the FBO at all.
	float now = ClientTime();
	int eligible = 0;
	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );
		if( !light->used || light->desc.type != kLightSpot )
			continue;
		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;
		if( light->budgetTier == kBudgetCull )
			continue;
		if( v3 && light->desc.isLocal )
			continue;                          // local beam excluded (slot 13.5 owns it)
		eligible++;
	}
	if( eligible == 0 )
		return;

	if( !EnsureBuilt() )
		return;

	// Half-res accumulation buffer (separate from HDR). div 2 = half-res per §V2.
	int fullW = view.viewport[0] + view.viewport[2];
	int fullH = view.viewport[1] + view.viewport[3];
	if( fullW < 1 ) fullW = 1;
	if( fullH < 1 ) fullH = 1;
	int halfW = fullW / 2; if( halfW < 1 ) halfW = 1;
	int halfH = fullH / 2; if( halfH < 1 ) halfH = 1;
	if( !EnsureVolTarget( halfW, halfH ) )
		return;

	// csz_flashlight_range (FogVolume-owned) caps the beam length; lazily fetched.
	if( !s_lookedRange )
	{
		s_lookedRange = true;
		s_cvarRange = gEngfuncs.pfnGetCvarPointer( "csz_flashlight_range" );
	}
	float range = ReadCvar( s_cvarRange, 1600.0f );
	float intensity = ReadCvar( s_cvarTpIntensity, 3.0f );

	float nlSurfFade = ReadCvar( s_cvarNlSurfFade, 1.0f );
	if( nlSurfFade < 0.0f ) nlSurfFade = 0.0f;

	float coneG = ReadCvar( s_cvarTpG, 0.70f );
	if( coneG < 0.60f ) coneG = 0.60f;
	if( coneG > 0.80f ) coneG = 0.80f;

	float halo = ReadCvar( s_cvarTpHalo, 0.30f );
	if( halo < 0.0f ) halo = 0.0f;
	if( halo > 1.0f ) halo = 1.0f;             // keep w0+w1<=1 (no energy double-count)

	float sigmaS = ReadCvar( s_cvarTpSigmaS, 0.05f );
	if( sigmaS < 1e-4f ) sigmaS = 1e-4f;
	float sigmaE = ReadCvar( s_cvarTpSigmaE, 0.05f );
	if( sigmaE < 1e-4f ) sigmaE = 1e-4f;

	float cap = ReadCvar( s_cvarTpCap, 2.5f );
	if( cap < 1e-3f ) cap = 1e-3f;

	float knee = ReadCvar( s_cvarTpKnee, 0.8f );
	if( knee < 0.05f ) knee = 0.05f;
	if( knee > 0.99f ) knee = 0.99f;

	// Optional density coupling (default OFF): scale intensity by a saturating fog
	// factor so beams brighten with the scene's actual fog instead of the cvar alone.
	float couple = ReadCvar( s_cvarTpFogCouple, 0.0f );
	if( couple < 0.0f ) couple = 0.0f;
	if( couple > 1.0f ) couple = 1.0f;
	if( couple > 0.0f )
	{
		float fogSigmaE = FogExtinctionFromDensity( view.ambience.fogDensity );
		float fogFactor = 1.0f - expf( -fogSigmaE * 400.0f );   // 0 (no fog) .. ~1 (thick fog)
		intensity *= ( 1.0f - couple ) + couple * fogFactor;
	}

	int cheapSteps = LightBudgetCheapSteps();
	int fullSteps = (int)( ReadCvar( s_cvarTpSteps, (float)kConeSteps ) + 0.5f );
	if( fullSteps < 8 )  fullSteps = 8;
	if( fullSteps > 32 ) fullSteps = 32;

	float fTarget[2]  = { (float)halfW, (float)halfH };
	float fFullSize[2] = { (float)fullW, (float)fullH };
	float fHalfSize[2] = { (float)halfW, (float)halfH };

	// ===================== Pass 1: half-res cone accumulation =================
	// Each non-local cone mesh adds its energy-conserving in-scatter into the separate
	// half-res buffer (kBlendAddPremul = ONE,ONE on rgb). Depth test/write OFF (no
	// depth attachment); occlusion is the in-shader scene-depth far clamp.
	BindFbo( s_vol.fbo );
	glViewport( 0, 0, halfW, halfH );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendNone );
	SetCull( false );
	glDisable( GL_SCISSOR_TEST );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	SetBlend( kBlendAddPremul );

	UseProgram( s_gpu.march.program );
	BindVao( s_gpu.vao );

	SkyComposeBindTex( 0, GL_TEXTURE_2D, depthTex );
	if( s_gpu.mDepthTex >= 0 )    glUniform1i( s_gpu.mDepthTex, kSkyTmuBase + 0 );
	if( s_gpu.mTargetSize >= 0 )  glUniform2fv( s_gpu.mTargetSize, 1, fTarget );
	if( s_gpu.mCamPos >= 0 )      glUniform3fv( s_gpu.mCamPos, 1, view.origin );
	if( s_gpu.mHgG >= 0 )         glUniform1f( s_gpu.mHgG, coneG );
	if( s_gpu.mHalo >= 0 )        glUniform1f( s_gpu.mHalo, halo );
	if( s_gpu.mSigmaS >= 0 )      glUniform1f( s_gpu.mSigmaS, sigmaS );
	if( s_gpu.mSigmaE >= 0 )      glUniform1f( s_gpu.mSigmaE, sigmaE );
	if( s_gpu.mCap >= 0 )         glUniform1f( s_gpu.mCap, cap );
	if( s_gpu.mIntensity >= 0 )   glUniform1f( s_gpu.mIntensity, intensity );
	if( s_gpu.mZNear >= 0 )       glUniform1f( s_gpu.mZNear, view.zNear );
	if( s_gpu.mZFar >= 0 )        glUniform1f( s_gpu.mZFar, view.zFar );
	if( s_gpu.mInvViewProj >= 0 ) glUniformMatrix4fv( s_gpu.mInvViewProj, 1, GL_FALSE, invViewProj.m );

	int drawnFull = 0, drawnCheap = 0;
	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );
		if( !light->used || light->desc.type != kLightSpot )
			continue;
		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;
		if( light->budgetTier == kBudgetCull )
			continue;
		if( v3 && light->desc.isLocal )
			continue;                          // local beam excluded (no double-energy)

		int steps = ( light->budgetTier == kBudgetCheap ) ? cheapSteps : fullSteps;

		SpotLightParams spot;
		g_lights.BuildSpotParams( *light, spot );
		DrawConeForSpot( view, spot, range, steps, nlSurfFade );

		if( light->budgetTier == kBudgetCheap ) drawnCheap++;
		else                                    drawnFull++;
	}

	BindVao( 0 );
	SkyComposeRestoreTmus();

	// =============== Pass 2: bilateral upsample + soft-knee + composite =======
	BindFbo( hdrFbo );
	glViewport( view.viewport[0], view.viewport[1], view.viewport[2], view.viewport[3] );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendAddPremul );
	SetCull( false );

	UseProgram( s_gpu.up.program );
	BindVao( s_gpu.vao );

	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_vol.colorTex );
	SkyComposeBindTex( 1, GL_TEXTURE_2D, depthTex );
	if( s_gpu.uInscatter >= 0 ) glUniform1i( s_gpu.uInscatter, kSkyTmuBase + 0 );
	if( s_gpu.uDepthTex >= 0 )  glUniform1i( s_gpu.uDepthTex, kSkyTmuBase + 1 );
	if( s_gpu.uFullSize >= 0 )  glUniform2fv( s_gpu.uFullSize, 1, fFullSize );
	if( s_gpu.uHalfSize >= 0 )  glUniform2fv( s_gpu.uHalfSize, 1, fHalfSize );
	if( s_gpu.uSmoothSigma >= 0 ) glUniform1f( s_gpu.uSmoothSigma, 1.5f );
	if( s_gpu.uKnee >= 0 )      glUniform1f( s_gpu.uKnee, knee );
	if( s_gpu.uZNear >= 0 )     glUniform1f( s_gpu.uZNear, view.zNear );
	if( s_gpu.uZFar >= 0 )      glUniform1f( s_gpu.uZFar, view.zFar );

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	BindVao( 0 );
	UseProgram( 0 );
	SkyComposeRestoreTmus();
	SetBlend( kBlendNone );
	// Restore the EnterTakeover baseline for the following Step-3 march / transparent
	// / viewmodel passes (HDR FBO still bound; depth test+write back on).
	SetDepthTest( true );
	SetDepthWrite( true );

	int drawn = drawnFull + drawnCheap;
	static float s_nextStats;
	if( drawn > 0 && now >= s_nextStats )
	{
		s_nextStats = now + 1.0f;
		CSZ_LogDev( "lightcone", "world beams drawn: %d (full %d + cheap %d)", drawn, drawnFull, drawnCheap );
	}
}

void LightConeShutdown()
{
	bool sameContext = ( s_vol.gpuGeneration == GpuGeneration() );
	if( sameContext )
		DestroyVolSameContext();
	else
		ForgetVol();

	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
		DestroyGpuSameContext();
	else
		ForgetGpu();
}

}  // namespace csz
