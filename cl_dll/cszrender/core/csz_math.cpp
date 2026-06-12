/*
 * csz_math.cpp -- CSOZ renderer: matrix math implementation (T1 subset)
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
#include "csz_math.h"

#include <math.h>
#include <string.h>

// T1 lands the identity/multiply/perspective trio (takeover spike needs no
// more). Mat4ViewQuake/Mat4ShadowBias/FrustumFromMatrix/AngleVectors/CullBox
// are implemented with the world pass in T2 (plan section 4, Files note).

namespace csz
{

void Mat4Identity( Mat4 &out )
{
	memset( out.m, 0, sizeof( out.m ));
	out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
}

void Mat4Multiply( const Mat4 &a, const Mat4 &b, Mat4 &out )
{
	Mat4 tmp;	// local result so out may alias a or b

	for( int col = 0; col < 4; col++ )
	{
		for( int row = 0; row < 4; row++ )
		{
			tmp.m[col * 4 + row] =
				a.m[0 * 4 + row] * b.m[col * 4 + 0] +
				a.m[1 * 4 + row] * b.m[col * 4 + 1] +
				a.m[2 * 4 + row] * b.m[col * 4 + 2] +
				a.m[3 * 4 + row] * b.m[col * 4 + 3];
		}
	}

	out = tmp;
}

void Mat4Perspective( float fovXDeg, float fovYDeg, float zNear, float zFar, Mat4 &out )
{
	const float kDegToRad = 3.14159265358979323846f / 180.0f;
	float sx = 1.0f / tanf( fovXDeg * kDegToRad * 0.5f );
	float sy = 1.0f / tanf( fovYDeg * kDegToRad * 0.5f );

	memset( out.m, 0, sizeof( out.m ));
	out.m[0] = sx;
	out.m[5] = sy;
	out.m[10] = ( zFar + zNear ) / ( zNear - zFar );
	out.m[11] = -1.0f;
	out.m[14] = ( 2.0f * zFar * zNear ) / ( zNear - zFar );
}

}
