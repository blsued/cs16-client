/*
 * csz_glcaps.h -- CSOZ renderer: GL capability probing (the one feature-probe spot)
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
struct GlCaps
{
	char versionString[128];
	char rendererString[128];
	int major, minor;
	int profileMask;               // GL_CONTEXT_PROFILE_MASK (0 on pre-3.2 contexts)
	int maxTextureSize;
	int maxVertexUniformComponents;
};
// Loads functions (csz_glfuncs) then probes. Logs exactly one Info summary line:
// "[CSZ:glcaps] GL <version> | <renderer> | profile=0x<mask> | maxtex=<n> | maxvtxuniform=<n>"
// Unmet hard requirements (any required function missing, maxtex < 1024,
// maxVertexUniformComponents < 1664) -> CSZ_FatalInit. Idempotent.
bool ProbeGlCaps();
const GlCaps &Caps();
}
