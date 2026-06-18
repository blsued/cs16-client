/*
 * csz_flashlight.cpp -- CSOZ renderer: player flashlight (torch) module
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
// Dependency rule (spec 4.6): core/ headers + the PUBLIC light registry only.
// The flashlight is just another client light (key=-3) re-published at the view
// each frame; all cone/attenuation/shadow math lives in the lighting pass.
#include "csz_flashlight.h"
#include "../core/csz_engine.h"		// gEngfuncs (cvars)
#include "../core/csz_log.h"
#include "../lighting/csz_light_registry.h"

#include <string.h>

namespace csz
{

Flashlight g_flashlight;

namespace
{

// Reserved client registry key for the player torch (slots < 32 are client
// lights; -1=csz_testspot, -2=csz_testlight demo, -3=flashlight).
const int kFlashlightKey = -3;

// A warm-neutral white torch contrasts the cool moon ambient (target image).
const float kFlashlightColor[3] = { 1.0f, 0.93f, 0.80f };

// Fallback tuning when CSZ_DEV_TOOLS tunables are compiled out (release).
const float kDefaultFov = 46.0f;	// full cone angle, degrees (Round-1 tuned)
const float kDefaultRange = 950.0f;	// far range, world units (Round-1 tuned)
const float kDefaultIntensity = 1.1f;	// color multiplier (V5: low premult so the world lit-pass Reinhard soft-cap keeps a warm, textured core instead of a blown-white disk)

cvar_t *s_cvarEnable;		// csz_flashlight: master player on/off (gameplay, A-class feature still ships)

#ifdef CSZ_DEV_TOOLS
cvar_t *s_cvarFov;		// csz_flashlight_fov
cvar_t *s_cvarRange;		// csz_flashlight_range
cvar_t *s_cvarIntensity;	// csz_flashlight_intensity
#endif

float CvarValueOr( cvar_t *c, float fallback )
{
	return ( c != NULL ) ? c->value : fallback;
}

}

void Flashlight::RegisterCvars()
{
	if( s_cvarEnable == NULL )
		s_cvarEnable = gEngfuncs.pfnRegisterVariable( "csz_flashlight", "1", FCVAR_CLIENTDLL );

#ifdef CSZ_DEV_TOOLS
	if( s_cvarFov == NULL )
		s_cvarFov = gEngfuncs.pfnRegisterVariable( "csz_flashlight_fov", "46", FCVAR_CLIENTDLL );
	if( s_cvarRange == NULL )
		s_cvarRange = gEngfuncs.pfnRegisterVariable( "csz_flashlight_range", "950", FCVAR_CLIENTDLL );
	if( s_cvarIntensity == NULL )
		s_cvarIntensity = gEngfuncs.pfnRegisterVariable( "csz_flashlight_intensity", "1.1", FCVAR_CLIENTDLL );
	CSZ_LogDev( "flashlight", "dev flashlight tunables registered (CSZ_DEV_TOOLS build)" );
#endif
}

void Flashlight::Update( const ViewSetup &view )
{
	bool enabled = ( s_cvarEnable != NULL && s_cvarEnable->value != 0.0f );

	if( !enabled )
	{
		// Player toggled the torch off: drop our registry slot once, then idle.
		if( m_active )
		{
			g_lights.Remove( kFlashlightKey );
			m_active = false;
		}
		return;
	}

#ifdef CSZ_DEV_TOOLS
	float fov = CvarValueOr( s_cvarFov, kDefaultFov );
	float range = CvarValueOr( s_cvarRange, kDefaultRange );
	float intensity = CvarValueOr( s_cvarIntensity, kDefaultIntensity );
#else
	float fov = kDefaultFov;
	float range = kDefaultRange;
	float intensity = kDefaultIntensity;
#endif

	LightDesc desc;

	memset( &desc, 0, sizeof( desc ));
	desc.type = kLightSpot;
	desc.origin[0] = view.origin[0];
	desc.origin[1] = view.origin[1];
	desc.origin[2] = view.origin[2];
	desc.angles[0] = view.angles[0];
	desc.angles[1] = view.angles[1];
	desc.angles[2] = view.angles[2];
	desc.color[0] = kFlashlightColor[0] * intensity;	// linear, intensity premultiplied
	desc.color[1] = kFlashlightColor[1] * intensity;
	desc.color[2] = kFlashlightColor[2] * intensity;
	desc.radius = range;
	desc.fov = fov;
	desc.die = 0.0f;			// persistent; we re-publish (in-place) each frame
	desc.castShadow = true;

	// Same-key in-place update: cheap per-frame origin/angle refresh, no churn.
	g_lights.AddOrUpdate( kFlashlightKey, desc );
	m_active = true;
}

}
