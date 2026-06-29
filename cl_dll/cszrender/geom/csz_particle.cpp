/*
 * csz_particle.cpp -- CSOZ renderer: self-drawn soft-particle/tracer pool (C-PAR)
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
#include "csz_particle.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_math.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"
#include "../core/csz_ambience_types.h"   // CszFogUniformVecs
#include "csz_sky_compose.h"              // SkyCompose depth/HDR handles (soft fade), TMU helpers

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace csz
{

#include "../fog/csz_fog_shaders.inl"     // kFogDepthReconstructGlsl (linViewZ helper)
#include "csz_particle_shaders.inl"       // kParticleVs / kParticleFsBody

namespace
{

const int kMaxParticles  = 4096;
const int kMaxTracers    = 256;
const int kVertsPerQuad  = 6;     // two triangles, non-indexed
const int kFloatsPerVert = 9;     // pos(3) + uv(2) + color(4)
const float kFadeBand    = 16.0f; // soft-particle depth-fade band (world units)

// gTracerColors (engine palette, r_efx.h comment block), 0..255 per channel.
// Index 4 is the cvar-driven "tracer default" -> a warm orange fallback here.
const float kPalette[12][3] = {
	{ 1.00f, 1.00f, 1.00f },  // 0 white
	{ 1.00f, 0.00f, 0.00f },  // 1 red
	{ 0.00f, 1.00f, 0.00f },  // 2 green
	{ 0.00f, 0.00f, 1.00f },  // 3 blue
	{ 1.00f, 0.71f, 0.16f },  // 4 default (warm orange fallback)
	{ 1.00f, 0.66f, 0.07f },  // 5 yellow-orange sparks (255,167,17)
	{ 1.00f, 0.51f, 0.35f },  // 6 yellowish streaks (255,130,90)
	{ 0.22f, 0.24f, 0.56f },  // 7 blue egon streak (55,60,144)
	{ 1.00f, 0.51f, 0.35f },  // 8
	{ 1.00f, 0.55f, 0.35f },  // 9
	{ 0.78f, 0.51f, 0.35f },  // 10
	{ 1.00f, 0.47f, 0.27f },  // 11
};

void Palette( int idx, float out[3] )
{
	if( idx < 0 || idx > 11 ) idx = 4;
	out[0] = kPalette[idx][0]; out[1] = kPalette[idx][1]; out[2] = kPalette[idx][2];
}

struct Particle
{
	float pos[3], vel[3];
	float color[3];     // base linear colour
	float intensity;    // radiance / coverage scale at spawn
	float size;         // world half-size
	float spawnTime, life;
	float gravity;      // downward accel (units/s^2; >0 falls, <0 rises)
	float drag;         // velocity retention per second (1 = none)
	bool  additive;     // true = spark (additive), false = smoke (alpha)
	bool  used;
};

struct Tracer
{
	float start[3], dir[3];
	float speed, len, width;
	float color[3];
	float spawnTime, life;
	bool  used;
};

Particle s_particles[kMaxParticles];
Tracer   s_tracers[kMaxTracers];
int      s_pCursor;     // ring write cursor (O(1) alloc; overwrites oldest under load)
int      s_tCursor;

float s_lastTime;       // once-per-frame sim clock (single-step guard)

// vertex scratch: additive batch (sparks + tracer ribbons) and alpha batch (smoke)
float s_addVerts[(size_t)( kMaxParticles + kMaxTracers ) * kVertsPerQuad * kFloatsPerVert];
float s_alphaVerts[(size_t)kMaxParticles * kVertsPerQuad * kFloatsPerVert];

// alpha sort scratch (back-to-front)
struct AlphaRef { int idx; float distSq; };
AlphaRef s_alphaRefs[kMaxParticles];

cvar_t *s_cvarParticle;   // csz_particle (default 1; 0 = no particle pass, clean A/B)

struct ParticleGpu
{
	ShaderProgram prog;
	GLuint vao, vbo;
	int    gpuGeneration;
	bool   built, failedThisGen;
	int    uMatViewProj, uMatView, uDepthTex, uViewport, uFade, uSoftFade, uFog, uFogParams, uCamPos, uZNear, uZFar;
};
ParticleGpu s_gpu;

float RandF( float lo, float hi )
{
	if( gEngfuncs.pfnRandomFloat != NULL )
		return gEngfuncs.pfnRandomFloat( lo, hi );
	return lo + ( hi - lo ) * 0.5f;
}

void RandSphere( float out[3] )
{
	// rejection-free unit-ish direction from the engine RNG
	out[0] = RandF( -1.0f, 1.0f );
	out[1] = RandF( -1.0f, 1.0f );
	out[2] = RandF( -1.0f, 1.0f );
	float l = sqrtf( out[0]*out[0] + out[1]*out[1] + out[2]*out[2] );
	if( l > 1e-4f ) { float inv = 1.0f/l; out[0]*=inv; out[1]*=inv; out[2]*=inv; }
	else { out[0]=0.0f; out[1]=0.0f; out[2]=1.0f; }
}

Particle *AllocParticle()
{
	Particle *p = &s_particles[s_pCursor];
	s_pCursor = ( s_pCursor + 1 ) % kMaxParticles;
	return p;
}

Tracer *AllocTracer()
{
	Tracer *t = &s_tracers[s_tCursor];
	s_tCursor = ( s_tCursor + 1 ) % kMaxTracers;
	return t;
}

void SpawnParticle( const float pos[3], const float vel[3], const float color[3],
	float intensity, float size, float life, float gravity, float drag, bool additive )
{
	Particle *p = AllocParticle();
	p->pos[0]=pos[0]; p->pos[1]=pos[1]; p->pos[2]=pos[2];
	p->vel[0]=vel[0]; p->vel[1]=vel[1]; p->vel[2]=vel[2];
	p->color[0]=color[0]; p->color[1]=color[1]; p->color[2]=color[2];
	p->intensity = intensity;
	p->size = size;
	p->spawnTime = ClientTime();
	p->life = ( life > 0.0f ) ? life : 0.0001f;
	p->gravity = gravity;
	p->drag = drag;
	p->additive = additive;
	p->used = true;
}

void Cross3( const float a[3], const float b[3], float out[3] )
{
	out[0]=a[1]*b[2]-a[2]*b[1]; out[1]=a[2]*b[0]-a[0]*b[2]; out[2]=a[0]*b[1]-a[1]*b[0];
}
void Normalize3( float v[3] )
{
	float l=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(l>1e-6f){float i=1.0f/l;v[0]*=i;v[1]*=i;v[2]*=i;}
}

void ForgetGpu()
{
	s_gpu.vao=0; s_gpu.vbo=0; s_gpu.prog.program=0; s_gpu.built=false; s_gpu.failedThisGen=false;
}
void DestroyGpuSameContext()
{
	if( s_gpu.vbo ) glDeleteBuffers( 1, &s_gpu.vbo );
	if( s_gpu.vao ) glDeleteVertexArrays( 1, &s_gpu.vao );
	if( s_gpu.prog.program ) DestroyProgram( s_gpu.prog );
	ForgetGpu();
}

bool EnsureBuilt()
{
	if( s_gpu.gpuGeneration != GpuGeneration() )
	{
		ForgetGpu();
		s_gpu.gpuGeneration = GpuGeneration();
	}
	if( s_gpu.built ) return true;
	if( s_gpu.failedThisGen ) return false;

	std::string fs = std::string( "#version 330 core\n" ) + kFogDepthReconstructGlsl + kParticleFsBody;
	if( !BuildProgram( "csz_particle", kParticleVs, fs.c_str(), false, s_gpu.prog ) )
	{
		s_gpu.failedThisGen = true;
		CSZ_LogError( "particle", "shader build failed; particles disabled this generation" );
		return false;
	}

	s_gpu.uMatViewProj = UniformLoc( s_gpu.prog, "u_matViewProj" );
	s_gpu.uMatView     = UniformLoc( s_gpu.prog, "u_matView" );
	s_gpu.uDepthTex    = UniformLoc( s_gpu.prog, "u_depthTex" );
	s_gpu.uViewport    = UniformLoc( s_gpu.prog, "u_viewport" );
	s_gpu.uFade        = UniformLoc( s_gpu.prog, "u_fade" );
	s_gpu.uSoftFade    = UniformLoc( s_gpu.prog, "u_softFade" );
	s_gpu.uFog         = UniformLoc( s_gpu.prog, "u_fog" );
	s_gpu.uFogParams   = UniformLoc( s_gpu.prog, "u_fogParams" );
	s_gpu.uCamPos      = UniformLoc( s_gpu.prog, "u_camPos" );
	s_gpu.uZNear       = UniformLoc( s_gpu.prog, "u_zNear" );
	s_gpu.uZFar        = UniformLoc( s_gpu.prog, "u_zFar" );

	glGenVertexArrays( 1, &s_gpu.vao );
	BindVao( s_gpu.vao );
	glGenBuffers( 1, &s_gpu.vbo );
	glBindBuffer( GL_ARRAY_BUFFER, s_gpu.vbo );
	// allocated to the LARGER of the two batch scratches; re-orphaned per draw
	const GLsizeiptr cap = (GLsizeiptr)sizeof( s_addVerts );
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
	CSZ_LogDev( "particle", "program + stream VBO built (cap %d particles, gpu gen %d)", kMaxParticles, s_gpu.gpuGeneration );
	return true;
}

// CPU billboard expansion -> 6 verts; uv = corner in [-1,1] (round falloff).
const float kCx[6] = { -1.0f,  1.0f,  1.0f, -1.0f,  1.0f, -1.0f };
const float kCy[6] = { -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f };

float *EmitBillboard( float *vp, const float p[3], const float right[3], const float up[3],
	float sz, const float rgba[4] )
{
	for( int v = 0; v < kVertsPerQuad; v++ )
	{
		float ox = kCx[v]*sz, oy = kCy[v]*sz;
		vp[0]=p[0]+right[0]*ox+up[0]*oy; vp[1]=p[1]+right[1]*ox+up[1]*oy; vp[2]=p[2]+right[2]*ox+up[2]*oy;
		vp[3]=kCx[v]; vp[4]=kCy[v];
		vp[5]=rgba[0]; vp[6]=rgba[1]; vp[7]=rgba[2]; vp[8]=rgba[3];
		vp += kFloatsPerVert;
	}
	return vp;
}

int CompareAlpha( const void *a, const void *b )
{
	const AlphaRef *ia=(const AlphaRef*)a, *ib=(const AlphaRef*)b;
	if( ia->distSq > ib->distSq ) return -1;   // far to near
	if( ia->distSq < ib->distSq ) return 1;
	return 0;
}

}  // anonymous namespace

// =========================================================================
// efx shim entry points -> emitter configs (S3 first-batch mapping)
// =========================================================================
void ParticleEmitBulletImpact( const float *pos )
{
	// dust puff: a few low, settling, grey-brown smoke motes (alpha)
	const float col[3] = { 0.32f, 0.28f, 0.24f };
	for( int i = 0; i < 7; i++ )
	{
		float d[3]; RandSphere( d );
		float spd = RandF( 10.0f, 40.0f );
		float vel[3] = { d[0]*spd, d[1]*spd, d[2]*spd + RandF( 5.0f, 25.0f ) };
		SpawnParticle( pos, vel, col, RandF( 0.7f, 0.95f ), RandF( 2.0f, 3.5f ),
			RandF( 0.4f, 0.7f ), 30.0f, 0.5f, false );
	}
}

void ParticleEmitStreakSplash( const float *pos, const float *dir, int color, int count, float speed, int velMin, int velMax )
{
	float col[3]; Palette( color, col );
	if( count < 1 ) count = 1;
	if( count > 64 ) count = 64;
	float ndir[3] = { dir[0], dir[1], dir[2] };
	Normalize3( ndir );
	for( int i = 0; i < count; i++ )
	{
		float base = RandF( (float)velMin, (float)velMax );
		float d[3]; RandSphere( d );
		float vel[3] = {
			ndir[0]*base + d[0]*base*0.4f + ndir[0]*speed,
			ndir[1]*base + d[1]*base*0.4f + ndir[1]*speed,
			ndir[2]*base + d[2]*base*0.4f + ndir[2]*speed };
		SpawnParticle( pos, vel, col, 1.4f, RandF( 0.8f, 1.6f ), RandF( 0.3f, 0.6f ), 220.0f, 0.2f, true );
	}
}

void ParticleEmitSparkEffect( const float *pos, int count, int velMin, int velMax )
{
	float col[3]; Palette( 5, col );   // yellow-orange sparks
	if( count < 1 ) count = 1;
	if( count > 64 ) count = 64;
	for( int i = 0; i < count; i++ )
	{
		float d[3]; RandSphere( d );
		float spd = RandF( (float)velMin, (float)velMax );
		float vel[3] = { d[0]*spd, d[1]*spd, d[2]*spd };
		SpawnParticle( pos, vel, col, 1.3f, RandF( 0.8f, 1.4f ), RandF( 0.3f, 0.6f ), 250.0f, 0.2f, true );
	}
}

void ParticleEmitSparkShower( const float *pos )
{
	ParticleEmitSparkEffect( pos, 6, 80, 180 );
}

void ParticleEmitSparkStreaks( const float *pos, int count, int velMin, int velMax )
{
	ParticleEmitSparkEffect( pos, count, velMin, velMax );
}

void ParticleEmitRocketTrail( const float *start, const float *end, int type )
{
	(void)type;
	float d[3] = { end[0]-start[0], end[1]-start[1], end[2]-start[2] };
	float dist = sqrtf( d[0]*d[0]+d[1]*d[1]+d[2]*d[2] );
	if( dist < 1.0f ) return;
	int steps = (int)( dist / 12.0f ) + 1;
	if( steps > 64 ) steps = 64;
	const float col[3] = { 0.30f, 0.30f, 0.30f };
	for( int i = 0; i < steps; i++ )
	{
		float t = (float)i / (float)steps;
		float pos[3] = { start[0]+d[0]*t, start[1]+d[1]*t, start[2]+d[2]*t };
		float vel[3] = { RandF(-6.0f,6.0f), RandF(-6.0f,6.0f), RandF( 4.0f, 14.0f ) };
		SpawnParticle( pos, vel, col, 0.6f, RandF( 3.0f, 6.0f ), RandF( 0.8f, 1.4f ), -8.0f, 0.6f, false );
	}
}

// NOTE: R_Sprite_Trail is intentionally NOT redirected here -- the engine draws
// its FTENT sprite tempents directly (csz_efx_shim passthrough), so a redirect
// would double-draw. The former ParticleEmitSpriteTrail helper was never called
// and has been removed.

void ParticleEmitTracer( const float *start, const float *end )
{
	float d[3] = { end[0]-start[0], end[1]-start[1], end[2]-start[2] };
	float dist = sqrtf( d[0]*d[0]+d[1]*d[1]+d[2]*d[2] );
	if( dist < 1.0f ) return;

	Tracer *t = AllocTracer();
	t->start[0]=start[0]; t->start[1]=start[1]; t->start[2]=start[2];
	t->dir[0]=d[0]/dist; t->dir[1]=d[1]/dist; t->dir[2]=d[2]/dist;
	t->speed = 5500.0f;
	t->len   = 90.0f;
	t->width = 1.2f;
	t->color[0]=1.0f; t->color[1]=0.78f; t->color[2]=0.35f;
	t->spawnTime = ClientTime();
	float travel = dist / t->speed;
	t->life = ( travel < 0.45f ) ? travel : 0.45f;
	if( t->life < 0.05f ) t->life = 0.05f;
	t->used = true;
}

// =========================================================================
void ParticleRegisterCvars()
{
	if( s_cvarParticle == NULL )
		s_cvarParticle = gEngfuncs.pfnRegisterVariable( "csz_particle", "1", FCVAR_CLIENTDLL );
	CSZ_LogDev( "particle", "cvars registered (csz_particle)" );
}

void ParticleDraw( const ViewSetup &view )
{
	if( ReadCvar( s_cvarParticle, 1.0f ) < 0.5f )
		return;

	const float now = ClientTime();
	float dt = ( s_lastTime > 0.0f ) ? ( now - s_lastTime ) : 0.0f;
	if( dt < 0.0f || dt > 0.25f ) dt = 0.0f;   // first frame / pause / map change: no jump
	s_lastTime = now;

	// soft-fade path needs the HDR depth texture; otherwise fall back to a hard
	// depth-tested additive draw into the bound FBO.
	const bool   hdr      = SkyComposeActive();
	const GLuint depthTex = hdr ? SkyComposeDepthTex() : 0;
	const bool   soft     = ( depthTex != 0 );

	// billboard axes from the view matrix rows (parallel orientation, sprite precedent)
	const float *m = view.matView.m;
	const float right[3] = { m[0], m[4], m[8] };
	const float up[3]    = { m[1], m[5], m[9] };
	const float cam[3]   = { view.origin[0], view.origin[1], view.origin[2] };

	// --- sim + expand ---
	float *addvp = s_addVerts;
	int    addVerts = 0;
	int    numAlpha = 0;

	for( int i = 0; i < kMaxParticles; i++ )
	{
		Particle &p = s_particles[i];
		if( !p.used )
			continue;
		float age = now - p.spawnTime;
		if( age >= p.life )
		{
			p.used = false;
			continue;
		}
		if( dt > 0.0f )
		{
			p.vel[2] -= p.gravity * dt;
			float damp = expf( -( 1.0f - p.drag ) * dt );   // drag in (0,1] -> gentle damping
			p.vel[0]*=damp; p.vel[1]*=damp; p.vel[2]*=damp;
			p.pos[0]+=p.vel[0]*dt; p.pos[1]+=p.vel[1]*dt; p.pos[2]+=p.vel[2]*dt;
		}

		float f = 1.0f - age / p.life;   // linear fade over life
		if( p.additive )
		{
			float rgba[4] = { p.color[0]*p.intensity*f, p.color[1]*p.intensity*f, p.color[2]*p.intensity*f, f };
			addvp = EmitBillboard( addvp, p.pos, right, up, p.size, rgba );
			addVerts += kVertsPerQuad;
		}
		else
		{
			// alpha smoke: collect for back-to-front sort (expanded below)
			float dx=p.pos[0]-cam[0], dy=p.pos[1]-cam[1], dz=p.pos[2]-cam[2];
			s_alphaRefs[numAlpha].idx = i;
			s_alphaRefs[numAlpha].distSq = dx*dx+dy*dy+dz*dz;
			numAlpha++;
		}
	}

	// tracers -> additive ribbons appended to the additive batch
	int liveTracers = 0;
	for( int i = 0; i < kMaxTracers; i++ )
	{
		Tracer &t = s_tracers[i];
		if( !t.used )
			continue;
		float age = now - t.spawnTime;
		if( age >= t.life )
		{
			t.used = false;
			continue;
		}
		liveTracers++;

		float head[3] = { t.start[0]+t.dir[0]*t.speed*age, t.start[1]+t.dir[1]*t.speed*age, t.start[2]+t.dir[2]*t.speed*age };
		float tail[3] = { head[0]-t.dir[0]*t.len, head[1]-t.dir[1]*t.len, head[2]-t.dir[2]*t.len };

		float toCam[3] = { cam[0]-head[0], cam[1]-head[1], cam[2]-head[2] };
		float lat[3]; Cross3( toCam, t.dir, lat );
		Normalize3( lat );
		float hw = t.width * 0.5f;
		float f = 1.0f - age / t.life;
		float rgba[4] = { t.color[0]*f, t.color[1]*f, t.color[2]*f, f };

		float a[3]={tail[0]-lat[0]*hw,tail[1]-lat[1]*hw,tail[2]-lat[2]*hw};
		float b[3]={tail[0]+lat[0]*hw,tail[1]+lat[1]*hw,tail[2]+lat[2]*hw};
		float c[3]={head[0]+lat[0]*hw,head[1]+lat[1]*hw,head[2]+lat[2]*hw};
		float e[3]={head[0]-lat[0]*hw,head[1]-lat[1]*hw,head[2]-lat[2]*hw};
		#define EMITR( P, UX ) do { addvp[0]=(P)[0]; addvp[1]=(P)[1]; addvp[2]=(P)[2]; \
			addvp[3]=(UX); addvp[4]=0.0f; addvp[5]=rgba[0]; addvp[6]=rgba[1]; addvp[7]=rgba[2]; addvp[8]=rgba[3]; \
			addvp += kFloatsPerVert; } while( 0 )
		EMITR( a, -1.0f ); EMITR( b, 1.0f ); EMITR( c, 1.0f );
		EMITR( a, -1.0f ); EMITR( c, 1.0f ); EMITR( e, -1.0f );
		#undef EMITR
		addVerts += kVertsPerQuad;
	}

	// expand alpha smoke in back-to-front order
	float *alphavp = s_alphaVerts;
	int    alphaVerts = 0;
	if( numAlpha > 0 )
	{
		qsort( s_alphaRefs, (size_t)numAlpha, sizeof( AlphaRef ), CompareAlpha );
		for( int k = 0; k < numAlpha; k++ )
		{
			const Particle &p = s_particles[s_alphaRefs[k].idx];
			float age = now - p.spawnTime;
			float f = 1.0f - age / p.life;
			float baseA = 0.6f;
			float rgba[4] = { p.color[0], p.color[1], p.color[2], baseA * f * p.intensity };
			alphavp = EmitBillboard( alphavp, p.pos, right, up, p.size, rgba );
			alphaVerts += kVertsPerQuad;
		}
	}

	if( addVerts == 0 && alphaVerts == 0 )
		return;
	if( !EnsureBuilt() )
		return;

	// --- shared uniform setup ---
	UseProgram( s_gpu.prog.program );
	BindVao( s_gpu.vao );

	if( s_gpu.uMatViewProj >= 0 ) glUniformMatrix4fv( s_gpu.uMatViewProj, 1, GL_FALSE, view.matViewProj.m );
	if( s_gpu.uMatView >= 0 )     glUniformMatrix4fv( s_gpu.uMatView, 1, GL_FALSE, view.matView.m );
	if( s_gpu.uCamPos >= 0 )      glUniform3fv( s_gpu.uCamPos, 1, view.origin );
	if( s_gpu.uFade >= 0 )        glUniform1f( s_gpu.uFade, kFadeBand );
	if( s_gpu.uZNear >= 0 )       glUniform1f( s_gpu.uZNear, view.zNear );
	if( s_gpu.uZFar >= 0 )        glUniform1f( s_gpu.uZFar, view.zFar );
	if( s_gpu.uSoftFade >= 0 )    glUniform1i( s_gpu.uSoftFade, soft ? 1 : 0 );

	float fogVec[4], fogParams[4];
	CszFogUniformVecs( view.ambience, fogVec, fogParams );
	if( s_gpu.uFog >= 0 )         glUniform4fv( s_gpu.uFog, 1, fogVec );
	if( s_gpu.uFogParams >= 0 )   glUniform4fv( s_gpu.uFogParams, 1, fogParams );

	if( soft )
	{
		SkyComposeBindTex( 0, GL_TEXTURE_2D, depthTex );
		if( s_gpu.uDepthTex >= 0 ) glUniform1i( s_gpu.uDepthTex, kSkyTmuBase + 0 );
		// Pass the actual 3D viewport rect (x,y,w,h). The shader samples the
		// viewport-sized depth tex at (gl_FragCoord.xy - vp.xy)/vp.zw, which is
		// origin-correct -- the old (vp.x+vp.w, vp.y+vp.h) divisor only matched
		// when the viewport origin was (0,0). Full-window pass: vp.xy==0, so the
		// same formula still reduces to gl_FragCoord/extent.
		float fViewport[4] = { (float)view.viewport[0], (float)view.viewport[1],
		                       (float)view.viewport[2], (float)view.viewport[3] };
		if( fViewport[2] < 1.0f ) fViewport[2] = 1.0f;   // guard zero extent (div)
		if( fViewport[3] < 1.0f ) fViewport[3] = 1.0f;
		if( s_gpu.uViewport >= 0 ) glUniform4fv( s_gpu.uViewport, 1, fViewport );
		// soft path = dust contract: depth test/write OFF, soft intersection in-shader
		SetDepthTest( false );
		SetDepthWrite( false );
	}
	else
	{
		// no HDR depth texture: hard depth test (world occludes), write off
		SetDepthTest( true );
		SetDepthWrite( false );
	}
	SetCull( false );

	// --- additive batch (sparks + tracers) ---
	if( addVerts > 0 )
	{
		SetBlend( kBlendAdditive );
		glBindBuffer( GL_ARRAY_BUFFER, s_gpu.vbo );
		glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)sizeof( s_addVerts ), NULL, GL_STREAM_DRAW );
		glBufferSubData( GL_ARRAY_BUFFER, 0, (GLsizeiptr)( (size_t)addVerts * kFloatsPerVert * sizeof( float ) ), s_addVerts );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDrawArrays( GL_TRIANGLES, 0, addVerts );
	}

	// --- alpha smoke batch (sorted back-to-front) ---
	if( alphaVerts > 0 )
	{
		SetBlend( kBlendAlpha );
		glBindBuffer( GL_ARRAY_BUFFER, s_gpu.vbo );
		glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)sizeof( s_addVerts ), NULL, GL_STREAM_DRAW );
		glBufferSubData( GL_ARRAY_BUFFER, 0, (GLsizeiptr)( (size_t)alphaVerts * kFloatsPerVert * sizeof( float ) ), s_alphaVerts );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDrawArrays( GL_TRIANGLES, 0, alphaVerts );
	}

	// restore the EnterTakeover baseline for the next pass (viewmodel)
	if( soft )
		SkyComposeRestoreTmus();
	SetBlend( kBlendNone );
	SetDepthTest( true );
	SetDepthWrite( true );
	BindVao( 0 );
	UseProgram( 0 );

	static float s_nextLog;
	if( now >= s_nextLog )
	{
		s_nextLog = now + 1.0f;
		CSZ_LogDev( "particle", "drawn add=%d alpha=%d verts (tracers %d, soft=%d)",
			addVerts, alphaVerts, liveTracers, soft ? 1 : 0 );
	}
}

void ParticleNewMap()
{
	for( int i = 0; i < kMaxParticles; i++ ) s_particles[i].used = false;
	for( int i = 0; i < kMaxTracers; i++ )   s_tracers[i].used = false;
	s_pCursor = 0; s_tCursor = 0; s_lastTime = 0.0f;
}

void ParticleShutdown()
{
	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
		DestroyGpuSameContext();
	else
		ForgetGpu();
	for( int i = 0; i < kMaxParticles; i++ ) s_particles[i].used = false;
	for( int i = 0; i < kMaxTracers; i++ )   s_tracers[i].used = false;
}

}  // namespace csz
