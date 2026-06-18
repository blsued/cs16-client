/*
 * csz_water.h -- CSOZ renderer: animated turb/water surface pass
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
#pragma once
#include "../core/csz_view.h"
#include "../core/csz_shader.h"
typedef struct model_s model_t;
namespace csz
{
// Animated turb/water surface pass (default tier). World opaque skips turb
// faces; this owns the dedicated water VBO + shader and draws them between
// studio opaque and the light passes. rainIntensity drives ripple strength so
// rain visibly disturbs the surface (fed from g_weather.RainIntensity()).
class WaterRenderer
{
public:
	void EnsureBuilt( model_s *world );  // build turb-surface VBO (lazy, keyed on GPU gen)
	void DrawWater( const ViewSetup &view, float rainIntensity, float skyPhase );  // water pass
	void Shutdown();                     // destroy GL objects
	void MarkNotDrawn() { m_lastDrawBatches = 0; }  // csz_water 0 path: water pass skipped this frame

	// Draw metrics (kept as SEPARATE fields, never conflated):
	int TurbVerts() const { return m_numVerts; }              // triangle-list vertex count in the VBO
	int DrawBatches() const { return m_numBatches; }          // batched draw count (== glDrawArrays calls)
	int TurbFaces() const { return m_turbFaces; }             // pre-batch per-face draw count (old fan-per-face path)
	int DrawBatchesLastFrame() const { return m_lastDrawBatches; }  // batches actually issued last frame (0 = skipped)

	// Build-time water body bounds (centroid + AABB over every turb vertex),
	// for capture auto-framing (csz_debugcam 2). hasWater=false when the map has
	// no turb surfaces -> center/mins/maxs are left zeroed. Cheap accessor; reads
	// the values computed once at build time.
	void GetWaterBounds( float center[3], float mins[3], float maxs[3], bool &hasWater ) const;

	// On-screen water evidence: count of per-face AABBs intersecting the view
	// frustum, computed by DrawWater each frame. -1 = not yet tested this map.
	int VisibleWaterFaces() const { return m_visibleFacesLastFrame; }

private:
	// One draw batch = all triangle-list vertices that share a single diffuse
	// texture slot, stored as a CONTIGUOUS run in the VBO. The turb faces are
	// pre-triangulated (fan -> triangle list) and sorted by texSlot at build
	// time, so each batch renders as ONE glDrawArrays(GL_TRIANGLES,...). A
	// typical liquid map has a single water texture -> a single draw call.
	struct WaterBatch
	{
		int firstVert;	// first vertex of this batch's run in the VBO
		int vertCount;	// triangle-list vertex count (multiple of 3)
		int texSlot;	// engine diffuse texture slot (gl_texturenum)
	};

	// Per-face world-space AABB, built once and used for per-frame frustum
	// counting (the REAL on-screen water evidence; the centroid line is
	// build-time only). One entry per turb face that made it into the VBO.
	struct FaceBounds
	{
		float mins[3];
		float maxs[3];
	};

	ShaderProgram m_program;	// program.program == 0 until built
	unsigned int m_vao;		// 0 = none
	unsigned int m_vbo;		// 0 = none
	WaterBatch *m_batches;		// owned; NULL when no turb surfaces
	int m_numBatches;		// distinct texture slots (== draw calls)
	int m_numVerts;			// total triangle-list vertices in the VBO
	int m_turbFaces;		// turb FACE count (pre-batch per-face draw baseline)
	int m_lastDrawBatches;		// batches issued last DrawWater (0 = skipped this frame)

	FaceBounds *m_faceBounds;	// owned; per turb face AABB (NULL when no water)
	int m_numFaceBounds;		// entries in m_faceBounds (== m_turbFaces)
	int m_visibleFacesLastFrame;	// faces whose AABB hit the frustum last DrawWater (-1 = untested)

	// Water body bounds over every turb vertex (build-time; for auto-framing).
	float m_waterCenter[3];
	float m_waterMins[3];
	float m_waterMaxs[3];
	bool m_hasWater;

	model_s *m_model;		// map identity (rebuild on change)
	int m_gpuGeneration;		// GPU generation that owns the GL names
	bool m_built;

	// Cached uniform locations (resolved once per program build).
	int m_uViewProj;
	int m_uTime;
	int m_uCamPos;
	int m_uFog;
	int m_uAmbTint;
	int m_uMoonDir;
	int m_uMoonColor;
	int m_uRain;
	int m_uPhase;
};
extern WaterRenderer g_water;
}
