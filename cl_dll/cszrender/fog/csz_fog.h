/*
 * csz_fog.h -- CSOZ renderer: client-side ambience state (fog/night/moon)
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
#include "../core/csz_ambience_types.h"
namespace csz
{
// Decoded + validated server CszFog state (fog M1 Step 6, spec 4.6'). Produced by
// the versioned wire decoder (fog/csz_fog_net.cpp) and applied by ApplyCszFog.
// Colors are bytes 0..255 (ApplyRaw divides by 255); extinction/falloff are the
// natural-exp coefficients in 1/units; maxOpacity/sunGlow are 0..1.
struct CszFogState
{
	bool  active;          // flags bit0; false => clear black fog back to neutral
	bool  blackFog;        // flags bit1: bypass the sky phase-tint (only on a black preset)
	int   preset;          // resolved kCszFogPreset* (unknown wire value -> environmental)
	float maxOpacity;      // reveal floor 0..1 (0 guarded to 1 downstream)
	int   fogR, fogG, fogB; // base fog color bytes 0..255
	float extinctionA;     // natural extinction a (1/units); black fog wants LARGE a
	float heightFalloffB;  // height falloff b (1/units); 0 = uniform density
	float sunGlow;         // directional in-scatter strength 0..1
};

// Client-side ambience state. The ONLY production writer is the server "CSZ"
// envelope (spec 3.2); there is no enable/disable cvar (A-class, spec 4.1).
// csz_devfog/csz_devtint/csz_devmoon (A1) and csz_devmoonlight (A4) exist
// solely in CSZ_DEV_TOOLS builds for pre-A5 verification (compiled out via C9).
class FogController
{
public:
	void Reset();                          // map change / disconnect -> Neutral (clears the CszFog latch)
	// payload = AMBIENCE cmd body (45 bytes, layout 2.6), cmd/version already
	// stripped by the dispatcher. Logs decoded values once at Info level.
	void OnAmbienceEnvelope( const unsigned char *payload, int size );
	// fog M1 Step 6: apply a decoded server CszFog state into the ambience
	// snapshot via the shared ApplyRaw mapping, and latch CszFog authority so a
	// later legacy Fog cannot downgrade it (spec 3.9). active=false clears to
	// neutral and drops the latch.
	void ApplyCszFog( const CszFogState &st );
	// spec 3.9 precedence latch: true once any active CszFog has been received and
	// not yet Reset(); legacy Fog must not overwrite ambience while this holds.
	bool HasCszState() const;
	const AmbienceParams &Current() const;
	void RegisterDevCommands();            // no-op unless CSZ_DEV_TOOLS
};
extern FogController g_fog;

// L0 black-fog decouple seam (CONVENTIONS.md). The fog the scene consumes is the
// client fog density scaled by serverFogMask:
//   fogDensityConsumed = clientDensity(= clientVisibilityFactor) * serverFogMask
// serverFogMask defaults to 1.0 (csz_fog_server_mask "1", no server override) so
// density*1.0 is an IEEE-exact identity => pixel-for-pixel the pre-L0 fog. The
// renderer applies this once at the view.ambience snapshot (the single chokepoint
// every fog consumer reads). RESERVED: a future server-authoritative blackout
// calls CszFogSetServerMask() (e.g. from MsgFunc_Fog); until then the cvar is the
// sole source.
void  CszFogRegisterCvars();            // registers csz_fog_server_mask + csz_fog_base (always; Release-safe)
void  CszFogSetServerMask( float m );   // future server drive; clamps to [0,1]; m<0 clears override
float CszFogServerMask();               // live [0,1]: override if armed, else the cvar (default 1.0)

// L1 fog-base correctness A/B switch (csz_fog_base, default "1"). The Step 2
// analytic base fog audited correct on all five optical points (radial 3D
// distance, the closed-form exponential-height integral with extinction in the
// numerator and b*dz in the denominator, the dz->0 near-horizontal limit, the
// linear-HDR pre-tonemap mix, and unit consistency) -- so there is no defect to
// gate. The switch instead exposes a clean A/B for the downstream visual gate:
//   1 (default) = the corrected analytic fog VERBATIM (IEEE-exact identity).
//   0           = legacy fallback: heightFalloff is forced to 0 at the single
//                 view.ambience chokepoint, so the shader takes its uniform-
//                 density branch (F = a*t) = the pre-analytic look. This isolates
//                 EXACTLY what the exponential-height integral buys (visible only
//                 when a map/csz_devfogx sets b>0); it manufactures no wrong path.
bool  CszFogBaseCorrected();            // true unless csz_fog_base == 0 (fails safe to corrected)
}
