/*
 * csz_light_cone.cpp -- CSOZ renderer: world-space visible flashlight beam volume (L6a)
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
// L6a render order (csz_renderer.cpp): AFTER RunLightPasses (spot DIRECT additive
// world+studio lighting, slot 13) and BEFORE the Step-3 first-person fog march
// (slot 13.5). Both add linear HDR radiance into the SAME bound HDR FBO, so the
// order among the additive passes is commutative; placing it right after the spot
// direct add matches the plan ordering (opaque -> spot direct -> cone additive ->
// dust L7). Reuses g_lights (the spot registry) + the fog Step-1 depth helpers.
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
#include "csz_light_cone_shaders.inl"   // kConeVs / kConeFsBody

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// Radial wedge count of the procedural cone (smooth rim at any near distance; a
// flashlight cone is small on screen so 64 is plenty). MUST match the draw count.
const int kConeSegments = 64;

// Bounded march samples through the cone volume. Only cone-silhouette pixels run
// this, so the cost is local; 16 anti-bands the gradient without temporal noise.
const int kConeSteps = 16;


// --- cvars (read live each frame) --------------------------------------------
cvar_t *s_cvarTp;          // csz_flashlight_tp           default "1": world beam visible (0 = off, A/B)
cvar_t *s_cvarTpIntensity; // csz_flashlight_tp_intensity default "1.5": beam brightness (dev tuning)
cvar_t *s_cvarNlSurfFade;  // csz_flashlight_nl_surffade default "1.0": non-local surface-fade aggressiveness (>0; bigger = beam dies further from surfaces; 0 = legacy patch, for A/B)
cvar_t *s_cvarRange;       // csz_flashlight_range (owned by FogVolume); caps beam length. Fetched lazily.
bool    s_lookedRange;
cvar_t *s_cvarV3;          // csz_flashlight_v3 (owned by light_pass); L5R master A/B. Fetched lazily.
cvar_t *s_cvarTpG;         // csz_flashlight_tp_g         default "0.7": §5.3 world-cone HG g (unify with the shaft; was const 0.35)
cvar_t *s_cvarTpSteps;     // csz_flashlight_tp_steps     default "16": full-tier world-cone march samples (clamp 8..32). Live knob for the visual gate now that animated IGN replaces the static dither grid; bump if any residual noise.
cvar_t *s_cvarTpFogCouple; // csz_flashlight_tp_fogcouple default "0": §5.3 density-couple amount 0..1 (0 = vacuum/legacy, 1 = beam scales with fog)
bool    s_lookedV3;

// --- GPU resources (generation-keyed; forget on a foreign context) -----------
struct ConeGpu
{
	ShaderProgram prog;
	GLuint vao;
	int    gpuGeneration;
	bool   built;
	bool   failedThisGen;

	int uMatViewProj, uApex, uAxis, uRight, uUp, uSegments;
	int uDepthTex, uViewSize, uCamPos, uAxisDir, uLen;
	int uCosInner, uCosOuter, uColor, uIntensity, uHgG, uSteps;
	int uInvViewProj, uZNear, uZFar, uSurfFade, uFrame;
};
ConeGpu s_gpu;

float ReadCvar( cvar_t *cv, float fallback )
{
	return ( cv != NULL ) ? cv->value : fallback;
}

void ForgetGpu()
{
	s_gpu.vao = 0;
	s_gpu.prog.program = 0;
	s_gpu.built = false;
	s_gpu.failedThisGen = false;
}

void DestroyGpuSameContext()
{
	if( s_gpu.vao != 0 )
		glDeleteVertexArrays( 1, &s_gpu.vao );
	if( s_gpu.prog.program != 0 )
		DestroyProgram( s_gpu.prog );
	ForgetGpu();
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

	// Final FS = "#version 330 core" + shared Step-1 reconstruct helpers + body.
	std::string fs = std::string( "#version 330 core\n" ) + kFogDepthReconstructGlsl + kConeFsBody;
	if( !BuildProgram( "csz_light_cone", kConeVs, fs.c_str(), false, s_gpu.prog ) )
	{
		s_gpu.failedThisGen = true;
		CSZ_LogError( "lightcone", "shader build failed; world beam disabled this generation" );
		return false;
	}

	s_gpu.uMatViewProj = UniformLoc( s_gpu.prog, "u_matViewProj" );
	s_gpu.uApex        = UniformLoc( s_gpu.prog, "u_apex" );
	s_gpu.uAxis        = UniformLoc( s_gpu.prog, "u_axis" );
	s_gpu.uRight       = UniformLoc( s_gpu.prog, "u_right" );
	s_gpu.uUp          = UniformLoc( s_gpu.prog, "u_up" );
	s_gpu.uSegments    = UniformLoc( s_gpu.prog, "u_segments" );
	s_gpu.uDepthTex    = UniformLoc( s_gpu.prog, "u_depthTex" );
	s_gpu.uViewSize    = UniformLoc( s_gpu.prog, "u_viewSize" );
	s_gpu.uCamPos      = UniformLoc( s_gpu.prog, "u_camPos" );
	s_gpu.uAxisDir     = UniformLoc( s_gpu.prog, "u_axisDir" );
	s_gpu.uLen         = UniformLoc( s_gpu.prog, "u_len" );
	s_gpu.uCosInner    = UniformLoc( s_gpu.prog, "u_cosInner" );
	s_gpu.uCosOuter    = UniformLoc( s_gpu.prog, "u_cosOuter" );
	s_gpu.uColor       = UniformLoc( s_gpu.prog, "u_color" );
	s_gpu.uIntensity   = UniformLoc( s_gpu.prog, "u_intensity" );
	s_gpu.uHgG         = UniformLoc( s_gpu.prog, "u_hgG" );
	s_gpu.uSteps       = UniformLoc( s_gpu.prog, "u_steps" );
	s_gpu.uInvViewProj = UniformLoc( s_gpu.prog, "u_invViewProj" );
	s_gpu.uZNear       = UniformLoc( s_gpu.prog, "u_zNear" );
	s_gpu.uZFar        = UniformLoc( s_gpu.prog, "u_zFar" );
	s_gpu.uSurfFade    = UniformLoc( s_gpu.prog, "u_surfFade" );
	s_gpu.uFrame       = UniformLoc( s_gpu.prog, "u_frame" );	// animated IGN temporal offset

	s_gpu.built = true;
	CSZ_LogDev( "lightcone", "world beam program built (gpu gen %d)", s_gpu.gpuGeneration );
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

// Draw ONE spot's beam volume. State (FBO/viewport/blend/depth/program/VAO/depth
// tex) is set up once by the caller; this only pushes per-spot uniforms + draws.
void DrawConeForSpot( const ViewSetup &view, const SpotLightParams &spot,
                      float range, float intensity, int steps, float surfFade )
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

	if( s_gpu.uMatViewProj >= 0 ) glUniformMatrix4fv( s_gpu.uMatViewProj, 1, GL_FALSE, view.matViewProj.m );
	if( s_gpu.uApex >= 0 )        glUniform3fv( s_gpu.uApex, 1, spot.origin );
	if( s_gpu.uAxis >= 0 )        glUniform3fv( s_gpu.uAxis, 1, axis );
	if( s_gpu.uRight >= 0 )       glUniform3fv( s_gpu.uRight, 1, rightS );
	if( s_gpu.uUp >= 0 )          glUniform3fv( s_gpu.uUp, 1, upS );
	if( s_gpu.uSegments >= 0 )    glUniform1f( s_gpu.uSegments, (float)kConeSegments );

	if( s_gpu.uAxisDir >= 0 )     glUniform3fv( s_gpu.uAxisDir, 1, spot.dir );
	if( s_gpu.uLen >= 0 )         glUniform1f( s_gpu.uLen, len );
	if( s_gpu.uCosInner >= 0 )    glUniform1f( s_gpu.uCosInner, spot.cosInner );
	if( s_gpu.uCosOuter >= 0 )    glUniform1f( s_gpu.uCosOuter, spot.cosOuter );
	if( s_gpu.uColor >= 0 )       glUniform3fv( s_gpu.uColor, 1, spot.color );
	if( s_gpu.uIntensity >= 0 )   glUniform1f( s_gpu.uIntensity, intensity );
	// Per-spot march steps: full tier = kConeSteps, cheap tier = reduced (L6b budget).
	if( s_gpu.uSteps >= 0 )       glUniform1i( s_gpu.uSteps, steps );
	// Non-local beams fade out before any surface (no deposited 圈); local keeps tight.
	if( s_gpu.uSurfFade >= 0 )    glUniform1f( s_gpu.uSurfFade, surfFade );

	glDrawArrays( GL_TRIANGLES, 0, 3 * kConeSegments );
}

}  // anonymous namespace

void LightConeRegisterCvars()
{
	if( s_cvarTp == NULL )
		// Default 1 = the USER-desired third-person visible beam. 0 = off (A/B; the
		// spot DIRECT lit pool stays, only the air volume goes away).
		s_cvarTp = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp", "1", FCVAR_CLIENTDLL );
	if( s_cvarTpIntensity == NULL )
		// Linear-HDR additive radiance scale. 3.0 makes the air shaft PROMINENTLY
		// visible from the side (the user ask: 明显看见光体积) without blowing out --
		// AGENT_OBSERVED in the L6a visual gate (1.5 read faint, 3.0 unmistakable;
		// additive delta scales exactly 2x). Dev-tunable down for a subtler beam.
		s_cvarTpIntensity = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_intensity", "3.0", FCVAR_CLIENTDLL );
	if( s_cvarNlSurfFade == NULL )
		// Non-local (other players') beam surface-fade aggressiveness. Scales the
		// shader's wide occlusion band so the air shaft dies well BEFORE any surface
		// -> no deposited lit patch (圈), only the airborne 光柱 (operator T4 ask).
		// 1.0 = tuned to fully kill the patch; larger fades even earlier; 0 = legacy
		// non-local (tight band, surface patch returns) for A/B.
		s_cvarNlSurfFade = gEngfuncs.pfnRegisterVariable( "csz_flashlight_nl_surffade", "1.0", FCVAR_CLIENTDLL );
	if( s_cvarTpG == NULL )
		// §5.3: unify the 3rd-person world cone with the first-person shaft -- raise the HG g
		// from the old vacuum 0.35 to the physical fog 0.70 (clamped 0.5-0.85 below). Tighter,
		// crisper world beam that reads as the same phenomenon as the flashlight march.
		s_cvarTpG = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_g", "0.7", FCVAR_CLIENTDLL );
	if( s_cvarTpFogCouple == NULL )
		// §5.3 (lower-risk, default OFF): density-couple the world cone so other players' beams
		// brighten with fog instead of scattering in vacuum. 0 = legacy (beam always visible,
		// no regression); 1 = beam intensity scales fully with fog density. Live-tunable.
		s_cvarTpFogCouple = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_fogcouple", "0", FCVAR_CLIENTDLL );

	if( s_cvarTpSteps == NULL )
		// Full-tier world-cone march samples. 16 anti-bands the gradient; now that the
		// dither is animated (golden-ratio per-frame lattice) the eye integrates the
		// noise to smooth, so 16 is normally plenty. Live knob 8..32 for the visual gate
		// to bump if any residual speckle remains (perf is local: cone-silhouette pixels only).
		s_cvarTpSteps = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_steps", "16", FCVAR_CLIENTDLL );

	CSZ_LogDev( "lightcone", "cvars registered (csz_flashlight_tp/_tp_intensity/_nl_surffade/_tp_g/_tp_fogcouple/_tp_steps)" );
}

void LightConeRender( const ViewSetup &view )
{
	// Gate 1: master switch (A/B-off contract: the world keeps the spot direct lit
	// pool, only the air volume disappears -> IEEE-exact when off, this pass no-ops).
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

	if( !EnsureBuilt() )
		return;

	// World-space reconstruction matrix for the per-fragment depth->world fade.
	Mat4 invViewProj;
	if( !Mat4Inverse( view.matViewProj, invViewProj ) )
		return;

	// csz_flashlight_range (FogVolume-owned) caps the beam length; lazily fetched.
	if( !s_lookedRange )
	{
		s_lookedRange = true;
		s_cvarRange = gEngfuncs.pfnGetCvarPointer( "csz_flashlight_range" );
	}
	float range = ReadCvar( s_cvarRange, 1600.0f );
	float intensity = ReadCvar( s_cvarTpIntensity, 3.0f );
	// Non-local surface-fade aggressiveness (scales the shader's wide occlusion band).
	float nlSurfFade = ReadCvar( s_cvarNlSurfFade, 1.0f );
	if( nlSurfFade < 0.0f ) nlSurfFade = 0.0f;

	// §5.3: world-cone HG g, clamped to the physical fog window 0.5-0.85 (default 0.70,
	// unified with the flashlight march so 1st- and 3rd-person beams read as one medium).
	float coneG = ReadCvar( s_cvarTpG, 0.70f );
	if( coneG < 0.50f ) coneG = 0.50f;
	if( coneG > 0.85f ) coneG = 0.85f;

	// §5.3 density coupling (default OFF = no regression): brighten the world cone with fog
	// instead of scattering in vacuum. couple=0 -> identity; couple=1 -> intensity scales
	// fully with a saturating fog factor (1-exp(-sigmaE*ref)) over a ~400u reference depth.
	float couple = ReadCvar( s_cvarTpFogCouple, 0.0f );
	if( couple < 0.0f ) couple = 0.0f;
	if( couple > 1.0f ) couple = 1.0f;
	if( couple > 0.0f )
	{
		float sigmaE = FogExtinctionFromDensity( view.ambience.fogDensity );
		float fogFactor = 1.0f - expf( -sigmaE * 400.0f );   // 0 (no fog) .. ~1 (thick fog)
		intensity *= ( 1.0f - couple ) + couple * fogFactor;
	}

	// L5R master switch: when on, the local first-person beam's air volume is rendered by the
	// L5 fog march (shadowed, view-aligned), so its redundant + dome-prone world cone is
	// skipped below. Non-local (3rd-person) world beams are UNCHANGED (kept at full intensity
	// + legacy profile) -- the L6/L6a/L6b third-person cone is preserved exactly.
	if( !s_lookedV3 )
	{
		s_lookedV3 = true;
		s_cvarV3 = gEngfuncs.pfnGetCvarPointer( "csz_flashlight_v3" );
	}
	bool v3 = ( ReadCvar( s_cvarV3, 1.0f ) >= 0.5f );

	float fViewSize[2] = { (float)( view.viewport[0] + view.viewport[2] ),
	                       (float)( view.viewport[1] + view.viewport[3] ) };
	if( fViewSize[0] < 1.0f ) fViewSize[0] = 1.0f;
	if( fViewSize[1] < 1.0f ) fViewSize[1] = 1.0f;

	// --- shared state: add into the HDR FBO, depth test/write OFF (the beam is a
	// volume sampled via a mesh proxy; HW depth-test on a single proxy face would
	// over-occlude -> occlusion is done in-shader by clamping to the scene depth,
	// which also gives the SOFT fade). Sampling the depth attachment is safe: we
	// never write it (same contract as the fog Step-3 upsample). ---
	BindFbo( hdrFbo );
	glViewport( view.viewport[0], view.viewport[1], view.viewport[2], view.viewport[3] );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendAddPremul );
	SetCull( false );

	UseProgram( s_gpu.prog.program );
	BindVao( s_gpu.vao );

	SkyComposeBindTex( 0, GL_TEXTURE_2D, depthTex );
	if( s_gpu.uDepthTex >= 0 )    glUniform1i( s_gpu.uDepthTex, kSkyTmuBase + 0 );
	if( s_gpu.uViewSize >= 0 )    glUniform2fv( s_gpu.uViewSize, 1, fViewSize );
	if( s_gpu.uCamPos >= 0 )      glUniform3fv( s_gpu.uCamPos, 1, view.origin );
	if( s_gpu.uHgG >= 0 )         glUniform1f( s_gpu.uHgG, coneG );	// §5.3 unified physical g (was const kConeHgG 0.35)
	if( s_gpu.uInvViewProj >= 0 ) glUniformMatrix4fv( s_gpu.uInvViewProj, 1, GL_FALSE, invViewProj.m );
	if( s_gpu.uZNear >= 0 )       glUniform1f( s_gpu.uZNear, view.zNear );
	if( s_gpu.uZFar >= 0 )        glUniform1f( s_gpu.uZFar, view.zFar );

	// Animated-IGN frame offset: a monotonically-advancing counter (wrapped to keep
	// float precision) so the dither lattice shifts every frame -- breaks the static
	// per-pixel noise grid (网点) into temporally-decorrelated noise the eye integrates
	// to smooth. Same range-derived 1024 wrap as the first-person fog march. Uploaded
	// once per frame (identical for every spot in the loop below).
	static unsigned int s_coneFrame = 0u;
	s_coneFrame = ( s_coneFrame + 1u ) & 1023u;
	if( s_gpu.uFrame >= 0 )       glUniform1f( s_gpu.uFrame, (float)s_coneFrame );

	// One beam per registered spot light. L6b hard cap (LightBudgetCompute, run
	// earlier this frame): full-tier beams march kConeSteps, cheap-tier beams march
	// the reduced step count, culled beams (off-screen or over budget) are skipped
	// entirely -- bounding the expensive volumetric work to maxFull+maxCheap cones
	// no matter how many players light up. A single beam is always tier full ->
	// identical to L6a.
	float now = ClientTime();
	int cheapSteps = LightBudgetCheapSteps();
	// Full-tier march samples: cvar-overridable (default kConeSteps); clamp 8..32.
	int fullSteps = (int)( ReadCvar( s_cvarTpSteps, (float)kConeSteps ) + 0.5f );
	if( fullSteps < 8 )  fullSteps = 8;
	if( fullSteps > 32 ) fullSteps = 32;
	int drawnFull = 0, drawnCheap = 0;
	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );
		if( !light->used || light->desc.type != kLightSpot )
			continue;
		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;
		if( light->budgetTier == kBudgetCull )
			continue;	// over budget or off-screen: no air volume

		// L5R: the local first-person beam's air volume is rendered by the L5 fog march
		// (shadowed, view-aligned), so skip its redundant + dome-prone world cone here.
		// Non-local (3rd-person) cones still draw -- L6 third-person world beams are kept.
		if( v3 && light->desc.isLocal )
			continue;

		int steps = ( light->budgetTier == kBudgetCheap ) ? cheapSteps : fullSteps;

		SpotLightParams spot;
		g_lights.BuildSpotParams( *light, spot );
		// Non-local beams (other players' flashlights) get the wide surface fade so they
		// read as a pure airborne 光柱 with no surface patch (operator T4 ask): u_surfFade
		// carries the csz_flashlight_nl_surffade scale (>0 = non-local aggressiveness).
		// Local first-person cones (only drawn when v3 0) pass 0 -> tight legacy band.
		float surfFade = ( v3 && !light->desc.isLocal ) ? nlSurfFade : 0.0f;
		DrawConeForSpot( view, spot, range, intensity, steps, surfFade );

		if( light->budgetTier == kBudgetCheap ) drawnCheap++;
		else                                    drawnFull++;
	}
	int drawn = drawnFull + drawnCheap;

	BindVao( 0 );
	UseProgram( 0 );
	SkyComposeRestoreTmus();
	SetBlend( kBlendNone );
	// Restore the EnterTakeover baseline for the following Step-3 march / transparent
	// / viewmodel passes (HDR FBO still bound; depth test+write back on).
	SetDepthTest( true );
	SetDepthWrite( true );

	static float s_nextStats;
	if( drawn > 0 && now >= s_nextStats )
	{
		s_nextStats = now + 1.0f;
		CSZ_LogDev( "lightcone", "world beams drawn: %d (full %d + cheap %d)", drawn, drawnFull, drawnCheap );
	}
}

void LightConeShutdown()
{
	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
		DestroyGpuSameContext();
	else
		ForgetGpu();
}

}  // namespace csz
