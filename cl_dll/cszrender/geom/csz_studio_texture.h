/*
 * csz_studio_texture.h -- CSOZ renderer: layered external studio texture resolution
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original work written for
 * CSOZ; no code in this file is copied or translated from PrimeXT, Paranoia,
 * Trinity, retail/leaked sources, or any other license-tainted source
 * (see csoz docs/provenance.md, section 6).
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
#include "../core/csz_engine.h"	// model_t / studiohdr_t (anonymous-struct typedefs)

namespace csz
{

// Studio texture resolution layers, spec 4.3.1 (top wins):
//   1. runtime override set (viewmodels only; M5 sends per-class arm sets,
//      dev cvar csz_dev_armskin exercises it today)
//   2. texture.ini explicit mapping ([<model>] section, case-insensitive;
//      texture_csoz.ini is the CSOZ repo-owned extension file, merged on top)
//   3. '#'-named placeholder auto-resolution (CSO convention)
//   4. embedded pixels (stock path; caller keeps mstudiotexture_t::index)
// A failed external load falls through to the next layer and warns once
// (deduplicated by file name).

struct ViewmodelTextureOverride
{
	char texName[64];	// mstudiotexture_t name to replace (case-insensitive)
	char file[160];		// external file under models/texture/
};

const int kMaxViewmodelOverrides = 16;

// Highest-priority layer. Copies the table (count 0 / NULL clears). Frees the
// cached GPU meshes so the next frame rebuilds with the new resolution.
void SetViewmodelTextureOverrides( const ViewmodelTextureOverride *table, int count );

// Resolves layers 1-3 for one texture of one model. Returns the engine
// texture slot (0 = no external source resolved / all failed; use embedded),
// fills *width/*height with the real texture size and *source with a static
// layer tag for logging.
int ResolveStudioTexture( model_t *mod, const studiohdr_t *hdr, const char *texName,
	int *width, int *height, const char **source );

// Registers csz_dev_armskin (set name from "[arms:<set>]" ini sections; empty
// or "0" = no override). Called once from Renderer::OnHudInit.
void RegisterStudioTextureCvars();

// Per-frame dev cvar poll (cheap string compare); applies override set
// changes. Called from the frame orchestration.
void StudioTexturePollDevCvars();

}
