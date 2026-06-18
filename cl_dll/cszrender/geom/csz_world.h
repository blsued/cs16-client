/*
 * csz_world.h -- CSOZ renderer: BSP world rendering (faces + lightmap)
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
class WorldRenderer
{
public:
	void EnsureBuilt( model_t *world );  // no-op when already built for this model + GPU generation
	void Destroy();
	void MarkLightmapsDirty();           // GL_BuildLightmaps / gamma change
	void BuildVisibleSet( const ViewSetup &view );   // fat-PVS leaf scan + frustum cull -> visible surface list
	void DrawOpaque( const ViewSetup &view );        // diffuse * lightmap(style 0); '{' alpha-test; sky/turb skipped
	// Brush submodels (func_*, doors) drawn from the SAME static VBO via a
	// per-entity model matrix (E1). Opaque ones go in the opaque domain right
	// after the world; transparent ones (resolved rendermode != kRenderNormal)
	// share the trans domain with sprites, sorted back-to-front internally.
	void DrawBrushOpaque( const ViewSetup &view, cl_entity_s *const *ents, int count );
	void DrawBrushTransparent( const ViewSetup &view, cl_entity_s *const *ents, int count );
	// Set the base-pass reflection clip plane (a,b,c,d) on the world program.
	// Pass {0,0,0,1e9} to restore the no-op. Only clips when the caller has also
	// enabled GL_CLIP_DISTANCE0 (reflection pass); harmless otherwise.
	void SetClipPlane( const float plane[4] );
	void DrawDepth( const ViewSetup &lightView, const Frustum &lightCull );
	void DrawLitAdditive( const ViewSetup &view, const SpotLightParams &light );
	bool IsBuilt() const;
};
extern WorldRenderer g_world;
}
