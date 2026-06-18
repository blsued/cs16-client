/*
 * csz_weather.h -- CSOZ renderer: weather precipitation + ground surface state
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
#include "../core/csz_shader.h"		// ShaderProgram (particle GL programs, B2)
#include "../core/csz_weather_types.h"	// WeatherSurfaceState POD (single source of truth, core-owned)
namespace csz
{
// WeatherSurfaceState (the world/turb surface contract) lives in
// core/csz_weather_types.h so geom can read it from ViewSetup.weather without
// reaching downstream into weather/ (spec 4.6 one-way rule).

// Weather subsystem (default tier). Owns the precipitation mode/intensity
// state and the screen-space particle pool, plus the derived surface-state
// contract consumed by the world/turb shaders.
class WeatherRenderer
{
public:
	void RegisterCvars();                                          // csz_weather* + dev command
	void Update( const ViewSetup &view, float phase, float time ); // derive surface state (+ B2 sim hook)
	void DrawPrecip( const ViewSetup &view );                      // B2: precipitation particle draw
	const WeatherSurfaceState &SurfaceState() const { return m_surf; }
	float RainIntensity() const;  // 0..1, > 0 only when mode == rain
	int   Mode() const;           // 0 off, 1 rain, 2 snow
	void  Shutdown();             // destroy GL objects (B2)

private:
	void EnsureBuilt();          // lazy GL init (programs + dynamic VBOs), keyed on GpuGeneration()
	void Simulate( const ViewSetup &view );  // CPU pool spawn/recycle/integrate (called from Update)

	WeatherSurfaceState m_surf;

	// --- B2: precipitation particle pool (view-relative, CPU-simulated) ---
	// One fixed-capacity pool sized for the high tier so a quality (count) change
	// never reallocates. Each particle stores a world-space position, a velocity,
	// a stable per-particle random seed and a depth-layer index (rain). The active
	// count is a tier/intensity-scaled prefix of the pool [0 .. m_activeCount).
	static const int kMaxParticles = 7000;   // high-tier rain max (>= snow high 4000)

	struct Particle
	{
		float pos[3];   // world position
		float vel[3];   // world velocity (units/sec)
		float seed;     // 0..1 stable per-particle random value (sway phase, jitter)
		float layer;    // 0..1 depth layer (rain: near=1 -> far=0); unused for snow
	};

	Particle m_pool[kMaxParticles];
	int   m_activeCount;     // particles currently simulated/drawn (<= pool size for tier)
	int   m_poolMode;        // mode the pool was last (re)seeded for: 0 none, 1 rain, 2 snow
	bool  m_poolSeeded;      // pool positions initialized at least once
	float m_lastTime;        // ClientTime() at the previous Simulate (for dt)
	unsigned int m_frameCounter;  // accumulating counter -> deterministic recycle randomness

	// GL resources (rebuilt on GpuGeneration() change; never glDelete a stale gen).
	struct WeatherGpu
	{
		ShaderProgram rainProgram;
		ShaderProgram snowProgram;
		unsigned int  rainVao, rainVbo;
		unsigned int  snowVao, snowVbo;
		int uRainViewProj;
		int uSnowViewProj;
		int gpuGeneration;
		bool built;
	};
	WeatherGpu m_gpu;
};
extern WeatherRenderer g_weather;
}
