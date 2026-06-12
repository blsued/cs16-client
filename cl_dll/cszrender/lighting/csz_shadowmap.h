/*
 * csz_shadowmap.h -- CSOZ renderer: spot light shadow map (depth pass driver)
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
#pragma once
struct cl_entity_s;
namespace csz
{
struct ActiveLight;
struct ViewSetup;
class SpotShadowMap
{
public:
	static const int kResolution = 1024;
	bool EnsureCreated();   // lazy: depth texture (engine slot, TF_DEPTHMAP|TF_CLAMP|TF_BORDER|TF_NOMIPMAP) + FBO
	                        // failure -> Error log once, light renders shadowless (B-class runtime degrade)
	void Destroy();
	// Depth-renders world + given studio entities from the light view
	// (all-visible PVS), fills light.matShadow + light.shadowTexSlot.
	// Caller restores main viewport afterwards (ApplyMainViewport).
	void RenderDepth( ActiveLight &light, const ViewSetup &mainView,
	                  cl_entity_s *const *studioEnts, int studioCount );
	int TexSlot() const;    // 0 when not created
};
extern SpotShadowMap g_spotShadow;   // M1: exactly one (single test light)
}
