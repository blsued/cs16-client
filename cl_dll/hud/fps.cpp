/*
fps.cpp -- CSOZ real-time FPS counter (top-right HUD element)
Copyright (c) 2026 CSOZ project contributors
This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2 of the License, or (at
your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software Foundation,
Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

In addition, as a special exception, the author gives permission to
link the code of this program with the Half-Life Game Engine ("HL
Engine") and Modified Game Libraries ("MODs") developed by Valve,
L.L.C ("Valve").  You must obey the GNU General Public License in all
respects for all of the code used other than the HL Engine and MODs
from Valve.  If you modify this file, you may extend this exception
to your version of the file, but you are not obligated to do so.  If
you do not wish to do so, delete this exception statement from your
version.
*/

#include "stdio.h"

#include "hud.h"
#include "cl_util.h"
#include <string.h>
#include "draw_util.h"

int CHudFPS::Init( void )
{
	m_iFlags = HUD_DRAW;
	m_fps = 0.0f;
	m_bInit = false;
	gHUD.AddHudElem( this );

	// csz_showfps (NOT cl_showfps, to avoid colliding with any engine cvar):
	// 1 = show the counter (default), 0 = hide. Archived so it persists.
	CVAR_CREATE( "csz_showfps", "1", FCVAR_ARCHIVE );

	return 1;
}

int CHudFPS::VidInit( void )
{
	m_bInit = false;	// reseed the EMA after a vid restart
	return 1;
}

int CHudFPS::Draw( float fTime )
{
	if( gHUD.m_iHideHUDDisplay & ( HIDEHUD_ALL ) )
		return 1;

	if( CVAR_GET_FLOAT( "csz_showfps" ) == 0 )
		return 1;

	// Instantaneous fps from the per-frame delta (same source CHudTimer reads),
	// smoothed with an exponential moving average so the number does not jitter.
	float dt = (float)gHUD.m_flTimeDelta;
	if( dt < 1e-5f )
		dt = 1e-5f;
	float inst = 1.0f / dt;

	if( !m_bInit )
	{
		m_fps = inst;		// seed the EMA on the first frame
		m_bInit = true;
	}
	else
	{
		m_fps = m_fps * 0.9f + inst * 0.1f;
	}

	char szBuf[32];
	snprintf( szBuf, sizeof( szBuf ), "FPS: %d", (int)( m_fps + 0.5f ) );

	// Right-align into the top-right corner.
	const int margin = 10;
	int width = DrawUtils::HudStringLen( szBuf, 1.0f );
	int x = ScreenWidth - width - margin;
	int y = margin;

	DrawUtils::DrawHudString( x, y, ScreenWidth, szBuf, 0, 255, 0 );

	return 1;
}
