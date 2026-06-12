/*
 * csz_math.h -- CSOZ renderer: matrix/frustum math (column-major, GL convention)
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
struct Mat4 { float m[16]; };          // column-major, OpenGL convention
struct Plane { float normal[3]; float dist; };
struct Frustum
{
	Plane planes[6];                   // left,right,bottom,top,near,far
	int numPlanes;                     // 5 = far plane disabled (spot lights)
	bool CullBox( const float mins[3], const float maxs[3] ) const;  // true = fully outside
};
void Mat4Identity( Mat4 &out );
void Mat4Multiply( const Mat4 &a, const Mat4 &b, Mat4 &out );        // out = a * b
void Mat4Perspective( float fovXDeg, float fovYDeg, float zNear, float zFar, Mat4 &out );
// Quake world -> GL eye: axis fix (rotate -90 deg about X, +90 deg about Z),
// then rotate -roll(X), -pitch(Y), -yaw(Z), then translate by -origin.
void Mat4ViewQuake( const float origin[3], const float anglesDeg[3], Mat4 &out );
// bias(scale 0.5 + offset 0.5) * lightProj * lightView  (shadow lookup matrix)
void Mat4ShadowBias( const Mat4 &lightProj, const Mat4 &lightView, Mat4 &out );
void FrustumFromMatrix( const Mat4 &viewProj, bool disableFar, Frustum &out );
void AngleVectors( const float anglesDeg[3], float fwd[3], float right[3], float up[3] );
}
