/*
 * csz_ambience_types.h -- CSOZ renderer: server-authoritative ambience POD
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
namespace csz
{
// Server-authoritative ambience snapshot (spec 3.2). Produced by fog/
// (envelope decode), consumed by geom + lighting through ViewSetup.ambience.
// Lives in core so fog/ never includes (and is never included by) lighting
// or geom (spec 4.6 one-way rule). All colors linear 0..1, premultiplied by
// their intensity; directions are normalized world-space unit vectors
// pointing FROM the scene TOWARD the sky object (see 2.6 angle convention).
// Fog preset ids (server-downlinked, fog M1 spec 4.6'). 0 = cosmetic
// environmental haze; >= kCszFogPresetBlackFirst = server-authoritative black
// gameplay fog (silhouettes / blackout) that bypasses the sky phase-tint.
enum
{
	kCszFogPresetEnvironmental = 0,
	kCszFogPresetBlackFirst    = 1,   // first black-fog preset (silhouettes)
	kCszFogPresetBlackout      = 2,   // full blackout (geometry fully occluded beyond range)
};
struct AmbienceParams
{
	float fogColor[3];
	float fogDensity;       // legacy exp2 density, 1/units; <= 0 disables fog. The
	                        // analytic base fog (fog M1 Step 2) converts this to a
	                        // natural-exp extinction via FogExtinctionFromDensity().
	float tint[3];          // night tint multiplier; (1,1,1) = neutral
	bool  moonEnabled;
	float moonDir[3];
	float moonCosRadius;    // cos(angular radius); disc test threshold
	float moonColor[3];
	float moonHalo;         // halo intensity 0..1
	bool  moonlightEnabled;
	float moonlightDir[3];  // surface -> moon (shader L vector, constant)
	float moonlightColor[3];
	// --- L2 moonlight light-model channels (sky-base D layer L2). PublishLighting
	// SEPARATES the single blended moonlightColor into the three physically distinct
	// channels a moonlit night actually has, so the downstream layers can drive each
	// independently WITHOUT the "one scalar dims everything -> ground black / air
	// bright" coupling. These are PURE EXPOSURE: no existing shader reads them, so at
	// every default they are computed-but-unconsumed and the approved full-moon look
	// is byte-identical. Premultiplied/linear, same convention as moonlightColor.
	//   * moonSurfaceDirect  = the MOON-ONLY component of the surface N.L directional
	//     (moonRGB * moonLit, after cloudDim). The surface pass still feeds off the
	//     blended moonlightColor above; this isolates the moon term for L3/L4 reasoning.
	//   * moonFogInScatter / *Intensity = a DEDICATED in-scatter channel for L4
	//     (moonlight Tyndall light-shafts / fog in-scatter), decoupled from the
	//     surface-coupled u_sunColor so L4 can brighten the air without touching ground.
	float moonSurfaceDirect[3];     // moon-only premultiplied surface directional (exposure)
	float moonFogInScatter[3];      // moon in-scatter color for L4 fog/light-shafts (unit-ish, NOT premul)
	float moonFogInScatterIntensity;// scalar strength for moonFogInScatter (0 = no moon in-scatter)
	// L3 cloud-cover dimmer on the MOON light (directional + exposed channels). 1.0 =
	// clear sky = IEEE-exact identity (the only value L2 ever produces, since no L3
	// driver exists yet). L3 sets <1.0 to attenuate moonlight under cloud cover; the
	// publisher applies it to moonLit so surface-direct, the blended directional, and
	// the exposed channels all dim coherently from one knob. Sun term is untouched.
	float cloudDim;
	// --- L3a OWNED cloud-state scalars (sky-base D layer L3a). The cloud dome
	// (geom/csz_clouds.cpp) computes these coarse GLOBAL average-cloud-state hints
	// each frame for DOWNSTREAM layers L3b (world/moon darkening) and L4 (light
	// shafts). SINGLE OWNERSHIP: L3a only WRITES them; it does NOT read them back to
	// dim anything, so at every default (clouds off / day) they are computed-but-
	// unconsumed and the approved look is unchanged. Identity = 1.0 (clear sky).
	//   * directTransmittance = moonlight direct transmission through cloud (1=clear)
	//   * skyAmbientScale     = sky-ambient multiplier (1=clear; dims gently to ~0.6)
	//   * shaftMask           = light-shaft gating (cloud-gap=1 / thick cloud=0)
	float directTransmittance;
	float skyAmbientScale;
	float shaftMask;
	// --- Analytic base fog (fog M1 Step 2, spec 3.6/4.3). Extends the legacy
	// exp2 fog with exponential height falloff, a directional sun/moon in-scatter
	// glow, and the server-controlled black-fog reveal floor. ---
	float heightFalloff;    // height falloff b (1/units, world Z-up); 0 = uniform density
	float sunGlow;          // directional in-scatter glow strength; 0 = plain fog (no glow)
	float maxOpacity;       // server reveal floor: shader clamps T to max(T, 1-maxOpacity); 1 = full fog
	int   fogPreset;        // kCszFogPreset* (0 = environmental cosmetic; >=1 = black gameplay)
	bool  fogBypassTint;    // black fog bypasses the sky phase-tint multiply (spec 3.6)
	// --- Client-side achromatic ambient in-scatter (fog rewrite §5.1). The physical
	// fix for "the map looks black, not foggy": a real participating medium does not
	// just SUBTRACT scene radiance (col*T), it ADDS back the in-scattered light it
	// catches. Server "black fog" ships fogColor=(0,0,0) so the in-scatter term was 0
	// and the blend collapsed to a pure multiplicative darken. This is a low-luminance
	// NEUTRAL (very slightly cool) gray the renderer computes from client cvars each
	// frame (csz_fog_ambient / csz_fog_ambient_cool, via CszFogComputeAmbient) and
	// folds into the base in-scatter at the world/studio feed sites (CszApplyFogAmbient).
	// Linear, premultiplied. (0,0,0) = legacy byte-identical (no ambient). NOT server-
	// authoritative -- it rides in the snapshot purely as a per-frame transport slot.
	float fogAmbient[3];
	// --- S2 physical night model (REWORK-SPEC §S2, codex findings 1,2,7,9; codex S2
	// red-team v1 fixes P0/P1a/P2b). Like fogAmbient above, these are CLIENT-side
	// per-frame transport slots (derived in PublishLighting from phase + cvars), NOT
	// server-authoritative. They turn the "go dark" mechanism from a global tiled
	// brightness multiplier into physical incident light gated by the geometric skyVis
	// baked in S1. All default to the approved 3fd8b7e look (nightModel drives the A/B;
	// nightness=0 day/sunset -> the shader's approvedDay branch is byte-identical).
	//   * nightModel   = master A/B (csz_night_model): 1 = new physical model,
	//                    0 = byte-identical pre-S2 tiled-multiplier night (one-knob revert).
	//   * nightness    = explicit phase gate [0,1] (finding 7, codex P0): driven by an
	//                    INDEPENDENT PHASE CURVE (NOT from u_ambTint.b-r, which mis-read
	//                    the cool dawn keyframe as full night and regressed the approved
	//                    dawn). 0 at sunset(phase 0.0), 1 at midnight(0.5), fallen back to
	//                    ~0 by dawn(0.86) and 0 by day(1.0). Drives ONLY the
	//                    approvedDay->physicalNight mix; the legacy day-for-night cool
	//                    grade keeps its own separate tint.b-r signal inside approvedDay.
	//   * sunSurfaceDirect = WARM SUN-ONLY premultiplied surface directional (sunRGB *
	//                    sunLit; codex P2b). The physical-night shader path lights the
	//                    surface with this UNGATED warm sun (dusk/dawn, finding 2) PLUS
	//                    the skyVis-GATED moon-only moonSurfaceDirect, so the moon can
	//                    never leak through walls at ANY nightness. The sun direction is
	//                    the pure antipode of the moon (-moonDir), so no separate dir slot
	//                    is needed. 0 when the sun is below the horizon (deep night). The
	//                    legacy model0 path keeps the blended moonlightColor (untouched).
	//   * nightK / nightSky / nightFloor are SEPARATELY calibrated for world [0] and
	//     studio [1] (finding 9: world has overbright+lightmap proxy, studio is the
	//     ambient/shadeColor domain -- they must not share one k+floor). nightSky/Floor
	//     are linear premultiplied night-ambient colors (cool hue x cvar intensity).
	//   * nightMoonGain = gain on the skyVis-GATED moon directional in physical night
	//     (the wallhack fix: 屋顶下 skyVis~0 -> no moonlight). Shared.
	float nightModel;          // 1 = new physical model, 0 = legacy 3fd8b7e (A/B)
	float nightness;           // explicit phase gate [0,1], driven by the phase curve (codex P0)
	float sunSurfaceDirect[3]; // warm sun-only premul surface directional (ungated; codex P2b)
	float nightK[2];           // skyVis exponent k; [0] = world, [1] = studio
	float nightSky[2][3];      // night sky-ambient color (premul); [0] = world, [1] = studio
	float nightFloor[2][3];    // competitive readable ambient floor (premul); [0] world [1] studio
	float nightMoonGain;       // gated moon directional gain (shared)
};
// (0,0,0,0)/(1,1,1)/disabled everything -- the vanilla daylight look.
inline AmbienceParams AmbienceNeutral()
{
	AmbienceParams p = AmbienceParams();	// value-initialized: every float 0, bools false

	p.tint[0] = 1.0f;
	p.tint[1] = 1.0f;
	p.tint[2] = 1.0f;
	p.maxOpacity = 1.0f;	// no reveal floor by default: fog may fully occlude (env look unchanged)
	p.cloudDim = 1.0f;	// L2: clear sky = no moonlight dimming (IEEE-exact identity until L3 drives it)
	p.directTransmittance = 1.0f;	// L3a: clear sky = full moonlight transmission (identity; computed-not-applied)
	p.skyAmbientScale     = 1.0f;	// L3a: clear sky = no sky-ambient dimming (identity)
	p.shaftMask           = 1.0f;	// L3a: clear sky = light shafts fully pass (identity)
	// S2 physical night defaults: neutral daylight is nightness=0 so the night model is
	// dormant. sunSurfaceDirect defaults to 0 (value-init) -- sun down -> no directional
	// until PublishLighting feeds it; approvedDay collapses to the baked lit term (never black).
	p.nightModel     = 1.0f;	// new physical model active by default (A/B: csz_night_model 0 reverts)
	p.nightness      = 0.0f;	// neutral = full day -> approvedDay branch (identity)
	p.nightK[0] = 0.7f;  p.nightK[1] = 0.7f;
	p.nightMoonGain  = 1.0f;
	return p;
}
// Legacy exp2 fog rendered 2^(-density*d); the analytic base fog renders the
// physically-cleaner e^(-a*d). To MATCH the legacy look at b=0 they must agree:
// 2^(-D*d) = e^(-D*ln2*d), so the natural extinction a = D*ln2. The conversion
// lives here (the one boundary between the legacy density wire field and the new
// shader uniform); a future CszFog channel that downlinks a true extinction would
// feed it directly. <=0 stays 0 (fog off).
inline float FogExtinctionFromDensity( float density )
{
	return density > 0.0f ? density * 0.6931471805599453f : 0.0f;	// ln(2)
}
// Build the two fog uniform vectors the analytic base shaders consume from an
// ambience snapshot (one chokepoint so every feed site -- world/studio/sprite/sky
// -- stays consistent). fogVec = (color.rgb, extinction a); fogParams = (height
// falloff b, sun glow, maxOpacity, reserved).
inline void CszFogUniformVecs( const AmbienceParams &amb, float fogVec[4], float fogParams[4] )
{
	fogVec[0] = amb.fogColor[0];
	fogVec[1] = amb.fogColor[1];
	fogVec[2] = amb.fogColor[2];
	fogVec[3] = FogExtinctionFromDensity( amb.fogDensity );
	fogParams[0] = amb.heightFalloff;
	fogParams[1] = amb.sunGlow;
	fogParams[2] = amb.maxOpacity > 0.0f ? amb.maxOpacity : 1.0f;	// 0 -> 1 (never clamp fog to fully transparent)
	fogParams[3] = 0.0f;
}
// Fold the client achromatic ambient in-scatter (amb.fogAmbient, computed once per
// frame by the renderer from cvars) into the fog in-scatter color the base shaders
// read as u_fog.rgb. u_fog.rgb is ADDITIVE in-scatter only (shader: inscatter =
// u_fog.rgb + ...), blended by (1-T), so adding a low neutral gray makes the medium
// CONTRIBUTE faint radiance that thickens with distance instead of only multiplying
// the scene toward black -- the core "darkness -> fog" fix (§5.1). It also softens the
// maxOpacity plateau for free: where T floors low, (1-T) is large, so the beyond-range
// region tends to the ambient gray (thickening haze) rather than a pure-black void.
// Applied ONLY at the world + studio feed sites (geometry + players); sprites (emitters)
// and the sky dome keep the server fog color untouched. fogAmbient=(0,0,0) -> no-op
// (legacy byte-identical). Scattering albedo sigma_s/sigma_t ~ 1 for fog, so no scale.
inline void CszApplyFogAmbient( const AmbienceParams &amb, float fogVec[4] )
{
	fogVec[0] += amb.fogAmbient[0];
	fogVec[1] += amb.fogAmbient[1];
	fogVec[2] += amb.fogAmbient[2];
}
// L2 downstream accessor: the dedicated moon in-scatter feed for L4 (light-shafts /
// fog in-scatter). One chokepoint so every future consumer reads the SAME channel
// instead of re-deriving from the surface-coupled moonlightColor. outColor = the
// moon in-scatter tint (linear, NOT premultiplied); returns the scalar intensity
// (0 when the moon is not contributing). Multiply outColor by the return value for
// a premultiplied feed matching the moonlightColor convention.
inline float CszMoonInScatter( const AmbienceParams &amb, float outColor[3] )
{
	outColor[0] = amb.moonFogInScatter[0];
	outColor[1] = amb.moonFogInScatter[1];
	outColor[2] = amb.moonFogInScatter[2];
	return amb.moonFogInScatterIntensity;
}
}
