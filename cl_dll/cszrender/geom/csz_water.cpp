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

// Interleaved vertex: pos(3) + uv(2) + worldNormal(3) + faceCenter(2) = 10 floats
// / 40-byte stride. faceCenter is the source surface's world-XY centroid, baked
// per-vertex so the rain ring term stays concentric per face after all faces are
// merged into one batched triangle-list draw (no per-face uniform / no per-face
// draw call).
const int kWaterVertexFloats = 10;
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

	// First pass: count turb surfaces and their total TRIANGLE-LIST vertices so
	// the VBO is sized once. Each N-gon fan triangulates into (N-2) triangles =
	// 3*(N-2) vertices. We pre-triangulate and batch by texture so the whole
	// water surface renders in one (or a few) glDrawArrays instead of one fan
	// per face -- de_aztec had 324 turb faces => 324 draw calls => CPU-bound.
	int totalSurfaces = bsp->numsurfaces;
	int turbFaces = 0;
	int totalVerts = 0;

	for( int g = 0; g < totalSurfaces; g++ )
	{
		const EngSurface &surf = bsp->surfaces[g];

		if(( surf.flags & kSurfDrawTurb ) == 0 )
			continue;

		if( surf.numedges < 3 )
			continue;	// degenerate; cannot triangulate

		// Face scratch in the build loop caps at 64 verts; mirror that here so the
		// VBO is sized to exactly what the build emits.
		int nE = ( surf.numedges <= 64 ) ? surf.numedges : 64;
		turbFaces++;
		totalVerts += 3 * ( nE - 2 );
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

	// Stash the pre-batch per-face count (old path = one fan draw per turb face)
	// so the build/runtime logs can report the genuine before/after baseline.
	m_turbFaces = turbFaces;

	// No water on this map: empty set, DrawWater early-returns. Still "built".
	if( turbFaces == 0 )
	{
		m_batches = NULL;
		m_numBatches = 0;
		m_numVerts = 0;
		m_lastDrawBatches = 0;
		m_faceBounds = NULL;
		m_numFaceBounds = 0;
		m_visibleFacesLastFrame = 0;	// no water -> nothing can be visible
		m_hasWater = false;
		m_waterCenter[0] = m_waterCenter[1] = m_waterCenter[2] = 0.0f;
		m_waterMins[0] = m_waterMins[1] = m_waterMins[2] = 0.0f;
		m_waterMaxs[0] = m_waterMaxs[1] = m_waterMaxs[2] = 0.0f;
		m_built = true;
		CSZ_LogDev( "water", "built %s: no turb surfaces", bsp->name );
		return;
	}

	float *verts = new( std::nothrow ) float[(size_t)totalVerts * kWaterVertexFloats];

	// Index of every turb surface, sorted by texture slot so faces sharing a
	// slot land contiguously in the VBO -> one glDrawArrays per distinct slot.
	int *faceIdx = new( std::nothrow ) int[turbFaces];

	if( verts == NULL || faceIdx == NULL )
		CSZ_FatalInit( "water", "out of memory building water surface buffers" );

	// Collect turb surface indices.
	int collected = 0;
	for( int g = 0; g < totalSurfaces; g++ )
	{
		const EngSurface &surf = bsp->surfaces[g];
		if(( surf.flags & kSurfDrawTurb ) == 0 || surf.numedges < 3 )
			continue;
		faceIdx[collected++] = g;
	}

	// Insertion sort by texSlot (turbFaces is small; avoids pulling in <algorithm>
	// / a comparator, and the build runs once per map).
	for( int a = 1; a < collected; a++ )
	{
		int key = faceIdx[a];
		const EngTexture *kt = bsp->surfaces[key].texinfo->texture;
		int kslot = ( kt != NULL ) ? kt->gl_texturenum : 0;
		int b = a - 1;
		while( b >= 0 )
		{
			const EngTexture *bt = bsp->surfaces[faceIdx[b]].texinfo->texture;
			int bslot = ( bt != NULL ) ? bt->gl_texturenum : 0;
			if( bslot <= kslot )
				break;
			faceIdx[b + 1] = faceIdx[b];
			b--;
		}
		faceIdx[b + 1] = key;
	}

	// Worst case every face has a distinct slot, so size the batch table to
	// turbFaces; m_numBatches records the actual (usually 1) count.
	m_batches = new( std::nothrow ) WaterBatch[turbFaces];
	if( m_batches == NULL )
		CSZ_FatalInit( "water", "out of memory building water surface buffers" );

	// Per-face world-space AABB (one per turb face, in collected order) for the
	// per-frame frustum count in DrawWater. Sized to turbFaces.
	m_faceBounds = new( std::nothrow ) FaceBounds[turbFaces];
	if( m_faceBounds == NULL )
		CSZ_FatalInit( "water", "out of memory building water surface buffers" );
	m_numFaceBounds = 0;

	int vertCursor = 0;
	int numBatches = 0;
	int curSlot = -1;

	for( int s = 0; s < collected; s++ )
	{
		const EngSurface &surf = bsp->surfaces[faceIdx[s]];

		const EngTexinfo *ti = surf.texinfo;
		const EngTexture *tex = ti->texture;
		const mplane_t *plane = surf.plane;
		int slot = ( tex != NULL ) ? tex->gl_texturenum : 0;

		// Open a new batch whenever the (sorted) texture slot changes.
		if( numBatches == 0 || slot != curSlot )
		{
			WaterBatch &nb = m_batches[numBatches];
			nb.firstVert = vertCursor;
			nb.vertCount = 0;
			nb.texSlot = slot;
			numBatches++;
			curSlot = slot;
		}

		// Outward world normal: flip when the face is on the plane's back side.
		bool planeBack = ( surf.flags & kSurfPlaneBack ) != 0;
		float nx = planeBack ? -plane->normal[0] : plane->normal[0];
		float ny = planeBack ? -plane->normal[1] : plane->normal[1];
		float nz = planeBack ? -plane->normal[2] : plane->normal[2];

		// First gather the face's polygon vertices (pos + uv), then fan-triangulate
		// into the VBO. Compute the world-XY centroid for the rain ring center.
		const int nE = surf.numedges;
		// Stack scratch for one face. Turb faces are small polygons; cap defensively.
		float fx[64], fy[64], fz[64], fu[64], fv[64];
		const int maxE = ( nE <= 64 ) ? nE : 64;
		float ccx = 0.0f, ccy = 0.0f;

		// Accumulate this face's world-space AABB while gathering its vertices.
		float fbMin[3] = {  1e30f,  1e30f,  1e30f };
		float fbMax[3] = { -1e30f, -1e30f, -1e30f };

		for( int e = 0; e < maxE; e++ )
		{
			int vi = WaterEdgeVertex( bsp, bsp->surfedges[surf.firstedge + e] );
			const float *pos = bsp->vertexes[vi].position;
			fx[e] = pos[0];
			fy[e] = pos[1];
			fz[e] = pos[2];
			for( int c = 0; c < 3; c++ )
			{
				if( pos[c] < fbMin[c] ) fbMin[c] = pos[c];
				if( pos[c] > fbMax[c] ) fbMax[c] = pos[c];
			}
			float su = pos[0] * ti->vecs[0][0] + pos[1] * ti->vecs[0][1] +
				pos[2] * ti->vecs[0][2] + ti->vecs[0][3];
			float tv = pos[0] * ti->vecs[1][0] + pos[1] * ti->vecs[1][1] +
				pos[2] * ti->vecs[1][2] + ti->vecs[1][3];
			fu[e] = ( tex != NULL ) ? su / (float)tex->width : 0.0f;
			fv[e] = ( tex != NULL ) ? tv / (float)tex->height : 0.0f;
			ccx += pos[0];
			ccy += pos[1];
		}

		ccx /= (float)maxE;
		ccy /= (float)maxE;

		// Record this face's AABB (collected order) for per-frame frustum counting.
		FaceBounds &fb = m_faceBounds[m_numFaceBounds++];
		fb.mins[0] = fbMin[0]; fb.mins[1] = fbMin[1]; fb.mins[2] = fbMin[2];
		fb.maxs[0] = fbMax[0]; fb.maxs[1] = fbMax[1]; fb.maxs[2] = fbMax[2];

		// Fan triangulation: (0, e, e+1) for e in 1..maxE-2 -> triangle list.
		// Preserves the same winding as the original GL_TRIANGLE_FAN.
		for( int e = 1; e <= maxE - 2; e++ )
		{
			const int idx[3] = { 0, e, e + 1 };
			for( int k = 0; k < 3; k++ )
			{
				int j = idx[k];
				float *out = &verts[(size_t)vertCursor * kWaterVertexFloats];
				out[0] = fx[j]; out[1] = fy[j]; out[2] = fz[j];
				out[3] = fu[j]; out[4] = fv[j];
				out[5] = nx;    out[6] = ny;    out[7] = nz;
				out[8] = ccx;   out[9] = ccy;	// per-face ring center (world XY)
				vertCursor++;
			}
		}

		m_batches[numBatches - 1].vertCount += 3 * ( maxE - 2 );
	}

	delete[] faceIdx;

	m_numBatches = numBatches;
	m_numVerts = vertCursor;

	// One-time diagnostic: centroid + AABB of every built turb vertex, so the
	// water channel can be framed for A/B capture (aim setpos/setang at the
	// centroid). Info level + the same "water" channel as the build line; cheap
	// (single pass over the staging buffer) and only runs on (re)build.
	{
		float mn[3] = {  1e30f,  1e30f,  1e30f };
		float mx[3] = { -1e30f, -1e30f, -1e30f };
		double sum[3] = { 0.0, 0.0, 0.0 };

		for( int v = 0; v < m_numVerts; v++ )
		{
			const float *pp = &verts[(size_t)v * kWaterVertexFloats];

			for( int c = 0; c < 3; c++ )
			{
				if( pp[c] < mn[c] ) mn[c] = pp[c];
				if( pp[c] > mx[c] ) mx[c] = pp[c];
				sum[c] += pp[c];
			}
		}

		float cx = ( m_numVerts > 0 ) ? (float)( sum[0] / m_numVerts ) : 0.0f;
		float cy = ( m_numVerts > 0 ) ? (float)( sum[1] / m_numVerts ) : 0.0f;
		float cz = ( m_numVerts > 0 ) ? (float)( sum[2] / m_numVerts ) : 0.0f;

		// Publish the water body bounds for capture auto-framing (csz_debugcam 2).
		m_hasWater = ( m_numVerts > 0 );
		m_waterCenter[0] = cx; m_waterCenter[1] = cy; m_waterCenter[2] = cz;
		m_waterMins[0] = mn[0]; m_waterMins[1] = mn[1]; m_waterMins[2] = mn[2];
		m_waterMaxs[0] = mx[0]; m_waterMaxs[1] = mx[1]; m_waterMaxs[2] = mx[2];

		CSZ_LogInfo( "water", "turb centroid=(%.0f %.0f %.0f) min=(%.0f %.0f %.0f) max=(%.0f %.0f %.0f)",
			cx, cy, cz, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2] );
	}

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
	glEnableVertexAttribArray( 3 );
	glVertexAttribPointer( 3, 2, GL_FLOAT, GL_FALSE, kWaterVertexStride, (const void *)( 8 * sizeof( float )));

	BindVao( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	delete[] verts;

	m_built = true;
	m_lastDrawBatches = 0;	// nothing drawn yet this map
	m_visibleFacesLastFrame = -1;	// not frustum-tested yet this map
	CSZ_LogInfo( "water", "built %s: %d turb faces (pre-batch per-face draws=%d) -> turb_verts=%d draw_batches=%d",
		bsp->name, turbFaces, turbFaces, m_numVerts, m_numBatches );
}

void WaterRenderer::DrawWater( const ViewSetup &view, float rainIntensity, float skyPhase )
{
	if( !m_built || m_numBatches == 0 || m_program.program == 0 || m_vao == 0 )
	{
		m_lastDrawBatches = 0;	// nothing issued this frame (no water / not built)
		return;
	}

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

	// On-screen water evidence: count per-face AABBs that survive the view
	// frustum. The build-time centroid line proves water EXISTS; this proves it
	// is in the shot. CullBox returns true when fully outside, so a face is
	// "visible" when CullBox is false. Cheap: a handful of plane dots per face.
	int visibleFaces = 0;
	for( int f = 0; f < m_numFaceBounds; f++ )
	{
		if( !view.frustum.CullBox( m_faceBounds[f].mins, m_faceBounds[f].maxs ))
			visibleFaces++;
	}
	m_visibleFacesLastFrame = visibleFaces;

	// Log once per view-origin change (matches the debugcam log throttle): the
	// capture rig moves the camera once, so this fires for the framed shot and
	// then stays quiet. Never per-frame spam.
	static float s_lastLogPos[3] = { 1e30f, 1e30f, 1e30f };
	if( s_lastLogPos[0] != view.origin[0] || s_lastLogPos[1] != view.origin[1]
		|| s_lastLogPos[2] != view.origin[2] )
	{
		s_lastLogPos[0] = view.origin[0];
		s_lastLogPos[1] = view.origin[1];
		s_lastLogPos[2] = view.origin[2];
		CSZ_LogInfo( "water", "visible_water_faces=%d of %d (frustum-tested)",
			visibleFaces, m_numFaceBounds );
	}

	// One glDrawArrays per distinct texture slot. The whole turb surface was
	// pre-triangulated and grouped by texture at build time, so a single-texture
	// liquid map (de_aztec) collapses 324 per-face fan draws into ONE draw call.
	for( int i = 0; i < m_numBatches; i++ )
	{
		const WaterBatch &b = m_batches[i];
		BindTextureSlot( 0, b.texSlot );
		glDrawArrays( GL_TRIANGLES, b.firstVert, b.vertCount );
	}

	m_lastDrawBatches = m_numBatches;	// runtime A/B proof: batches actually issued

	// Restore the EnterTakeover baseline for the passes that follow: depth test
	// + write on, blend none, cull off (it was already off here). TMU0 was just
	// bound to a water texture above; LeaveTakeover restores the engine's binding
	// at end-of-frame, but unbind here too so a CSZ pass that follows in the same
	// takeover window never inherits a stale water texture on slot 0.
	BindTextureSlot( 0, 0 );
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

	delete[] m_batches;
	m_batches = NULL;
	m_numBatches = 0;
	m_numVerts = 0;

	delete[] m_faceBounds;
	m_faceBounds = NULL;
	m_numFaceBounds = 0;
	m_visibleFacesLastFrame = -1;
	m_hasWater = false;

	m_model = NULL;
	m_built = false;
}

void WaterRenderer::GetWaterBounds( float center[3], float mins[3], float maxs[3], bool &hasWater ) const
{
	hasWater = m_hasWater;
	for( int c = 0; c < 3; c++ )
	{
		center[c] = m_waterCenter[c];
		mins[c] = m_waterMins[c];
		maxs[c] = m_waterMaxs[c];
	}
}

}
