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
#include "csz_world.h"			// S1: g_world.SkyVisAtPoint for per-entity sky visibility
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_shader.h"

#include <math.h>
#include <stdlib.h>	// qsort (transparent studio back-to-front sort)

namespace csz
{

StudioRenderer g_studio;

#include "csz_studio_shaders.inl"

namespace
{

// Engine-extended texture flags (pinned engine/studio.h values; the fork's
// HLSDK studio.h predates them). Layout-independent constants.
const int kStudioNfMasked = 0x0040;	// alpha-tested texture

// Uniform locations of one studio program. Both passes share the per-mesh
// uniform helper: locations a program lacks are -1 and glUniform* on -1 is
// ignored by GL, so base-only/lit-only members stay harmless cross-program.
struct PassLocs
{
	int uViewProj, uBones, uAlphaTest, uChrome, uViewRight, uViewUp;
	int uAmbient, uShadeColor;					// base program only
	int uFog, uAmbTint;						// base program only (M2a fog/night; lit/depth fog-free, pitfall 23)
	int uSkyAmbScale;						// L3b sky-ambient cloud dimmer scalar (base program only); 1.0 neutral
	int uFogParams, uCamPos;					// analytic base fog (fog M1 Step 2): height b/sunGlow/maxOpacity + ray origin
	int uFogParams2, uFogParams3, uFogLit;				// S3 fog rework: per-channel ext + noise + Start/Cutoff + toward-body lit color
	int uSunDir, uSunColor;						// base program only (sky 档1 directional N.L; lit/depth exempt, pitfall 23)
	int uSkyVis;							// S1: per-entity geometric sky visibility (base program only); 1.0 default
	int uMoonInScatter, uShaftMask, uMoonShaft;			// fog M1 L4 moon Tyndall air-glow (base program only; identity until fed)
	int uNightModel, uNightness, uSunWarmColor;			// S2 physical night model (base program only)
	int uMoonDir, uMoonColor;					// S2 gated night moon directional (base program only)
	int uNightSky, uNightFloor, uNightK, uNightMoon;		// S2 studio night ambient calibration (base program only)
	int uRimStrength, uRimPower;					// S4 competitive rim light (base program only)
	int uStudioRender, uRenderAmt, uRenderColor;			// MUST 2 rendermode (base program only)
	int uLightOrigin, uLightDir, uLightColor;			// lit program only
	int uLightRadius, uCosInner, uCosOuter, uMatShadow, uHasShadow;	// lit program only
	int uV3, uEdgeExp, uHotspotGain, uHotspotSharp, uDirectGain;	// L5R crisp profile (lit program only)
};

struct StudioState
{
	bool shaderReady;
	int shaderGeneration;
	ShaderProgram program;		// base (opaque) pass
	ShaderProgram litProgram;	// additive per-light pass (T6)
	ShaderProgram depthProgram;	// shadow map depth pass (T7)
	ShaderProgram shellProgram;	// MUST 3 kRenderFxGlowShell additive extrude pass
	PassLocs baseLocs, litLocs, depthLocs;
	PassLocs shellLocs;		// only uViewProj/uBones resolve (others -1); reused by DrawModelMeshes
	int shellExtrude, shellColor, shellAlpha;	// shell-program-only uniforms
	const PassLocs *locs;		// active pass locations (set by Begin*Pass)
	bool litPass;			// true while DrawLitAdditive runs (skips LightVec)
	bool depthPass;			// true while DrawDepth runs (skips LightVec + texture binds)
	bool shellPass;			// true while DrawGlowShells runs (skips LightVec + texture binds)
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
	out.uFog = UniformLoc( prog, "u_fog" );
	out.uFogParams = UniformLoc( prog, "u_fogParams" );
	out.uFogParams2 = UniformLoc( prog, "u_fogParams2" );	// S3
	out.uFogParams3 = UniformLoc( prog, "u_fogParams3" );	// S3
	out.uFogLit = UniformLoc( prog, "u_fogLit" );		// S3
	out.uCamPos = UniformLoc( prog, "u_camPos" );
	out.uAmbTint = UniformLoc( prog, "u_ambTint" );
	out.uSkyAmbScale = UniformLoc( prog, "u_skyAmbScale" );
	out.uSunDir = UniformLoc( prog, "u_sunDir" );
	out.uSunColor = UniformLoc( prog, "u_sunColor" );
	out.uSkyVis = UniformLoc( prog, "u_skyVis" );			// S1 per-entity sky visibility

	out.uMoonInScatter = UniformLoc( prog, "u_moonInScatter" );	// L4
	out.uShaftMask = UniformLoc( prog, "u_shaftMask" );		// L4
	out.uMoonShaft = UniformLoc( prog, "u_moonShaft" );		// L4
	out.uNightModel = UniformLoc( prog, "u_nightModel" );		// S2
	out.uNightness = UniformLoc( prog, "u_nightness" );		// S2
	out.uSunWarmColor = UniformLoc( prog, "u_sunWarmColor" );	// S2 (codex P2b)
	out.uMoonDir = UniformLoc( prog, "u_moonDir" );			// S2
	out.uMoonColor = UniformLoc( prog, "u_moonColor" );		// S2
	out.uNightSky = UniformLoc( prog, "u_nightSky" );		// S2
	out.uNightFloor = UniformLoc( prog, "u_nightFloor" );		// S2
	out.uNightK = UniformLoc( prog, "u_nightK" );			// S2
	out.uNightMoon = UniformLoc( prog, "u_nightMoon" );		// S2
	out.uRimStrength = UniformLoc( prog, "u_rimStrength" );		// S4
	out.uRimPower = UniformLoc( prog, "u_rimPower" );		// S4
	out.uStudioRender = UniformLoc( prog, "u_studioRender" );	// MUST 2 (base only; -1 on lit/depth/shell)
	out.uRenderAmt = UniformLoc( prog, "u_renderAmt" );		// MUST 2
	out.uRenderColor = UniformLoc( prog, "u_renderColor" );		// MUST 2
	out.uLightOrigin = UniformLoc( prog, "u_lightOrigin" );
	out.uLightDir = UniformLoc( prog, "u_lightDir" );
	out.uLightColor = UniformLoc( prog, "u_lightColor" );
	out.uLightRadius = UniformLoc( prog, "u_lightRadius" );
	out.uCosInner = UniformLoc( prog, "u_cosInner" );
	out.uCosOuter = UniformLoc( prog, "u_cosOuter" );
	out.uMatShadow = UniformLoc( prog, "u_matShadow" );
	out.uHasShadow = UniformLoc( prog, "u_hasShadow" );
	out.uV3 = UniformLoc( prog, "u_v3" );				// L5R crisp profile (lit only; -1 on base)
	out.uEdgeExp = UniformLoc( prog, "u_edgeExp" );
	out.uHotspotGain = UniformLoc( prog, "u_hotspotGain" );
	out.uHotspotSharp = UniformLoc( prog, "u_hotspotSharp" );
	out.uDirectGain = UniformLoc( prog, "u_directGain" );
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
		s_studio.depthProgram.program = 0;
	}

	// Init-time shaders: compile failure is FATAL (spec 3.2).
	BuildProgram( "csz_studio", kStudioVs, kStudioFs, true, s_studio.program );
	QueryPassLocs( s_studio.program, s_studio.baseLocs );

	UseProgram( s_studio.program.program );
	glUniform1i( UniformLoc( s_studio.program, "u_texDiffuse" ), 0 );

	// Fixed top-down shade direction (plan section 6 step 3 M1 lighting).
	const float kShadeDir[3] = { 0.0f, 0.0f, 1.0f };

	glUniform3fv( UniformLoc( s_studio.program, "u_shadeDir" ), 1, kShadeDir );

	// Ambience defaults until the per-pass feed (fog off, tint neutral --
	// a zeroed u_ambTint would render everything black).
	const float kFogOff[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const float kTintNeutral[3] = { 1.0f, 1.0f, 1.0f };
	const float kFogParamsDefault[4] = { 0.0f, 0.0f, 1.0f, 0.0f };	// b=0, glow=0, maxOpacity=1 (no floor)
	const float kCamPosZero[3] = { 0.0f, 0.0f, 0.0f };

	glUniform4fv( s_studio.baseLocs.uFog, 1, kFogOff );
	glUniform4fv( s_studio.baseLocs.uFogParams, 1, kFogParamsDefault );
	// S3 fog defaults = legacy identity (extTint achromatic, noise off, fogLit 0).
	const float kFogParams2Default[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
	const float kFogParams3Default[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	if( s_studio.baseLocs.uFogParams2 >= 0 ) glUniform4fv( s_studio.baseLocs.uFogParams2, 1, kFogParams2Default );
	if( s_studio.baseLocs.uFogParams3 >= 0 ) glUniform4fv( s_studio.baseLocs.uFogParams3, 1, kFogParams3Default );
	if( s_studio.baseLocs.uFogLit >= 0 )     glUniform3fv( s_studio.baseLocs.uFogLit, 1, kCamPosZero );
	glUniform3fv( s_studio.baseLocs.uCamPos, 1, kCamPosZero );
	glUniform3fv( s_studio.baseLocs.uAmbTint, 1, kTintNeutral );
	glUniform1f( s_studio.baseLocs.uSkyAmbScale, 1.0f );	// L3b: neutral until fed (no sky-ambient dimming)
	glUniform1f( s_studio.baseLocs.uSkyVis, 1.0f );		// S1: outdoor fail-safe until a per-entity value is fed
	const float kMoonInScatterZero[3] = { 0.0f, 0.0f, 0.0f };	// L4: no moon in-scatter until fed (no-op)
	if( s_studio.baseLocs.uMoonInScatter >= 0 ) glUniform3fv( s_studio.baseLocs.uMoonInScatter, 1, kMoonInScatterZero );
	if( s_studio.baseLocs.uShaftMask >= 0 )     glUniform1f( s_studio.baseLocs.uShaftMask, 1.0f );	// gaps-pass identity
	if( s_studio.baseLocs.uMoonShaft >= 0 )     glUniform1f( s_studio.baseLocs.uMoonShaft, 0.0f );	// disabled until fed
	// S2: default to the approved DAY look until the per-frame feed (nightness 0 ->
	// approvedDay branch, sunWarmColor/moon/night colors 0 so the model is dormant).
	const float kNightZero3[3] = { 0.0f, 0.0f, 0.0f };
	if( s_studio.baseLocs.uNightModel >= 0 )     glUniform1f( s_studio.baseLocs.uNightModel, 1.0f );
	if( s_studio.baseLocs.uNightness >= 0 )      glUniform1f( s_studio.baseLocs.uNightness, 0.0f );
	if( s_studio.baseLocs.uSunWarmColor >= 0 )   glUniform3fv( s_studio.baseLocs.uSunWarmColor, 1, kNightZero3 );
	if( s_studio.baseLocs.uMoonDir >= 0 )        glUniform3fv( s_studio.baseLocs.uMoonDir, 1, kCamPosZero );
	if( s_studio.baseLocs.uMoonColor >= 0 )      glUniform3fv( s_studio.baseLocs.uMoonColor, 1, kNightZero3 );
	if( s_studio.baseLocs.uNightSky >= 0 )       glUniform3fv( s_studio.baseLocs.uNightSky, 1, kNightZero3 );
	if( s_studio.baseLocs.uNightFloor >= 0 )     glUniform3fv( s_studio.baseLocs.uNightFloor, 1, kNightZero3 );
	if( s_studio.baseLocs.uNightK >= 0 )         glUniform1f( s_studio.baseLocs.uNightK, 0.7f );
	if( s_studio.baseLocs.uNightMoon >= 0 )      glUniform1f( s_studio.baseLocs.uNightMoon, 1.0f );
	// S4 rim defaults: off until the per-frame feed (rim is gated by nightness anyway).
	if( s_studio.baseLocs.uRimStrength >= 0 )    glUniform1f( s_studio.baseLocs.uRimStrength, 0.0f );
	if( s_studio.baseLocs.uRimPower >= 0 )       glUniform1f( s_studio.baseLocs.uRimPower, 3.0f );
	// MUST 2 rendermode defaults: opaque (mode 0). The opaque pass keeps these (BeginStudioPass
	// re-pins mode 0); only the transparent sub-pass overrides them per entity. Mode 0 ->
	// fragColor = vec4(col,1) byte-identical to the pre-rendermode studio path.
	if( s_studio.baseLocs.uStudioRender >= 0 )   glUniform1i( s_studio.baseLocs.uStudioRender, 0 );
	if( s_studio.baseLocs.uRenderAmt >= 0 )      glUniform1f( s_studio.baseLocs.uRenderAmt, 1.0f );
	if( s_studio.baseLocs.uRenderColor >= 0 )    glUniform3fv( s_studio.baseLocs.uRenderColor, 1, kTintNeutral );

	BuildProgram( "csz_studio_lit", kStudioLitVs, kStudioLitFs, true, s_studio.litProgram );
	QueryPassLocs( s_studio.litProgram, s_studio.litLocs );

	UseProgram( s_studio.litProgram.program );
	glUniform1i( UniformLoc( s_studio.litProgram, "u_texDiffuse" ), 0 );
	glUniform1i( UniformLoc( s_studio.litProgram, "u_shadowMap" ), 2 );
	UseProgram( 0 );

	// Depth program (T7 shadow map pass): only u_viewProj/u_bones resolve,
	// every other PassLocs member is -1 (glUniform* no-ops) by design.
	BuildProgram( "csz_studio_depth", kStudioDepthVs, kStudioDepthFs, true, s_studio.depthProgram );
	QueryPassLocs( s_studio.depthProgram, s_studio.depthLocs );

	// MUST 3 glow-shell program: u_viewProj/u_bones resolve via QueryPassLocs (reused by
	// DrawModelMeshes for the bone upload); u_extrude/u_shellColor/u_shellAlpha are shell-only.
	BuildProgram( "csz_studio_shell", kStudioShellVs, kStudioShellFs, true, s_studio.shellProgram );
	QueryPassLocs( s_studio.shellProgram, s_studio.shellLocs );
	s_studio.shellExtrude = UniformLoc( s_studio.shellProgram, "u_extrude" );
	s_studio.shellColor = UniformLoc( s_studio.shellProgram, "u_shellColor" );
	s_studio.shellAlpha = UniformLoc( s_studio.shellProgram, "u_shellAlpha" );

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

		// Depth + shell passes read positions/bones only: no texture, no per-mesh
		// uniforms (their programs resolve none of them anyway).
		if( !s_studio.depthPass && !s_studio.shellPass )
		{
			int texSlot = ( mesh.texSlot != 0 ) ? mesh.texSlot : s_studio.whiteTexSlot;

			if( texSlot != curTex )
			{
				BindTextureSlot( 0, texSlot );
				curTex = texSlot;
			}

			SetMeshUniforms( mesh.texFlags, lightColor, false );
		}

		BindVao( mesh.vao );
		glDrawElements( GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, (const void *)0 );
	}
}

// MUST 2 per-entity rendermode uniforms, resolved on the CPU and fed to the base program
// (the GL blend state itself is chosen by the DrawTransparent loop / ResolveRendermode).
struct StudioTransUniforms
{
	int mode;		// u_studioRender: 1 trans / 2 color / 3 texalpha
	float amt;		// u_renderAmt [0,1]
	float color[3];		// u_renderColor (kRenderTransColor flat fill)
};

// Full single-entity path: model resolve, cull, bones, light, meshes,
// player p_ weapon merge. Returns true when something was drawn.
// spotCull != NULL additionally rejects entities outside that light's reach
// (lit-additive pass). trans != NULL feeds the MUST 2 rendermode uniforms
// (transparent sub-pass; base program only).
bool DrawEntity( const ViewSetup &view, cl_entity_s *ent, bool doCull, const SpotLightParams *spotCull,
	const StudioTransUniforms *trans = NULL )
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

	// The lit/depth passes ignore ambient/shade (locations -1); skip the
	// LightVec trace there -- it would run once per entity per pass otherwise.
	if( !s_studio.litPass && !s_studio.depthPass )
	{
		SampleEntityLight( ent, lightColor );
		// S1: per-entity geometric sky visibility sampled at the entity origin
		// (base pass only). The studio FS does not consume u_skyVis yet, so the
		// GLSL compiler strips it (uSkyVis == -1) and we skip the sample entirely
		// -- this auto-activates in S2 the moment the FS reads u_skyVis. S1 proves
		// the wiring; S2 reads it.
		if( s_studio.locs->uSkyVis >= 0 )
			glUniform1f( s_studio.locs->uSkyVis, g_world.SkyVisAtPoint( ent->origin ));
	}

	// MUST 2: feed the per-entity rendermode uniforms (base program only; the lit/depth/shell
	// programs resolve these to -1, so the glUniform* calls no-op there). NULL = opaque pass.
	if( trans != NULL )
	{
		if( s_studio.locs->uStudioRender >= 0 ) glUniform1i( s_studio.locs->uStudioRender, trans->mode );
		if( s_studio.locs->uRenderAmt >= 0 )    glUniform1f( s_studio.locs->uRenderAmt, trans->amt );
		if( s_studio.locs->uRenderColor >= 0 )  glUniform3fv( s_studio.locs->uRenderColor, 1, trans->color );
	}

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

// S4 §7 competitive rim-light cvars (csz_rim strength + csz_rim_power Fresnel exponent).
// USER "口味" knobs, registered once at HUD init via StudioRegisterCvars() (defined below,
// in the csz namespace) for init parity with the other layers' …RegisterCvars() seams
// (CONVENTIONS.md), then read live in BeginStudioPassWith. NULL until that registration runs;
// the feed below falls back to the baked defaults if so.
cvar_t *s_rimCvar = NULL;
cvar_t *s_rimPowerCvar = NULL;

// MUST 2/3 feature cvars (registered in StudioRegisterCvars; default 1). NULL until that runs;
// the read sites below fail-safe ON (feature active) so a missing registration never silently
// drops the transparent / glowshell passes.
cvar_t *s_rendermodeCvar = NULL;	// csz_studio_rendermode
cvar_t *s_renderfxCvar = NULL;		// csz_renderfx

inline bool RendermodeEnabled() { return ( s_rendermodeCvar == NULL ) || ( s_rendermodeCvar->value != 0.0f ); }
inline bool RenderfxEnabled()   { return ( s_renderfxCvar == NULL ) || ( s_renderfxCvar->value != 0.0f ); }

// Per-frame observability counters (reset at the top of each owning pass).
int s_transDrawn = 0;
int s_shellDrawn = 0;

// ---------------------------------------------------------------------------
// MUST 3 render-FX blend (csz_renderfx). Stock CL_FxBlend maps curstate.renderfx +
// renderamt + time to an effective 0..255 alpha. The time-driven families (Pulse, Strobe,
// Flicker) are reproduced statelessly from the latched frame time plus a per-entity phase
// (ent->index) so models do not blink in lockstep. The ramp families (Fade*/Solid*) need a
// per-entity latched spawn time the takeover does not track -> they pass renderamt through
// (documented approximation; honest-cut in the report). Returns alpha in [0,1].
float CszStudioFxBlend( const cl_entity_s *ent, float time )
{
	float amt = (float)ent->curstate.renderamt;	// 0..255

	if( !RenderfxEnabled())
		return amt * ( 1.0f / 255.0f );

	float phase = (float)( ent->index & 31 );	// decorrelate per entity
	float blend = amt;

	switch( ent->curstate.renderfx )
	{
	case kRenderFxPulseSlow:
		blend = amt + 16.0f * sinf( time * 2.0f + phase );
		break;
	case kRenderFxPulseFast:
		blend = amt + 16.0f * sinf( time * 8.0f + phase );
		break;
	case kRenderFxPulseSlowWide:
		blend = amt + 64.0f * sinf( time * 2.0f + phase );
		break;
	case kRenderFxPulseFastWide:
		blend = amt + 64.0f * sinf( time * 8.0f + phase );
		break;
	case kRenderFxStrobeSlow:
		blend = ( sinf( time * 4.0f + phase ) * 20.0f < 0.0f ) ? 0.0f : amt;
		break;
	case kRenderFxStrobeFast:
		blend = ( sinf( time * 16.0f + phase ) * 20.0f < 0.0f ) ? 0.0f : amt;
		break;
	case kRenderFxStrobeFaster:
		blend = ( sinf( time * 36.0f + phase ) * 20.0f < 0.0f ) ? 0.0f : amt;
		break;
	case kRenderFxFlickerSlow:
		blend = amt + 16.0f * ( sinf( time * 16.0f ) + sinf( time * 23.0f + phase ));
		break;
	case kRenderFxFlickerFast:
		blend = amt + 16.0f * ( sinf( time * 36.0f ) + sinf( time * 43.0f + phase ));
		break;
	case kRenderFxFadeSlow:
	case kRenderFxFadeFast:
	case kRenderFxSolidSlow:
	case kRenderFxSolidFast:
		// True monotonic ramp needs a per-entity latched start (deferred); pass amt through.
		blend = amt;
		break;
	default:
		blend = amt;
		break;
	}

	if( blend < 0.0f ) blend = 0.0f;
	if( blend > 255.0f ) blend = 255.0f;
	return blend * ( 1.0f / 255.0f );
}

// MUST 2 rendermode classification. An entity is opaque when rendermode is kRenderNormal
// (or rendermode handling is disabled). Everything else is routed to the transparent sub-pass.
inline bool IsTransparentEntity( const cl_entity_s *ent )
{
	if( !RendermodeEnabled())
		return false;	// A/B off: everything stays in the opaque pass (pre-MUST-2 behavior)

	return ent->curstate.rendermode != kRenderNormal;
}

// Transparent studio sort buffer. Capacity restated locally (geom must not include the
// composition root's FrameEntities::kMaxEntities = 1024; csz_sprite.cpp does the same).
const int kMaxTransStudio = 1024;

struct TransItem
{
	cl_entity_s *ent;
	float distSq;	// to view origin, for back-to-front compositing
};

TransItem s_transItems[kMaxTransStudio];

int CompareTransItems( const void *a, const void *b )
{
	const TransItem *ia = (const TransItem *)a;
	const TransItem *ib = (const TransItem *)b;

	if( ia->distSq > ib->distSq ) return -1;	// far first
	if( ia->distSq < ib->distSq ) return 1;
	return 0;
}

// Maps curstate.rendermode -> the base-program color/alpha SOURCE (u_studioRender) and the
// GL blend/depth state, mirroring the sprite-pass rendermode mapping in csz_sprite.cpp.
// Returns false for kRenderNormal (should never reach here -- those stay opaque).
bool ResolveRendermode( const cl_entity_s *ent, StudioTransUniforms &u, BlendMode &blend, bool &depthTest )
{
	depthTest = true;

	switch( ent->curstate.rendermode )
	{
	case kRenderTransColor:
		u.mode = 2;			// flat rendercolor fill
		blend = kBlendAlpha;
		break;
	case kRenderTransTexture:
		u.mode = 1;			// lit texture * alpha
		blend = kBlendAlpha;
		break;
	case kRenderTransAlpha:
		u.mode = 3;			// texture's own alpha
		blend = kBlendAlpha;
		break;
	case kRenderTransAdd:
		u.mode = 1;
		blend = kBlendAdditive;
		break;
	case kRenderGlow:
	case kRenderWorldGlow:
		u.mode = 1;
		blend = kBlendAdditive;
		depthTest = false;		// glow: no Z checks (engine parity)
		break;
	default:
		return false;
	}

	// rendercolor (0,0,0 promoted to white, notes-mechanisms f-16) for the flat-fill mode.
	const color24 &c = ent->curstate.rendercolor;

	if( c.r || c.g || c.b )
	{
		u.color[0] = (float)c.r * ( 1.0f / 255.0f );
		u.color[1] = (float)c.g * ( 1.0f / 255.0f );
		u.color[2] = (float)c.b * ( 1.0f / 255.0f );
	}
	else
	{
		u.color[0] = u.color[1] = u.color[2] = 1.0f;
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

	// Ambience feed (M2a A1, plan 2.5 slot 7.2 snapshot riding on the view).
	// Only the base program has these uniforms; lit/depth locations are -1
	// (glUniform* no-op), keeping those passes fog-free (pitfall 23).
	const AmbienceParams &amb = view.ambience;
	// S3 fog (REWORK-SPEC §S3, finding 5: ONE inscatter owner). Players/models fog with
	// the SAME extended equation as the world (consistency); the retired CszApplyFogAmbient
	// achromatic gray is gone. s_studio.time = the noise drift clock (latched at BeginFrame).
	float fogVec[4], fogParams[4], fogParams2[4], fogParams3[4], fogLit[3];
	CszFogUniformVecsEx( amb, s_studio.time, fogVec, fogParams, fogParams2, fogParams3, fogLit );
	glUniform4fv( locs.uFog, 1, fogVec );
	glUniform4fv( locs.uFogParams, 1, fogParams );
	if( locs.uFogParams2 >= 0 ) glUniform4fv( locs.uFogParams2, 1, fogParams2 );
	if( locs.uFogParams3 >= 0 ) glUniform4fv( locs.uFogParams3, 1, fogParams3 );
	if( locs.uFogLit >= 0 )     glUniform3fv( locs.uFogLit, 1, fogLit );
	glUniform3fv( locs.uCamPos, 1, view.origin );	// ray origin for the height-fog integral
	glUniform3fv( locs.uAmbTint, 1, amb.tint );
	glUniform1f( locs.uSkyAmbScale, amb.skyAmbientScale );	// L3b sky-ambient cloud dimmer (1.0 when clouds off; floored >=0.6 in L3a)
	// Directional N.L (sky 档1): only the base program declares u_sunDir/u_sunColor;
	// lit/depth locs are -1 (glUniform* no-op), so those passes stay exempt (pitfall 23).
	glUniform3fv( locs.uSunDir, 1, amb.moonlightDir );
	glUniform3fv( locs.uSunColor, 1, amb.moonlightColor );	// (0,0,0) when the body light is off
	// fog M1 L4 moon Tyndall air-glow: dedicated L2 in-scatter channel (premultiplied),
	// cloud-gap shaftMask (L3a, consumed), csz_moonshaft toggle (0 -> exact pre-L4).
	{
		float moonRGB[3];
		float intensity = CszMoonInScatter( amb, moonRGB );
		float premul[3] = { moonRGB[0] * intensity, moonRGB[1] * intensity, moonRGB[2] * intensity };
		if( locs.uMoonInScatter >= 0 ) glUniform3fv( locs.uMoonInScatter, 1, premul );
		if( locs.uShaftMask >= 0 )     glUniform1f( locs.uShaftMask, amb.shaftMask );
		if( locs.uMoonShaft >= 0 )     glUniform1f( locs.uMoonShaft, CszMoonShaftEnabled() );
	}
	// S2 physical night model feed (REWORK-SPEC §S2). Studio-specific night calibration
	// ([1] index, finding 9). The gated moon directional reuses amb.moonDir + the premul
	// moon-only amb.moonSurfaceDirect (0 unless the moon is up). The per-entity u_skyVis
	// is fed separately at the per-mesh site (DARK indoors / no moon leak). lit/depth locs
	// are -1 (glUniform* no-op), so those passes stay exempt (pitfall 23).
	if( locs.uNightModel >= 0 )     glUniform1f( locs.uNightModel, amb.nightModel );
	if( locs.uNightness >= 0 )      glUniform1f( locs.uNightness, amb.nightness );
	if( locs.uSunWarmColor >= 0 )   glUniform3fv( locs.uSunWarmColor, 1, amb.sunSurfaceDirect );	// warm sun-only premul (ungated; codex P2b)
	if( locs.uMoonDir >= 0 )        glUniform3fv( locs.uMoonDir, 1, amb.moonDir );
	if( locs.uMoonColor >= 0 )      glUniform3fv( locs.uMoonColor, 1, amb.moonSurfaceDirect );	// premul, 0 when moon down
	if( locs.uNightSky >= 0 )       glUniform3fv( locs.uNightSky, 1, amb.nightSky[1] );		// studio calibration ([1])
	if( locs.uNightFloor >= 0 )     glUniform3fv( locs.uNightFloor, 1, amb.nightFloor[1] );
	if( locs.uNightK >= 0 )         glUniform1f( locs.uNightK, amb.nightK[1] );
	if( locs.uNightMoon >= 0 )      glUniform1f( locs.uNightMoon, amb.nightMoonGain );
	// S4 §7 competitive rim light feed. USER "口味" knobs (csz_rim strength, csz_rim_power
	// Fresnel exponent), registered once at HUD init via StudioRegisterCvars() (S4-fix: codex
	// S4 Low -- init parity, no more lazy register in this draw prologue) and read live here.
	// The shader gates the rim on u_nightness so DAY is byte-identical regardless.
	{
		float rim = ( s_rimCvar != NULL ) ? s_rimCvar->value : 0.30f;
		float rimP = ( s_rimPowerCvar != NULL ) ? s_rimPowerCvar->value : 3.0f;
		if( rim < 0.0f ) rim = 0.0f;  if( rim > 2.0f ) rim = 2.0f;	// sane band
		if( rimP < 0.1f ) rimP = 0.1f;  if( rimP > 16.0f ) rimP = 16.0f;
		if( locs.uRimStrength >= 0 )    glUniform1f( locs.uRimStrength, rim );
		if( locs.uRimPower >= 0 )       glUniform1f( locs.uRimPower, rimP );
	}

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

	// MUST 2: re-pin opaque rendermode (mode 0). The transparent sub-pass shares this base
	// program and leaves u_studioRender at 1/2/3, so the opaque pass (and DrawSingle) MUST
	// reset it or it would inherit a stale transparent mode the next frame.
	if( s_studio.baseLocs.uStudioRender >= 0 ) glUniform1i( s_studio.baseLocs.uStudioRender, 0 );
	if( s_studio.baseLocs.uRenderAmt >= 0 )    glUniform1f( s_studio.baseLocs.uRenderAmt, 1.0f );
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
	// L5R crisp direct profile (matches the world lit pass).
	glUniform1f( locs.uV3, light.v3 );
	glUniform1f( locs.uEdgeExp, light.edgeExp );
	glUniform1f( locs.uHotspotGain, light.hotspotGain );
	glUniform1f( locs.uHotspotSharp, light.hotspotSharp );
	glUniform1f( locs.uDirectGain, light.directGain );

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

// Shadow depth prologue: depth program + far-side-only facing. The base pass
// culls GL_FRONT because .mdl outward triangles are CW wound (= GL back
// facing, T3 step 4 verdict); from the light's viewpoint the away side is
// therefore GL FRONT facing, so rendering ONLY it (the acne trick of
// notes-mechanisms e) means culling GL_BACK here.
void BeginStudioDepthPass( const ViewSetup &lightView )
{
	s_studio.depthPass = true;
	BeginStudioPassWith( lightView, s_studio.depthProgram, s_studio.depthLocs );
	SetCullFront( false );
}

void EndStudioDepthPass()
{
	SetCullFront( true );	// base-pass facing (BeginStudioPassWith default)
	s_studio.depthPass = false;
	s_studio.locs = &s_studio.baseLocs;
	EndStudioPass();
}

// MUST 3 glow-shell prologue: shell program + additive blend, depth test ON (shells are
// occluded by nearer opaque geometry) but depth WRITE off. Front-face cull matches the base
// pass (.mdl outward winding = GL back, T3); the extruded shell keeps that orientation.
void BeginStudioShellPass( const ViewSetup &view )
{
	EnsureShader();
	s_studio.shellPass = true;
	s_studio.locs = &s_studio.shellLocs;
	UseProgram( s_studio.shellProgram.program );
	glUniformMatrix4fv( s_studio.shellLocs.uViewProj, 1, GL_FALSE, view.matViewProj.m );
	SetCull( true );
	SetCullFront( true );
	SetBlend( kBlendAdditive );
	SetDepthWrite( false );
	SetDepthTest( true );
}

void EndStudioShellPass()
{
	SetBlend( kBlendNone );
	SetDepthWrite( true );
	s_studio.shellPass = false;
	s_studio.locs = &s_studio.baseLocs;
	EndStudioPass();
}

// Resolves the actual studio model for an entity (player userinfo/forced models included),
// returning NULL when the entity has no drawable studio model. Shared by the shell pass.
studiohdr_t *ResolveStudioModel( cl_entity_s *ent, model_t **outMod )
{
	model_t *mod = ent->model;

	if( ent->player )
	{
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
		return NULL;

	studiohdr_t *hdr = (studiohdr_t *)IEngineStudio.Mod_Extradata( mod );

	if( hdr == NULL || hdr->numbodyparts <= 0 || hdr->numbones <= 0 || hdr->numseq <= 0 )
		return NULL;

	*outMod = mod;
	return hdr;
}

// Draws one glow-shell entity (extruded additive flat-color shell). Returns true if drawn.
bool DrawShellEntity( const ViewSetup &view, cl_entity_s *ent, float extrude )
{
	model_t *mod = NULL;
	studiohdr_t *hdr = ResolveStudioModel( ent, &mod );

	if( hdr == NULL )
		return false;

	if( CullEntity( view, ent, hdr ))
		return false;

	// Shell color from rendercolor; all-zero (no color set) -> nothing to add, skip.
	const color24 &c = ent->curstate.rendercolor;

	if( !c.r && !c.g && !c.b )
		return false;

	float shellColor[3] = { (float)c.r * ( 1.0f / 255.0f ),
		(float)c.g * ( 1.0f / 255.0f ), (float)c.b * ( 1.0f / 255.0f ) };

	const BoneSetup *bones = NULL;

	if( !SetupBones( ent, hdr, s_studio.time, &bones ))
		return false;

	StudioModelGpu *gpu = GetOrBuild( mod, hdr );

	if( gpu == NULL )
		return false;

	const float kShellAlpha = 0.5f;	// fixed glow brightness (renderamt drives sprites, not shells)

	glUniform1f( s_studio.shellExtrude, extrude );
	glUniform3fv( s_studio.shellColor, 1, shellColor );
	glUniform1f( s_studio.shellAlpha, kShellAlpha );

	float dummyLight[3] = { 1.0f, 1.0f, 1.0f };	// unused (shell pass skips per-mesh uniforms)

	DrawModelMeshes( gpu, hdr, ent->curstate.body, bones, dummyLight );
	return true;
}

}

// S4-fix (codex S4 Low): register the studio rim cvars once at HUD init, parity with the
// other layers' …RegisterCvars() seams. Idempotent (guards re-entry on vid restart paths).
void StudioRegisterCvars()
{
	if( s_rimCvar == NULL )
		s_rimCvar = gEngfuncs.pfnRegisterVariable( "csz_rim", "0.30", FCVAR_CLIENTDLL );
	if( s_rimPowerCvar == NULL )
		s_rimPowerCvar = gEngfuncs.pfnRegisterVariable( "csz_rim_power", "3.0", FCVAR_CLIENTDLL );
	// MUST 1/2/3 observability cvars (default 1 = feature ON). csz_attach is read by the bones
	// module via pfnGetCvarPointer; csz_studio_rendermode / csz_renderfx are read here.
	gEngfuncs.pfnRegisterVariable( "csz_attach", "1", FCVAR_CLIENTDLL );
	if( s_rendermodeCvar == NULL )
		s_rendermodeCvar = gEngfuncs.pfnRegisterVariable( "csz_studio_rendermode", "1", FCVAR_CLIENTDLL );
	if( s_renderfxCvar == NULL )
		s_renderfxCvar = gEngfuncs.pfnRegisterVariable( "csz_renderfx", "1", FCVAR_CLIENTDLL );
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

	if( s_studio.depthProgram.program != 0 )
	{
		if( s_studio.shaderGeneration == GpuGeneration())
			DestroyProgram( s_studio.depthProgram );
		else
			s_studio.depthProgram.program = 0;
	}

	if( s_studio.shellProgram.program != 0 )
	{
		if( s_studio.shaderGeneration == GpuGeneration())
			DestroyProgram( s_studio.shellProgram );
		else
			s_studio.shellProgram.program = 0;
	}

	s_studio.shaderReady = false;
}

void StudioRenderer::BeginFrame( float time )
{
	s_studio.time = time;
	ResetBoneCache();
}

void StudioRenderer::DrawOpaque( const ViewSetup &view, cl_entity_s *const *ents, int count )
{
	if( count <= 0 )
		return;

	BeginStudioPass( view );

	int drawn = 0;

	for( int i = 0; i < count; i++ )
	{
		// MUST 2: non-opaque entities are handled by DrawTransparent (transparent domain).
		if( ents[i] != NULL && !IsTransparentEntity( ents[i] ) && DrawEntity( view, ents[i], true, NULL ))
			drawn++;
	}

	EndStudioPass();

	// Per-frame stats at Dev level with 1s self-throttle (R8). MUST 1: the attach count
	// rides this line (one bone setup per entity this frame wrote ent->attachment[]).
	static float s_nextStatsTime;
	float now = ClientTime();

	if( now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "studio", "drawn %d / %d studio entities; attach %d", drawn, count, StudioFrameAttachCount());
	}
}

void StudioRenderer::DrawDepth( const ViewSetup &lightView, const Frustum &lightCull,
	cl_entity_s *const *ents, int count )
{
	if( count <= 0 )
		return;

	// Per-entity culling runs against the LIGHT frustum (contract parameter;
	// 5 planes, far disabled): route it through DrawEntity's view cull.
	ViewSetup cullView = lightView;

	cullView.frustum = lightCull;

	BeginStudioDepthPass( lightView );

	int drawn = 0;

	for( int i = 0; i < count; i++ )
	{
		if( ents[i] != NULL && DrawEntity( cullView, ents[i], true, NULL ))
			drawn++;
	}

	EndStudioDepthPass();

	// Per-frame stats at Dev level with 1s self-throttle (R8).
	static float s_nextStatsTime;
	float now = ClientTime();

	if( now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "studio", "shadow depth: %d / %d studio entities", drawn, count );
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

void StudioRenderer::DrawTransparent( const ViewSetup &view, cl_entity_s *const *ents, int count )
{
	if( count <= 0 || !RendermodeEnabled())
		return;

	// Gather non-opaque entities with a view-distance key for back-to-front compositing.
	int numItems = 0;

	for( int i = 0; i < count && numItems < kMaxTransStudio; i++ )
	{
		cl_entity_s *ent = ents[i];

		if( ent == NULL || !IsTransparentEntity( ent ))
			continue;

		float dx = ent->origin[0] - view.origin[0];
		float dy = ent->origin[1] - view.origin[1];
		float dz = ent->origin[2] - view.origin[2];

		s_transItems[numItems].ent = ent;
		s_transItems[numItems].distSq = dx * dx + dy * dy + dz * dz;
		numItems++;
	}

	if( numItems == 0 )
		return;

	qsort( s_transItems, (size_t)numItems, sizeof( TransItem ), CompareTransItems );

	BeginStudioPass( view );
	SetDepthWrite( false );	// transparent geometry never writes depth

	s_transDrawn = 0;

	for( int i = 0; i < numItems; i++ )
	{
		cl_entity_s *ent = s_transItems[i].ent;
		StudioTransUniforms u;
		BlendMode blend;
		bool depthTest;

		if( !ResolveRendermode( ent, u, blend, depthTest ))
			continue;	// unmapped rendermode (should not happen: IsTransparentEntity filtered)

		u.amt = CszStudioFxBlend( ent, s_studio.time );	// MUST 3 renderfx-modulated alpha

		if( u.amt <= 0.0f )
			continue;	// strobe off-phase / fully faded

		SetBlend( blend );
		SetDepthTest( depthTest );

		if( DrawEntity( view, ent, true, NULL, &u ))
			s_transDrawn++;
	}

	// Hand a clean baseline to the next pass.
	SetBlend( kBlendNone );
	SetDepthTest( true );
	SetDepthWrite( true );
	EndStudioPass();

	static float s_nextStatsTime;
	float now = ClientTime();

	if( s_transDrawn > 0 && now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "studio", "transparent %d / %d studio entities", s_transDrawn, numItems );
	}
}

void StudioRenderer::DrawGlowShells( const ViewSetup &view, cl_entity_s *const *ents, int count )
{
	if( count <= 0 || !RenderfxEnabled())
		return;

	// Distance cap (perf): glow shells are an extra full-mesh additive pass, so skip distant
	// ones -- the effect is unreadable far away anyway.
	const float kShellMaxDist = 2000.0f;
	const float kShellMaxDistSq = kShellMaxDist * kShellMaxDist;
	const float kShellExtrude = 2.0f;	// outward shell thickness (world units)

	// One cheap pre-scan: do any entities actually request a glow shell? Keeps the pass a
	// true no-op (no program switch / state churn) on the common shell-free frame.
	int candidates = 0;

	for( int i = 0; i < count; i++ )
	{
		if( ents[i] != NULL && ents[i]->curstate.renderfx == kRenderFxGlowShell )
		{
			candidates++;
			break;
		}
	}

	if( candidates == 0 )
		return;

	BeginStudioShellPass( view );

	s_shellDrawn = 0;

	for( int i = 0; i < count; i++ )
	{
		cl_entity_s *ent = ents[i];

		if( ent == NULL || ent->curstate.renderfx != kRenderFxGlowShell )
			continue;

		float dx = ent->origin[0] - view.origin[0];
		float dy = ent->origin[1] - view.origin[1];
		float dz = ent->origin[2] - view.origin[2];

		if( dx * dx + dy * dy + dz * dz > kShellMaxDistSq )
			continue;

		if( DrawShellEntity( view, ent, kShellExtrude ))
			s_shellDrawn++;
	}

	EndStudioShellPass();

	static float s_nextStatsTime;
	float now = ClientTime();

	if( s_shellDrawn > 0 && now >= s_nextStatsTime )
	{
		s_nextStatsTime = now + 1.0f;
		CSZ_LogDev( "studio", "glowshell %d studio entities", s_shellDrawn );
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
