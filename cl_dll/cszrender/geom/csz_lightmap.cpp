/*
 * csz_lightmap.cpp -- CSOZ renderer: lightmap atlas implementation
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
#include "csz_lightmap.h"
#include "../core/csz_engine.h"
#include "../core/csz_glfuncs.h"
#include "../core/csz_glstate.h"
#include "../core/csz_log.h"

#include <stdio.h>
#include <string.h>

namespace csz
{

LightmapAtlas g_lightmaps;

namespace
{

// Atlas pages are ENGINE texture slots (GL_CreateTexture): the engine owns
// the GL object lifetime across context events, so no GPU-generation logic
// is needed here, only GL_FreeTexture on Reset.
struct Page
{
	int texSlot;				// 0 = not created yet
	int heights[LightmapAtlas::kPageSize];	// skyline: used height per column
};

Page s_pages[LightmapAtlas::kMaxPages];

// Largest block we ever convert (smax/tmax for standard maps is <= 17 at
// sample size 16; 128 covers exotic sample sizes with headroom).
const int kMaxBlockDim = 128;
unsigned char s_rgbaScratch[kMaxBlockDim * kMaxBlockDim * 4];

// Style-0 sample -> displayed pixel, replicating the stock engine pipeline
// (verified against the pinned engine: R_BuildLightMap accumulates
// sample * lightstylevalue, scales >> 14 into a 10-bit value, then applies
// the light gamma table; the render_api byte-domain LightToTexGamma is that
// same table). 264 = lightstyle 'm' (the static style-0 normal value). The
// remaining x2 overbright lives in the world shader (plan section 2.4).
unsigned char SampleToPixel( unsigned char sample )
{
	unsigned int scaled = ((unsigned int)sample * 264u ) >> 8;

	if( scaled > 255u )
		scaled = 255u;

	if( gRenderAPI.LightToTexGamma != NULL )
		return gRenderAPI.LightToTexGamma( (unsigned char)scaled );

	return (unsigned char)scaled;
}

}

void LightmapAtlas::Reset()
{
	for( int i = 0; i < kMaxPages; i++ )
	{
		if( s_pages[i].texSlot != 0 && gRenderAPI.GL_FreeTexture != NULL )
			gRenderAPI.GL_FreeTexture( s_pages[i].texSlot );

		s_pages[i].texSlot = 0;
		memset( s_pages[i].heights, 0, sizeof( s_pages[i].heights ));
	}
}

bool LightmapAtlas::Allocate( int w, int h, int *page, int *x, int *y )
{
	if( w <= 0 || h <= 0 || w > kPageSize || h > kPageSize )
		return false;

	for( int p = 0; p < kMaxPages; p++ )
	{
		// Classic skyline scan: find the x whose w-column window has the
		// lowest maximum height.
		int bestY = kPageSize;
		int bestX = -1;

		for( int sx = 0; sx <= kPageSize - w; sx++ )
		{
			int top = 0;

			for( int j = 0; j < w; j++ )
			{
				if( s_pages[p].heights[sx + j] >= bestY )
				{
					// Window cannot beat the current best; skip ahead.
					sx += j;
					top = kPageSize + 1;
					break;
				}

				if( s_pages[p].heights[sx + j] > top )
					top = s_pages[p].heights[sx + j];
			}

			if( top <= kPageSize && top + h <= kPageSize && top < bestY )
			{
				bestY = top;
				bestX = sx;
			}
		}

		if( bestX >= 0 )
		{
			for( int j = 0; j < w; j++ )
				s_pages[p].heights[bestX + j] = bestY + h;

			*page = p;
			*x = bestX;
			*y = bestY;
			return true;
		}
	}

	return false;
}

void LightmapAtlas::UploadBlock( int page, int x, int y, int w, int h, const unsigned char *rgb888 )
{
	if( page < 0 || page >= kMaxPages || rgb888 == NULL )
		return;

	if( w > kMaxBlockDim || h > kMaxBlockDim )
	{
		CSZ_LogError( "lightmap", "block %dx%d exceeds scratch limit %d, clamped", w, h, kMaxBlockDim );
		w = ( w > kMaxBlockDim ) ? kMaxBlockDim : w;
		h = ( h > kMaxBlockDim ) ? kMaxBlockDim : h;
	}

	int slot = PageTexSlot( page );

	if( slot == 0 )
		return;	// PageTexSlot already logged the failure

	for( int t = 0; t < h; t++ )
	{
		for( int s = 0; s < w; s++ )
		{
			const unsigned char *src = &rgb888[( t * w + s ) * 3];
			unsigned char *dst = &s_rgbaScratch[( t * w + s ) * 4];

			dst[0] = SampleToPixel( src[0] );
			dst[1] = SampleToPixel( src[1] );
			dst[2] = SampleToPixel( src[2] );
			dst[3] = 255;
		}
	}

	// Bind via the engine wrapper (TMU hygiene, T1 rule); RGBA rows are
	// always 4-byte aligned so the default UNPACK_ALIGNMENT 4 is correct.
	BindTextureSlot( 0, slot );
	glTexSubImage2D( GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s_rgbaScratch );
}

int LightmapAtlas::PageTexSlot( int page )
{
	if( page < 0 || page >= kMaxPages )
		return 0;

	if( s_pages[page].texSlot == 0 )
	{
		char name[32];

		snprintf( name, sizeof( name ), "csz_lightmap_%d", page );

		if( gRenderAPI.GL_CreateTexture == NULL )
			return 0;

		s_pages[page].texSlot = gRenderAPI.GL_CreateTexture( name, kPageSize, kPageSize,
			NULL, (texFlags_t)( TF_NOMIPMAP | TF_CLAMP ));

		if( s_pages[page].texSlot == 0 )
		{
			// Per-frame caller dedups via the zero return; log once here.
			CSZ_LogError( "lightmap", "GL_CreateTexture failed for %s", name );
		}
		else
		{
			CSZ_LogDev( "lightmap", "created atlas page %d (slot=%d)", page, s_pages[page].texSlot );
		}
	}

	return s_pages[page].texSlot;
}

}
