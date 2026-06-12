/*
 * csz_studio.h -- CSOZ renderer: studio model rendering (public geom interface)
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
#include "../core/csz_view.h"
#include "../core/csz_light_types.h"
typedef struct model_s model_t;
struct cl_entity_s;
namespace csz
{
class StudioRenderer
{
public:
	void OnModelUnloaded( model_t *mod );    // Mod_ProcessUserData(create=false)
	void DestroyAll();                        // vid restart / shutdown
	void BeginFrame( float time );            // reset per-frame bone stamps
	void DrawOpaque( const ViewSetup &view, cl_entity_s *const *ents, int count );
	void DrawDepth( const ViewSetup &lightView, const Frustum &lightCull, cl_entity_s *const *ents, int count );
	void DrawLitAdditive( const ViewSetup &view, const SpotLightParams &light, cl_entity_s *const *ents, int count );
	void DrawSingle( const ViewSetup &view, cl_entity_s *ent );   // viewmodel path; caller owns depth range/projection
};
extern StudioRenderer g_studio;
}
