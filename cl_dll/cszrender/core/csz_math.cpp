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
// Each builds the same [c -s; s c] 2x2 block in the (a,b) coordinate plane of an
// identity matrix; the per-axis wrappers just pick the plane.
void Mat4AxisRotate( float deg, int a, int b, Mat4 &out )
{
	float c = cosf( deg * kDegToRad );
	float s = sinf( deg * kDegToRad );

	Mat4Identity( out );
	out.m[a * 4 + a] = c;
	out.m[b * 4 + b] = c;
	out.m[b * 4 + a] = -s;
	out.m[a * 4 + b] = s;
}

void Mat4RotateX( float deg, Mat4 &out ) { Mat4AxisRotate( deg, 1, 2, out ); }
void Mat4RotateY( float deg, Mat4 &out ) { Mat4AxisRotate( deg, 2, 0, out ); }
void Mat4RotateZ( float deg, Mat4 &out ) { Mat4AxisRotate( deg, 0, 1, out ); }

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

// General 4x4 inverse via the adjugate (cofactor) method. Column-major: element
// (row, col) lives at m[col * 4 + row]. The cofactor expansion below is layout-
// agnostic -- it computes the true inverse of the matrix the flat array
// represents under the OpenGL column-major convention. Returns false on a
// (near-)singular matrix and leaves out untouched, so callers can guard.
bool Mat4Inverse( const Mat4 &in, Mat4 &out )
{
	// Accumulate in double: the perspective viewProj (zNear=4, zFar=16384) is
	// ill-conditioned (huge z dynamic range), and float32 cofactor sums lose
	// precision. Done once per frame on the CPU, so the cost is irrelevant.
	double m[16];
	for( int i = 0; i < 16; i++ )
		m[i] = (double)in.m[i];
	double inv[16];

	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
	         + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
	         - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
	         + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
	         - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
	         - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
	         + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
	         - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
	         + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
	         + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
	         - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
	         + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
	         - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
	inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
	         - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
	         + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
	         - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
	         + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

	double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if( det > -1e-12 && det < 1e-12 )
		return false;

	double invDet = 1.0 / det;
	for( int i = 0; i < 16; i++ )
		out.m[i] = (float)( inv[i] * invDet );

	return true;
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
	// The Quake->GL axis fix (RotateX(-90) * RotateZ(90)) is a compile-time
	// invariant; build it once and reuse.
	static Mat4 s_axisFix;
	static bool s_axisFixReady = false;
	if( !s_axisFixReady )
	{
		Mat4 a, b;
		Mat4RotateX( -90.0f, a );
		Mat4RotateZ( 90.0f, b );
		Mat4Multiply( a, b, s_axisFix );
		s_axisFixReady = true;
	}

	Mat4 m = s_axisFix, r;
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
	// Invariant; build once.
	static Mat4 s_bias;
	static bool s_biasReady = false;
	if( !s_biasReady )
	{
		Mat4Identity( s_bias );
		s_bias.m[0] = s_bias.m[5] = s_bias.m[10] = 0.5f;
		s_bias.m[12] = s_bias.m[13] = s_bias.m[14] = 0.5f;
		s_biasReady = true;
	}

	Mat4 pv;
	Mat4Multiply( lightProj, lightView, pv );
	Mat4Multiply( s_bias, pv, out );
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
