/*
 * csz_dust.cpp -- CSOZ renderer: gated airborne dust motes (L7)
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
// L7 render order (csz_renderer.cpp): AFTER the L6a cone (slot 13.4) and the fog
// Step-3 flashlight march + Step-4 god rays (slot 13.5, kTmVolume), BEFORE the
// transparent/sprite pass (slot 14). A SEPARATE additive pass into the SAME bound
// HDR FBO -- the order among the additive in-scatter passes is commutative; placing
// dust last among them matches the plan ordering (opaque -> spot direct -> cone ->
// fog march/god rays -> dust L7). Reuses g_lights (the spot registry) for the cone
// gate, the ambience moon-shaft channels (csz_ambience_types.h) for the moon gate,
// and the SkyCompose depth texture for the soft-particle fade.
//
// THE PERF CONTRACT: a mote is illuminated ONLY when it is inside a flashlight cone
// OR the moon Tyndall shaft. Unlit motes (bright < epsilon) are not simulated into
// the draw and NEVER enter the dynamic VBO -- the gate lives in the CPU spawn/cull
// fill, not in a "draw all N then alpha->0" shader. Walking the persistent pool is
// cheap scalar work; the GPU only ever sees the handful of currently-lit motes.
#include "csz_dust.h"
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
#include "../core/csz_ambience_types.h"
#include "../geom/csz_sky_compose.h"

#include <math.h>
#include <string.h>
#include <string>

namespace csz
{

#include "../fog/csz_fog_shaders.inl"   // kFogDepthReconstructGlsl (linViewZ helper)
#include "csz_dust_shaders.inl"         // kDustVs / kDustFsBody

namespace
{

// Fixed pool capacity. csz_dust_count clamps into [0, kMaxDust]; the pool is seeded
// to capacity once so any count works without re-seeding (a re-seed would pop motes).
// Playtest r1 (operator ask 灰尘很小很小、多一点点): raised 4096 -> 8192 so the denser
// default count (7000) fits under the cap with headroom. The per-mote area shrinks
// ~5x (size 1.0 -> 0.45) so total fill DROPS despite the higher count (see DustRender).
// Fog rewrite §5.4 (USER ask: MORE dust): raised 8192 -> 16384 so csz_dust_count can be
// pushed to a denser haze. Motes are tiny (size 0.45) and only LIT motes ever enter the
// VBO, so the cap is a headroom ceiling, not a per-frame cost; the scratch grows to
// ~16384*6*8 floats (~3 MB static), still trivial.
const int kMaxDust = 16384;

const int   kVertsPerQuad  = 6;   // two triangles, non-indexed (no instancing)
const int   kFloatsPerVert = 8;   // world(3) + uv(2) + color(3)
const float kPi            = 3.14159265358979323846f;

// Scatter model (CPU): motes inside the lit volume are DIRECTLY illuminated, so a base
// term carries first-person visibility (camera == lamp -> the HG forward lobe degenerates
// to back-scatter, exactly as in the L6a cone); the HG term then adds the look-into-beam
// brightening that matters for third-person / moon shafts.
const float kScatterBase = 0.80f;
const float kScatterHg   = 0.60f;
// §5.4: aligned to the shaft's physical g (0.60 -> 0.70, same fog anisotropy as the
// flashlight march) so the dust glint forward-peaks consistently with the beam.
const float kHgG         = 0.70f;

const float kNearSkip    = 8.0f;   // ignore motes basically at the muzzle (axial < this)
const float kCullEps     = 0.0004f;// luminance below this = unlit -> not simmed/drawn. Lower
                                   // than the pre-refinement 0.0008 so the now-finer/dimmer
                                   // motes still populate a believable haze instead of thinning
                                   // out to sparse dots at the cone edges.
const float kConeGain    = 3.0f;   // cone radiance scale (folded with csz_dust_intensity)
const float kMoonGain    = 3.0f;   // moon-shaft radiance scale
const float kMoonFrac    = 0.06f;  // moon-shaft: stable-ID stochastic density (no moon shadow map ->
                                   // thin the otherwise-uniform moonlit-fog motes to a sparse shaft-like
                                   // field; keeps the 门外不进 VBO perf contract). True cloud-gap shadow
                                   // confinement is OWED (needs a directional-moon shadow/visibility source).
const float kDriftSpeed  = 3.0f;   // slow curl drift (world units / second)
const float kSettleSpeed = 1.0f;   // gentle downward settle (world units / second)
const float kFadeBand    = 16.0f;  // soft-particle depth-fade band (world units)

// --- cvars (read live each frame) --------------------------------------------
cvar_t *s_cvarDust;        // csz_dust            default "1": master (0 = no dust, clean A/B)
cvar_t *s_cvarCount;       // csz_dust_count      default "4096": active pool size (fine dense haze)
cvar_t *s_cvarIntensity;   // csz_dust_intensity  default "1.0": radiance scale (dev tuning)
cvar_t *s_cvarSize;        // csz_dust_size       default "1.0": mote world half-size (fine specks)
cvar_t *s_cvarOcclusion;   // csz_dust_occlusion  default "0.20": per-mote extinction coverage (0 = pure additive)
cvar_t *s_cvarRange;       // csz_flashlight_range (FogVolume-owned): cone length cap + pool box. Lazy.
bool    s_lookedRange;

// --- persistent world pool (stable ID = index) -------------------------------
bool  s_seeded;
float s_px[kMaxDust];
float s_py[kMaxDust];
float s_pz[kMaxDust];
float s_lastTime;

// --- CPU vertex scratch (lit motes only) -------------------------------------
float s_verts[(size_t)kMaxDust * kVertsPerQuad * kFloatsPerVert];

// --- GPU resources (generation-keyed; forget on a foreign context) -----------
struct DustGpu
{
	ShaderProgram prog;
	GLuint vao;
	GLuint vbo;
	int    gpuGeneration;
	bool   built;
	bool   failedThisGen;

	int uMatViewProj, uMatView, uDepthTex, uViewSize, uFade, uZNear, uZFar, uOcclusion;
};
DustGpu s_gpu;

float ReadCvar( cvar_t *cv, float fallback )
{
	return ( cv != NULL ) ? cv->value : fallback;
}

// Deterministic per-index hash in [0,1) (spatial blue-noise-ish; NOT time-varying,
// so size/brightness jitter is stable per mote -- no temporal shimmer).
float Hash01( int i, int salt )
{
	float h = sinf( (float)i * 12.9898f + (float)salt * 78.233f ) * 43758.5453f;
	return h - floorf( h );
}

// Toroidal wrap of one axis into [-half, half) centred on the camera. Identity while
// |rel| < half, so motes inside the lit region (|rel| <= range < half) never move from
// the wrap -- only motes far out in the dark wrap, invisibly (no cull-respawn jitter).
float WrapAxis( float rel, float boxSize )
{
	return rel - boxSize * floorf( rel / boxSize + 0.5f );
}

float HgPhase( float c, float g )
{
	float g2 = g * g;
	float denom = 1.0f + g2 - 2.0f * g * c;
	if( denom < 1e-4f ) denom = 1e-4f;
	return ( 1.0f - g2 ) / ( 4.0f * kPi * powf( denom, 1.5f ) );
}

float Luma( const float c[3] )
{
	return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

// Cheap divergence-light "curl" flow (sines of position + time) -> a slow swirling
// drift with no clumping; GL3.3 has no compute, but a few hundred-thousand scalar ops
// for the whole pool is negligible CPU.
void CurlFlow( float x, float y, float z, float t, float out[3] )
{
	const float a = 0.015f;
	const float w = 0.6f;
	out[0] = sinf( y * a + t * w )        - cosf( z * a * 1.3f - t * w * 0.7f );
	out[1] = sinf( z * a * 0.9f + t * w * 1.1f ) - cosf( x * a * 1.1f - t * w * 0.8f );
	out[2] = sinf( x * a * 1.2f + t * w * 0.9f ) - cosf( y * a * 0.8f - t * w * 1.2f );
}

void ForgetGpu()
{
	s_gpu.vao = 0;
	s_gpu.vbo = 0;
	s_gpu.prog.program = 0;
	s_gpu.built = false;
	s_gpu.failedThisGen = false;
}

void DestroyGpuSameContext()
{
	if( s_gpu.vbo != 0 )
		glDeleteBuffers( 1, &s_gpu.vbo );
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

	// Final FS = "#version 330 core" + shared depth-reconstruct helpers + body.
	std::string fs = std::string( "#version 330 core\n" ) + kFogDepthReconstructGlsl + kDustFsBody;
	if( !BuildProgram( "csz_dust", kDustVs, fs.c_str(), false, s_gpu.prog ) )
	{
		s_gpu.failedThisGen = true;
		CSZ_LogError( "dust", "shader build failed; airborne dust disabled this generation" );
		return false;
	}

	s_gpu.uMatViewProj = UniformLoc( s_gpu.prog, "u_matViewProj" );
	s_gpu.uMatView     = UniformLoc( s_gpu.prog, "u_matView" );
	s_gpu.uDepthTex    = UniformLoc( s_gpu.prog, "u_depthTex" );
	s_gpu.uViewSize    = UniformLoc( s_gpu.prog, "u_viewSize" );
	s_gpu.uFade        = UniformLoc( s_gpu.prog, "u_fade" );
	s_gpu.uZNear       = UniformLoc( s_gpu.prog, "u_zNear" );
	s_gpu.uZFar        = UniformLoc( s_gpu.prog, "u_zFar" );
	s_gpu.uOcclusion   = UniformLoc( s_gpu.prog, "u_occlusion" );

	glGenVertexArrays( 1, &s_gpu.vao );
	BindVao( s_gpu.vao );
	glGenBuffers( 1, &s_gpu.vbo );
	glBindBuffer( GL_ARRAY_BUFFER, s_gpu.vbo );
	// FIXED-capacity stream VBO: allocated ONCE at full capacity; each frame we orphan
	// (glBufferData NULL) + glBufferSubData only the lit prefix -- never resized, no
	// persistent mapping (the loaded GL table has no glMapBufferRange; orphaning gives
	// the same no-stall streaming).
	const GLsizeiptr cap = (GLsizeiptr)kMaxDust * kVertsPerQuad * kFloatsPerVert * (GLsizeiptr)sizeof( float );
	glBufferData( GL_ARRAY_BUFFER, cap, NULL, GL_STREAM_DRAW );

	const int stride = kFloatsPerVert * (int)sizeof( float );
	glEnableVertexAttribArray( 0 );   // a_world (vec3)
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)0 );
	glEnableVertexAttribArray( 1 );   // a_uv    (vec2)
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)( 3 * sizeof( float ) ) );
	glEnableVertexAttribArray( 2 );   // a_color (vec3)
	glVertexAttribPointer( 2, 3, GL_FLOAT, GL_FALSE, stride, (const void *)( 5 * sizeof( float ) ) );

	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	s_gpu.built = true;
	CSZ_LogDev( "dust", "program + stream VBO built (cap %d motes, gpu gen %d)", kMaxDust, s_gpu.gpuGeneration );
	return true;
}

// One cone gate candidate, pre-resolved from a registry spot (avoids re-deriving per mote).
struct ConeGate
{
	float origin[3];
	float dir[3];
	float color[3];
	float cosInner, cosOuter, range;
};

}  // anonymous namespace

void DustRegisterCvars()
{
	if( s_cvarDust == NULL )
		s_cvarDust = gEngfuncs.pfnRegisterVariable( "csz_dust", "1", FCVAR_CLIENTDLL );
	if( s_cvarCount == NULL )
		// Fine dust reads as a dense haze, not sparse dots. Playtest r1 (operator ask
		// 多一点点): 4096 -> 7000 for a believable fine haze. The CPU pool walk is cheap
		// scalar work and the GPU only ever draws the lit prefix; smaller motes (size
		// 0.45) shrink per-mote fill ~5x so 7000 fine motes stay inside the L7 perf
		// contract (net fill DROPS vs the old 4096x size-1.0 dust). Live-tunable.
		// Fog rewrite §5.4 (USER ask: MORE dust): 7000 -> 10000 for a denser, more obviously
		// airborne haze in the beam. Only LIT motes draw and they are tiny (size 0.45), so
		// net fill stays inside the L7 perf contract; cap is kMaxDust (16384). Live-tunable.
		s_cvarCount = gEngfuncs.pfnRegisterVariable( "csz_dust_count", "10000", FCVAR_CLIENTDLL );
	if( s_cvarIntensity == NULL )
		// Playtest r1 (operator ask 像漂浮的光点 -> 应该是faint灰尘): lowered 1.0 -> 0.6 so each
		// mote is a faint fine speck, NOT a bright glowing "光点". The brightness is purely
		// this scale on the CPU radiance (vColor); the FS has no extra glint. Live-tunable.
		s_cvarIntensity = gEngfuncs.pfnRegisterVariable( "csz_dust_intensity", "0.6", FCVAR_CLIENTDLL );
	if( s_cvarSize == NULL )
		// Fine specks (was 3.0 = glowing balls/bokeh -> "不像灰尘"; then 1.0). Playtest r1
		// (operator ask 很小很小): 1.0 -> 0.45 half-size reads as very fine airborne motes;
		// per-mote jitter (0.6..1.4x) keeps them varied. Live-tunable 0.35..0.8.
		s_cvarSize = gEngfuncs.pfnRegisterVariable( "csz_dust_size", "0.45", FCVAR_CLIENTDLL );
	if( s_cvarOcclusion == NULL )
		// Per-mote extinction coverage. Under kBlendPremulOver each lit mote dims the beam
		// behind it by occlusion*coverage while still adding its own glint, so the flashlight
		// light reads as a touch eaten/scattered by the dust (the USER ask). 0.20 = subtle but
		// perceptible; 0 = byte-for-byte the old pure-additive mote (clean A/B). Live-tunable.
		s_cvarOcclusion = gEngfuncs.pfnRegisterVariable( "csz_dust_occlusion", "0.20", FCVAR_CLIENTDLL );

	CSZ_LogDev( "dust", "cvars registered (csz_dust/_count/_intensity/_size/_occlusion)" );
}

void DustRender( const ViewSetup &view )
{
	// Gate 1: master switch (A/B-off contract: no dust pass at all -> IEEE-exact off).
	if( ReadCvar( s_cvarDust, 1.0f ) < 0.5f )
		return;

	// Gate 2: needs the HDR path (linear RGBA16F target to add into) + the sampleable
	// scene depth texture for camera-side occlusion / soft fade.
	if( !SkyComposeActive() )
		return;
	GLuint depthTex = SkyComposeDepthTex();
	GLuint hdrFbo   = SkyComposeHdrFbo();
	if( depthTex == 0 || hdrFbo == 0 )
		return;

	if( !EnsureBuilt() )
		return;

	int count = (int)( ReadCvar( s_cvarCount, 7000.0f ) + 0.5f );
	if( count < 0 )        count = 0;
	if( count > kMaxDust ) count = kMaxDust;
	if( count == 0 )
		return;

	const float intensity = ReadCvar( s_cvarIntensity, 0.6f );
	const float moteSize  = ReadCvar( s_cvarSize, 0.45f );
	float occlusion = ReadCvar( s_cvarOcclusion, 0.20f );
	if( occlusion < 0.0f ) occlusion = 0.0f;
	if( occlusion > 1.0f ) occlusion = 1.0f;

	if( !s_lookedRange )
	{
		s_lookedRange = true;
		s_cvarRange = gEngfuncs.pfnGetCvarPointer( "csz_flashlight_range" );
	}
	const float range  = ReadCvar( s_cvarRange, 1600.0f );
	// Pool box: a bit wider than the cone range so the lit region is strictly inside the
	// box (lit motes never reach the wrap boundary). Moon-shaft motes are also confined
	// to this near-field box around the camera (kept simple; the shaft look is near-field).
	float boxHalf = ( range > 300.0f ? range : 300.0f ) * 1.1f;
	const float boxSize = 2.0f * boxHalf;

	const float cam[3] = { view.origin[0], view.origin[1], view.origin[2] };

	// One-time seed: scatter the whole capacity around the first camera position. The
	// per-frame toroidal wrap re-centres the pool around the camera regardless, so the
	// seed anchor is immaterial -- this just gives a uniform initial spread.
	if( !s_seeded )
	{
		s_seeded = true;
		for( int i = 0; i < kMaxDust; i++ )
		{
			s_px[i] = cam[0] + ( Hash01( i, 1 ) * 2.0f - 1.0f ) * boxHalf;
			s_py[i] = cam[1] + ( Hash01( i, 2 ) * 2.0f - 1.0f ) * boxHalf;
			s_pz[i] = cam[2] + ( Hash01( i, 3 ) * 2.0f - 1.0f ) * boxHalf;
		}
		s_lastTime = 0.0f;
	}

	const float now = ClientTime();
	float dt = ( s_lastTime > 0.0f ) ? ( now - s_lastTime ) : 0.0f;
	if( dt < 0.0f || dt > 0.25f ) dt = 0.0f;   // first frame / pause / map change: no jump
	s_lastTime = now;

	// Camera billboard basis (screen-aligned quads from the view angles).
	float camFwd[3], camRight[3], camUp[3];
	AngleVectors( view.angles, camFwd, camRight, camUp );

	// --- pre-resolve the cone gates from the spot registry. v3.1 (FIX-4): LOCAL first-person
	// beam ONLY -- the fog march does NOT draw dust, so the viewer's own beam must light it
	// (the USER's main ask, "MORE dust"); but non-local third-person cones are clean clear-fog
	// cones whose dust glint would re-introduce the removed haze/Tyndall (gated out below).
	ConeGate cones[LightRegistry::kMaxLights];
	int numCones = 0;
	for( int i = 0; i < LightRegistry::kMaxLights; i++ )
	{
		ActiveLight *light = g_lights.Slot( i );
		if( !light->used || light->desc.type != kLightSpot )
			continue;
		if( light->desc.die > 0.0f && light->desc.die < now )
			continue;
		if( light->budgetTier == kBudgetCull )
			continue;   // off-screen / over budget: its dust is not visible anyway
		// FIX-4 (v3.1): glint dust ONLY in the LOCAL first-person beam. Non-local (other
		// players') cones are v3.1 "clear-fog" cones with no volumetric tell -- letting their
		// dust glint re-introduces exactly the haze/Tyndall the USER asked to remove, and the
		// per-mote premul-over add is non-MAX (overlap brightens). Skipping them keeps the
		// approved first-person beam dust ("MORE dust") while the third-person cones stay clean.
		if( !light->desc.isLocal )
			continue;

		SpotLightParams sp;
		g_lights.BuildSpotParams( *light, sp );

		ConeGate &g = cones[numCones++];
		g.origin[0] = sp.origin[0]; g.origin[1] = sp.origin[1]; g.origin[2] = sp.origin[2];
		g.dir[0] = sp.dir[0]; g.dir[1] = sp.dir[1]; g.dir[2] = sp.dir[2];
		g.color[0] = sp.color[0]; g.color[1] = sp.color[1]; g.color[2] = sp.color[2];
		g.cosInner = sp.cosInner;
		g.cosOuter = sp.cosOuter;
		g.range = ( range > 0.0f && range < sp.radius ) ? range : sp.radius;  // cap by csz_flashlight_range
	}

	// --- moon Tyndall shaft gate (L4 channels) ---
	float moonRGB[3];
	const float moonInScatter = CszMoonInScatter( view.ambience, moonRGB );
	const float shaftMask = ( view.ambience.shaftMask < 0.0f ) ? 0.0f
	                      : ( view.ambience.shaftMask > 1.0f ) ? 1.0f : view.ambience.shaftMask;
	const bool  moonShaftOn = ( CszMoonShaftEnabled() != 0.0f )
	                       && view.ambience.moonlightEnabled
	                       && moonInScatter > 0.0f
	                       && shaftMask > 0.0f
	                       && view.ambience.fogDensity > 0.0f;
	const float moonDir[3] = { view.ambience.moonlightDir[0], view.ambience.moonlightDir[1], view.ambience.moonlightDir[2] };

	// --- walk the pool: gate -> (lit only) expand into the VBO scratch ---
	int   litCount = 0;
	int   coneLit = 0, moonLit = 0;
	float *vp = s_verts;

	for( int i = 0; i < count; i++ )
	{
		// toroidal wrap around camera, then slow curl drift (world-space anchored)
		float p[3];
		p[0] = cam[0] + WrapAxis( s_px[i] - cam[0], boxSize );
		p[1] = cam[1] + WrapAxis( s_py[i] - cam[1], boxSize );
		p[2] = cam[2] + WrapAxis( s_pz[i] - cam[2], boxSize );

		if( dt > 0.0f )
		{
			float flow[3];
			CurlFlow( p[0], p[1], p[2], now, flow );
			p[0] += flow[0] * kDriftSpeed * dt;
			p[1] += flow[1] * kDriftSpeed * dt;
			p[2] += ( flow[2] * kDriftSpeed - kSettleSpeed ) * dt;
		}
		s_px[i] = p[0]; s_py[i] = p[1]; s_pz[i] = p[2];

		// view direction (camera -> mote), shared by every HG term
		float V[3] = { p[0] - cam[0], p[1] - cam[1], p[2] - cam[2] };
		float vlen = sqrtf( V[0]*V[0] + V[1]*V[1] + V[2]*V[2] );
		if( vlen < 1e-3f ) continue;
		float invV = 1.0f / vlen;
		V[0] *= invV; V[1] *= invV; V[2] *= invV;

		float emit[3] = { 0.0f, 0.0f, 0.0f };
		float bestLum = 0.0f;
		bool  wonByMoon = false;

		// --- cone gate ---
		for( int c = 0; c < numCones; c++ )
		{
			const ConeGate &g = cones[c];
			float ap[3] = { p[0] - g.origin[0], p[1] - g.origin[1], p[2] - g.origin[2] };
			float s = ap[0]*g.dir[0] + ap[1]*g.dir[1] + ap[2]*g.dir[2];   // axial distance
			if( s <= kNearSkip || s > g.range )
				continue;
			float r = sqrtf( ap[0]*ap[0] + ap[1]*ap[1] + ap[2]*ap[2] );
			float cosAx = s / ( r > 1e-4f ? r : 1e-4f );
			if( cosAx <= g.cosOuter )
				continue;
			float coneFall = ( cosAx - g.cosOuter ) / ( g.cosInner - g.cosOuter );  // smoothstep band
			if( coneFall > 1.0f ) coneFall = 1.0f;
			coneFall = coneFall * coneFall * ( 3.0f - 2.0f * coneFall );             // smoothstep ease
			float atten = 1.0f - s / g.range;
			if( atten < 0.0f ) atten = 0.0f;
			atten *= atten;
			// HG forward (sample -> lamp); first-person degenerates to back-scatter, base carries it.
			float toL[3] = { g.origin[0]-p[0], g.origin[1]-p[1], g.origin[2]-p[2] };
			float ll = sqrtf( toL[0]*toL[0]+toL[1]*toL[1]+toL[2]*toL[2] );
			float mu = ( ll > 1e-4f ) ? ( V[0]*toL[0]+V[1]*toL[1]+V[2]*toL[2] ) / ll : 0.0f;
			float scat = kScatterBase + kScatterHg * HgPhase( mu, kHgG );
			float k = coneFall * atten * scat * kConeGain * intensity;
			float cand[3] = { g.color[0]*k, g.color[1]*k, g.color[2]*k };
			float lum = Luma( cand );
			if( lum > bestLum )
			{
				bestLum = lum;
				emit[0] = cand[0]; emit[1] = cand[1]; emit[2] = cand[2];
			}
		}

		// --- moon shaft gate ---
		// Confined to a sparse, in-view subset: a directional light with no occluder would
		// light EVERY mote (uniform moonlit fog -> defeats the spawn/cull perf gate). With no
		// moon shadow map, thin to a stable-ID stochastic density in the front hemisphere so
		// the result reads as sparse drifting shaft motes, not a wall of 3000. shadow-occluded
		// cloud-gap confinement is OWED (needs a moon visibility source).
		if( moonShaftOn
		    && Hash01( i, 5 ) < kMoonFrac
		    && ( V[0]*camFwd[0] + V[1]*camFwd[1] + V[2]*camFwd[2] ) > 0.1f )
		{
			// HG toward the moon (sample -> moon == moonlightDir, the L vector).
			float mu = V[0]*moonDir[0] + V[1]*moonDir[1] + V[2]*moonDir[2];
			float scat = kScatterBase + kScatterHg * HgPhase( mu, kHgG );
			float k = moonInScatter * shaftMask * scat * kMoonGain * intensity;
			float cand[3] = { moonRGB[0]*k, moonRGB[1]*k, moonRGB[2]*k };
			float lum = Luma( cand );
			if( lum > bestLum )
			{
				bestLum = lum;
				emit[0] = cand[0]; emit[1] = cand[1]; emit[2] = cand[2];
				wonByMoon = true;
			}
		}

		if( bestLum < kCullEps )
			continue;   // UNLIT: not simmed into the draw, never enters the VBO (the perf gate)

		// per-mote spatial jitter (blue-noise-ish, stable): size + brightness
		float szJ = 0.6f + 0.8f * Hash01( i, 7 );
		float brJ = 0.7f + 0.6f * Hash01( i, 11 );
		float sz = moteSize * szJ;
		float col[3] = { emit[0]*brJ, emit[1]*brJ, emit[2]*brJ };

		// CPU billboard expansion: 6 verts (2 tris), corners in [-1,1] view plane.
		static const float kCx[6] = { -1.0f,  1.0f,  1.0f, -1.0f,  1.0f, -1.0f };
		static const float kCy[6] = { -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f };
		for( int v = 0; v < kVertsPerQuad; v++ )
		{
			float ox = kCx[v] * sz;
			float oy = kCy[v] * sz;
			vp[0] = p[0] + camRight[0]*ox + camUp[0]*oy;
			vp[1] = p[1] + camRight[1]*ox + camUp[1]*oy;
			vp[2] = p[2] + camRight[2]*ox + camUp[2]*oy;
			vp[3] = kCx[v];
			vp[4] = kCy[v];
			vp[5] = col[0]; vp[6] = col[1]; vp[7] = col[2];
			vp += kFloatsPerVert;
		}
		litCount++;
		if( wonByMoon ) moonLit++; else coneLit++;   // gate-count evidence (door-test)
	}

	if( litCount > 0 )
	{
		// --- shared state: add into the HDR FBO, depth test/write OFF (occlusion is done
		// in-shader from the SAMPLED depth, same safe contract as the L6a cone -- avoids a
		// read/test feedback loop on the shared depth attachment). ---
		BindFbo( hdrFbo );
		glViewport( view.viewport[0], view.viewport[1], view.viewport[2], view.viewport[3] );
		SetDepthTest( false );
		SetDepthWrite( false );
		// Premultiplied "over": rgb adds the mote's glint, alpha (=occlusion coverage)
		// attenuates the beam behind it. With csz_dust_occlusion 0 the FS emits alpha 0,
		// so ONE_MINUS_SRC_ALPHA == 1 and this reduces EXACTLY to the old ONE,ONE additive
		// (byte-for-byte A/B against the pre-refinement pure-additive dust).
		SetBlend( kBlendPremulOver );
		SetCull( false );

		UseProgram( s_gpu.prog.program );
		BindVao( s_gpu.vao );
		glBindBuffer( GL_ARRAY_BUFFER, s_gpu.vbo );

		const GLsizeiptr cap = (GLsizeiptr)kMaxDust * kVertsPerQuad * kFloatsPerVert * (GLsizeiptr)sizeof( float );
		const GLsizeiptr used = (GLsizeiptr)litCount * kVertsPerQuad * kFloatsPerVert * (GLsizeiptr)sizeof( float );
		glBufferData( GL_ARRAY_BUFFER, cap, NULL, GL_STREAM_DRAW );   // orphan (no-stall, no resize)
		glBufferSubData( GL_ARRAY_BUFFER, 0, used, s_verts );

		float fViewSize[2] = { (float)( view.viewport[0] + view.viewport[2] ),
		                       (float)( view.viewport[1] + view.viewport[3] ) };
		if( fViewSize[0] < 1.0f ) fViewSize[0] = 1.0f;
		if( fViewSize[1] < 1.0f ) fViewSize[1] = 1.0f;

		SkyComposeBindTex( 0, GL_TEXTURE_2D, depthTex );
		if( s_gpu.uMatViewProj >= 0 ) glUniformMatrix4fv( s_gpu.uMatViewProj, 1, GL_FALSE, view.matViewProj.m );
		if( s_gpu.uMatView >= 0 )     glUniformMatrix4fv( s_gpu.uMatView, 1, GL_FALSE, view.matView.m );
		if( s_gpu.uDepthTex >= 0 )    glUniform1i( s_gpu.uDepthTex, kSkyTmuBase + 0 );
		if( s_gpu.uViewSize >= 0 )    glUniform2fv( s_gpu.uViewSize, 1, fViewSize );
		if( s_gpu.uFade >= 0 )        glUniform1f( s_gpu.uFade, kFadeBand );
		if( s_gpu.uZNear >= 0 )       glUniform1f( s_gpu.uZNear, view.zNear );
		if( s_gpu.uZFar >= 0 )        glUniform1f( s_gpu.uZFar, view.zFar );
		if( s_gpu.uOcclusion >= 0 )   glUniform1f( s_gpu.uOcclusion, occlusion );

		glDrawArrays( GL_TRIANGLES, 0, litCount * kVertsPerQuad );

		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		BindVao( 0 );
		UseProgram( 0 );
		SkyComposeRestoreTmus();
		SetBlend( kBlendNone );
		// Restore the EnterTakeover baseline for the following transparent / viewmodel passes.
		SetDepthTest( true );
		SetDepthWrite( true );
	}

	static float s_nextStats;
	if( now >= s_nextStats )
	{
		s_nextStats = now + 1.0f;
		CSZ_LogDev( "dust", "motes lit %d / pool %d (cone %d + moon %d), cones %d",
			litCount, count, coneLit, moonLit, numCones );
	}
}

void DustShutdown()
{
	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
		DestroyGpuSameContext();
	else
		ForgetGpu();
	s_seeded = false;   // re-seed against the next context's first camera
}

}  // namespace csz
