/*
 * csz_engine_lights.h -- CSOZ renderer: engine dynamic/entity light mirror (M2)
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
namespace csz
{
struct ViewSetup;
// M2: mirror the engine's live dynamic lights (render_api GetDynamicLight --
// the CL_AllocDlight pool: muzzle flash, explosions, TE_DLIGHT) and entity
// lights (GetEntityLight -- attached projectile/rocket glows) into the registry
// point-light bands each frame, nearest-N by camera distance. RunLightPasses
// then adds their omni contribution to world + brush + studio. Honors cvars
// csz_dlight / csz_elight (default 1) and emits the [CSZ:light] stat line.
void CollectEngineLights( const ViewSetup &mainView );  // slot 7.55 (with CollectRealFlashlights)
void RegisterEngineLightCvars();                        // csz_dlight / csz_elight / *_max / *_intensity
}
