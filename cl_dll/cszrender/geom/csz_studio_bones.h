/*
 * csz_studio_bones.h -- CSOZ renderer: CPU studio bone setup (anim + gait)
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
#include "../core/csz_engine.h"	// studiohdr_t is an anonymous-struct typedef in the
				// fork's studio.h, so it cannot be forward-declared
				// (plan 2.2 contract deviation, progress-t3.md)
#include "../core/csz_math.h"
namespace csz
{
const int kMaxGpuBones = 128;
struct BoneSetup
{
	int numBones;
	bool mirrored;                      // viewmodel right-hand flip applied (winding reversed; draw cull-off)
	float gpuBones[kMaxGpuBones][12];   // per bone: 3 rows of vec4 (world-from-bone 3x4, row vectors)
};
// Main-sequence pose + player gait blending (bone-name boundary weights),
// frame estimation from curstate(frame/animtime/framerate), out-of-range
// sequence index resets to 0 (stock parity, notes-mechanisms f-14). One
// evaluation per entity per frame (stamp cached); multi-pass callers reuse
// the cache.
bool SetupBones( cl_entity_s *ent, studiohdr_t *hdr, float time, const BoneSetup **out );

// p_ weapon model riding a player skeleton: bones whose names match the
// carrier model copy the carrier's world transform; unmatched bones animate
// from the weapon's own (clamped) sequence under the carrier entity
// transform. Adapted from this fork's StudioModelRenderer::StudioMergeBones
// (HLSDK lineage). Cached per (ent, weaponHdr) per frame like SetupBones.
bool SetupBonesMerged( cl_entity_s *ent, studiohdr_t *carrierHdr, const BoneSetup *carrierBones,
                       studiohdr_t *weaponHdr, float time, const BoneSetup **out );

// Resets the per-frame bone cache; called once per frame (StudioRenderer::BeginFrame).
void ResetBoneCache( float time );
}
