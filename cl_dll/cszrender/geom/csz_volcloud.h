/*
 * csz_volcloud.h -- CSOZ renderer: volumetric raymarch clouds (spike/cloud-volumetric)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Clean-room implementation written
 * from PUBLISHED physical/algorithm descriptions ONLY (Beer-Lambert, Henyey-
 * Greenstein, fBm); no code copied or translated from any license-tainted source
 * (Shadertoy/iQ, Unreal/Unity/Frostbite/Hillaire samples, GPU-Gems, PrimeXT,
 * Paranoia, Trinity, retail/leaked). See csoz docs/provenance.md section 6 and the
 * header of csz_volcloud_shaders.inl.
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
#include "../core/csz_view.h"
namespace csz
{
// Volumetric raymarch night/day cloud layer (the replacement for the deleted dome-
// shell csz_clouds). A vertexless fullscreen triangle reconstructs the world view
// ray (camera basis, identical to the sky pass), a QUARTER-RES RGBA16F offscreen
// target is ray-marched (procedural clean-room fBm density, Beer-Lambert extinction,
// cone self-shadowing, dual-lobe HG, cheap multiscatter, height-aware ambient), then
// horizon-aware bilinear-upsampled and premultiplied-composited into the HDR scene
// FBO at the same seam the old clouds used (after stars, before the moon disc).
//
// Master cvar csz_volcloud default 0 -> Contribute early-outs on its very first line,
// so the production path is BYTE-IDENTICAL when off. The pass also carries the spike
// MEASUREMENT harness (dedicated GL_TIME_ELAPSED timer ring + deterministic perf
// preset + cost-ladder autosweep + 3D-texture capability probe / microbench), all
// gated behind csz_volcloud_perf.
class VolCloudRenderer
{
public:
	void RegisterCvars();                       // all csz_volcloud* cvars (eager, at HUD init)
	void Contribute( const ViewSetup &view );   // the pass; first-line early-out when csz_volcloud 0
	void Shutdown();                            // generation-safe GL teardown (FBO/programs/queries/3D tex)
};
extern VolCloudRenderer g_volcloud;
}
