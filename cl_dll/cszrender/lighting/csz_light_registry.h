/*
 * csz_light_registry.h -- CSOZ renderer: dynamic light source registry
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
// Public interface of the future flashlight/ module: frozen after M1
// (plan section 2.2 -- additions only, no changes).
#pragma once
#include "../core/csz_math.h"
#include "../core/csz_light_types.h"
namespace csz
{
enum LightType { kLightSpot = 0, kLightOmni = 1, kLightDirectional = 2 };  // M1 implements Spot only

struct LightDesc
{
	LightType type;
	float origin[3];
	float angles[3];    // quake pitch/yaw/roll -> spot direction
	float color[3];     // linear 0..1, intensity premultiplied
	float radius;       // far range (world units)
	float fov;          // full cone angle, degrees (spot)
	float die;          // absolute client time to expire; 0 = persistent
	bool castShadow;
};

struct ActiveLight
{
	bool used;
	int key;
	LightDesc desc;
	// derived (UpdateMatrices):
	Mat4 matView, matProj, matShadow;
	Frustum frustum;        // far plane disabled (numPlanes=5, notes-mechanisms e)
	int shadowTexSlot;      // set by shadow pass each frame; 0 = none
};

class LightRegistry
{
public:
	static const int kMaxLights = 64;
	static const int kEngineDlightBase = 32;  // slots 32..63 reserved for engine dlight mirroring (M2)
	int  AddOrUpdate( int key, const LightDesc &desc );  // same-key slot reuse, else first free slot < 32; -1 when full (Error, throttled)
	void Remove( int key );
	void DecayFrame( float time );                       // expire die>0 lights; called from ClearScene
	void UpdateMatrices();                               // spot: proj(fov, near 0.1, far radius) + Mat4ViewQuake + Mat4ShadowBias + frustum
	ActiveLight *Slot( int i );                          // 0..kMaxLights-1; NULL contract: never (asserts range)
	void BuildSpotParams( const ActiveLight &light, SpotLightParams &out ) const;
};
extern LightRegistry g_lights;
}
