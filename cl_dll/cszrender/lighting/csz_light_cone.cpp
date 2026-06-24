/*
 * csz_light_cone.cpp -- CSOZ renderer: third-person flashlight cone INDICATOR (slot 13.4)
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
// Slot 13.4 render order (csz_renderer.cpp): AFTER RunLightPasses (spot DIRECT additive
// world+studio lighting + the non-local ground pool) and BEFORE the Step-3 first-person
// fog march (slot 13.5).
//
// === SPEC v3 (USER 2026-06-24): VERY FAINT, SHARP cone indicator only ===
// MECHANISM PIVOT. v1/v2 drew the third-person cone as a VOLUMETRIC in-scatter glow (a lit
// air shaft). The USER rejected that twice on the real 31-bot machine (hazy glow that, in
// dense overlap, washed into a detail-less bright blob and dimmed the sky). v3 splits the
// third-person flashlight into two independent jobs:
//   (1) CLEAR FOG inside the cone (see THROUGH to lit surfaces + enemies): an extension of
//       the first-person csz_flashlight_defog, applied in the WORLD + STUDIO base fragment
//       shaders (NOT here). See FogVolumeNonLocalDefogCones + csz_world_shaders.inl /
//       csz_studio_shaders.inl. That defog is idempotent and MAX-combined across cones.
//   (2) This pass draws ONLY a VERY FAINT, SHARP cone-shape INDICATOR -- a flat, hard-edged,
//       low-brightness cone shell so a third-person observer sees "someone is shining a
//       flashlight there", with NO hazy volumetric god-ray.
// NO-BRIGHTEN OVERLAP is STRUCTURAL: the indicator is composited with glBlendEquation(GL_MAX)
// (kBlendMax) -> N overlapping cones = max(...) = a SINGLE cone's brightness, never a sum.
// There is no half-res buffer, no march, no bilateral upsample, no soft-knee: a single direct
// full-res cone-mesh pass into HDR. The LOCAL first-person beam is excluded (slot 13.5 owns
// it; the local defog is the world FS view-ray term) so there is no double-draw.
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

namespace csz
{

#include "csz_light_cone_shaders.inl"   // kConeVs / kConeFs (faint sharp indicator)

namespace
{

// Radial wedge count of the procedural cone shell (smooth rim at any near distance; a
// flashlight cone is small on screen so 64 is plenty). MUST match the draw count.
const int kConeSegments = 64;

// --- cvars (read live each frame) --------------------------------------------
cvar_t *s_cvarTp;          // csz_flashlight_tp        default "1": third-person cone indicator on (0 = off, A/B)
cvar_t *s_cvarTpEdge;      // csz_flashlight_tp_edge   default "0.12": indicator brightness (linear HDR; very faint)
cvar_t *s_cvarRange;       // csz_flashlight_range (owned by FogVolume); caps indicator length. Fetched lazily.
bool    s_lookedRange;

// --- GPU resources (generation-keyed; forget on a foreign context) -----------
struct ConeGpu
{
	ShaderProgram prog;      // cone-mesh indicator program (kConeVs + kConeFs)
	GLuint vao;
	int    gpuGeneration;
	bool   built;
	bool   failedThisGen;

	int uMatViewProj, uApex, uAxis, uRight, uUp, uSegments;   // VS
	int uDepthTex, uFullSize, uColor, uEdge, uCamPos;         // FS
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

	if( !BuildProgram( "csz_light_cone_indicator", kConeVs, kConeFs, false, s_gpu.prog ) )
	{
		if( s_gpu.prog.program != 0 ) DestroyProgram( s_gpu.prog );
		s_gpu.prog.program = 0;
		s_gpu.failedThisGen = true;
		CSZ_LogError( "lightcone", "indicator shader build failed; third-person cone disabled this generation" );
		return false;
	}

	s_gpu.uMatViewProj = UniformLoc( s_gpu.prog, "u_matViewProj" );
	s_gpu.uApex        = UniformLoc( s_gpu.prog, "u_apex" );
	s_gpu.uAxis        = UniformLoc( s_gpu.prog, "u_axis" );
	s_gpu.uRight       = UniformLoc( s_gpu.prog, "u_right" );
	s_gpu.uUp          = UniformLoc( s_gpu.prog, "u_up" );
	s_gpu.uSegments    = UniformLoc( s_gpu.prog, "u_segments" );
	s_gpu.uDepthTex    = UniformLoc( s_gpu.prog, "u_depthTex" );
	s_gpu.uFullSize    = UniformLoc( s_gpu.prog, "u_fullSize" );
	s_gpu.uColor       = UniformLoc( s_gpu.prog, "u_color" );
	s_gpu.uEdge        = UniformLoc( s_gpu.prog, "u_edge" );
	s_gpu.uCamPos      = UniformLoc( s_gpu.prog, "u_camPos" );   // FIX-3: silhouette-rim view dir

	s_gpu.built = true;
	CSZ_LogDev( "lightcone", "cone indicator program built (gpu gen %d)", s_gpu.gpuGeneration );
	return true;
}

// Orthonormal basis perpendicular to the cone axis (any consistent choice; the
// cone is rotationally symmetric so the seam location is irrelevant).
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

// Draw ONE spot's cone shell into the bound HDR buffer (MAX blend). Shared state (FBO/
// viewport/blend/program/VAO/depth tex + the frame-constant FS uniforms) is set by the
// caller; this pushes the per-spot mesh-shaping uniforms + draws.
void DrawConeForSpot( const ViewSetup &view, const SpotLightParams &spot, float range )
{
	float len = spot.radius;
	if( range > 0.0f && range < len )
		len = range;                       // csz_flashlight_range caps the indicator length
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

	glDrawArrays( GL_TRIANGLES, 0, 3 * kConeSegments );
}

}  // anonymous namespace

void LightConeRegisterCvars()
{
	if( s_cvarTp == NULL )
		// Default 1 = the third-person cone-shape INDICATOR (the faint sharp tell). 0 = off
		// (A/B; the spot DIRECT lit pool + the non-local defog stay; only the tell disappears).
		s_cvarTp = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp", "1", FCVAR_CLIENTDLL );
	if( s_cvarTpEdge == NULL )
		// Indicator brightness (linear-HDR). VERY faint by design (USER: "很淡"): the tell must
		// be visible against the dark fog without reading as a glowing volume. MAX-blended so
		// overlap never exceeds this. Dev-tunable; raise slightly if the tell is too subtle.
		s_cvarTpEdge = gEngfuncs.pfnRegisterVariable( "csz_flashlight_tp_edge", "0.12", FCVAR_CLIENTDLL );

	CSZ_LogDev( "lightcone", "cvars registered (tp / tp_edge)" );
}

void LightConeRender( const ViewSetup &view )
{
	// Gate 1: master switch (A/B-off contract: the world keeps the spot direct lit pool + the
	// non-local defog; only the faint cone indicator disappears).
	if( ReadCvar( s_cvarTp, 1.0f ) < 0.5f )
		return;

	// Gate 2: needs the HDR path (a linear RGBA16F target to draw into) + the sampleable scene
	// depth texture for the per-fragment occlusion test.
	if( !SkyComposeActive() )
		return;
	GLuint depthTex = SkyComposeDepthTex();
	GLuint hdrFbo   = SkyComposeHdrFbo();
	if( depthTex == 0 || hdrFbo == 0 )
		return;

	// Count eligible NON-LOCAL cones first: if none, do not touch the FBO at all. The LOCAL
	// first-person beam is excluded (no third-person tell for your own light; slot 13.5 owns it).
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
		if( light->desc.isLocal )
			continue;                          // local beam excluded (no third-person tell)
		eligible++;
	}
	if( eligible == 0 )
		return;

	if( !EnsureBuilt() )
		return;

	// csz_flashlight_range (FogVolume-owned) caps the indicator length; lazily fetched.
	if( !s_lookedRange )
	{
		s_lookedRange = true;
		s_cvarRange = gEngfuncs.pfnGetCvarPointer( "csz_flashlight_range" );
	}
	float range = ReadCvar( s_cvarRange, 1600.0f );

	float edge = ReadCvar( s_cvarTpEdge, 0.12f );
	if( edge < 0.0f ) edge = 0.0f;

	int fullW = view.viewport[0] + view.viewport[2];
	int fullH = view.viewport[1] + view.viewport[3];
	if( fullW < 1 ) fullW = 1;
	if( fullH < 1 ) fullH = 1;
	float fFullSize[2] = { (float)fullW, (float)fullH };
	const float kWarmWhite[3] = { 1.0f, 0.95f, 0.85f };   // uniform tint -> all cones equally faint (MAX-safe)

	// ===================== Single direct full-res indicator pass ==============
	// kBlendMax (glBlendEquation(GL_MAX)): dst = max(srcCone, dst). Every cone is the SAME faint
	// warm constant, so N overlapping shells composite to exactly that single faint brightness --
	// overlap CANNOT brighten (structural, per USER §1). Depth test OFF (the FS does the raw
	// depth-compare occlusion); depth write OFF (never disturb the scene depth).
	BindFbo( hdrFbo );
	glViewport( view.viewport[0], view.viewport[1], view.viewport[2], view.viewport[3] );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendMax );
	SetCull( false );

	UseProgram( s_gpu.prog.program );
	BindVao( s_gpu.vao );

	SkyComposeBindTex( 0, GL_TEXTURE_2D, depthTex );
	if( s_gpu.uDepthTex >= 0 ) glUniform1i( s_gpu.uDepthTex, kSkyTmuBase + 0 );
	if( s_gpu.uFullSize >= 0 ) glUniform2fv( s_gpu.uFullSize, 1, fFullSize );
	if( s_gpu.uColor >= 0 )    glUniform3fv( s_gpu.uColor, 1, kWarmWhite );
	if( s_gpu.uEdge >= 0 )     glUniform1f( s_gpu.uEdge, edge );
	if( s_gpu.uCamPos >= 0 )   glUniform3fv( s_gpu.uCamPos, 1, view.origin );   // FIX-3: silhouette rim view dir (frame-constant)

	int drawn = 0;
	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );
		if( !light->used || light->desc.type != kLightSpot )
			continue;
		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;
		if( light->budgetTier == kBudgetCull )
			continue;
		if( light->desc.isLocal )
			continue;

		SpotLightParams spot;
		g_lights.BuildSpotParams( *light, spot );
		DrawConeForSpot( view, spot, range );
		drawn++;
	}

	BindVao( 0 );
	UseProgram( 0 );
	SkyComposeRestoreTmus();
	SetBlend( kBlendNone );   // resets glBlendEquation back to GL_FUNC_ADD for following passes
	// Restore the EnterTakeover baseline for the following Step-3 march / transparent / viewmodel.
	SetDepthTest( true );
	SetDepthWrite( true );

	static float s_nextStats;
	if( drawn > 0 && now >= s_nextStats )
	{
		s_nextStats = now + 1.0f;
		CSZ_LogDev( "lightcone", "third-person cone indicators drawn: %d", drawn );
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
