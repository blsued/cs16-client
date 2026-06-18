/*
 * csz_weather.cpp -- CSOZ renderer: weather precipitation + ground surface state
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
#include "csz_weather.h"
#include "../core/csz_engine.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_glcaps.h"	// GpuGeneration()
#include "../core/csz_math.h"	// AngleVectors
#include "../core/csz_shader.h"
#include "../core/csz_log.h"

#include "csz_weather_shaders.inl"	// rain/snow particle shader sources (B2)

#include <math.h>
#include <stdlib.h>	// atof/atoi for the dev command

namespace csz
{

WeatherRenderer g_weather;	// zero-initialized (static storage duration)

namespace
{
// csz_weather: precipitation mode -- 0 = off, 1 = rain, 2 = snow.
cvar_t *s_modeCvar;
// csz_weather_intensity: 0..1 strength (rain wetness / snowfall amount).
cvar_t *s_intensityCvar;
// csz_weather_quality: 0 low / 1 med / 2 high -- scales particle COUNT only
// (B2), never whether precipitation is present.
cvar_t *s_qualityCvar;

float Clamp01( float v )
{
	if( v < 0.0f ) return 0.0f;
	if( v > 1.0f ) return 1.0f;
	return v;
}

float Clampf( float v, float lo, float hi )
{
	if( v < lo ) return lo;
	if( v > hi ) return hi;
	return v;
}

// --- B2 particle constants -------------------------------------------------
// Spawn/recycle box around the camera (view-relative). Particles always live
// in this box so the weather follows the player ("presence is the same across
// tiers"). Horizontal half-extent + vertical top/bottom relative to the camera.
const float kBoxHalf = 700.0f;   // +/- horizontal extent around view.origin
const float kBoxTop  = 600.0f;   // spawn ceiling above the camera
const float kBoxBot  = -200.0f;  // recycle floor below the camera

// Per-tier max active counts (csz_weather_quality 0/1/2). Rain is denser than
// snow. The pool capacity (kMaxParticles) equals the largest of these so a tier
// change only moves m_activeCount -- it never reallocates.
const int kRainCount[3] = { 1200, 3500, 7000 };
const int kSnowCount[3] = {  800, 2000, 4000 };

// Vertex layout shared by both programs: pos(3) + corner(2) + color(4) = 9.
const int kVertFloats = 9;
const int kVertsPerQuad = 6;     // two triangles, non-indexed (simple + cheap)

// CPU vertex scratch, sized for the high tier so it is allocated once (static
// storage, not per-frame). MUST stay >= WeatherRenderer::kMaxParticles quads
// (= 7000); kRainCount[2] above is the high-tier rain max that drives it.
const int kScratchMaxQuads = 7000;	// == WeatherRenderer::kMaxParticles
float s_vertScratch[kScratchMaxQuads * kVertsPerQuad * kVertFloats];

// Deterministic 32-bit integer hash (public-domain "wang/xxhash-style" mix).
// Used to seed/recycle particles without relying on a seeded std::rand.
unsigned int Hash32( unsigned int x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

// Deterministic float in [0,1) from two integer keys (particle index + salt).
float Rand01( unsigned int key, unsigned int salt )
{
	unsigned int h = Hash32( key * 0x9e3779b1U + salt );
	return (float)( h & 0x00FFFFFFU ) * ( 1.0f / 16777216.0f );
}

// Per-tier active count from cvar, scaled by intensity (active fraction).
int ActiveCountForTier( int mode, int quality, float intensity )
{
	int q = (int)Clampf( (float)quality, 0.0f, 2.0f );
	int maxCount = ( mode == 1 ) ? kRainCount[q] : kSnowCount[q];
	int n = (int)( (float)maxCount * Clamp01( intensity ) + 0.5f );

	if( n < 0 ) n = 0;
	if( n > maxCount ) n = maxCount;
	return n;
}

#if defined( CSZ_DEV_TOOLS )
// csz_devweather <0|1|2> [intensity]: set csz_weather (+ optional intensity)
// for testing. Mirrors the fog/sky dev-command idiom (Cvar_SetValue writes the
// live cvar so Mode()/RainIntensity() pick it up next Update()).
void DevWeatherCommand()
{
	if( gEngfuncs.Cmd_Argc() < 2 )
	{
		CSZ_LogInfo( "weather", "usage: csz_devweather <0|1|2> [intensity 0..1]" );
		return;
	}

	int mode = atoi( gEngfuncs.Cmd_Argv( 1 ));

	mode = (int)Clampf( (float)mode, 0.0f, 2.0f );
	gEngfuncs.Cvar_SetValue( "csz_weather", (float)mode );

	if( gEngfuncs.Cmd_Argc() >= 3 )
	{
		float intensity = Clamp01( (float)atof( gEngfuncs.Cmd_Argv( 2 )));
		gEngfuncs.Cvar_SetValue( "csz_weather_intensity", intensity );
	}

	CSZ_LogInfo( "weather", "csz_devweather mode=%d", mode );
}
#endif

}

void WeatherRenderer::RegisterCvars()
{
	if( s_modeCvar == NULL )
		s_modeCvar = gEngfuncs.pfnRegisterVariable( "csz_weather", "0", FCVAR_CLIENTDLL );

	if( s_intensityCvar == NULL )
		s_intensityCvar = gEngfuncs.pfnRegisterVariable( "csz_weather_intensity", "0.7", FCVAR_CLIENTDLL );

	if( s_qualityCvar == NULL )
		s_qualityCvar = gEngfuncs.pfnRegisterVariable( "csz_weather_quality", "1", FCVAR_CLIENTDLL );

#if defined( CSZ_DEV_TOOLS )
	gEngfuncs.pfnAddCommand( "csz_devweather", DevWeatherCommand );
	CSZ_LogDev( "weather", "dev weather command registered (CSZ_DEV_TOOLS build)" );
#endif
}

int WeatherRenderer::Mode() const
{
	if( s_modeCvar == NULL )
		return 0;

	return (int)Clampf( (float)(int)s_modeCvar->value, 0.0f, 2.0f );
}

float WeatherRenderer::RainIntensity() const
{
	if( Mode() != 1 || s_intensityCvar == NULL )
		return 0.0f;

	return Clamp01( s_intensityCvar->value );
}

void WeatherRenderer::Update( const ViewSetup &view, float phase, float time )
{
	(void)phase;
	(void)time;

	int mode = Mode();
	float intensity = ( s_intensityCvar != NULL ) ? Clamp01( s_intensityCvar->value ) : 0.0f;

	// Surface contract (read by B3's world-shader splice). Exactly one of
	// wetness / snowAmount is non-zero; the other is forced to 0 so the world
	// shader never blends rain-wet and snow-cover at the same time.
	if( mode == 1 )			// rain
	{
		m_surf.wetness = intensity;
		m_surf.snowAmount = 0.0f;
	}
	else if( mode == 2 )		// snow
	{
		m_surf.snowAmount = intensity;
		m_surf.wetness = 0.0f;
	}
	else				// off
	{
		m_surf.wetness = 0.0f;
		m_surf.snowAmount = 0.0f;
	}

	if( mode == 2 )
	{
		// Snow color: near-white base cooled by the published night tint so snow
		// goes cool/dark at night and never overexposes. The tint ALREADY carries
		// the night darkening (its channels drop below 1 at night), so darken
		// ONCE: base * tint, clamped <= 1. (Previously this also multiplied by the
		// tint luminance, applying the night dim ~twice -> night snow far too dark.)
		const float base[3] = { 0.85f, 0.88f, 0.95f };
		const float *tint = view.ambience.tint;

		for( int i = 0; i < 3; i++ )
		{
			float c = base[i] * tint[i];

			if( c > 1.0f )
				c = 1.0f;

			m_surf.snowColor[i] = c;
		}
	}
	else
	{
		// Unused outside snow mode: leave neutral white.
		m_surf.snowColor[0] = 1.0f;
		m_surf.snowColor[1] = 1.0f;
		m_surf.snowColor[2] = 1.0f;
	}

	// ===B2: particle simulation goes here===
	// Advance the rain/snow particle pool (spawn/recycle/integrate). Pure CPU;
	// runs pre-takeover. No-op when weather is off (Simulate early-returns).
	Simulate( view );
}

void WeatherRenderer::Simulate( const ViewSetup &view )
{
	int mode = Mode();

	// No-op when off: leave the pool idle (do not reset m_lastTime so the first
	// frame after re-enabling still gets a clamped, sane dt).
	if( mode == 0 )
	{
		m_poolMode = 0;
		return;
	}

	int quality = ( s_qualityCvar != NULL ) ? (int)s_qualityCvar->value : 1;
	float intensity = ( s_intensityCvar != NULL ) ? Clamp01( s_intensityCvar->value ) : 0.0f;

	m_activeCount = ActiveCountForTier( mode, quality, intensity );
	if( m_activeCount > kMaxParticles )
		m_activeCount = kMaxParticles;

	float now = ClientTime();
	float dt = now - m_lastTime;

	m_lastTime = now;
	dt = Clampf( dt, 0.0f, 0.1f );	// survive pauses / hitches / first frame

	m_frameCounter++;

	const float *org = view.origin;

	// (Re)seed the whole pool when the mode changes or it has never been seeded:
	// scatter every particle through the box so the effect is full immediately
	// (no ramp-up) and each particle gets a stable seed/layer. Done for the FULL
	// capacity so later tier increases reveal already-placed particles.
	if( !m_poolSeeded || m_poolMode != mode )
	{
		for( int i = 0; i < kMaxParticles; i++ )
		{
			Particle &p = m_pool[i];
			unsigned int key = (unsigned int)i;

			p.seed = Rand01( key, 1u );
			p.layer = Rand01( key, 2u );	// 0..1 depth layer (rain)

			p.pos[0] = org[0] + ( Rand01( key, 3u ) * 2.0f - 1.0f ) * kBoxHalf;
			p.pos[1] = org[1] + ( Rand01( key, 4u ) * 2.0f - 1.0f ) * kBoxHalf;
			p.pos[2] = org[2] + kBoxBot + Rand01( key, 5u ) * ( kBoxTop - kBoxBot );

			p.vel[0] = p.vel[1] = p.vel[2] = 0.0f;
		}

		m_poolSeeded = true;
		m_poolMode = mode;
	}

	// Integrate + recycle the active prefix. A particle that leaves the box (fell
	// below the floor, or drifted out of the horizontal extent) is recycled to a
	// fresh randomized spot at the TOP of the box, so density stays constant.
	for( int i = 0; i < m_activeCount; i++ )
	{
		Particle &p = m_pool[i];
		unsigned int key = (unsigned int)i;

		if( mode == 1 )		// RAIN: fast vertical streaks, slight wind, depth-layered speed
		{
			// Near layer (layer~1) falls faster; far layer (layer~0) a touch slower.
			float fall = -1500.0f - p.layer * 700.0f;	// -1500..-2200 z
			float windX = 60.0f + p.seed * 40.0f;		// slight constant wind
			float windY = -30.0f + p.seed * 60.0f;

			p.pos[0] += windX * dt;
			p.pos[1] += windY * dt;
			p.pos[2] += fall * dt;
		}
		else			// SNOW: slow fall + lateral drift/sway
		{
			float fall = -90.0f - p.seed * 60.0f;		// -90..-150 z
			float t = now + p.seed * 6.2831853f;
			float swayX = sinf( t * 0.8f ) * 30.0f;		// lateral sway
			float swayY = cosf( t * 0.6f ) * 24.0f;
			float driftX = ( p.seed - 0.5f ) * 30.0f;	// steady drift bias

			p.pos[0] += ( swayX + driftX ) * dt;
			p.pos[1] += swayY * dt;
			p.pos[2] += fall * dt;
		}

		// Recycle if outside the view-relative box (vertical floor or horizontal).
		float dx = p.pos[0] - org[0];
		float dy = p.pos[1] - org[1];
		float relZ = p.pos[2] - org[2];

		if( relZ < kBoxBot || dx < -kBoxHalf || dx > kBoxHalf || dy < -kBoxHalf || dy > kBoxHalf )
		{
			// Fresh randomized x/y near the top; salt with the frame counter so the
			// recycled position is deterministic yet varies frame-to-frame.
			unsigned int salt = m_frameCounter;

			p.pos[0] = org[0] + ( Rand01( key, salt + 11u ) * 2.0f - 1.0f ) * kBoxHalf;
			p.pos[1] = org[1] + ( Rand01( key, salt + 22u ) * 2.0f - 1.0f ) * kBoxHalf;
			p.pos[2] = org[2] + kBoxTop - Rand01( key, salt + 33u ) * 40.0f;	// just under the ceiling
		}
	}
}

void WeatherRenderer::EnsureBuilt()
{
	if( m_gpu.built && m_gpu.gpuGeneration == GpuGeneration() )
		return;

	if( m_gpu.gpuGeneration != GpuGeneration() )
	{
		// Stale generation (context loss / vid restart): forget the names, never
		// glDelete them -- the context that owned them is gone (mirrors csz_sky).
		m_gpu.rainProgram.program = 0;
		m_gpu.snowProgram.program = 0;
		m_gpu.rainVao = m_gpu.rainVbo = 0;
		m_gpu.snowVao = m_gpu.snowVbo = 0;
	}

	m_gpu.gpuGeneration = GpuGeneration();

	// Init-time programs: a compile failure is FATAL (matches world/sky/sprite).
	BuildProgram( "csz_weather_rain", kRainVs, kRainFs, true, m_gpu.rainProgram );
	BuildProgram( "csz_weather_snow", kSnowVs, kSnowFs, true, m_gpu.snowProgram );

	m_gpu.uRainViewProj = UniformLoc( m_gpu.rainProgram, "u_viewProj" );
	m_gpu.uSnowViewProj = UniformLoc( m_gpu.snowProgram, "u_viewProj" );

	const int stride = kVertFloats * (int)sizeof( float );

	// Rain VAO/VBO (dynamic; orphaned each frame via glBufferData).
	glGenVertexArrays( 1, &m_gpu.rainVao );
	BindVao( m_gpu.rainVao );
	glGenBuffers( 1, &m_gpu.rainVbo );
	glBindBuffer( GL_ARRAY_BUFFER, m_gpu.rainVbo );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)( 3 * sizeof( float )));
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 4, GL_FLOAT, GL_FALSE, stride, (const void *)( 5 * sizeof( float )));

	// Snow VAO/VBO (same layout).
	glGenVertexArrays( 1, &m_gpu.snowVao );
	BindVao( m_gpu.snowVao );
	glGenBuffers( 1, &m_gpu.snowVbo );
	glBindBuffer( GL_ARRAY_BUFFER, m_gpu.snowVbo );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)( 3 * sizeof( float )));
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 4, GL_FLOAT, GL_FALSE, stride, (const void *)( 5 * sizeof( float )));

	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	m_gpu.built = true;
	CSZ_LogDev( "weather", "weather particle programs built (gpu gen %d)", m_gpu.gpuGeneration );
}

void WeatherRenderer::DrawPrecip( const ViewSetup &view )
{
	int mode = Mode();

	if( mode == 0 || m_activeCount <= 0 )
		return;	// off, or nothing active this frame

	EnsureBuilt();

	if( !m_gpu.built )
		return;

	// Camera basis (Quake world space, Z up). right/up billboard the quads.
	float fwd[3], right[3], up[3];

	AngleVectors( view.angles, fwd, right, up );
	(void)fwd;

	const float *org = view.origin;
	float fogDensity = view.ambience.fogDensity;

	float *v = s_vertScratch;
	int quads = 0;

	if( mode == 1 )		// ---- RAIN: depth-layered vertical streaks ----
	{
		// Cool desaturated rain color (grey-blue).
		const float rainR = 0.62f, rainG = 0.70f, rainB = 0.82f;

		for( int i = 0; i < m_activeCount; i++ )
		{
			const Particle &p = m_pool[i];

			// Distance to camera (for fog darkening + size/alpha falloff).
			float dx = p.pos[0] - org[0];
			float dy = p.pos[1] - org[1];
			float dz = p.pos[2] - org[2];
			float dist = sqrtf( dx * dx + dy * dy + dz * dz );

			// Depth layer: near (layer~1) = larger/brighter/longer streaks; far
			// (layer~0) = thinner/dimmer. Width/length/alpha all scale by layer.
			float layer = p.layer;
			float halfWidth = 0.9f + layer * 1.9f;		// ~0.9..2.8 units
			float length = 36.0f + layer * 60.0f;		// ~36..96 units
			float baseAlpha = 0.16f + layer * 0.42f;	// near streaks far more readable

			// Fog factor (exp2 falloff with distance): FAR streaks get darkened and
			// further faded so distant rain reads as compressed "rain mist", near
			// rain stays crisp. fogDensity<=0 -> fog=1 (no darkening).
			float fog = ( fogDensity > 0.0f ) ? exp2f( -fogDensity * dist ) : 1.0f;

			fog = Clamp01( fog );

			// Extra distance fade independent of map fog so far rain never clutters.
			float distFade = Clamp01( 1.0f - dist / ( kBoxHalf * 1.25f ) );
			float alpha = baseAlpha * fog * ( 0.35f + 0.65f * distFade );

			if( alpha <= 0.004f )
				continue;	// skip invisible far streaks (cheaper draw)

			// Streak color darkened by fog (far rain = dim rain-mist tone toward
			// the fog color so it sinks into the haze).
			float cr, cg, cb;

			if( fogDensity > 0.0f )
			{
				const float *fc = view.ambience.fogColor;

				cr = rainR * fog + fc[0] * ( 1.0f - fog );
				cg = rainG * fog + fc[1] * ( 1.0f - fog );
				cb = rainB * fog + fc[2] * ( 1.0f - fog );
			}
			else
			{
				cr = rainR; cg = rainG; cb = rainB;
			}

			// Streak axis: along world-up (Z). Half-extents from width/length.
			float ax = 0.0f, ay = 0.0f, az = length * 0.5f;	// along-streak (Z up)
			float rx = right[0] * halfWidth;		// across-streak (camera right)
			float ry = right[1] * halfWidth;
			float rz = right[2] * halfWidth;

			float cx = p.pos[0], cy = p.pos[1], cz = p.pos[2];

			// Four corners: bottom-left, top-left, top-right, bottom-right.
			// corner.y: 0 at bottom, 1 at top (vertical gradient in the FS).
			float blx = cx - rx - ax, bly = cy - ry - ay, blz = cz - rz - az;
			float tlx = cx - rx + ax, tly = cy - ry + ay, tlz = cz - rz + az;
			float trx = cx + rx + ax, try_ = cy + ry + ay, trz = cz + rz + az;
			float brx = cx + rx - ax, bry = cy + ry - ay, brz = cz + rz - az;

			// Two triangles (BL,TL,TR) + (BL,TR,BR). 6 verts, pos+corner+color.
			#define CSZ_PUSH_RAIN( px, py, pz, ux, uy ) \
				v[0]=(px); v[1]=(py); v[2]=(pz); v[3]=(ux); v[4]=(uy); \
				v[5]=cr; v[6]=cg; v[7]=cb; v[8]=alpha; v += kVertFloats;

			CSZ_PUSH_RAIN( blx, bly, blz, 0.0f, 0.0f )
			CSZ_PUSH_RAIN( tlx, tly, tlz, 0.0f, 1.0f )
			CSZ_PUSH_RAIN( trx, try_, trz, 1.0f, 1.0f )
			CSZ_PUSH_RAIN( blx, bly, blz, 0.0f, 0.0f )
			CSZ_PUSH_RAIN( trx, try_, trz, 1.0f, 1.0f )
			CSZ_PUSH_RAIN( brx, bry, brz, 1.0f, 0.0f )
			#undef CSZ_PUSH_RAIN

			quads++;
		}
	}
	else			// ---- SNOW: camera-facing soft flakes ----
	{
		// Cool-white tinted by night so snow is not overexposed at night. Derived
		// from the surface snow color (already tint/dim adjusted in Update()).
		const float *sc = m_surf.snowColor;
		float snowR = sc[0], snowG = sc[1], snowB = sc[2];

		for( int i = 0; i < m_activeCount; i++ )
		{
			const Particle &p = m_pool[i];

			float dx = p.pos[0] - org[0];
			float dy = p.pos[1] - org[1];
			float dz = p.pos[2] - org[2];
			float dist = sqrtf( dx * dx + dy * dy + dz * dz );

			float size = 1.6f + p.seed * 2.2f;		// small flakes, varied
			float fog = ( fogDensity > 0.0f ) ? Clamp01( exp2f( -fogDensity * dist ) ) : 1.0f;
			float distFade = Clamp01( 1.0f - dist / ( kBoxHalf * 1.25f ) );
			float alpha = ( 0.55f + 0.35f * p.seed ) * fog * ( 0.4f + 0.6f * distFade );

			if( alpha <= 0.004f )
				continue;

			// Camera-facing billboard: corners = center +/- right*size +/- up*size.
			float rxv[3] = { right[0] * size, right[1] * size, right[2] * size };
			float uxv[3] = { up[0] * size,    up[1] * size,    up[2] * size };
			float cx = p.pos[0], cy = p.pos[1], cz = p.pos[2];

			float blx = cx - rxv[0] - uxv[0], bly = cy - rxv[1] - uxv[1], blz = cz - rxv[2] - uxv[2];
			float tlx = cx - rxv[0] + uxv[0], tly = cy - rxv[1] + uxv[1], tlz = cz - rxv[2] + uxv[2];
			float trx = cx + rxv[0] + uxv[0], try_ = cy + rxv[1] + uxv[1], trz = cz + rxv[2] + uxv[2];
			float brx = cx + rxv[0] - uxv[0], bry = cy + rxv[1] - uxv[1], brz = cz + rxv[2] - uxv[2];

			#define CSZ_PUSH_SNOW( px, py, pz, ux, uy ) \
				v[0]=(px); v[1]=(py); v[2]=(pz); v[3]=(ux); v[4]=(uy); \
				v[5]=snowR; v[6]=snowG; v[7]=snowB; v[8]=alpha; v += kVertFloats;

			CSZ_PUSH_SNOW( blx, bly, blz, 0.0f, 0.0f )
			CSZ_PUSH_SNOW( tlx, tly, tlz, 0.0f, 1.0f )
			CSZ_PUSH_SNOW( trx, try_, trz, 1.0f, 1.0f )
			CSZ_PUSH_SNOW( blx, bly, blz, 0.0f, 0.0f )
			CSZ_PUSH_SNOW( trx, try_, trz, 1.0f, 1.0f )
			CSZ_PUSH_SNOW( brx, bry, brz, 1.0f, 0.0f )
			#undef CSZ_PUSH_SNOW

			quads++;
		}
	}

	if( quads == 0 )
		return;

	// GL state: transparent, depth-tested (occluded by walls) but no depth write.
	SetDepthTest( true );
	SetDepthWrite( false );
	SetBlend( kBlendAlpha );
	SetCull( false );

	unsigned int prog = ( mode == 1 ) ? m_gpu.rainProgram.program : m_gpu.snowProgram.program;
	unsigned int vao  = ( mode == 1 ) ? m_gpu.rainVao : m_gpu.snowVao;
	unsigned int vbo  = ( mode == 1 ) ? m_gpu.rainVbo : m_gpu.snowVbo;
	int uViewProj     = ( mode == 1 ) ? m_gpu.uRainViewProj : m_gpu.uSnowViewProj;

	UseProgram( prog );
	glUniformMatrix4fv( uViewProj, 1, GL_FALSE, view.matViewProj.m );

	BindVao( vao );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glBufferData( GL_ARRAY_BUFFER,
		(GLsizeiptr)( (size_t)quads * kVertsPerQuad * kVertFloats * sizeof( float )),
		s_vertScratch, GL_DYNAMIC_DRAW );	// orphan + upload this frame's quads
	glBindBuffer( GL_ARRAY_BUFFER, 0 );	// attribute bindings live in the VAO

	glDrawArrays( GL_TRIANGLES, 0, quads * kVertsPerQuad );

	BindVao( 0 );

	// Restore the EnterTakeover baseline (depth write ON, blend NONE) so later
	// passes (viewmodel, post) see the expected state.
	SetDepthWrite( true );
	SetBlend( kBlendNone );
}

void WeatherRenderer::Shutdown()
{
	// Idempotent; safe if GL never inited (names stay 0). Only delete objects we
	// created in the CURRENT generation -- a stale-generation context is gone, so
	// its names were already forgotten in EnsureBuilt (never glDelete those).
	if( m_gpu.gpuGeneration == GpuGeneration() )
	{
		if( m_gpu.rainVbo != 0 ) glDeleteBuffers( 1, &m_gpu.rainVbo );
		if( m_gpu.snowVbo != 0 ) glDeleteBuffers( 1, &m_gpu.snowVbo );
		if( m_gpu.rainVao != 0 ) glDeleteVertexArrays( 1, &m_gpu.rainVao );
		if( m_gpu.snowVao != 0 ) glDeleteVertexArrays( 1, &m_gpu.snowVao );

		DestroyProgram( m_gpu.rainProgram );
		DestroyProgram( m_gpu.snowProgram );
	}

	m_gpu.rainVbo = m_gpu.snowVbo = 0;
	m_gpu.rainVao = m_gpu.snowVao = 0;
	m_gpu.rainProgram.program = 0;
	m_gpu.snowProgram.program = 0;
	m_gpu.built = false;
}

}
