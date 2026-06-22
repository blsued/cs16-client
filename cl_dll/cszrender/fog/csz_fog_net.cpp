/*
 * csz_fog_net.cpp -- CSOZ renderer: server CszFog channel decoder (fog M1 Step 6)
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
// Step 6: the versioned, length-tolerant decoder for the server CszFog usermsg.
// It feeds the EXISTING ApplyRaw -> s_current ambience path via g_fog.ApplyCszFog
// (it does NOT revive the dead 45-byte OnAmbienceEnvelope path).
#include "csz_fog.h"
#include "csz_fog_net.h"
#include "../core/csz_log.h"
#ifdef CSZ_DEV_TOOLS
#include "../core/csz_engine.h"		// gEngfuncs (dev command registration)
#endif

namespace csz
{

namespace
{

// u16 extinction/falloff fixed-point divisor (F8). MUST match the server
// (regamedll csz/csz_fog_net.cpp kCszFogFixedDivisor). 65535/16384 ~= 4.0 (1/units)
// max, ~6.1e-5 resolution -- covers the LARGE extinction black fog needs (a ~ 0.02
// .. 0.1 vanishes geometry within a short radius) with hundreds of steps to spare.
const float kCszFogFixedDivisor = 16384.0f;

// Wire v1 byte count (version..sunGlow). A shorter packet defaults the missing
// tail; a longer one (future version) ignores the extra bytes.
const int kCszFogWireV1Bytes = 12;

}

void CszFogOnMessage( const unsigned char *payload, int size )
{
	// Malformed -> log + ignore, KEEP prior state (no null/silent fallback). size<5
	// means we cannot even read version..maxOpacity reliably; version==0 is reserved.
	if( payload == nullptr || size < 5 )
	{
		CSZ_LogWarn( "fog", "CszFog ignored: size %d < 5 (prior fog state kept)", size );
		return;
	}

	int version = (int)payload[0] & 0xFF;

	if( version == 0 )
	{
		CSZ_LogWarn( "fog", "CszFog ignored: version 0 invalid (prior fog state kept)" );
		return;
	}

	int flags     = (int)payload[1] & 0xFF;
	int presetRaw = (int)payload[2] & 0xFF;

	CszFogState st;
	st.active = ( flags & 0x01 ) != 0;

	// Unknown preset -> environmental (fail-safe: never an accidental blackout).
	int preset = presetRaw;
	if( preset != kCszFogPresetEnvironmental && preset != kCszFogPresetBlackFirst && preset != 2 )
		preset = kCszFogPresetEnvironmental;
	st.preset = preset;

	// bit1 = blackFog/bypassPhaseTint, gated on a black preset so an environmental
	// preset can never bypass the tint even if the bit is set (spec 3.8 fail-safe).
	bool wantBypass = ( flags & 0x02 ) != 0;
	st.blackFog = wantBypass && ( preset >= kCszFogPresetBlackFirst );

	int maxByte = (int)payload[3] & 0xFF;
	st.maxOpacity = maxByte > 0 ? (float)maxByte * ( 1.0f / 255.0f ) : 1.0f;	// 0 -> 1

	// Length-tolerant reads: default any field a short packet does not carry.
	st.fogR = ( size > 4 ) ? ( (int)payload[4] & 0xFF ) : 0;
	st.fogG = ( size > 5 ) ? ( (int)payload[5] & 0xFF ) : 0;
	st.fogB = ( size > 6 ) ? ( (int)payload[6] & 0xFF ) : 0;

	int densRaw = ( size >= 9 )  ? ( ( (int)payload[7] & 0xFF ) | ( ( (int)payload[8]  & 0xFF ) << 8 ) ) : 0;
	int fallRaw = ( size >= 11 ) ? ( ( (int)payload[9] & 0xFF ) | ( ( (int)payload[10] & 0xFF ) << 8 ) ) : 0;
	int sunByte = ( size >= 12 ) ? ( (int)payload[11] & 0xFF ) : 0;

	st.extinctionA    = (float)densRaw / kCszFogFixedDivisor;
	st.heightFalloffB = (float)fallRaw / kCszFogFixedDivisor;
	st.sunGlow        = (float)sunByte * ( 1.0f / 255.0f );

	// size > kCszFogWireV1Bytes: any appended fields from a higher version are
	// ignored here (forward-compat); a v1 client decodes a v2 packet cleanly.
	(void)kCszFogWireV1Bytes;

	g_fog.ApplyCszFog( st );
}

#ifdef CSZ_DEV_TOOLS

namespace
{

int  s_testPass = 0;
int  s_testFail = 0;

bool ApproxEq( float a, float b, float eps )
{
	float d = a - b;
	if( d < 0.0f ) d = -d;
	return d <= eps;
}

void Check( bool cond, const char *what )
{
	if( cond ) s_testPass++; else s_testFail++;
	CSZ_LogInfo( "fognet-test", "%s: %s", cond ? "PASS" : "FAIL", what );
}

// csz_devfognet_test: protocol/contract self-test (acceptor = codebase). Feeds
// crafted byte buffers through the real decoder and asserts the resulting
// AmbienceParams + the precedence latch. Restores neutral state on exit.
void DevFogNetTestCommand()
{
	s_testPass = 0;
	s_testFail = 0;
	g_fog.Reset();

	// Case 1: valid v1 black-blackout. a = 0.1 -> densityA = round(0.1*16384) = 1638.
	{
		unsigned char buf[12] = {
			1,        // version
			0x03,     // flags: active | blackFog
			2,        // preset = black-blackout
			255,      // maxOpacity -> 1.0
			0, 0, 0,  // fog RGB
			(unsigned char)( 1638 & 0xFF ), (unsigned char)( ( 1638 >> 8 ) & 0xFF ), // densityA u16 LE
			0, 0,     // heightFalloffB u16 = 0
			0         // sunGlow
		};
		CszFogOnMessage( buf, sizeof( buf ) );
		const AmbienceParams &a = g_fog.Current();
		Check( g_fog.HasCszState(),                      "case1 latch set after active CszFog" );
		Check( a.fogBypassTint,                          "case1 bypassTint true (black preset)" );
		Check( a.fogPreset == 2,                         "case1 preset = blackout" );
		Check( ApproxEq( a.maxOpacity, 1.0f, 1e-4f ),    "case1 maxOpacity = 1.0" );
		Check( ApproxEq( a.fogColor[0], 0.0f, 1e-4f ),   "case1 fogColor black" );
		// extinction reproduced via the chokepoint: density*ln2 ~= a (0.0999...).
		Check( ApproxEq( a.fogDensity * 0.6931472f, 0.0999756f, 2e-4f ), "case1 extinction a ~= 0.1" );
	}

	// Case 2: version==0 -> ignored, prior (black) state kept.
	{
		unsigned char buf[12] = { 0, 0x01, 0, 255, 9,9,9, 0,0, 0,0, 0 };
		CszFogOnMessage( buf, sizeof( buf ) );
		const AmbienceParams &a = g_fog.Current();
		Check( g_fog.HasCszState() && a.fogPreset == 2,  "case2 version0 ignored, black kept" );
	}

	// Case 3: size < 5 -> ignored, prior state kept.
	{
		unsigned char buf[3] = { 1, 0x01, 2 };
		CszFogOnMessage( buf, sizeof( buf ) );
		const AmbienceParams &a = g_fog.Current();
		Check( g_fog.HasCszState() && a.fogPreset == 2,  "case3 short packet ignored, black kept" );
	}

	// Case 4: unknown preset (99) with bit1 set -> environmental fail-safe, no bypass.
	{
		unsigned char buf[12] = { 1, 0x03, 99, 200, 10,20,30, 0,0, 0,0, 0 };
		CszFogOnMessage( buf, sizeof( buf ) );
		const AmbienceParams &a = g_fog.Current();
		Check( a.fogPreset == kCszFogPresetEnvironmental, "case4 unknown preset -> environmental" );
		Check( !a.fogBypassTint,                          "case4 no tint bypass on env fail-safe" );
	}

	// Case 5: v2 packet (14 bytes) with trailing extra -> decodes like v1, extra ignored.
	{
		unsigned char buf[14] = {
			2,        // version 2 (future)
			0x03, 1,  // active|black, preset=silhouettes
			128,      // maxOpacity ~0.502
			0,0,0,
			(unsigned char)( 819 & 0xFF ), (unsigned char)( ( 819 >> 8 ) & 0xFF ), // a=0.05 -> 819
			0,0, 0,
			0xAB, 0xCD // trailing extra (must be ignored)
		};
		CszFogOnMessage( buf, sizeof( buf ) );
		const AmbienceParams &a = g_fog.Current();
		Check( a.fogPreset == kCszFogPresetBlackFirst,   "case5 v2 decodes preset" );
		Check( ApproxEq( a.maxOpacity, 128.0f/255.0f, 1e-3f ), "case5 v2 maxOpacity decoded" );
		Check( ApproxEq( a.fogDensity * 0.6931472f, 0.05f, 5e-4f ), "case5 v2 extinction ~= 0.05 (extra ignored)" );
	}

	// Case 6: inactive packet -> neutral + latch dropped.
	{
		unsigned char buf[12] = { 1, 0x00, 0, 255, 0,0,0, 0,0, 0,0, 0 };
		CszFogOnMessage( buf, sizeof( buf ) );
		const AmbienceParams &a = g_fog.Current();
		Check( !g_fog.HasCszState(),                     "case6 inactive drops latch" );
		Check( a.fogPreset == kCszFogPresetEnvironmental && ApproxEq( a.maxOpacity, 1.0f, 1e-4f ),
		                                                 "case6 inactive -> neutral" );
	}

	// Case 7 (precedence): re-apply black, then Reset() (map change) clears latch.
	{
		unsigned char buf[12] = { 1, 0x03, 1, 200, 0,0,0, (unsigned char)(819&0xFF),(unsigned char)((819>>8)&0xFF), 0,0, 0 };
		CszFogOnMessage( buf, sizeof( buf ) );
		Check( g_fog.HasCszState(),                      "case7 latch set" );
		g_fog.Reset();
		Check( !g_fog.HasCszState(),                     "case7 Reset() clears latch (spec 3.9)" );
	}

	CSZ_LogInfo( "fognet-test", "RESULT %d passed, %d failed", s_testPass, s_testFail );
	g_fog.Reset();
}

}

void CszFogNetRegisterDevCommands()
{
	gEngfuncs.pfnAddCommand( "csz_devfognet_test", DevFogNetTestCommand );
}

#endif // CSZ_DEV_TOOLS

}
