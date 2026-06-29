/*
 * csz_beam.cpp -- CSOZ renderer: self-drawn beam pool (M2c decision C, C-BEAM)
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
#include "csz_beam.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_math.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"
#include "../core/csz_ambience_types.h"   // CszFogUniformVecs

#include "beamdef.h"   // BEAM / beam_s (the pool slot embeds one; client mutates die/flags)

#include <math.h>
#include <string.h>
#include <string>

namespace csz
{

#include "csz_beam_shaders.inl"   // kBeamVs / kBeamFsBody

namespace
{

// --- pool + ribbon limits ----------------------------------------------------
const int kMaxBeams      = 64;     // gauss spams ~5-10 short beams/shot; 64 is ample headroom
const int kMaxSegments   = 48;     // ribbon fold count cap (long beams subdivide more)
const int kVertsPerSeg   = 6;      // two triangles, non-indexed
const int kFloatsPerVert = 9;      // pos(3) + uv(2) + color(4)
const float kPi          = 3.14159265358979323846f;

// One fixed beam. pub MUST be first: BeamAlloc* returns &pub and the egon emit
// site writes pub.flags / pub.die through that pointer (ABI fact, header note).
struct BeamSlot
{
	beam_s pub;
	bool   used;
	bool   entStart;     // start point re-resolved from an entity each frame
	int    startEnt;     // engine-encoded (entity in low 12 bits, attachment in 12-15)
	float  spawnTime;
	float  life;         // original requested life (for fade); pub.die is the absolute death clock
	int    glTexture;    // resolved beam-sprite texture slot (0 = procedural core)
	bool   texTried;
};

BeamSlot s_beams[kMaxBeams];

// --- minimal engine sprite ABI mirror (csz_sprite.cpp precedent) -------------
// Only what beam-texture resolution needs: the first frame's gl_texturenum.
// Group/angled frames take sub-frame 0 (beam strips animate; per-frame animation
// is OWED). A failed/implausible resolve falls back to the procedural core.
struct EngSpriteFrame { int width, height; float up, down, left, right; int gl_texturenum; };
struct EngSpriteFrameDesc { int type; EngSpriteFrame *frameptr; };
struct EngSpriteGroup { int numframes; float *intervals; EngSpriteFrame *frames[1]; };
struct EngSprite { short type; short texFormat; int maxwidth, maxheight; int numframes; int radius; int facecull; int synctype; EngSpriteFrameDesc frames[1]; };

int ResolveBeamTexture( int modelIndex, int frame )
{
	if( modelIndex <= 0 || gRenderAPI.pfnGetModel == NULL )
		return 0;

	model_t *mod = (model_t *)gRenderAPI.pfnGetModel( modelIndex );
	if( mod == NULL || mod->type != mod_sprite || mod->cache.data == NULL )
		return 0;

	const EngSprite *spr = (const EngSprite *)mod->cache.data;
	if( spr->numframes < 1 || spr->numframes > 4096 )
		return 0;

	if( frame < 0 ) frame = 0;
	else if( frame >= spr->numframes ) frame = spr->numframes - 1;

	const EngSpriteFrameDesc &desc = spr->frames[frame];
	const EngSpriteFrame *f;

	if( desc.type == 0 )   // single
	{
		f = desc.frameptr;
	}
	else                    // group / angled: sub-frame 0
	{
		const EngSpriteGroup *grp = (const EngSpriteGroup *)desc.frameptr;
		if( grp == NULL || grp->numframes < 1 || grp->numframes > 4096 )
			return 0;
		f = grp->frames[0];
	}

	if( f == NULL || f->gl_texturenum <= 0 )
		return 0;

	return f->gl_texturenum;
}

// --- 128-entry sine noise table (smooth, interpolated; endpoints pinned by the
// per-beam window so the table value itself need not be zero at the ends) ------
const int kNoiseSize = 128;
float s_noise[kNoiseSize];
bool  s_noiseInit;

void InitNoise()
{
	if( s_noiseInit )
		return;
	s_noiseInit = true;
	// Two summed sines -> a smooth, non-repetitive-looking wiggle in [-1,1].
	for( int i = 0; i < kNoiseSize; i++ )
	{
		float t = (float)i / (float)kNoiseSize;
		s_noise[i] = 0.6f * sinf( t * 2.0f * kPi ) + 0.4f * sinf( t * 6.0f * kPi + 1.3f );
	}
}

float SampleNoise( float phase )
{
	phase -= floorf( phase );                 // wrap to [0,1)
	float fx = phase * (float)kNoiseSize;
	int   i0 = (int)fx;
	float fr = fx - (float)i0;
	int   i1 = ( i0 + 1 ) % kNoiseSize;
	i0 %= kNoiseSize;
	return s_noise[i0] * ( 1.0f - fr ) + s_noise[i1] * fr;
}

// --- cvar + GPU resources ----------------------------------------------------
cvar_t *s_cvarBeam;   // csz_beam (default 1; 0 = no beam pass, clean A/B)

float s_lastTime;     // for the once-per-frame sim clock (single-step guard)

float s_verts[(size_t)kMaxBeams * kMaxSegments * kVertsPerSeg * kFloatsPerVert];

struct BeamGpu
{
	ShaderProgram prog;
	GLuint vao, vbo;
	int    gpuGeneration;
	bool   built, failedThisGen;
	int    uViewProj, uTexDiffuse, uTextured, uFog, uFogParams, uCamPos;
};
BeamGpu s_gpu;

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
	if( s_gpu.vbo != 0 ) glDeleteBuffers( 1, &s_gpu.vbo );
	if( s_gpu.vao != 0 ) glDeleteVertexArrays( 1, &s_gpu.vao );
	if( s_gpu.prog.program != 0 ) DestroyProgram( s_gpu.prog );
	ForgetGpu();
}

bool EnsureBuilt()
{
	if( s_gpu.gpuGeneration != GpuGeneration() )
	{
		ForgetGpu();
		s_gpu.gpuGeneration = GpuGeneration();
	}
	if( s_gpu.built )
		return true;
	if( s_gpu.failedThisGen )
		return false;

	std::string fs = std::string( "#version 330 core\n" ) + kBeamFsBody;
	if( !BuildProgram( "csz_beam", kBeamVs, fs.c_str(), false, s_gpu.prog ) )
	{
		s_gpu.failedThisGen = true;
		CSZ_LogError( "beam", "shader build failed; beams disabled this generation" );
		return false;
	}

	s_gpu.uViewProj   = UniformLoc( s_gpu.prog, "u_viewProj" );
	s_gpu.uTexDiffuse = UniformLoc( s_gpu.prog, "u_texDiffuse" );
	s_gpu.uTextured   = UniformLoc( s_gpu.prog, "u_textured" );
	s_gpu.uFog        = UniformLoc( s_gpu.prog, "u_fog" );
	s_gpu.uFogParams  = UniformLoc( s_gpu.prog, "u_fogParams" );
	s_gpu.uCamPos     = UniformLoc( s_gpu.prog, "u_camPos" );

	glGenVertexArrays( 1, &s_gpu.vao );
	BindVao( s_gpu.vao );
	glGenBuffers( 1, &s_gpu.vbo );
	glBindBuffer( GL_ARRAY_BUFFER, s_gpu.vbo );
	const GLsizeiptr cap = (GLsizeiptr)sizeof( s_verts );
	glBufferData( GL_ARRAY_BUFFER, cap, NULL, GL_STREAM_DRAW );

	const int stride = kFloatsPerVert * (int)sizeof( float );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)( 3 * sizeof( float ) ) );
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 4, GL_FLOAT, GL_FALSE, stride, (const void *)( 5 * sizeof( float ) ) );

	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	s_gpu.built = true;
	CSZ_LogDev( "beam", "program + stream VBO built (cap %d beams, gpu gen %d)", kMaxBeams, s_gpu.gpuGeneration );
	return true;
}

BeamSlot *AllocSlot()
{
	for( int i = 0; i < kMaxBeams; i++ )
	{
		if( !s_beams[i].used )
			return &s_beams[i];
	}
	return NULL;
}

// Fill the shared beam_s fields from the emit args (engine convention: r/g/b and
// brightness arrive in 0..255; we normalise to linear-ish [0,1] for the shader).
void FillCommon( BeamSlot *s, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b )
{
	beam_s &p = s->pub;
	memset( &p, 0, sizeof( p ) );
	const float now = ClientTime();
	p.flags      = FBEAM_ISACTIVE;
	p.die        = now + life;
	p.width      = width;
	p.amplitude  = amplitude;
	p.brightness = brightness;
	p.speed      = speed;
	p.frameRate  = framerate;
	p.frame      = (float)startFrame;
	p.modelIndex = modelIndex;
	p.r = r * ( 1.0f / 255.0f );
	p.g = g * ( 1.0f / 255.0f );
	p.b = b * ( 1.0f / 255.0f );

	s->used      = true;
	s->spawnTime = now;
	s->life      = ( life > 0.0f ) ? life : 0.0001f;
	s->texTried  = false;
	s->glTexture = 0;
}

// Re-resolve an entity-anchored start point. startEnt low 12 bits = entity index,
// bits 12-15 = attachment index (engine beam convention). Studio attachment when
// present, else the entity origin. Returns false if the entity is gone.
bool ResolveEntStart( int startEnt, float out[3] )
{
	int entIndex   = startEnt & 0x0FFF;
	int attachment = ( startEnt >> 12 ) & 0x000F;

	cl_entity_t *ent = gEngfuncs.GetEntityByIndex( entIndex );
	if( ent == NULL )
		return false;

	if( attachment > 0 && attachment <= 4 )
	{
		out[0] = ent->attachment[attachment - 1][0];
		out[1] = ent->attachment[attachment - 1][1];
		out[2] = ent->attachment[attachment - 1][2];
	}
	else
	{
		out[0] = ent->origin[0];
		out[1] = ent->origin[1];
		out[2] = ent->origin[2];
	}
	return true;
}

void Normalize3( float v[3] )
{
	float l = sqrtf( v[0]*v[0] + v[1]*v[1] + v[2]*v[2] );
	if( l > 1e-6f ) { float inv = 1.0f / l; v[0]*=inv; v[1]*=inv; v[2]*=inv; }
}

void Cross3( const float a[3], const float b[3], float out[3] )
{
	out[0] = a[1]*b[2] - a[2]*b[1];
	out[1] = a[2]*b[0] - a[0]*b[2];
	out[2] = a[0]*b[1] - a[1]*b[0];
}

}  // anonymous namespace

void BeamRegisterCvars()
{
	if( s_cvarBeam == NULL )
		s_cvarBeam = gEngfuncs.pfnRegisterVariable( "csz_beam", "1", FCVAR_CLIENTDLL );
	CSZ_LogDev( "beam", "cvars registered (csz_beam)" );
}

beam_s *BeamAllocPoints( const float *start, const float *end, int modelIndex,
	float life, float width, float amplitude, float brightness, float speed,
	int startFrame, float framerate, float r, float g, float b )
{
	BeamSlot *s = AllocSlot();
	if( s == NULL )
		return NULL;

	FillCommon( s, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
	s->entStart = false;
	s->startEnt = 0;
	s->pub.source[0] = start[0]; s->pub.source[1] = start[1]; s->pub.source[2] = start[2];
	s->pub.target[0] = end[0];   s->pub.target[1] = end[1];   s->pub.target[2] = end[2];
	return &s->pub;
}

beam_s *BeamAllocEntPoint( int startEnt, const float *end, int modelIndex,
	float life, float width, float amplitude, float brightness, float speed,
	int startFrame, float framerate, float r, float g, float b )
{
	BeamSlot *s = AllocSlot();
	if( s == NULL )
		return NULL;

	FillCommon( s, modelIndex, life, width, amplitude, brightness, speed, startFrame, framerate, r, g, b );
	s->entStart = true;
	s->startEnt = startEnt;
	s->pub.startEntity = startEnt;
	// Resolve once now; re-resolved every frame in BeamDraw so the start tracks.
	if( !ResolveEntStart( startEnt, s->pub.source ) )
	{
		s->pub.source[0] = end[0]; s->pub.source[1] = end[1]; s->pub.source[2] = end[2];
	}
	s->pub.target[0] = end[0]; s->pub.target[1] = end[1]; s->pub.target[2] = end[2];
	return &s->pub;
}

void BeamDraw( const ViewSetup &view )
{
	if( ReadCvar( s_cvarBeam, 1.0f ) < 0.5f )
		return;

	InitNoise();

	const float now = ClientTime();
	// Once-per-frame sim clock. The MAIN transparent pass is the only caller, so
	// this advances exactly once on real dt (single-step contract, §4): we never
	// read the engine frametime, and any zero-dt sub-pass re-entry would compute
	// dt~0 anyway. (Beams are time-parametric, so dt is used only for the U scroll.)
	float dt = ( s_lastTime > 0.0f ) ? ( now - s_lastTime ) : 0.0f;
	if( dt < 0.0f || dt > 0.25f ) dt = 0.0f;
	s_lastTime = now;
	(void)dt;

	// Reap dead beams + count live ones first (cheap; avoids GL setup when empty).
	int live = 0;
	for( int i = 0; i < kMaxBeams; i++ )
	{
		if( !s_beams[i].used )
			continue;
		if( now >= s_beams[i].pub.die )   // honours the client's pub.die = 0 stop (egon)
		{
			s_beams[i].used = false;
			continue;
		}
		live++;
	}
	if( live == 0 )
		return;

	if( !EnsureBuilt() )
		return;

	const float cam[3] = { view.origin[0], view.origin[1], view.origin[2] };

	// --- expand every live beam into the vertex scratch ---
	float *vp = s_verts;
	int    totalVerts = 0;
	const int maxVerts = kMaxBeams * kMaxSegments * kVertsPerSeg;
	int    drawn = 0;

	for( int i = 0; i < kMaxBeams; i++ )
	{
		BeamSlot &s = s_beams[i];
		if( !s.used )
			continue;

		// entity-anchored start tracks the firer each frame
		if( s.entStart )
			ResolveEntStart( s.startEnt, s.pub.source );

		float start[3] = { s.pub.source[0], s.pub.source[1], s.pub.source[2] };
		float end[3]   = { s.pub.target[0], s.pub.target[1], s.pub.target[2] };
		float delta[3] = { end[0]-start[0], end[1]-start[1], end[2]-start[2] };
		float len = sqrtf( delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2] );
		if( len < 1.0f )
			continue;

		float tangent[3] = { delta[0]/len, delta[1]/len, delta[2]/len };

		// two stable perpendiculars to the tangent (for the noise displacement)
		float up[3] = { 0.0f, 0.0f, 1.0f };
		if( fabsf( tangent[2] ) > 0.99f ) { up[0]=1.0f; up[1]=0.0f; up[2]=0.0f; }
		float perp1[3]; Cross3( tangent, up, perp1 ); Normalize3( perp1 );
		float perp2[3]; Cross3( tangent, perp1, perp2 ); Normalize3( perp2 );

		int segments = (int)( len / 32.0f ) + 1;
		if( segments < 4 ) segments = 4;
		if( segments > kMaxSegments ) segments = kMaxSegments;

		if( totalVerts + segments * kVertsPerSeg > maxVerts )
			break;   // scratch saturated this frame (logged via the count below)

		float halfWidth = s.pub.width * 0.5f;
		if( halfWidth < 0.6f ) halfWidth = 0.6f;

		const float noiseScale  = s.pub.amplitude * 35.0f;        // amplitude(0.2)->~7u wiggle
		const float noiseCycles = 4.0f;                           // table phases along the arc
		const float noiseScroll = now * ( 1.0f + s.pub.speed * 0.05f );
		const float uScale      = len / ( ( halfWidth > 0.5f ? halfWidth : 0.5f ) * 8.0f );
		const float uScroll     = now * s.pub.speed * 0.01f;

		// fade: short finite beams fade out over the last quarter of life; long
		// (egon-style) beams just hold full brightness until the client kills them.
		float alpha = s.pub.brightness * ( 1.0f / 255.0f );
		if( alpha > 1.0f ) alpha = 1.0f;
		if( s.life < 5.0f )
		{
			float tNorm = ( now - s.spawnTime ) / s.life;
			if( tNorm > 0.75f ) alpha *= ( 1.0f - tNorm ) / 0.25f;
		}
		if( alpha <= 0.0f )
			continue;

		const float col[3] = { s.pub.r, s.pub.g, s.pub.b };

		// Resolve the beam sprite texture once (best effort); fall back to the
		// procedural core. NOTE: this batch draws ALL live beams with a single
		// state, so we honour only the FIRST beam's texture choice per frame to
		// avoid mid-batch texture binds; mixed textured/procedural beams are OWED.
		if( !s.texTried )
		{
			s.texTried = true;
			s.glTexture = ResolveBeamTexture( s.pub.modelIndex, (int)s.pub.frame );
		}

		// build fold points (with pinned-endpoint noise) then ribbon quads
		float prevC[3], prevLat[3];
		for( int seg = 0; seg <= segments; seg++ )
		{
			float tA = (float)seg / (float)segments;
			float c[3] = { start[0] + delta[0]*tA, start[1] + delta[1]*tA, start[2] + delta[2]*tA };

			if( noiseScale > 0.0f )
			{
				float window = sinf( kPi * tA );   // 0 at both endpoints -> pinned
				float n1 = SampleNoise( tA * noiseCycles + noiseScroll );
				float n2 = SampleNoise( tA * noiseCycles + noiseScroll + 0.37f );
				float off = noiseScale * window;
				c[0] += ( perp1[0]*n1 + perp2[0]*n2 ) * off;
				c[1] += ( perp1[1]*n1 + perp2[1]*n2 ) * off;
				c[2] += ( perp1[2]*n1 + perp2[2]*n2 ) * off;
			}

			// camera-facing lateral axis at this fold
			float toCam[3] = { cam[0]-c[0], cam[1]-c[1], cam[2]-c[2] };
			float lat[3]; Cross3( toCam, tangent, lat );
			if( lat[0]*lat[0] + lat[1]*lat[1] + lat[2]*lat[2] < 1e-6f )
			{ lat[0]=perp1[0]; lat[1]=perp1[1]; lat[2]=perp1[2]; }
			Normalize3( lat );

			if( seg > 0 )
			{
				float u0 = ( (float)( seg-1 ) / (float)segments ) * uScale + uScroll;
				float u1 = ( (float)seg       / (float)segments ) * uScale + uScroll;
				// quad: a(prev,-), b(prev,+), c(cur,+), d(cur,-)
				float a[3] = { prevC[0]-prevLat[0]*halfWidth, prevC[1]-prevLat[1]*halfWidth, prevC[2]-prevLat[2]*halfWidth };
				float b[3] = { prevC[0]+prevLat[0]*halfWidth, prevC[1]+prevLat[1]*halfWidth, prevC[2]+prevLat[2]*halfWidth };
				float d[3] = { c[0]-lat[0]*halfWidth, c[1]-lat[1]*halfWidth, c[2]-lat[2]*halfWidth };
				float e[3] = { c[0]+lat[0]*halfWidth, c[1]+lat[1]*halfWidth, c[2]+lat[2]*halfWidth };

				#define EMIT( P, UX, UY ) do { \
					vp[0]=(P)[0]; vp[1]=(P)[1]; vp[2]=(P)[2]; \
					vp[3]=(UX); vp[4]=(UY); \
					vp[5]=col[0]; vp[6]=col[1]; vp[7]=col[2]; vp[8]=alpha; \
					vp += kFloatsPerVert; } while( 0 )
				EMIT( a, u0, 0.0f ); EMIT( b, u0, 1.0f ); EMIT( e, u1, 1.0f );
				EMIT( a, u0, 0.0f ); EMIT( e, u1, 1.0f ); EMIT( d, u1, 0.0f );
				#undef EMIT
				totalVerts += kVertsPerSeg;
			}

			prevC[0]=c[0]; prevC[1]=c[1]; prevC[2]=c[2];
			prevLat[0]=lat[0]; prevLat[1]=lat[1]; prevLat[2]=lat[2];
		}
		drawn++;
	}

	if( totalVerts == 0 )
		return;

	// --- draw: additive, depth TEST on (occluded by world) / WRITE off, in the
	// currently-bound FBO (HDR scene FBO when csz_hdr 1; FBO 0 otherwise). ---
	UseProgram( s_gpu.prog.program );
	BindVao( s_gpu.vao );
	glBindBuffer( GL_ARRAY_BUFFER, s_gpu.vbo );
	glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)sizeof( s_verts ), NULL, GL_STREAM_DRAW );   // orphan
	glBufferSubData( GL_ARRAY_BUFFER, 0, (GLsizeiptr)( (size_t)totalVerts * kFloatsPerVert * sizeof( float ) ), s_verts );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	if( s_gpu.uViewProj >= 0 ) glUniformMatrix4fv( s_gpu.uViewProj, 1, GL_FALSE, view.matViewProj.m );
	if( s_gpu.uCamPos >= 0 )   glUniform3fv( s_gpu.uCamPos, 1, view.origin );

	// fog (analytic base fog, same uniforms as the sprite pass -> consistent
	// black-fog darkening of the beam at distance)
	float fogVec[4], fogParams[4];
	CszFogUniformVecs( view.ambience, fogVec, fogParams );
	if( s_gpu.uFog >= 0 )       glUniform4fv( s_gpu.uFog, 1, fogVec );
	if( s_gpu.uFogParams >= 0 ) glUniform4fv( s_gpu.uFogParams, 1, fogParams );

	// One texture choice for the whole batch (mixed beams OWED): use the first
	// live beam's resolved texture if it has one.
	int batchTex = 0;
	for( int i = 0; i < kMaxBeams; i++ )
	{
		if( s_beams[i].used && s_beams[i].glTexture > 0 ) { batchTex = s_beams[i].glTexture; break; }
	}
	if( batchTex > 0 )
	{
		BindTextureSlot( 0, batchTex );
		if( s_gpu.uTexDiffuse >= 0 ) glUniform1i( s_gpu.uTexDiffuse, 0 );
		if( s_gpu.uTextured >= 0 )   glUniform1i( s_gpu.uTextured, 1 );
	}
	else if( s_gpu.uTextured >= 0 )
	{
		glUniform1i( s_gpu.uTextured, 0 );
	}

	SetBlend( kBlendAdditive );
	SetDepthTest( true );
	SetDepthWrite( false );
	SetCull( false );

	glDrawArrays( GL_TRIANGLES, 0, totalVerts );

	// hand a clean EnterTakeover-style baseline to the next pass (viewmodel)
	SetBlend( kBlendNone );
	SetDepthWrite( true );
	SetDepthTest( true );
	BindVao( 0 );
	UseProgram( 0 );

	static float s_nextLog;
	if( now >= s_nextLog )
	{
		s_nextLog = now + 1.0f;
		CSZ_LogDev( "beam", "drawn %d beams (%d ribbon verts)", drawn, totalVerts );
	}
}

void BeamNewMap()
{
	for( int i = 0; i < kMaxBeams; i++ )
		s_beams[i].used = false;
	s_lastTime = 0.0f;
}

void BeamShutdown()
{
	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
		DestroyGpuSameContext();
	else
		ForgetGpu();
	for( int i = 0; i < kMaxBeams; i++ )
		s_beams[i].used = false;
}

}  // namespace csz
