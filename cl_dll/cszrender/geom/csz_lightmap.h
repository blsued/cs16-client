/*
 * csz_lightmap.h -- CSOZ renderer: lightmap atlas (style 0, row allocator)
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
class LightmapAtlas
{
public:
	static const int kPageSize = 1024;
	static const int kMaxPages = 8;
	void Reset();                                                  // new map / destroy
	bool Allocate( int w, int h, int *page, int *x, int *y );      // classic row (skyline) allocator
	void UploadBlock( int page, int x, int y, int w, int h, const unsigned char *rgb888 ); // style-0 block -> RGBA8 subimage
	int  PageTexSlot( int page );                                  // engine texture slot (lazy GL_CreateTexture "csz_lightmap_<page>")
};
extern LightmapAtlas g_lightmaps;
}
