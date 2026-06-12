/*
 * csz_studio.cpp -- CSOZ renderer: studio model rendering implementation
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
#include "csz_studio.h"
#include "csz_studio_bones.h"
#include "csz_studio_mesh.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_shader.h"

#include <math.h>

namespace csz
{

StudioRenderer g_studio;

#include "csz_studio_shaders.inl"

namespace
{

// Engine-extended texture flags (pinned engine/studio.h values; the fork's
// HLSDK studio.h predates them). Layout-independent constants.
const int kStudioNfMasked = 0x0040;	// alpha-tested texture
const int kStudioNfAdditive = 0x0020;	// additive (M1: drawn opaque, known gap)

// Uniform locations of one studio program. Both passes share the per-mesh
// uniform helper: locations a program lacks are -1 and glUniform* on -1 is
// ignored by GL, so base-only/lit-only members stay harmless cross-program.
struct PassLocs
{
	int uViewProj, uBones, uAlphaTest, uChrome, uViewRight, uViewUp;
	int uAmbient, uShadeColor;					// base program only
	int uLightOrigin, uLightDir, uLightColor;			// lit program only
	int uLightRadius, uCosInner, uCosOuter, uMatShadow, uHasShadow;	// lit program only
};

struct StudioState
{
	bool shaderReady;
	int shaderGeneration;
	ShaderProgram program;		// base (opaque) pass
	ShaderProgram litProgram;	// additive per-light pass (T6)
	PassLocs baseLocs, litLocs;
	const PassLocs *locs;		// active pass locations (set by Begin*Pass)
	bool litPass;			// true while DrawLitAdditive runs (skips LightVec)
	int whiteTexSlot;
	float time;		// latched at BeginFrame
	// per-draw uniform dedup
	float curAlphaTest;
	int curChrome;
	bool curFullbright;
	float curLight[3];
};

StudioState s_studio;

void QueryPassLocs( const ShaderProgram &prog, PassLocs &out )
{
	out.uViewProj = UniformLoc( prog, "u_viewProj" );
	out.uBones = UniformLoc( prog, "u_bones" );
	out.uAlphaTest = UniformLoc( prog, "u_alphaTest" );
	out.uChrome = UniformLoc( prog, "u_chrome" );
	out.uViewRight = UniformLoc( prog, "u_viewRight" );
	out.uViewUp = UniformLoc( prog, "u_viewUp" );
	out.uAmbient = UniformLoc( prog, "u_ambient" );
	out.uShadeColor = UniformLoc( prog, "u_shadeColor" );
	out.uLightOrigin = UniformLoc( prog, "u_lightOrigin" );
	out.uLightDir = UniformLoc( prog, "u_lightDir" );
	out.uLightColor = UniformLoc( prog, "u_lightColor" );
	out.uLightRadius = UniformLoc( prog, "u_lightRadius" );
	out.uCosInner = UniformLoc( prog, "u_cosInner" );
	out.uCosOuter = UniformLoc( prog, "u_cosOuter" );
	out.uMatShadow = UniformLoc( prog, "u_matShadow" );
	out.uHasShadow = UniformLoc( prog, "u_hasShadow" );
}

void EnsureShader()
{
	if( s_studio.shaderReady && s_studio.shaderGeneration == GpuGeneration())
		return;

	if( s_studio.shaderGeneration != GpuGeneration())
	{
		// Stale generation: forget, never delete (T1 rule).
		s_studio.program.program = 0;
		s_studio.litProgram.program = 0;
	}

	// Init-time shaders: compile failure is FATAL (spec 3.2).
	BuildProgram( "csz_studio", kStudioVs, kStudioFs, true, s_studio.program );
	QueryPassLocs( s_studio.program, s_studio.baseLocs );

	UseProgram( s_studio.program.program );
	glUniform1i( UniformLoc( s_studio.program, "u_texDiffuse" ), 0 );

	// Fixed top-down shade direction (plan section 6 step 3 M1 lighting).
	const float kShadeDir[3] = { 0.0f, 0.0f, 1.0f };

	glUniform3fv( UniformLoc( s_studio.program, "u_shadeDir" ), 1, kShadeDir );

	BuildProgram( "csz_studio_lit", kStudioLitVs, kStudioLitFs, true, s_studio.litProgram );
	QueryPassLocs( s_studio.litProgram, s_studio.litLocs );

	UseProgram( s_studio.litProgram.program );
	glUniform1i( UniformLoc( s_studio.litProgram, "u_texDiffuse" ), 0 );
	glUniform1i( UniformLoc( s_studio.litProgram, "u_shadowMap" ), 2 );
	UseProgram( 0 );

	s_studio.locs = &s_studio.baseLocs;
	s_studio.whiteTexSlot = ( gRenderAPI.GL_FindTexture != NULL )
		? gRenderAPI.GL_FindTexture( "*white" ) : 0;
	s_studio.shaderGeneration = GpuGeneration();
	s_studio.shaderReady = true;
}

// Samples the floor lightmap below the entity and converts it through the
// SAME gamma + overbright pipeline the world pass uses (T2 parity formula;
// R_LightVec already applies the lightstyle scale, pinned ref_light.c).
void SampleEntityLight( const cl_entity_s *ent, float out[3] )
{
	out[0] = out[1] = out[2] = 0.5f;	// flat gray fallback

	if( gRenderAPI.LightVec == NULL || gRenderAPI.LightToTexGamma == NULL )
		return;

	float start[3] = { ent->origin[0], ent->origin[1], ent->origin[2] };
	float end[3] = { ent->origin[0], ent->origin[1], ent->origin[2] - 2048.0f };
	float lightspot[3], lightvec[3];
	colorVec cv = gRenderAPI.LightVec( start, end, lightspot, lightvec );

	if( cv.r + cv.g + cv.b == 0 )
	{
		// In-frame degrade, deduplicated Error (plan section 6 step 3).
		static bool s_logged;

		if( !s_logged )
		{
			s_logged = true;
			CSZ_LogError( "studio", "LightVec returned black at (%.0f %.0f %.0f); using 0.5 gray (logged once)",
				ent->origin[0], ent->origin[1], ent->origin[2] );
		}

		return;
	}

	// World-brightness parity: lightstyle scale is already inside the
	// sample; apply the engine light gamma table + the same overbright
	// factor as csz_world_shaders.inl (2.0 * 128 / 192 = 4/3).
	const float kOverbright = 2.0f * 128.0f / 192.0f;
	unsigned int rgb[3] = { cv.r, cv.g, cv.b };

	for( int i = 0; i < 3; i++ )
	{
		unsigned int b = ( rgb[i] > 255 ) ? 255 : rgb[i];

		out[i] = (float)gRenderAPI.LightToTexGamma((byte)b ) / 255.0f * kOverbright;
	}
}

// Conservative bounding-sphere radius from the sequence bbox corner radius
// (rotation-safe; progress-t3.md decision 8).
float EntityProbeRadius( const cl_entity_s *ent, const studiohdr_t *hdr )
{
	int seq = ent->curstate.sequence;

	if( seq < 0 || seq >= hdr->numseq )
		seq = 0;

	const mstudioseqdesc_t *pseqdesc = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex ) + seq;
	float r1 = sqrtf( pseqdesc->bbmin[0] * pseqdesc->bbmin[0] + pseqdesc->bbmin[1] * pseqdesc->bbmin[1] +
		pseqdesc->bbmin[2] * pseqdesc->bbmin[2] );
	float r2 = sqrtf( pseqdesc->bbmax[0] * pseqdesc->bbmax[0] + pseqdesc->bbmax[1] * pseqdesc->bbmax[1] +
		pseqdesc->bbmax[2] * pseqdesc->bbmax[2] );
	float r = ( r1 > r2 ) ? r1 : r2;

	if( r < 16.0f )
		r = 16.0f;	// degenerate bbox: keep a minimum probe

	return r;
}

bool CullEntity( const ViewSetup &view, const cl_entity_s *ent, const studiohdr_t *hdr )
{
	float r = EntityProbeRadius( ent, hdr );
	float mins[3] = { ent->origin[0] - r, ent->origin[1] - r, ent->origin[2] - r };
	float maxs[3] = { ent->origin[0] + r, ent->origin[1] + r, ent->origin[2] + r };

	return view.frustum.CullBox( mins, maxs );
}

// Conservative spot-light vs entity test (T6): bounding spheres + apex
// half-space (SpotLightParams carries no frustum; see csz_world.cpp twin).
bool SpotTouchesEntity( const SpotLightParams &light, const cl_entity_s *ent, const studiohdr_t *hdr )
{
	float r = EntityProbeRadius( ent, hdr );
	float dx = ent->origin[0] - light.origin[0];
	float dy = ent->origin[1] - light.origin[1];
	float dz = ent->origin[2] - light.origin[2];
	float reach = light.radius + r;

	if( dx * dx + dy * dy + dz * dz > reach * reach )
		return false;

	// Fully behind the cone apex plane (by more than the probe radius).
	return ( dx * light.dir[0] + dy * light.dir[1] + dz * light.dir[2] ) >= -r;
}

void SetMeshUniforms( int texFlags, const float lightColor[3], bool force )
{
	float alphaTest = ( texFlags & kStudioNfMasked ) ? 0.25f : 0.0f;
	int chrome = ( texFlags & STUDIO_NF_CHROME ) ? 1 : 0;
	bool fullbright = ( texFlags & STUDIO_NF_FULLBRIGHT ) != 0;

	if( force || alphaTest != s_studio.curAlphaTest )
	{
		glUniform1f( s_studio.locs->uAlphaTest, alphaTest );
		s_studio.curAlphaTest = alphaTest;
	}

	if( force || chrome != s_studio.curChrome )
	{
		glUniform1i( s_studio.locs->uChrome, chrome );
		s_studio.curChrome = chrome;
	}

	bool lightChanged = force ||
		lightColor[0] != s_studio.curLight[0] ||
		lightColor[1] != s_studio.curLight[1] ||
		lightColor[2] != s_studio.curLight[2];

	if( lightChanged || fullbright != s_studio.curFullbright )
	{
		// Base-pass-only uniforms; -1 (ignored) in the lit program.
		if( fullbright )
		{
			const float kOne[3] = { 1.0f, 1.0f, 1.0f };
			const float kZero[3] = { 0.0f, 0.0f, 0.0f };

			glUniform3fv( s_studio.locs->uAmbient, 1, kOne );
			glUniform3fv( s_studio.locs->uShadeColor, 1, kZero );
		}
		else
		{
			// M1 split (plan section 6 step 3): ambient 0.6, shade 0.4.
			float ambient[3] = { 0.6f * lightColor[0], 0.6f * lightColor[1], 0.6f * lightColor[2] };
			float shade[3] = { 0.4f * lightColor[0], 0.4f * lightColor[1], 0.4f * lightColor[2] };

			glUniform3fv( s_studio.locs->uAmbient, 1, ambient );
			glUniform3fv( s_studio.locs->uShadeColor, 1, shade );
		}

		s_studio.curFullbright = fullbright;
		s_studio.curLight[0] = lightColor[0];
		s_studio.curLight[1] = lightColor[1];
		s_studio.curLight[2] = lightColor[2];
	}
}

// Draws every mesh of the submodels selected by the entity body value.
void DrawModelMeshes( StudioModelGpu *gpu, const studiohdr_t *hdr, int body,
	const BoneSetup *bones, const float lightColor[3] )
{
	glUniform4fv( s_studio.locs->uBones, bones->numBones * 3, &bones->gpuBones[0][0] );

	const mstudiobodyparts_t *pbodyparts = (const mstudiobodyparts_t *)((const byte *)hdr + hdr->bodypartindex );
	int selected[MAXSTUDIOBODYPARTS];
	int numParts = ( hdr->numbodyparts < MAXSTUDIOBODYPARTS ) ? hdr->numbodyparts : MAXSTUDIOBODYPARTS;

	for( int bp = 0; bp < numParts; bp++ )
	{
		// Standard bodygroup select: (body / base) % nummodels.
		const mstudiobodyparts_t &part = pbodyparts[bp];

		selected[bp] = ( part.base > 0 && part.nummodels > 0 )
			? ( body / part.base ) % part.nummodels : 0;
	}

	int meshCount = 0;
	const StudioMeshGpu *meshes = Meshes( gpu, &meshCount );
	int curTex = -1;

	for( int i = 0; i < meshCount; i++ )
	{
		const StudioMeshGpu &mesh = meshes[i];

		if( mesh.indexCount <= 0 || mesh.bodypart >= numParts )
			continue;

		if( mesh.submodel != selected[mesh.bodypart] )
			continue;

		int texSlot = ( mesh.texSlot != 0 ) ? mesh.texSlot : s_studio.whiteTexSlot;

		if( texSlot != curTex )
		{
			BindTextureSlot( 0, texSlot );
			curTex = texSlot;
		}

		SetMeshUniforms( mesh.texFlags, lightColor, false );
		BindVao( mesh.vao );
		glDrawElements( GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, (const void *)0 );
	}
}

// Full single-entity path: model resolve, cull, bones, light, meshes,
// player p_ weapon merge. Returns true when something was drawn.
// spotCull != NULL additionally rejects entities outside that light's reach
// (lit-additive pass).
bool DrawEntity( const ViewSetup &view, cl_entity_s *ent, bool doCull, const SpotLightParams *spotCull )
{
	model_t *mod = ent->model;

	if( ent->player )
	{
		// Stock parity: the engine resolves the player's actual model
		// (userinfo / forced models); fall back to ent->model when absent.
		int playerIndex = ent->index - 1;

		if( IEngineStudio.SetupPlayerModel != NULL &&
			playerIndex >= 0 && playerIndex < gEngfuncs.GetMaxClients())
		{
			model_t *pm = IEngineStudio.SetupPlayerModel( playerIndex );

			if( pm != NULL )
				mod = pm;
		}
	}

	if( mod == NULL || mod->type != mod_studio || IEngineStudio.Mod_Extradata == NULL )
		return false;

	studiohdr_t *hdr = (studiohdr_t *)IEngineStudio.Mod_Extradata( mod );

	if( hdr == NULL || hdr->numbodyparts <= 0 || hdr->numbones <= 0 || hdr->numseq <= 0 )
		return false;

	if( doCull && CullEntity( view, ent, hdr ))
		return false;

	if( spotCull != NULL && !SpotTouchesEntity( *spotCull, ent, hdr ))
		return false;

	const BoneSetup *bones = NULL;

	if( !SetupBones( ent, hdr, s_studio.time, &bones ))
		return false;

	StudioModelGpu *gpu = GetOrBuild( mod, hdr );

	if( gpu == NULL )
		return false;

	float lightColor[3] = { 0.5f, 0.5f, 0.5f };

	// The lit pass ignores ambient/shade (locations -1); skip the LightVec
	// trace there -- it would run once per entity per light otherwise.
	if( !s_studio.litPass )
		SampleEntityLight( ent, lightColor );

	// Mirrored setups (right-hand viewmodel) have reversed triangle winding;
	// the stock path solves this by drawing the flipped viewmodel with
	// culling off (pinned ref/gl/gl_studio.c R_StudioDrawPoints) -- same here.
	if( bones->mirrored )
		SetCull( false );

	DrawModelMeshes( gpu, hdr, ent->curstate.body, bones, lightColor );

	if( bones->mirrored )
		SetCull( true );

	// p_ weapon model riding the player skeleton (visual A/B parity).
	if( ent->player && ent->curstate.weaponmodel != 0 && IEngineStudio.GetModelByIndex != NULL )
	{
		model_t *wmod = IEngineStudio.GetModelByIndex( ent->curstate.weaponmodel );

		if( wmod != NULL && wmod->type == mod_studio )
		{
			studiohdr_t *whdr = (studiohdr_t *)IEngineStudio.Mod_Extradata( wmod );

			if( whdr != NULL && whdr->numbodyparts > 0 && whdr->numbones > 0 && whdr->numseq > 0 )
			{
				const BoneSetup *wbones = NULL;

				if( SetupBonesMerged( ent, hdr, bones, whdr, s_studio.time, &wbones ))
				{
					StudioModelGpu *wgpu = GetOrBuild( wmod, whdr );

					if( wgpu != NULL )
						DrawModelMeshes( wgpu, whdr, 0, wbones, lightColor );
				}
			}
		}
	}

	return true;
}

// Common pass prologue: program, view uniforms, chrome basis, dedup reset.
void BeginStudioPassWith( const ViewSetup &view, const ShaderProgram &prog, const PassLocs &locs )
{
	EnsureShader();
	s_studio.locs = &locs;
	UseProgram( prog.program );
	glUniformMatrix4fv( locs.uViewProj, 1, GL_FALSE, view.matViewProj.m );

	float fwd[3], right[3], up[3];

	AngleVectors( view.angles, fwd, right, up );
	glUniform3fv( locs.uViewRight, 1, right );
	glUniform3fv( locs.uViewUp, 1, up );

	// Pin known uniform state so per-mesh dedup stays truthful.
	const float kGray[3] = { 0.5f, 0.5f, 0.5f };

	SetMeshUniforms( 0, kGray, true );

	// Closed meshes get real face culling. GL_FRONT (not BACK) matches how
	// the stock GL studio path orients .mdl triangle winding; verified
	// against the engine renderer in the same session (T3 step 4).
	SetCull( true );
	SetCullFront( true );
}

void BeginStudioPass( const ViewSetup &view )
{
	// EnsureShader runs first inside BeginStudioPassWith; the references
	// below are stable storage, so taking them before is safe.
	BeginStudioPassWith( view, s_studio.program, s_studio.baseLocs );
}

// Lit-additive prologue: lit program + per-light uniforms + additive blend
// with depth writes off (equal-depth LEQUAL on top of the opaque pass).
void BeginStudioLitPass( const ViewSetup &view, const SpotLightParams &light )
{
	s_studio.litPass = true;
	BeginStudioPassWith( view, s_studio.litProgram, s_studio.litLocs );

	const PassLocs &locs = s_studio.litLocs;

	glUniform3fv( locs.uLightOrigin, 1, light.origin );
	glUniform3fv( locs.uLightDir, 1, light.dir );
	glUniform3fv( locs.uLightColor, 1, light.color );
	glUniform1f( locs.uLightRadius, light.radius );
	glUniform1f( locs.uCosInner, light.cosInner );
	glUniform1f( locs.uCosOuter, light.cosOuter );
	glUniformMatrix4fv( locs.uMatShadow, 1, GL_FALSE, light.matShadow.m );
	glUniform1i( locs.uHasShadow, ( light.shadowTexSlot != 0 ) ? 1 : 0 );

	if( light.shadowTexSlot != 0 )
		BindTextureSlot( 2, light.shadowTexSlot );	// T7 depth map (T6: never taken)

	SetBlend( kBlendAdditive );
	SetDepthWrite( false );
}

void EndStudioPass()
{
	SetCull( false );	// EnterTakeover baseline (world pass relies on it)
	BindVao( 0 );
}

void EndStudioLitPass()
{
	SetBlend( kBlendNone );
	SetDepthWrite( true );
	s_studio.litPass = false;
	s_studio.locs = &s_studio.baseLocs;
	EndStudioPass();
}

}

void StudioRenderer::OnModelUnloaded( model_t *mod )
{
	Free( mod );
}

void StudioRenderer::DestroyAll()
{
	FreeAll();

	if( s_studio.program.program != 0 )
	{
		if( s_studio.shaderGeneration == GpuGeneration())
			DestroyProgram( s_studio.program );
		else
			s_studio.program.program = 0;
	}

	if( s_studio.litProgram.program != 0 )
	{
		if( s_studio.shaderGeneration == GpuGeneration())
			DestroyProgram( s_studio.litProgram );
		else
			s_studio.litProgram.program = 0;
	}

	s_studio.shaderReady = false;
}

void StudioRenderer::BeginFrame( float time )
{
	s_studio.time = time;
	ResetBoneCache( time );
}

void StudioRenderer::DrawOpaque( const ViewSetup &view, cl_entity_s *const *ents, int count )
{
	if( count <= 0 )
		return;

	BeginStudioPass( view );

	int drawn = 0;

	for( int i = 0; i < count; i++ )
	{
		if( ents[i] != NULL && DrawEntity( view, ents[i], true, NULL ))
			drawn++;
	}

	EndStudioPass();

	// Per-frame stats at Dev level with 1s self-throttle (R8).
	static float s_nextStatsTime;
	float now = ClientTime();

	if( now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "studio", "drawn %d / %d studio entities", drawn, count );
	}
}

void StudioRenderer::DrawDepth( const ViewSetup &lightView, const Frustum &lightCull,
	cl_entity_s *const *ents, int count )
{
	// Filled by T7 (shadow depth pass); signature fixed by plan section 2.2.
	(void)lightView;
	(void)lightCull;
	(void)ents;
	(void)count;

	static bool s_logged;

	if( !s_logged )
	{
		s_logged = true;
		CSZ_LogDev( "studio", "DrawDepth stub called (lands in T7)" );
	}
}

void StudioRenderer::DrawLitAdditive( const ViewSetup &view, const SpotLightParams &light,
	cl_entity_s *const *ents, int count )
{
	if( count <= 0 )
		return;

	BeginStudioLitPass( view, light );

	int drawn = 0;

	for( int i = 0; i < count; i++ )
	{
		if( ents[i] != NULL && DrawEntity( view, ents[i], true, &light ))
			drawn++;
	}

	EndStudioLitPass();

	// Per-frame stats at Dev level with 1s self-throttle (R8).
	static float s_nextStatsTime;
	float now = ClientTime();

	if( drawn > 0 && now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "studio", "lit %d / %d studio entities", drawn, count );
	}
}

void StudioRenderer::DrawSingle( const ViewSetup &view, cl_entity_s *ent )
{
	// Viewmodel path (T4 caller): no frustum cull, caller owns depth
	// range / projection. Same full pipeline otherwise.
	if( ent == NULL )
		return;

	BeginStudioPass( view );
	DrawEntity( view, ent, false, NULL );
	EndStudioPass();
}

}
