/*
 * csz_cloudvol.h -- CSOZ renderer: volumetric cloud REBUILD v2 (Phase 0 LOOK slice)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Clean-room implementation written
 * from PUBLISHED physical/algorithm descriptions ONLY (Beer-Lambert, Henyey-
 * Greenstein, Perlin/Worley noise, the Nubis density-remap model). No code is
 * copied or translated from any license-tainted source (Shadertoy/iQ,
 * Unreal/Unity/Frostbite/Hillaire samples, GPU-Gems/GPU-Pro snippets, PrimeXT,
 * Paranoia, Trinity, retail/leaked). See csoz docs/provenance.md section 6.
 *
 * This program is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * In addition, as a special exception, the author gives permission to link the code
 * of this program with the Half-Life Game Engine ("HL Engine") and Modified Game
 * Libraries ("MODs") developed by Valve, L.L.C ("Valve"). You must obey the GNU
 * General Public License in all respects for all of the code used other than the HL
 * Engine and MODs from Valve. If you modify this file, you may extend this exception
 * to your version of the file, but you are not obligated to do so. If you do not wish
 * to do so, delete this exception statement from your version.
 */
#pragma once
#include "../../core/csz_view.h"
namespace csz
{
// PHASE 0 -- LOOK vertical slice of the volumetric-cloud REBUILD. The goal of this
// thin end-to-end slice is to PROVE real volume (visible sides, parallax, terrain
// occlusion) + sun/moon response BEFORE investing in the full asset/tooling pipeline.
//
// What this module does, and how it differs from the REJECTED geom/csz_volcloud:
//   * Structured 3D DENSITY baked IN-PROCESS at init: a 128^3 tileable Perlin-Worley
//     base + a 32^3 high-freq Worley detail volume, uploaded via glTexImage3D. This
//     replaces the in-shader fBm that read as a flat 2D texture.
//   * WORLD-SPACE, scene-depth composited at the kTmVolume seam (AFTER world geometry,
//     depth populated) -- so terrain OCCLUDES the clouds and the cloud SIDES are
//     visible, instead of the old camera-relative sky slab composited before the world.
//   * A single bounded "hero" AABB volume (ray-box march) to prove visible sides +
//     parallax at mid-range.
//   * One SampleCloudDensity() shared by BOTH the view march and the light cone-march
//     (no hidden "delete density / keep lighting" coupling).
//   * Sun + moon driven through ONE celestial-light abstraction, blended by a SMOOTH
//     read of AmbienceParams.nightness (continuous twilight, no hard night>0.5 step).
//
// Master cvar csz_clouds default 0 -> Contribute early-outs on its first line, so the
// production path is byte-identical when off. The OLD csz_volcloud stays in place but
// default-off; only one cloud system composites per frame (this one at kTmVolume when
// csz_clouds is on; the old one at the sky seam when csz_volcloud is on).
class CloudVolRenderer
{
public:
	void RegisterCvars();                       // csz_clouds + csz_clouds_* (eager, at HUD init)
	void Contribute( const ViewSetup &view );   // the pass; first-line early-out when csz_clouds 0
	void Shutdown();                            // generation-safe GL teardown (FBO/programs/queries/3D tex)
};
extern CloudVolRenderer g_cloudvol;
}
