/*
 * csz_particle.h -- CSOZ renderer: self-drawn soft-particle/tracer pool (C-PAR)
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
// ---------------------------------------------------------------------------
// C-PAR (architecture doc 2026-06-29 §3.3, decision C). A unified self-drawn
// soft-particle pool that REUSES the csz_dust soft-particle + depth-soft-fade
// shading approach (round CPU-billboarded motes, in-shader soft intersection
// against the SkyCompose scene depth texture). The efx shim redirects the
// S3 first-batch combat emitters here; each maps to an emitter config
// (initial velocity / gravity / lifetime / color / size / blend):
//   R_BulletImpactParticles -> dust puff (alpha smoke)
//   R_StreakSplash          -> impact sparks (additive, palette colour)
//   R_SparkEffect/Shower/Streaks -> ricochet sparks (additive, orange)
//   R_RocketTrail           -> smoke/ember trail along the segment
//   (R_Sprite_Trail is NOT redirected: engine draws its FTENT sprite tempents
//    directly via the shim passthrough; redirecting would double-draw)
//   R_TracerEffect          -> a short bright VIEW-ALIGNED streak (degenerate ribbon)
// The shader INCLUDES fog/csz_fog_shaders.inl (linViewZ for the soft depth fade)
// and applies the analytic base fog so black-fog darkens distant particles --
// they never glow brighter than the fogged world.
//
// Seam discipline (§5): C-PAR does NOT reverse-include lighting/csz_dust; it
// reuses the SAME public SkyCompose depth/HDR handles dust uses, independently.
// OWED long tail: every efx entry not in the first batch (shim passes through
// + logs once; under takeover the engine does not draw them).
// ---------------------------------------------------------------------------
namespace csz
{
struct ViewSetup;

void ParticleRegisterCvars();                 // csz_particle (default 1); call from OnHudInit
void ParticleDraw( const ViewSetup &view );   // slot 14.5: transparent soft-particle + tracer pass
void ParticleShutdown();                      // generation-safe GL teardown
void ParticleNewMap();                        // map change: drop every live particle/tracer

// --- efx shim entry points (csz_efx_shim redirects the engine creators here) --
void ParticleEmitBulletImpact( const float *pos );
void ParticleEmitStreakSplash( const float *pos, const float *dir, int color, int count, float speed, int velMin, int velMax );
void ParticleEmitSparkEffect( const float *pos, int count, int velMin, int velMax );
void ParticleEmitSparkShower( const float *pos );
void ParticleEmitSparkStreaks( const float *pos, int count, int velMin, int velMax );
void ParticleEmitRocketTrail( const float *start, const float *end, int type );
void ParticleEmitTracer( const float *start, const float *end );
}
