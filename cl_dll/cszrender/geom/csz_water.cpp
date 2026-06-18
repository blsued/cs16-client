/*
 * csz_water.cpp -- CSOZ renderer: animated turb/water surface pass
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
#include "csz_water.h"
#include "../core/csz_engine.h"
#include "../core/csz_engine_bsp.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_glcaps.h"
#include "../core/csz_shader.h"
#include "../core/csz_log.h"
#include "../core/csz_fatal.h"

#include <new>
#include <string.h>

#include "csz_water_shaders.inl"	// shader source strings (single translation unit)

namespace csz
{

WaterRenderer g_water;	// zero-initialized (static storage duration)

namespace
{

// Interleaved vertex: pos(3) + uv(2) + worldNormal(3) = 8 floats / 32-byte stride.
const int kWaterVertexFloats = 8;
const int kWaterVertexStride = kWaterVertexFloats * (int)sizeof( float );

// surfedges sign selects edge direction; QBSP2 maps widen the edge union to
// 32-bit. Clean-room reimplementation of the same contract as the world build
// (csz_world.cpp FetchEdgeVertex): positive surfedge -> edge.v[0], negative ->
// edge.v[1], indexing the 16- or 32-bit edge array per kModelQbsp2.
int WaterEdgeVertex( const EngModel *bsp, int surfEdge )
{
	if(( bsp->flags & kModelQbsp2 ) != 0 )
	{
		if( surfEdge >= 0 )
			return (int)bsp->edges32[surfEdge].v[0];
		return (int)bsp->edges32[-surfEdge].v[1];
	}

	if( surfEdge >= 0 )
		return (int)bsp->edges16[surfEdge].v[0];
	return (int)bsp->edges16[-surfEdge].v[1];
}

}	// anonymous namespace

void WaterRenderer::EnsureBuilt( model_s *world )
{
	if( world == NULL )
		return;

	// Cheap when already current: same map + same GPU generation -> nothing to do.
	if( m_built && m_model == world && m_gpuGeneration == GpuGeneration() )
		return;

	// Map changed or context lost: drop any prior build before rebuilding.
	Shutdown();

	const EngModel *bsp = EngBsp( world );

	m_model = world;
	m_gpuGeneration = GpuGeneration();

	// First pass: count turb surfaces and their total vertices so the VBO and
	// face table are each sized once.
	int totalSurfaces = bsp->numsurfaces;
	int turbFaces = 0;
	int totalVerts = 0;

	for( int g = 0; g < totalSurfaces; g++ )
	{
		const EngSurface &surf = bsp->surfaces[g];

		if(( surf.flags & kSurfDrawTurb ) == 0 )
			continue;

		turbFaces++;
		totalVerts += surf.numedges;
	}

	// Build the program regardless (so DrawWater can early-return cleanly and a
	// later map with water reuses it). Init-time shader: a compile failure is
	// FATAL, matching world/sky.
	BuildProgram( "csz_water", kWaterVs, kWaterFs, true, m_program );
	m_uViewProj  = UniformLoc( m_program, "u_viewProj" );
	m_uTime      = UniformLoc( m_program, "u_time" );
	m_uCamPos    = UniformLoc( m_program, "u_camPos" );
	m_uFog       = UniformLoc( m_program, "u_fog" );
	m_uAmbTint   = UniformLoc( m_program, "u_ambTint" );
	m_uMoonDir   = UniformLoc( m_program, "u_moonDir" );
	m_uMoonColor = UniformLoc( m_program, "u_moonColor" );
	m_uRain      = UniformLoc( m_program, "u_rain" );
	m_uPhase     = UniformLoc( m_program, "u_phase" );

	UseProgram( m_program.program );
	glUniform1i( UniformLoc( m_program, "u_tex" ), 0 );	// diffuse on TMU 0
	UseProgram( 0 );

	// No water on this map: empty set, DrawWater early-returns. Still "built".
	if( turbFaces == 0 )
	{
		m_faces = NULL;
		m_numFaces = 0;
		m_numVerts = 0;
		m_built = true;
		CSZ_LogDev( "water", "built %s: no turb surfaces", bsp->name );
		return;
	}

	float *verts = new( std::nothrow ) float[(size_t)totalVerts * kWaterVertexFloats];
	m_faces = new( std::nothrow ) WaterFace[turbFaces];

	if( verts == NULL || m_faces == NULL )
		CSZ_FatalInit( "water", "out of memory building water surface buffers" );

	int vertCursor = 0;
	int faceCursor = 0;

	for( int g = 0; g < totalSurfaces; g++ )
	{
		const EngSurface &surf = bsp->surfaces[g];

		if(( surf.flags & kSurfDrawTurb ) == 0 )
			continue;

		const EngTexinfo *ti = surf.texinfo;
		const EngTexture *tex = ti->texture;
		const mplane_t *plane = surf.plane;

		// Outward world normal: flip when the face is on the plane's back side.
		bool planeBack = ( surf.flags & kSurfPlaneBack ) != 0;
		float nx = planeBack ? -plane->normal[0] : plane->normal[0];
		float ny = planeBack ? -plane->normal[1] : plane->normal[1];
		float nz = planeBack ? -plane->normal[2] : plane->normal[2];

		WaterFace &f = m_faces[faceCursor];
		f.firstVert = vertCursor;
		f.vertCount = surf.numedges;
		f.texSlot = ( tex != NULL ) ? tex->gl_texturenum : 0;

		for( int e = 0; e < surf.numedges; e++ )
		{
			int vi = WaterEdgeVertex( bsp, bsp->surfedges[surf.firstedge + e] );
			const float *pos = bsp->vertexes[vi].position;
			float *out = &verts[(size_t)( vertCursor + e ) * kWaterVertexFloats];

			out[0] = pos[0];
			out[1] = pos[1];
			out[2] = pos[2];

			float su = pos[0] * ti->vecs[0][0] + pos[1] * ti->vecs[0][1] +
				pos[2] * ti->vecs[0][2] + ti->vecs[0][3];
			float tv = pos[0] * ti->vecs[1][0] + pos[1] * ti->vecs[1][1] +
				pos[2] * ti->vecs[1][2] + ti->vecs[1][3];

			out[3] = ( tex != NULL ) ? su / (float)tex->width : 0.0f;
			out[4] = ( tex != NULL ) ? tv / (float)tex->height : 0.0f;

			out[5] = nx;
			out[6] = ny;
			out[7] = nz;
		}

		vertCursor += surf.numedges;
		faceCursor++;
	}

	m_numFaces = faceCursor;
	m_numVerts = vertCursor;

	// Upload the static VBO. Build runs outside the takeover window, so leave
	// VAO/VBO unbound for the engine afterwards (matches the world build).
	glGenVertexArrays( 1, &m_vao );
	BindVao( m_vao );
	glGenBuffers( 1, &m_vbo );
	glBindBuffer( GL_ARRAY_BUFFER, m_vbo );
	glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr)( (size_t)totalVerts * kWaterVertexStride ),
		verts, GL_STATIC_DRAW );

	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, kWaterVertexStride, (const void *)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, kWaterVertexStride, (const void *)( 3 * sizeof( float )));
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 3, GL_FLOAT, GL_FALSE, kWaterVertexStride, (const void *)( 5 * sizeof( float )));

	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	delete[] verts;

	m_built = true;
	CSZ_LogInfo( "water", "built %s: %d turb surfaces, %d verts", bsp->name, m_numFaces, m_numVerts );
}

void WaterRenderer::DrawWater( const ViewSetup &view, float rainIntensity, float skyPhase )
{
	if( !m_built || m_numFaces == 0 || m_program.program == 0 || m_vao == 0 )
		return;

	// Water is solid (opaque): depth test + write ON, no blend, cull OFF so a
	// face reads regardless of which side the camera sees. It depth-tests
	// against the already-drawn world, so it never punches holes.
	SetDepthTest( true );
	SetDepthWrite( true );
	SetBlend( kBlendNone );
	SetCull( false );

	UseProgram( m_program.program );
	BindVao( m_vao );

	const AmbienceParams &amb = view.ambience;
	const float fogVec[4] = { amb.fogColor[0], amb.fogColor[1], amb.fogColor[2], amb.fogDensity };

	glUniformMatrix4fv( m_uViewProj, 1, GL_FALSE, view.matViewProj.m );
	glUniform1f( m_uTime, ClientTime() );
	glUniform3fv( m_uCamPos, 1, view.origin );
	glUniform4fv( m_uFog, 1, fogVec );
	glUniform3fv( m_uAmbTint, 1, amb.tint );
	glUniform3fv( m_uMoonDir, 1, amb.moonlightDir );
	glUniform3fv( m_uMoonColor, 1, amb.moonlightColor );
	glUniform1f( m_uRain, rainIntensity );
	glUniform1f( m_uPhase, skyPhase );

	int curTex = -1;

	for( int i = 0; i < m_numFaces; i++ )
	{
		const WaterFace &f = m_faces[i];

		if( f.texSlot != curTex )
		{
			BindTextureSlot( 0, f.texSlot );
			curTex = f.texSlot;
		}

		glDrawArrays( GL_TRIANGLE_FAN, f.firstVert, f.vertCount );
	}

	// Restore the EnterTakeover baseline for the passes that follow: depth test
	// + write on, blend none, cull off (it was already off here).
	BindVao( 0 );
	UseProgram( 0 );
	SetDepthTest( true );
	SetDepthWrite( true );
	SetBlend( kBlendNone );
}

void WaterRenderer::Shutdown()
{
	// Generation rule (matches world/shadow): GL names from an older GPU
	// generation must be forgotten, never glDelete'd -- the context that owned
	// them is gone and the name may now belong to a foreign object.
	bool sameContext = ( m_gpuGeneration == GpuGeneration() );

	if( m_vao != 0 || m_vbo != 0 )
	{
		if( sameContext )
		{
			if( m_vao != 0 )
				glDeleteVertexArrays( 1, &m_vao );
			if( m_vbo != 0 )
				glDeleteBuffers( 1, &m_vbo );
		}

		m_vao = 0;
		m_vbo = 0;
	}

	if( m_program.program != 0 )
	{
		if( sameContext )
			DestroyProgram( m_program );
		else
			m_program.program = 0;
	}

	delete[] m_faces;
	m_faces = NULL;
	m_numFaces = 0;
	m_numVerts = 0;
	m_model = NULL;
	m_built = false;
}

}
