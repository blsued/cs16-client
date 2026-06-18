/*
 * csz_volumetric.cpp -- CSOZ renderer: volumetric light cone (in-scatter shaft)
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
// Dependency rule (spec 4.6): lighting/ is a PURE lighting client; this
// light-scattering pass lives in lighting/ (NOT flashlight/, which carries no
// rendering code). Reads g_lights, builds SpotLightParams, draws its OWN new
// shader program -- it never touches the fog/world/studio/sky programs (pitfall
// 23: no u_fog / u_ambTint added to any existing program).
#include "csz_volumetric.h"
#include "csz_light_registry.h"
#include "../core/csz_engine.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_log.h"
#include "../core/csz_math.h"
#include "../core/csz_shader.h"

#include <math.h>
#include <stddef.h>

namespace csz
{

VolumetricPass g_volumetric;	// zero-initialized (static storage duration)

#include "csz_volumetric_shaders.inl"

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// Cone tessellation: apex + a base ring. 24 radial segments reads as a smooth
// shaft at gameplay FOV and is trivially cheap to rebuild per frame.
const int kConeSegments = 24;

// Default look tuning. scatterCoef/strength chosen so the shaft is clearly
// visible yet subtle at the default flashlight intensity (1.5) and fog density
// (0.0018) -- soft light-in-air, not a solid beam (the FS soft-cap enforces the
// ceiling). u_strength is a CSZ_DEV_TOOLS tunable so the visual agent can dial
// the look without a rebuild.
const float kDefaultScatter = 0.035f;
const float kDefaultStrength = 1.0f;

const int kStepsMin = 4;
const int kStepsMax = 96;

// QUALITY TIERS (B-class; A-class cone+fog unchanged across all tiers; no tier
// grants extra visibility -- the shaft is purely additive light-in-fog):
//   LOW : csz_flashlight_volumetric 0  (no shaft) + csz_light_shadow 0
//   MED : csz_flashlight_volumetric 1, steps ~24  + no shadow-carve
//   HIGH: csz_flashlight_volumetric 1, steps ~48  + csz_light_shadow 1 (carved)
// Shadow-carve in the shaft is automatic when csz_light_shadow=1 AND the light
// owns the single shadow map (shadowTexSlot != 0) -- no separate cvar.

cvar_t *s_cvarEnable;	// csz_flashlight_volumetric (master on/off; B-class)
cvar_t *s_cvarSteps;	// csz_flashlight_volumetric_steps (main quality/perf knob)
cvar_t *s_cvarQuality;	// csz_volumetric_quality (0 OFF / 1 LOW / 2 MED / 3 HIGH)
#if defined( CSZ_DEV_TOOLS )
cvar_t *s_cvarStrength;	// csz_flashlight_volumetric_strength (dev look tunable)
#endif

struct ConeVertex
{
	float pos[3];
};

}

struct VolumetricPass::Gpu
{
	ShaderProgram program;
	unsigned int vao;
	unsigned int vbo;
	int gpuGeneration;
	bool built;

	int uViewProj, uCamPos, uLightOrigin, uLightDir, uLightColor;
	int uRadius, uCosInner, uCosOuter, uScatter, uStrength, uFogDensity, uSteps;
	int uMatShadow, uShadowMap, uHasShadow;
	int uSceneDepth, uHasSceneDepth, uScreenSize, uZNear, uZFar;

	// Scene-depth copy (MED/HIGH scene-depth clamp): a plain (non-compare)
	// GL_DEPTH_COMPONENT24 engine texture sized to the viewport, created once
	// (resized only on viewport change), never per-frame. 0 = unavailable.
	int depthTexSlot;	// engine slot (GL_CreateTexture); 0 = none
	int depthW, depthH;	// dimensions the copy texture was allocated at
	bool depthFailed;	// creation failed this generation -> stop retrying

	// Per-frame rebuilt cone hull as a non-indexed GL_TRIANGLES list: 2 triangles
	// per segment (one side wall + one base cap), 3 verts each = kConeSegments*6.
	ConeVertex verts[kConeSegments * 6];
	int numVerts;
};

namespace
{
VolumetricPass::Gpu s_vol;
}

void VolumetricPass::RegisterCvars()
{
	if( s_cvarEnable == NULL )
		s_cvarEnable = gEngfuncs.pfnRegisterVariable( "csz_flashlight_volumetric", "1", FCVAR_CLIENTDLL );
	if( s_cvarSteps == NULL )
		s_cvarSteps = gEngfuncs.pfnRegisterVariable( "csz_flashlight_volumetric_steps", "32", FCVAR_CLIENTDLL );
	if( s_cvarQuality == NULL )
		s_cvarQuality = gEngfuncs.pfnRegisterVariable( "csz_volumetric_quality", "2", FCVAR_CLIENTDLL );
#if defined( CSZ_DEV_TOOLS )
	if( s_cvarStrength == NULL )
		s_cvarStrength = gEngfuncs.pfnRegisterVariable( "csz_flashlight_volumetric_strength", "1.0", FCVAR_CLIENTDLL );
	CSZ_LogDev( "lighting", "volumetric dev tunable registered (csz_flashlight_volumetric_strength)" );
#endif
}

void VolumetricPass::EnsureBuilt()
{
	if( s_vol.built && s_vol.gpuGeneration == GpuGeneration() )
		return;

	// GPU generation bumped (context loss / vid restart): forget stale names,
	// never glDelete them (the owning context is gone) -- same rule as sky/world.
	s_vol.gpuGeneration = GpuGeneration();

	// Forget the scene-depth copy texture too; it is re-created lazily on demand
	// (MED/HIGH only) at the live viewport size.
	s_vol.depthTexSlot = 0;
	s_vol.depthW = 0;
	s_vol.depthH = 0;
	s_vol.depthFailed = false;

	glGenVertexArrays( 1, &s_vol.vao );
	glGenBuffers( 1, &s_vol.vbo );

	BindVao( s_vol.vao );
	glBindBuffer( GL_ARRAY_BUFFER, s_vol.vbo );
	// Allocate the max-size dynamic buffer once; refilled each frame.
	glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)sizeof( s_vol.verts ), NULL, GL_DYNAMIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, sizeof( ConeVertex ), (const void *)0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	BindVao( 0 );

	// Runtime program: a compile failure must NOT be fatal (the A-class cone+fog
	// keep working without the shaft). Log the full info log and disable.
	if( !BuildProgram( "csz_volumetric", kVolumetricVs, kVolumetricFs, false, s_vol.program ))
	{
		CSZ_LogError( "lighting", "volumetric shader build failed; shaft disabled this session" );
		s_vol.built = true;	// don't retry every frame; program==0 gates Render
		return;
	}

	s_vol.uViewProj    = UniformLoc( s_vol.program, "u_viewProj" );
	s_vol.uCamPos      = UniformLoc( s_vol.program, "u_camPos" );
	s_vol.uLightOrigin = UniformLoc( s_vol.program, "u_lightOrigin" );
	s_vol.uLightDir    = UniformLoc( s_vol.program, "u_lightDir" );
	s_vol.uLightColor  = UniformLoc( s_vol.program, "u_lightColor" );
	s_vol.uRadius      = UniformLoc( s_vol.program, "u_radius" );
	s_vol.uCosInner    = UniformLoc( s_vol.program, "u_cosInner" );
	s_vol.uCosOuter    = UniformLoc( s_vol.program, "u_cosOuter" );
	s_vol.uScatter     = UniformLoc( s_vol.program, "u_scatter" );
	s_vol.uStrength    = UniformLoc( s_vol.program, "u_strength" );
	s_vol.uFogDensity  = UniformLoc( s_vol.program, "u_fogDensity" );
	s_vol.uSteps       = UniformLoc( s_vol.program, "u_steps" );
	s_vol.uMatShadow   = UniformLoc( s_vol.program, "u_matShadow" );
	s_vol.uShadowMap   = UniformLoc( s_vol.program, "u_shadowMap" );
	s_vol.uHasShadow   = UniformLoc( s_vol.program, "u_hasShadow" );
	s_vol.uSceneDepth    = UniformLoc( s_vol.program, "u_sceneDepth" );
	s_vol.uHasSceneDepth = UniformLoc( s_vol.program, "u_hasSceneDepth" );
	s_vol.uScreenSize    = UniformLoc( s_vol.program, "u_screenSize" );
	s_vol.uZNear         = UniformLoc( s_vol.program, "u_zNear" );
	s_vol.uZFar          = UniformLoc( s_vol.program, "u_zFar" );

	s_vol.built = true;
	CSZ_LogDev( "lighting", "volumetric program built (gpu gen %d)", s_vol.gpuGeneration );
}

namespace
{
// Conservative world-space bounds of the spot cone: apex + the four corners of
// the (square) far plane. Mirrors csz_light_pass.cpp SpotConeBounds so the shaft
// applies the SAME light-vs-view cull the main light pass and shadow loop use.
void SpotConeBounds( const LightDesc &d, float mins[3], float maxs[3] )
{
	float fwd[3], right[3], up[3];

	AngleVectors( d.angles, fwd, right, up );

	float halfTan = tanf( d.fov * 0.5f * kDegToRad );

	for( int j = 0; j < 3; j++ )
	{
		mins[j] = d.origin[j];
		maxs[j] = d.origin[j];
	}

	for( int sx = -1; sx <= 1; sx += 2 )
	{
		for( int sy = -1; sy <= 1; sy += 2 )
		{
			for( int j = 0; j < 3; j++ )
			{
				float v = d.origin[j] + ( fwd[j] + right[j] * (float)sx * halfTan +
					up[j] * (float)sy * halfTan ) * d.radius;

				if( v < mins[j] ) mins[j] = v;
				if( v > maxs[j] ) maxs[j] = v;
			}
		}
	}
}

// Build a world-space cone mesh from spot params: apex at origin, axis = dir,
// half-angle = acos(cosOuter), length = radius. Emits a non-indexed GL_TRIANGLES
// cone hull: a side wall (apex + two adjacent ring points per segment) plus a
// base cap (center + two adjacent ring points per segment), so one draw covers
// both. Writes exactly kConeSegments*6 verts (24*6 = 144).
int BuildCone( const SpotLightParams &p, ConeVertex *out )
{
	float fwd[3] = { p.dir[0], p.dir[1], p.dir[2] };

	// Orthonormal basis around the cone axis.
	float up[3] = { 0.0f, 0.0f, 1.0f };
	if( fabsf( fwd[2] ) > 0.99f )
	{
		up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f;
	}
	float right[3] = {
		fwd[1] * up[2] - fwd[2] * up[1],
		fwd[2] * up[0] - fwd[0] * up[2],
		fwd[0] * up[1] - fwd[1] * up[0]
	};
	float rl = sqrtf( right[0] * right[0] + right[1] * right[1] + right[2] * right[2] );
	if( rl < 1e-5f ) rl = 1e-5f;
	right[0] /= rl; right[1] /= rl; right[2] /= rl;
	float trueUp[3] = {
		right[1] * fwd[2] - right[2] * fwd[1],
		right[2] * fwd[0] - right[0] * fwd[2],
		right[0] * fwd[1] - right[1] * fwd[0]
	};

	// Base-ring geometry: half-angle from cosOuter, ring radius at cone length.
	float cosHalf = p.cosOuter;
	if( cosHalf < 1e-3f ) cosHalf = 1e-3f;
	if( cosHalf > 0.9999f ) cosHalf = 0.9999f;
	float halfAng = acosf( cosHalf );
	float ringR = p.radius * tanf( halfAng );
	float baseDist = p.radius;

	float apex[3] = { p.origin[0], p.origin[1], p.origin[2] };
	float baseCenter[3] = {
		apex[0] + fwd[0] * baseDist,
		apex[1] + fwd[1] * baseDist,
		apex[2] + fwd[2] * baseDist
	};

	// Precompute ring points.
	float ring[kConeSegments + 1][3];
	for( int i = 0; i <= kConeSegments; i++ )
	{
		float a = ( (float)i / (float)kConeSegments ) * 2.0f * 3.14159265358979323846f;
		float c = cosf( a ), s = sinf( a );
		for( int j = 0; j < 3; j++ )
			ring[i][j] = baseCenter[j] + ( right[j] * c + trueUp[j] * s ) * ringR;
	}

	int n = 0;
	// Side: apex + two adjacent ring points per segment.
	for( int i = 0; i < kConeSegments; i++ )
	{
		out[n].pos[0] = apex[0]; out[n].pos[1] = apex[1]; out[n].pos[2] = apex[2]; n++;
		out[n].pos[0] = ring[i][0]; out[n].pos[1] = ring[i][1]; out[n].pos[2] = ring[i][2]; n++;
		out[n].pos[0] = ring[i + 1][0]; out[n].pos[1] = ring[i + 1][1]; out[n].pos[2] = ring[i + 1][2]; n++;
	}
	// Base cap: center + two adjacent ring points (so the shaft has a far wall to
	// integrate against when the cone points into open space).
	for( int i = 0; i < kConeSegments; i++ )
	{
		out[n].pos[0] = baseCenter[0]; out[n].pos[1] = baseCenter[1]; out[n].pos[2] = baseCenter[2]; n++;
		out[n].pos[0] = ring[i + 1][0]; out[n].pos[1] = ring[i + 1][1]; out[n].pos[2] = ring[i + 1][2]; n++;
		out[n].pos[0] = ring[i][0]; out[n].pos[1] = ring[i][1]; out[n].pos[2] = ring[i][2]; n++;
	}
	return n;
}

// Scene-depth copy for the MED/HIGH tExit clamp. Lazily (re)allocates a plain
// (non-compare) GL_DEPTH_COMPONENT24 engine texture at the current viewport
// size -- created ONCE, reallocated ONLY when the viewport size changes; never
// per-frame. Then copies the default framebuffer's depth buffer into it with a
// single glCopyTexSubImage2D. Returns the engine texture slot ready to sample,
// or 0 on failure (caller then runs the no-clamp path, u_hasSceneDepth=0).
// TF_NOCOMPARE keeps GL_TEXTURE_COMPARE_MODE off so the FS reads raw depth via
// a regular sampler2D (unlike the shadow map's sampler2DShadow).
int EnsureSceneDepthCopy( VolumetricPass::Gpu &g, int vw, int vh )
{
	if( g.depthFailed )
		return 0;
	if( vw <= 0 || vh <= 0 )
		return 0;
	if( gRenderAPI.GL_CreateTexture == NULL )
	{
		g.depthFailed = true;
		CSZ_LogError( "lighting", "volumetric scene-depth clamp unavailable: GL_CreateTexture is NULL" );
		return 0;
	}

	// (Re)allocate only on first use or a viewport-size change.
	if( g.depthTexSlot == 0 || g.depthW != vw || g.depthH != vh )
	{
		if( g.depthTexSlot != 0 && gRenderAPI.GL_FreeTexture != NULL )
			gRenderAPI.GL_FreeTexture( g.depthTexSlot );
		g.depthTexSlot = 0;

		g.depthTexSlot = gRenderAPI.GL_CreateTexture( "csz_volumetric_scenedepth", vw, vh,
			NULL, (texFlags_t)( TF_DEPTHMAP | TF_NOCOMPARE | TF_NEAREST | TF_CLAMP | TF_NOMIPMAP ));

		if( g.depthTexSlot == 0 || TexSlotToGlName( g.depthTexSlot ) == 0 )
		{
			g.depthTexSlot = 0;
			g.depthFailed = true;
			CSZ_LogError( "lighting", "volumetric scene-depth texture creation failed; shaft uses HW-depth occlusion only" );
			return 0;
		}

		g.depthW = vw;
		g.depthH = vh;
		CSZ_LogDev( "lighting", "volumetric scene-depth copy ready (%dx%d, slot=%d)", vw, vh, g.depthTexSlot );
	}

	// Copy the default framebuffer depth into the texture. Bind via the engine
	// wrapper (keeps the ref glState TMU cache coherent -- raw glActiveTexture is
	// forbidden in this renderer) on the highest CSZ unit; glCopyTexSubImage2D
	// then targets the now-current GL_TEXTURE_2D. Default framebuffer is bound
	// here (post opaque passes); read buffer is the back buffer's depth.
	BindTextureSlot( 3, g.depthTexSlot );
	glCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, vw, vh );
	return g.depthTexSlot;
}

// P0 guard: the per-frame ConeVertex buffer (VolumetricPass::Gpu::verts,
// kConeSegments*6) must hold everything BuildCone can emit -- a side triangle
// list (kConeSegments*3) plus a base cap (kConeSegments*3). Sizing the buffer
// below this makes the overflow a compile error, not a runtime memory
// corruption (the original P0 was exactly such a silent mismatch).
static_assert( kConeSegments * 6 >= kConeSegments * 3 + kConeSegments * 3,
               "ConeVertex vertex buffer is smaller than BuildCone's max emission" );
}

void VolumetricPass::Render( const ViewSetup &view )
{
	// B-class master off: A-class cone+fog already drew fully; nothing here.
	if( s_cvarEnable == NULL || s_cvarEnable->value == 0.0f )
		return;

	// Quality tier (csz_volumetric_quality, clamped 0..3): maps directly to the
	// effective ray-march step count and the shadow-carve enable. Tier 0 skips the
	// shaft entirely this frame -> guaranteed zero shaft cost (200-FPS floor).
	int quality = ( s_cvarQuality != NULL ) ? (int)s_cvarQuality->value : 2;
	if( quality < 0 ) quality = 0;
	if( quality > 3 ) quality = 3;
	if( quality == 0 )
		return;	// OFF: no shaft drawn this frame

	int qSteps;
	bool shadowCarve;
	switch( quality )
	{
	case 1:  qSteps = 12; shadowCarve = false; break;	// LOW
	case 3:  qSteps = 48; shadowCarve = true;  break;	// HIGH
	default: qSteps = 24; shadowCarve = false; break;	// MED (2)
	}

	EnsureBuilt();
	if( s_vol.program.program == 0 )
		return;	// shader build failed earlier; shaft disabled, cone+fog unaffected

	int steps = qSteps;
	if( steps < kStepsMin ) steps = kStepsMin;
	if( steps > kStepsMax ) steps = kStepsMax;

#if defined( CSZ_DEV_TOOLS )
	float strength = ( s_cvarStrength != NULL ) ? s_cvarStrength->value : kDefaultStrength;
#else
	float strength = kDefaultStrength;
#endif

	float fogDensity = view.ambience.fogDensity;
	if( fogDensity <= 0.0f )
		return;	// no fog -> no medium to scatter in; shaft would be invisible anyway

	// Scene-depth clamp is a MED/HIGH-only feature (LOW/OFF stay on the cheap
	// hardware-depth-only path so they keep the FPS floor). When enabled, copy
	// the opaque scene depth ONCE this frame, before any cone is drawn, so the
	// copy is pure opaque depth (additive cones never write depth anyway).
	int sceneDepthSlot = 0;
	if( quality >= 2 )
		sceneDepthSlot = EnsureSceneDepthCopy( s_vol, view.viewport[2], view.viewport[3] );
	int hasSceneDepth = ( sceneDepthSlot != 0 ) ? 1 : 0;

	float now = ClientTime();
	int drawn = 0;

	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );

		if( !light->used || light->desc.type != kLightSpot )
			continue;

		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;	// expired timed spot: no shaft (mirrors csz_light_pass.cpp)

		// Light-vs-view visibility: same conservative cone-bbox-vs-frustum cull the
		// main light pass and shadow loop apply (csz_light_pass.cpp).
		float mins[3], maxs[3];

		SpotConeBounds( light->desc, mins, maxs );

		if( view.frustum.CullBox( mins, maxs ))
			continue;

		SpotLightParams params;
		g_lights.BuildSpotParams( *light, params );

		s_vol.numVerts = BuildCone( params, s_vol.verts );

		// Upload this frame's cone mesh.
		BindVao( s_vol.vao );
		glBindBuffer( GL_ARRAY_BUFFER, s_vol.vbo );
		glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)( (size_t)s_vol.numVerts * sizeof( ConeVertex )),
			s_vol.verts, GL_DYNAMIC_DRAW );

		if( drawn == 0 )
		{
			// State setup shared by all lights this frame.
			UseProgram( s_vol.program.program );
			// Additive in-fog scatter, DEPTH-CORRECT (V5):
			//  - depth-test ON at the LEQUAL baseline: a cone hull fragment behind
			//    an opaque surface is z-rejected by hardware, so a wall BETWEEN the
			//    camera and the cone fully occludes the shaft for free;
			//  - depth-write OFF so later passes are unaffected;
			//  - draw BACK faces only (cull on + cull FRONT): the back-face hull
			//    covers the cone's full screen footprint AND stays valid when the
			//    camera is inside the cone (front faces would clip behind near).
			SetBlend( kBlendAdditive );
			SetDepthWrite( false );
			SetDepthTest( true );
			SetCull( true );
			SetCullFront( true );	// cull FRONT faces -> rasterize the back-face hull

			glUniformMatrix4fv( s_vol.uViewProj, 1, GL_FALSE, view.matViewProj.m );
			glUniform3fv( s_vol.uCamPos, 1, (const float *)view.origin );
			glUniform1f( s_vol.uScatter, kDefaultScatter );
			glUniform1f( s_vol.uStrength, strength );
			glUniform1f( s_vol.uFogDensity, fogDensity );
			glUniform1i( s_vol.uSteps, steps );
			glUniform1i( s_vol.uShadowMap, 2 );	// sampler unit 2 (matches world lit pass)

			// Scene-depth clamp uniforms (active MED/HIGH only). When inactive the
			// FS skips the sampler entirely (no unit-3 dependency on LOW/OFF).
			glUniform1i( s_vol.uHasSceneDepth, hasSceneDepth );
			glUniform1f( s_vol.uZNear, view.zNear );
			glUniform1f( s_vol.uZFar, view.zFar );
			glUniform2f( s_vol.uScreenSize, (float)view.viewport[2], (float)view.viewport[3] );
			if( hasSceneDepth )
			{
				glUniform1i( s_vol.uSceneDepth, 3 );	// sampler unit 3 (depth copy)
				BindTextureSlot( 3, sceneDepthSlot );
			}
		}

		glUniform3fv( s_vol.uLightOrigin, 1, params.origin );
		glUniform3fv( s_vol.uLightDir, 1, params.dir );
		glUniform3fv( s_vol.uLightColor, 1, params.color );
		glUniform1f( s_vol.uRadius, params.radius );
		glUniform1f( s_vol.uCosInner, params.cosInner );
		glUniform1f( s_vol.uCosOuter, params.cosOuter );

		// Shadow-carve (high tier): only when this light owns the single shadow
		// map (csz_light_shadow=1 -> shadowTexSlot != 0). Same projection + unit
		// as the world lit pass (world_shaders.inl).
		int hasShadow = ( shadowCarve && params.shadowTexSlot != 0 ) ? 1 : 0;
		glUniform1i( s_vol.uHasShadow, hasShadow );
		if( hasShadow )
		{
			glUniformMatrix4fv( s_vol.uMatShadow, 1, GL_FALSE, params.matShadow.m );
			BindTextureSlot( 2, params.shadowTexSlot );
		}

		glDrawArrays( GL_TRIANGLES, 0, s_vol.numVerts );
		drawn++;
	}

	if( drawn > 0 )
	{
		// Restore the takeover baseline (like the lit pass leaves it). We drew
		// back faces (cull front) with depth-test on / write off, so put depth
		// write back on, the cull face back to the BACK default, and cull off.
		BindVao( 0 );
		SetBlend( kBlendNone );
		SetDepthWrite( true );
		SetDepthTest( true );
		SetCullFront( false );	// back to GL_BACK (baseline cull face)
		SetCull( false );
	}
}

}
