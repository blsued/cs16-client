/*
 * csz_polyblend.cpp -- CSOZ renderer: fullscreen screen-tint / polyblend pass
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
#include "csz_polyblend.h"
#include "../core/csz_engine.h"
#include "../core/csz_glcaps.h"	// GpuGeneration()
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"
#include "../core/csz_math.h"
#include "../core/csz_shader.h"
#include "../core/csz_view.h"

// geom stays CORE-ONLY: the local-player health for the damage shift arrives via
// ViewSetup.localHealth, populated at the composition root (csz_renderer.cpp) from
// gHUD.m_Health.m_iHealth -- no HUD / cl_entity include here.

namespace csz
{

#include "csz_polyblend_shaders.inl"

namespace
{

cvar_t *s_cvPolyblend;	// csz_polyblend (default 1)

struct PolyblendState
{
	bool shaderReady;
	int gpuGeneration;
	ShaderProgram program;
	int uBlend;
	unsigned int vao;
};

// Damage color-shift tracker (reconstructs cl.cshifts[CSHIFT_DAMAGE], which the
// client DLL can't read directly under takeover): kick the red shift on a local-
// player health drop, then decay it like the engine.
struct DamageState
{
	bool valid;		// a plausible health sample has been seen
	int lastHealth;
	float percent;		// 0..150, the cshift percent
	float lastTime;
};

PolyblendState s_pb;
DamageState s_dmg;

// One engine color-shift: destination color (0..255) + coverage percent (0..255).
struct CShift
{
	float dest[3];
	float percent;
};

void EnsureGpuObjects()
{
	if( s_pb.shaderReady && s_pb.gpuGeneration == GpuGeneration())
		return;

	if( s_pb.gpuGeneration != GpuGeneration())
	{
		// Stale generation: forget names, never delete (T1 rule).
		s_pb.program.program = 0;
		s_pb.vao = 0;
	}

	// Init-time shader: compile failure is FATAL (spec 3.2 parity with sky/sprite).
	BuildProgram( "csz_polyblend", kPolyblendVs, kPolyblendFs, true, s_pb.program );
	s_pb.uBlend = UniformLoc( s_pb.program, "u_blend" );

	// Attributeless fullscreen triangle: a VAO must still be bound in core profile.
	glGenVertexArrays( 1, &s_pb.vao );

	s_pb.gpuGeneration = GpuGeneration();
	s_pb.shaderReady = true;
}

// Content color-shift from the contents at the eye (GoldSrc CSHIFT_CONTENTS
// colors): underwater blue-brown, lava orange, slime green. Everything else is
// transparent.
CShift ContentShift( const ViewSetup &view )
{
	CShift c = {{ 0.0f, 0.0f, 0.0f }, 0.0f };

	int contents = ( gEngfuncs.PM_PointContents != NULL )
		? gEngfuncs.PM_PointContents( (float *)view.origin, NULL ) : CONTENTS_EMPTY;

	switch( contents )
	{
	case CONTENTS_LAVA:
		c.dest[0] = 255.0f; c.dest[1] = 80.0f; c.dest[2] = 0.0f; c.percent = 150.0f;
		break;
	case CONTENTS_SLIME:
		c.dest[0] = 0.0f; c.dest[1] = 25.0f; c.dest[2] = 5.0f; c.percent = 150.0f;
		break;
	case CONTENTS_WATER:
		c.dest[0] = 130.0f; c.dest[1] = 80.0f; c.dest[2] = 50.0f; c.percent = 128.0f;
		break;
	default:
		break;
	}

	return c;
}

// Damage red flash (CSHIFT_DAMAGE). Reads the local player's health each frame from
// ViewSetup.localHealth (the reliable HUD health threaded in by the composition root --
// curstate.health is NOT server-populated for the local player, so the old read was a
// silent no-op). On a drop, kicks the red shift (proxying the engine's blood/armor count
// by the health delta) and decays it at the GoldSrc rate (percent -= frametime*150).
// If health is unreadable (stays 0), this is a silent no-op -- the content shift still
// works. Returns {255,0,0, percent}.
CShift DamageShift( const ViewSetup &view )
{
	float now = ClientTime();
	float dt = ( s_dmg.lastTime > 0.0f && now > s_dmg.lastTime ) ? ( now - s_dmg.lastTime ) : 0.0f;
	s_dmg.lastTime = now;

	// Decay first (engine V_UpdatePalette / V_CalcBlend fade).
	s_dmg.percent -= dt * 150.0f;
	if( s_dmg.percent < 0.0f )
		s_dmg.percent = 0.0f;

	int hp = view.localHealth;

	if( hp > 0 && hp <= 255 )
	{
		if( s_dmg.valid )
		{
			int delta = s_dmg.lastHealth - hp;

			// Damage only (delta>0); ignore respawn/heal jumps and implausibly
			// large drops (map change / first valid frame noise).
			if( delta > 0 && delta < 200 )
			{
				s_dmg.percent += 3.0f * (float)delta + 15.0f;	// base kick + per-point
				if( s_dmg.percent > 150.0f )
					s_dmg.percent = 150.0f;
			}
		}

		s_dmg.lastHealth = hp;
		s_dmg.valid = true;
	}

	CShift d = {{ 255.0f, 0.0f, 0.0f }, s_dmg.percent };
	return d;
}

// GoldSrc V_CalcBlend: composite the cshifts into one premultiplied-over rgba.
// out[0..2] in 0..1 (display space), out[3] = combined coverage 0..1. Returns
// the count of contributing shifts (observability).
int CalcBlend( const CShift *shifts, int count, float out[4] )
{
	float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
	int active = 0;

	for( int j = 0; j < count; j++ )
	{
		float a2 = shifts[j].percent / 255.0f;

		if( a2 <= 0.0f )
			continue;

		active++;
		a = a + a2 * ( 1.0f - a );

		if( a <= 0.0f )
			continue;

		a2 = a2 / a;
		r = r * ( 1.0f - a2 ) + shifts[j].dest[0] * a2;
		g = g * ( 1.0f - a2 ) + shifts[j].dest[1] * a2;
		b = b * ( 1.0f - a2 ) + shifts[j].dest[2] * a2;
	}

	out[0] = r / 255.0f;
	out[1] = g / 255.0f;
	out[2] = b / 255.0f;
	out[3] = clampf( a, 0.0f, 1.0f );
	return active;
}

}

void DrawPolyblend( const ViewSetup &view )
{
	if( s_cvPolyblend != NULL && s_cvPolyblend->value == 0.0f )
		return;

	CShift shifts[2] = { ContentShift( view ), DamageShift( view ) };
	float blend[4];
	int active = CalcBlend( shifts, 2, blend );

	// Throttled observability (>= 1s; logging rule R8).
	{
		static float s_nextLog;
		float now = ClientTime();

		if( now >= s_nextLog )
		{
			s_nextLog = now + 1.0f;
			CSZ_LogDev( "tint", "[CSZ:tint] active=%d rgba=%.2f,%.2f,%.2f,%.2f",
				active, blend[0], blend[1], blend[2], blend[3] );
		}
	}

	// Sub-perceptual coverage: nothing to draw (the common case).
	if( blend[3] < ( 0.5f / 255.0f ))
		return;

	EnsureGpuObjects();

	UseProgram( s_pb.program.program );
	glUniform4fv( s_pb.uBlend, 1, blend );

	// Over-blend on the resolved backbuffer; no depth interaction (it is a 2D
	// screen tint, exactly like GoldSrc R_PolyBlend).
	SetBlend( kBlendAlpha );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetCull( false );

	BindVao( s_pb.vao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	BindVao( 0 );

	// Hand a clean baseline back to LeaveTakeover.
	SetBlend( kBlendNone );
	SetDepthTest( true );
	SetDepthWrite( true );
	UseProgram( 0 );
}

void RegisterPolyblendCvars()
{
	s_cvPolyblend = gEngfuncs.pfnRegisterVariable( "csz_polyblend", "1", FCVAR_CLIENTDLL );
}

}
