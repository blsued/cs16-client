/*
 * csz_math.cpp -- CSOZ renderer: matrix/frustum/angle math implementation
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

namespace csz
{

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// Column-major axis rotations; element (row, col) lives at m[col * 4 + row].
void Mat4RotateX( float deg, Mat4 &out )
{
	float c = cosf( deg * kDegToRad );
	float s = sinf( deg * kDegToRad );

	Mat4Identity( out );
	out.m[5] = c;
	out.m[9] = -s;
	out.m[6] = s;
	out.m[10] = c;
}

void Mat4RotateY( float deg, Mat4 &out )
{
	float c = cosf( deg * kDegToRad );
	float s = sinf( deg * kDegToRad );

	Mat4Identity( out );
	out.m[0] = c;
	out.m[8] = s;
	out.m[2] = -s;
	out.m[10] = c;
}

void Mat4RotateZ( float deg, Mat4 &out )
{
	float c = cosf( deg * kDegToRad );
	float s = sinf( deg * kDegToRad );

	Mat4Identity( out );
	out.m[0] = c;
	out.m[4] = -s;
	out.m[1] = s;
	out.m[5] = c;
}

}

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
	float sx = 1.0f / tanf( fovXDeg * kDegToRad * 0.5f );
	float sy = 1.0f / tanf( fovYDeg * kDegToRad * 0.5f );

	memset( out.m, 0, sizeof( out.m ));
	out.m[0] = sx;
	out.m[5] = sy;
	out.m[10] = ( zFar + zNear ) / ( zNear - zFar );
	out.m[11] = -1.0f;
	out.m[14] = ( 2.0f * zFar * zNear ) / ( zNear - zFar );
}

void Mat4ViewQuake( const float origin[3], const float anglesDeg[3], Mat4 &out )
{
	// Quake world (X forward, Y left, Z up) -> GL eye (X right, Y up, -Z
	// forward): the classic fixed-function sequence, composed left to right
	// exactly as consecutive glRotatef/glTranslatef calls post-multiply.
	// Quake angle order: anglesDeg[0]=pitch, [1]=yaw, [2]=roll.
	Mat4 m, r;

	Mat4RotateX( -90.0f, m );
	Mat4RotateZ( 90.0f, r );
	Mat4Multiply( m, r, m );
	Mat4RotateX( -anglesDeg[2], r );	// -roll
	Mat4Multiply( m, r, m );
	Mat4RotateY( -anglesDeg[0], r );	// -pitch
	Mat4Multiply( m, r, m );
	Mat4RotateZ( -anglesDeg[1], r );	// -yaw
	Mat4Multiply( m, r, m );

	Mat4Identity( r );
	r.m[12] = -origin[0];
	r.m[13] = -origin[1];
	r.m[14] = -origin[2];
	Mat4Multiply( m, r, out );
}

void Mat4ShadowBias( const Mat4 &lightProj, const Mat4 &lightView, Mat4 &out )
{
	// bias = scale(0.5) then offset(0.5): maps clip [-1,1] to texture [0,1].
	Mat4 bias;

	Mat4Identity( bias );
	bias.m[0] = bias.m[5] = bias.m[10] = 0.5f;
	bias.m[12] = bias.m[13] = bias.m[14] = 0.5f;

	Mat4 pv;
	Mat4Multiply( lightProj, lightView, pv );
	Mat4Multiply( bias, pv, out );
}

void FrustumFromMatrix( const Mat4 &viewProj, bool disableFar, Frustum &out )
{
	// Gribb-Hartmann plane extraction. For column-major m, matrix row i is
	// (m[i], m[4+i], m[8+i], m[12+i]). Plane equation: dot(normal, p) + dist
	// >= 0 means inside. Order: left, right, bottom, top, near, far.
	float rows[4][4];

	for( int i = 0; i < 4; i++ )
	{
		rows[i][0] = viewProj.m[0 + i];
		rows[i][1] = viewProj.m[4 + i];
		rows[i][2] = viewProj.m[8 + i];
		rows[i][3] = viewProj.m[12 + i];
	}

	const int kSigns[6] = { +1, -1, +1, -1, +1, -1 };	// row3 +/- row{0,0,1,1,2,2}
	const int kRows[6] = { 0, 0, 1, 1, 2, 2 };

	for( int p = 0; p < 6; p++ )
	{
		float plane[4];

		for( int j = 0; j < 4; j++ )
			plane[j] = rows[3][j] + (float)kSigns[p] * rows[kRows[p]][j];

		float len = sqrtf( plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2] );

		if( len > 1e-6f )
		{
			float inv = 1.0f / len;

			for( int j = 0; j < 4; j++ )
				plane[j] *= inv;
		}

		out.planes[p].normal[0] = plane[0];
		out.planes[p].normal[1] = plane[1];
		out.planes[p].normal[2] = plane[2];
		out.planes[p].dist = plane[3];
	}

	out.numPlanes = disableFar ? 5 : 6;
}

bool Frustum::CullBox( const float mins[3], const float maxs[3] ) const
{
	// Positive-vertex test: if the box corner farthest along the plane
	// normal is still outside, the whole box is outside.
	for( int p = 0; p < numPlanes; p++ )
	{
		const Plane &pl = planes[p];
		float d = 0.0f;

		for( int j = 0; j < 3; j++ )
			d += pl.normal[j] * (( pl.normal[j] >= 0.0f ) ? maxs[j] : mins[j] );

		if( d + pl.dist < 0.0f )
			return true;
	}

	return false;
}

void AngleVectors( const float anglesDeg[3], float fwd[3], float right[3], float up[3] )
{
	// Adapted from this fork's own pm_shared/pm_math.cpp AngleVectors
	// (HLSDK lineage, sanctioned source). anglesDeg[0]=pitch, [1]=yaw,
	// [2]=roll.
	float sp = sinf( anglesDeg[0] * kDegToRad ), cp = cosf( anglesDeg[0] * kDegToRad );
	float sy = sinf( anglesDeg[1] * kDegToRad ), cy = cosf( anglesDeg[1] * kDegToRad );
	float sr = sinf( anglesDeg[2] * kDegToRad ), cr = cosf( anglesDeg[2] * kDegToRad );

	if( fwd != NULL )
	{
		fwd[0] = cp * cy;
		fwd[1] = cp * sy;
		fwd[2] = -sp;
	}

	if( right != NULL )
	{
		right[0] = -sr * sp * cy + cr * sy;
		right[1] = -sr * sp * sy - cr * cy;
		right[2] = -sr * cp;
	}

	if( up != NULL )
	{
		up[0] = cr * sp * cy + sr * sy;
		up[1] = cr * sp * sy - sr * cy;
		up[2] = cr * cp;
	}
}

}
