/*
 * csz_fog.cpp -- CSOZ renderer: client-side ambience state (fog/night/moon)
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
// Dependency rule (plan 2.1 / spec 4.6): fog/ includes core/ ONLY. Ambience
// flows to geom/lighting through ViewSetup.ambience, never through this file.
#include "csz_fog.h"
#include "csz_fog_net.h"			// Step 6 CszFog decoder + dev self-test command
#include "../core/csz_engine.h"		// gEngfuncs (commands, Cmd_Argv)
#include "../core/csz_log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace csz
{

FogController g_fog;

namespace
{

// AMBIENCE payload length (plan 2.3: cmd 1 body, cmd/version already stripped).
const int kAmbiencePayloadBytes = 45;

// L0 black-fog decouple seam (CONVENTIONS.md). serverFogMask is a [0,1] scalar the
// renderer multiplies into the client fog density once per frame at the single
// view.ambience snapshot. Default source = the csz_fog_server_mask cvar ("1" =
// 1.0 = IEEE-exact identity => pixel-for-pixel the pre-L0 fog). A future
// server-authoritative black-fog drive overrides it via CszFogSetServerMask();
// s_serverMaskOverride < 0 means "no override, use the cvar".
cvar_t *s_cvarServerMask;            // csz_fog_server_mask, default "1"
float   s_serverMaskOverride = -1.0f;

// L1 fog-base correctness A/B switch (csz_fog_base, default "1"). 1 = corrected
// analytic base fog (verbatim); 0 = legacy uniform-density fallback (heightFalloff
// forced 0 at the renderer chokepoint). See CszFogBaseCorrected() / csz_fog.h.
cvar_t *s_cvarFogBase;               // csz_fog_base, default "1"

float ClampUnit( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// Byte-domain mirror of the protocol/config values (plan 2.4 semantics). Kept
// so dev commands can edit one group at a time and so the shared "ambience
// set" Info line echoes the exact configured bytes (protocol runtime "unit
// test", clean-room pitfall 20 countermeasure).
struct RawAmbience
{
	int fogR, fogG, fogB;
	float fogDensity;		// 1/units, exp2; <= 0 -> fog off
	int tintR, tintG, tintB;	// 255,255,255 = neutral
	bool moonOn;
	float moonElevDeg, moonYawDeg, moonSizeDeg;
	int moonR, moonG, moonB;
	float moonHalo;
	bool moonlightOn;
	float mlElevDeg, mlYawDeg;
	int mlR, mlG, mlB;
	float mlIntensity;
	// Analytic base fog (fog M1 Step 2). No wire field yet (the versioned CszFog
	// channel is Step 6); these are set only via csz_devfogx for now and default
	// to the environmental look (b=0, glow=0, maxOpacity=1, no bypass).
	float heightFalloff;	// height b (1/units); 0 = uniform density
	float sunGlow;		// directional in-scatter glow strength; 0 = plain fog
	float maxOpacity;	// reveal floor; 1 = full fog, <1 = silhouettes/blackout cap
	int   fogPreset;	// kCszFogPreset* (0 = environmental)
	bool  fogBypassTint;	// black fog bypasses the sky phase-tint multiply
};

RawAmbience NeutralRaw()
{
	RawAmbience r = RawAmbience();	// all zero / false

	r.tintR = 255;
	r.tintG = 255;
	r.tintB = 255;
	r.maxOpacity = 1.0f;	// no reveal floor by default (env fog may fully occlude)
	return r;
}

// Dynamic initializers run at client.dll load, well before any frame; Current()
// can therefore never observe a zeroed (tint-black) snapshot.
RawAmbience s_raw = NeutralRaw();
AmbienceParams s_current = AmbienceNeutral();

// spec 3.9 precedence latch (fog M1 Step 6): set once an active CszFog is applied,
// cleared by Reset() (map change / disconnect). Legacy Fog never writes s_current
// (it only drives g_FogParameters/cl_fog_*, see hud_msg.cpp), so CszFog authority
// is enforced structurally; this latch makes the invariant explicit and queryable.
bool s_hasCszState = false;

// ln(2): the analytic base shader's extinction is FogExtinctionFromDensity(density)
// = density*ln2 (csz_ambience_types.h). CszFog wires the natural extinction a
// directly (F8), so we store fogDensity = a/ln2 and the existing single chokepoint
// reproduces exactly a -- the server never deals in legacy exp2 density.
const float kCszLn2 = 0.6931471805599453f;

int ClampByte( int v )
{
	if( v < 0 ) return 0;
	if( v > 255 ) return 255;
	return v;
}

// Angle -> unit direction, both ends' shared convention (plan 2.3): elevation
// above the horizon (NOT quake pitch; sign pitfall, plan section 8 risk table)
// plus yaw, pointing FROM the scene TOWARD the sky object.
void ElevYawToDir( float elevDeg, float yawDeg, float out[3] )
{
	float e = elevDeg * kDegToRad;
	float y = yawDeg * kDegToRad;

	out[0] = cosf( e ) * cosf( y );
	out[1] = cosf( e ) * sinf( y );
	out[2] = sinf( e );
}

// The single conversion + print point shared by the envelope path and the
// CSZ_DEV_TOOLS commands: byte colors /255 into linear (NO extra gamma,
// clean-room pitfall 156), moonlight premultiplied by intensity, angles
// converted once here.
void ApplyRaw( const RawAmbience &raw )
{
	AmbienceParams p = AmbienceNeutral();

	p.fogColor[0] = (float)raw.fogR * ( 1.0f / 255.0f );
	p.fogColor[1] = (float)raw.fogG * ( 1.0f / 255.0f );
	p.fogColor[2] = (float)raw.fogB * ( 1.0f / 255.0f );
	p.fogDensity = raw.fogDensity;

	p.tint[0] = (float)raw.tintR * ( 1.0f / 255.0f );
	p.tint[1] = (float)raw.tintG * ( 1.0f / 255.0f );
	p.tint[2] = (float)raw.tintB * ( 1.0f / 255.0f );

	// Analytic base fog params (fog M1 Step 2): carried through verbatim. maxOpacity
	// guards against a 0 that would clamp fog fully transparent (AmbienceNeutral=1).
	p.heightFalloff = raw.heightFalloff;
	p.sunGlow = raw.sunGlow;
	p.maxOpacity = raw.maxOpacity > 0.0f ? raw.maxOpacity : 1.0f;
	p.fogPreset = raw.fogPreset;
	p.fogBypassTint = raw.fogBypassTint;

	if( raw.moonOn )
	{
		p.moonEnabled = true;
		ElevYawToDir( raw.moonElevDeg, raw.moonYawDeg, p.moonDir );
		// moonSizeDeg is the angular DIAMETER (plan 2.3); the disc test
		// threshold is cos(angular radius).
		p.moonCosRadius = cosf( raw.moonSizeDeg * 0.5f * kDegToRad );
		p.moonColor[0] = (float)raw.moonR * ( 1.0f / 255.0f );
		p.moonColor[1] = (float)raw.moonG * ( 1.0f / 255.0f );
		p.moonColor[2] = (float)raw.moonB * ( 1.0f / 255.0f );
		p.moonHalo = raw.moonHalo;
	}

	if( raw.moonlightOn )
	{
		p.moonlightEnabled = true;
		ElevYawToDir( raw.mlElevDeg, raw.mlYawDeg, p.moonlightDir );
		p.moonlightColor[0] = (float)raw.mlR * ( 1.0f / 255.0f ) * raw.mlIntensity;
		p.moonlightColor[1] = (float)raw.mlG * ( 1.0f / 255.0f ) * raw.mlIntensity;
		p.moonlightColor[2] = (float)raw.mlB * ( 1.0f / 255.0f ) * raw.mlIntensity;
	}

	s_raw = raw;
	s_current = p;

	// Info-level echo of every decoded value (shared by dev commands and the
	// envelope; the per-field comparison against the server config is this
	// protocol's runtime unit test).
	char moonText[96];
	char moonlightText[96];

	if( raw.moonOn )
	{
		snprintf( moonText, sizeof( moonText ), "moon(elev=%.1f yaw=%.1f size=%.1f %d,%d,%d halo=%.2f)",
			raw.moonElevDeg, raw.moonYawDeg, raw.moonSizeDeg, raw.moonR, raw.moonG, raw.moonB, raw.moonHalo );
	}
	else
	{
		snprintf( moonText, sizeof( moonText ), "moon=off" );
	}

	if( raw.moonlightOn )
	{
		snprintf( moonlightText, sizeof( moonlightText ), "moonlight(elev=%.1f yaw=%.1f %d,%d,%d i=%.2f)",
			raw.mlElevDeg, raw.mlYawDeg, raw.mlR, raw.mlG, raw.mlB, raw.mlIntensity );
	}
	else
	{
		snprintf( moonlightText, sizeof( moonlightText ), "moonlight=off" );
	}

	CSZ_LogInfo( "fog", "ambience set: fog(%d,%d,%d d=%f) tint(%d,%d,%d) %s %s",
		raw.fogR, raw.fogG, raw.fogB, raw.fogDensity, raw.tintR, raw.tintG, raw.tintB,
		moonText, moonlightText );
}

// --- little-endian readers (explicit shift-and-mask discipline, plan 2.3) ---

unsigned int ReadU32le( const unsigned char *p )
{
	return ( (unsigned int)p[0] & 0xFF )
	     | (( (unsigned int)p[1] & 0xFF ) << 8 )
	     | (( (unsigned int)p[2] & 0xFF ) << 16 )
	     | (( (unsigned int)p[3] & 0xFF ) << 24 );
}

float ReadF32le( const unsigned char *p )
{
	unsigned int bits = ReadU32le( p );
	float f;

	memcpy( &f, &bits, sizeof( f ));	// raw IEEE-754 bits (WRITE_LONG(*(int*)&f) on the server)
	return f;
}

#ifdef CSZ_DEV_TOOLS

// --- dev-only ambience commands (pre-A5 verification; compiled out via C9) ---

void DevFogCommand()
{
	RawAmbience raw = s_raw;

	if( gEngfuncs.Cmd_Argc() >= 2 && strcmp( gEngfuncs.Cmd_Argv( 1 ), "off" ) == 0 )
	{
		raw.fogR = 0;
		raw.fogG = 0;
		raw.fogB = 0;
		raw.fogDensity = 0.0f;
		ApplyRaw( raw );
		return;
	}

	if( gEngfuncs.Cmd_Argc() < 5 )
	{
		CSZ_LogInfo( "fog", "usage: csz_devfog <r g b density> | off  (bytes 0-255, density 1/units; <= 0 disables)" );
		return;
	}

	raw.fogR = ClampByte( atoi( gEngfuncs.Cmd_Argv( 1 )));
	raw.fogG = ClampByte( atoi( gEngfuncs.Cmd_Argv( 2 )));
	raw.fogB = ClampByte( atoi( gEngfuncs.Cmd_Argv( 3 )));
	raw.fogDensity = (float)atof( gEngfuncs.Cmd_Argv( 4 ));
	ApplyRaw( raw );
}

// Dev-only: set the analytic base-fog params (no wire field until Step 6's CszFog
// channel). Lets the Step 2 A/B exercise height falloff, sun glow, and the black
// reveal floor / phase-tint bypass without the network path.
void DevFogXCommand()
{
	if( gEngfuncs.Cmd_Argc() < 5 )
	{
		CSZ_LogInfo( "fog", "usage: csz_devfogx <heightB> <sunGlow> <maxOpacity 0..1> <bypassTint 0|1>" );
		return;
	}

	RawAmbience raw = s_raw;

	raw.heightFalloff = (float)atof( gEngfuncs.Cmd_Argv( 1 ));
	raw.sunGlow = (float)atof( gEngfuncs.Cmd_Argv( 2 ));
	raw.maxOpacity = (float)atof( gEngfuncs.Cmd_Argv( 3 ));
	raw.fogBypassTint = ( atoi( gEngfuncs.Cmd_Argv( 4 )) != 0 );
	raw.fogPreset = raw.fogBypassTint ? kCszFogPresetBlackFirst : kCszFogPresetEnvironmental;
	ApplyRaw( raw );
}

void DevTintCommand()
{
	if( gEngfuncs.Cmd_Argc() < 4 )
	{
		CSZ_LogInfo( "fog", "usage: csz_devtint <r g b>  (bytes 0-255; 255 255 255 = neutral)" );
		return;
	}

	RawAmbience raw = s_raw;

	raw.tintR = ClampByte( atoi( gEngfuncs.Cmd_Argv( 1 )));
	raw.tintG = ClampByte( atoi( gEngfuncs.Cmd_Argv( 2 )));
	raw.tintB = ClampByte( atoi( gEngfuncs.Cmd_Argv( 3 )));
	ApplyRaw( raw );
}

void DevMoonCommand()
{
	RawAmbience raw = s_raw;

	if( gEngfuncs.Cmd_Argc() >= 2 && strcmp( gEngfuncs.Cmd_Argv( 1 ), "off" ) == 0 )
	{
		raw.moonOn = false;
		ApplyRaw( raw );
		return;
	}

	if( gEngfuncs.Cmd_Argc() < 8 )
	{
		CSZ_LogInfo( "fog", "usage: csz_devmoon <elev yaw sizeDeg r g b halo> | off  (degrees, bytes 0-255, halo 0-1)" );
		return;
	}

	raw.moonOn = true;
	raw.moonElevDeg = (float)atof( gEngfuncs.Cmd_Argv( 1 ));
	raw.moonYawDeg = (float)atof( gEngfuncs.Cmd_Argv( 2 ));
	raw.moonSizeDeg = (float)atof( gEngfuncs.Cmd_Argv( 3 ));
	raw.moonR = ClampByte( atoi( gEngfuncs.Cmd_Argv( 4 )));
	raw.moonG = ClampByte( atoi( gEngfuncs.Cmd_Argv( 5 )));
	raw.moonB = ClampByte( atoi( gEngfuncs.Cmd_Argv( 6 )));
	raw.moonHalo = (float)atof( gEngfuncs.Cmd_Argv( 7 ));
	ApplyRaw( raw );
}

#endif // CSZ_DEV_TOOLS

}

void FogController::Reset()
{
	// Map change / disconnect: never carry one map's night into the next.
	s_raw = NeutralRaw();
	s_current = AmbienceNeutral();
	s_hasCszState = false;	// drop CszFog authority (spec 3.9 latch cleared)
	CSZ_LogDev( "fog", "ambience reset to neutral" );
}

// fog M1 Step 6: apply a decoded server CszFog state. active=false is an explicit
// "clear black fog" -> neutral + latch dropped. active=true maps the wire fields
// through the shared ApplyRaw mapping (one chokepoint) and latches CszFog authority
// so a later legacy Fog cannot downgrade the ambience (spec 3.9). The non-fog
// ambience (tint/moon) starts from Neutral: CszFog is the authoritative fog source
// for M1 and there is no other production ambience writer (OnAmbienceEnvelope dead).
void FogController::ApplyCszFog( const CszFogState &st )
{
	if( !st.active )
	{
		Reset();	// clears ambience to neutral and drops the latch
		return;
	}

	RawAmbience raw = NeutralRaw();

	raw.fogR = st.fogR;
	raw.fogG = st.fogG;
	raw.fogB = st.fogB;
	// Store extinction as legacy exp2 density so the single shader chokepoint
	// (FogExtinctionFromDensity = density*ln2) reproduces the wired extinction a.
	raw.fogDensity = st.extinctionA / kCszLn2;
	raw.heightFalloff = st.heightFalloffB;
	raw.sunGlow = st.sunGlow;
	raw.maxOpacity = st.maxOpacity;	// ApplyRaw guards 0 -> 1
	raw.fogBypassTint = st.blackFog;
	raw.fogPreset = st.preset;

	ApplyRaw( raw );	// maps into s_current + Info-level decoded-value echo
	s_hasCszState = true;
}

bool FogController::HasCszState() const
{
	return s_hasCszState;
}

void FogController::OnAmbienceEnvelope( const unsigned char *payload, int size )
{
	// Fixed-length body; every group is always present, flags gate effect
	// only (plan 2.3: the parser has no branch-dependent lengths).
	if( payload == NULL || size != kAmbiencePayloadBytes )
	{
		CSZ_LogWarn( "fog", "AMBIENCE payload size %d (expected %d); message ignored", size, kAmbiencePayloadBytes );
		return;
	}

	int flags = (int)payload[0] & 0xFF;
	RawAmbience raw = NeutralRaw();

	if( flags & 0x01 )	// bit0 fogEnabled
	{
		raw.fogR = (int)payload[1] & 0xFF;
		raw.fogG = (int)payload[2] & 0xFF;
		raw.fogB = (int)payload[3] & 0xFF;
		raw.fogDensity = ReadF32le( payload + 4 );
	}

	if( flags & 0x02 )	// bit1 tintEnabled
	{
		raw.tintR = (int)payload[8] & 0xFF;
		raw.tintG = (int)payload[9] & 0xFF;
		raw.tintB = (int)payload[10] & 0xFF;
	}

	if( flags & 0x04 )	// bit2 moonEnabled
	{
		raw.moonOn = true;
		raw.moonElevDeg = ReadF32le( payload + 11 );
		raw.moonYawDeg = ReadF32le( payload + 15 );
		raw.moonSizeDeg = ReadF32le( payload + 19 );
		raw.moonR = (int)payload[23] & 0xFF;
		raw.moonG = (int)payload[24] & 0xFF;
		raw.moonB = (int)payload[25] & 0xFF;
		raw.moonHalo = ReadF32le( payload + 26 );
	}

	if( flags & 0x08 )	// bit3 moonlightEnabled
	{
		raw.moonlightOn = true;
		raw.mlElevDeg = ReadF32le( payload + 30 );
		raw.mlYawDeg = ReadF32le( payload + 34 );
		raw.mlR = (int)payload[38] & 0xFF;
		raw.mlG = (int)payload[39] & 0xFF;
		raw.mlB = (int)payload[40] & 0xFF;
		raw.mlIntensity = ReadF32le( payload + 41 );
	}

	ApplyRaw( raw );	// Info-level decoded-value echo lives there
}

const AmbienceParams &FogController::Current() const
{
	return s_current;
}

void FogController::RegisterDevCommands()
{
#ifdef CSZ_DEV_TOOLS
	gEngfuncs.pfnAddCommand( "csz_devfog", DevFogCommand );
	gEngfuncs.pfnAddCommand( "csz_devfogx", DevFogXCommand );
	gEngfuncs.pfnAddCommand( "csz_devtint", DevTintCommand );
	gEngfuncs.pfnAddCommand( "csz_devmoon", DevMoonCommand );
	CszFogNetRegisterDevCommands();		// csz_devfognet_test (Step 6 protocol self-test)
	CSZ_LogDev( "fog", "dev ambience commands registered (CSZ_DEV_TOOLS build)" );
#endif
}

// --- L0 black-fog decouple seam (always registered, Release-safe) -------------
void CszFogRegisterCvars()
{
	if( s_cvarServerMask == NULL )
		s_cvarServerMask = gEngfuncs.pfnRegisterVariable( "csz_fog_server_mask", "1", FCVAR_CLIENTDLL );

	if( s_cvarFogBase == NULL )
		s_cvarFogBase = gEngfuncs.pfnRegisterVariable( "csz_fog_base", "1", FCVAR_CLIENTDLL );

	CSZ_LogDev( "fog", "L0/L1 fog cvars registered (csz_fog_server_mask=1 identity; csz_fog_base=1 corrected)" );
}

// L1 fog-base correctness A/B switch. true = corrected analytic fog (verbatim,
// the default); false ONLY when csz_fog_base is explicitly 0 (legacy uniform-
// density fallback). Fails safe to corrected if the cvar was never registered.
bool CszFogBaseCorrected()
{
	return s_cvarFogBase != NULL ? ( s_cvarFogBase->value != 0.0f ) : true;
}

// Reserved hook for a future server-authoritative black-fog drive (e.g. mapped
// from gmsgFog in hud_msg.cpp MsgFunc_Fog). m < 0 clears the override and falls
// back to the cvar; m in [0,1] forces serverFogMask. Not wired this period.
void CszFogSetServerMask( float m )
{
	s_serverMaskOverride = m < 0.0f ? -1.0f : ClampUnit( m );
}

// Live serverFogMask in [0,1]: the server override if armed, else the
// csz_fog_server_mask cvar (default 1.0). Fails safe to 1.0 (identity) if the
// cvar was never registered.
float CszFogServerMask()
{
	if( s_serverMaskOverride >= 0.0f )
		return s_serverMaskOverride;

	return s_cvarServerMask != NULL ? ClampUnit( s_cvarServerMask->value ) : 1.0f;
}

}
