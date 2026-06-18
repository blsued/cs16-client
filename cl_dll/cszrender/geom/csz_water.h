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
	void EnsureBuilt( model_s *world );  // B1: build turb-surface VBO (lazy, keyed on GPU gen)
	void DrawWater( const ViewSetup &view, float rainIntensity, float skyPhase );  // B1: water pass
	void Shutdown();                     // destroy GL objects (B1)

private:
	// One static triangle-fan face per turb surface, drawn from a single VBO.
	struct WaterFace
	{
		int firstVert;	// first vertex in the VBO (GL_TRIANGLE_FAN start)
		int vertCount;	// numedges of the source surface
		int texSlot;	// engine diffuse texture slot (gl_texturenum)
	};

	ShaderProgram m_program;	// program.program == 0 until built
	unsigned int m_vao;		// 0 = none
	unsigned int m_vbo;		// 0 = none
	WaterFace *m_faces;		// owned; NULL when no turb surfaces
	int m_numFaces;
	int m_numVerts;

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
