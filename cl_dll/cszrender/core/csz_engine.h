/*
 * csz_engine.h -- CSOZ renderer: upstream SDK/engine type aggregation point
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
// Single aggregation point for upstream SDK types and globals used by cszrender.
// Include order is HLSDK-sensitive; adjust ONLY here if compilation requires.
#include "wrect.h"
#include "cl_dll.h"        // gEngfuncs / gRenderAPI externs (cl_dll/include/cl_dll.h:100-105)
#include "const.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "com_model.h"
#include "studio.h"
#include "r_studioint.h"
#include "ref_params.h"    // ref_viewpass_t, RF_DRAW_WORLD...
#include "render_api.h"    // render_api_t / render_interface_t / PARM_* / TF_*
#include "cvardef.h"

extern engine_studio_api_t IEngineStudio;  // defined in GameStudioModelRenderer.cpp

namespace csz
{
model_t *WorldModel();                  // gRenderAPI.pfnGetModel( 1 ); NULL when no map
int TexSlotToGlName( int texSlot );     // RenderGetParm( PARM_TEX_TEXNUM, texSlot ); 0 on failure
float ClientTime();                     // gEngfuncs.GetClientTime()
}
