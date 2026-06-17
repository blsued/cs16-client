/*
 * csz_studio_bones.cpp -- CSOZ renderer: CPU studio bone setup (anim + gait)
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
 * Bone math adapted from this fork's own StudioModelRenderer.cpp /
 * GameStudioModelRenderer.cpp / studio_util.cpp / pm_shared/pm_math.cpp
 * (HLSDK lineage, sanctioned source per plan section 6). Differences from
 * stock are deliberate M1 reductions: no bone controllers, no sequence
 * transition fade, no latched-state interpolation, seqgroup 0 only, and
 * NO mutation of cl_entity_t/curstate (gait state lives in this file).
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
#include "csz_studio_bones.h"
#include "../core/csz_engine.h"
#include "../core/csz_log.h"

#include <math.h>
#include <string.h>

namespace csz
{

namespace
{

const float kPiF = 3.14159265358979323846f;
const float kDegToRad = kPiF / 180.0f;

// CS player model sequence map (same numeric ranges as this fork's
// GameStudioModelRenderer.cpp): these sequences must not get the gait leg
// overlay, and the walk gait needs a yaw-blend bias.
const int kAnimWalkSequence = 3;
const int kAnimSwim1 = 8;
const int kAnimSwim2 = 9;
const int kAnimFirstDeathSequence = 101;
const int kAnimLastDeathSequence = 159;
const int kAnimFirstEmotionSequence = 198;
const int kAnimLastEmotionSequence = 207;

inline float ClampF( float v, float lo, float hi )
{
	return ( v < lo ) ? lo : (( v > hi ) ? hi : v );
}

inline int ClampI( int v, int lo, int hi )
{
	return ( v < lo ) ? lo : (( v > hi ) ? hi : v );
}

bool NameEq( const char *a, const char *b )
{
	while( *a != '\0' && *b != '\0' )
	{
		char ca = ( *a >= 'A' && *a <= 'Z' ) ? (char)( *a + 32 ) : *a;
		char cb = ( *b >= 'A' && *b <= 'Z' ) ? (char)( *b + 32 ) : *b;

		if( ca != cb )
			return false;

		a++;
		b++;
	}

	return *a == *b;
}

// ---------------------------------------------------------------------------
// Quaternion / 3x4 matrix helpers (adapted from studio_util.cpp, HLSDK
// lineage). All angles in RADIANS unless the name says Deg.
// ---------------------------------------------------------------------------
void AngleQuaternionRad( const float angles[3], float q[4] )
{
	float sy = sinf( angles[2] * 0.5f ), cy = cosf( angles[2] * 0.5f );
	float sp = sinf( angles[1] * 0.5f ), cp = cosf( angles[1] * 0.5f );
	float sr = sinf( angles[0] * 0.5f ), cr = cosf( angles[0] * 0.5f );

	q[0] = sr * cp * cy - cr * sp * sy;	// X
	q[1] = cr * sp * cy + sr * cp * sy;	// Y
	q[2] = cr * cp * sy - sr * sp * cy;	// Z
	q[3] = cr * cp * cy + sr * sp * sy;	// W
}

// Unlike the stock helper this never mutates its inputs (the sign flip works
// on a local copy), so cached blend sources stay pristine across passes.
void QuaternionSlerpLocal( const float p[4], const float qIn[4], float t, float qt[4] )
{
	float q[4] = { qIn[0], qIn[1], qIn[2], qIn[3] };
	float a = 0.0f, b = 0.0f;
	int i;

	for( i = 0; i < 4; i++ )
	{
		a += ( p[i] - q[i] ) * ( p[i] - q[i] );
		b += ( p[i] + q[i] ) * ( p[i] + q[i] );
	}

	if( a > b )
	{
		for( i = 0; i < 4; i++ )
			q[i] = -q[i];
	}

	float cosom = p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3];

	if( 1.0f + cosom > 0.000001f )
	{
		float sclp, sclq;

		if( 1.0f - cosom > 0.000001f )
		{
			float omega = acosf( cosom );
			float sinom = sinf( omega );

			sclp = sinf(( 1.0f - t ) * omega ) / sinom;
			sclq = sinf( t * omega ) / sinom;
		}
		else
		{
			sclp = 1.0f - t;
			sclq = t;
		}

		for( i = 0; i < 4; i++ )
			qt[i] = sclp * p[i] + sclq * q[i];
	}
	else
	{
		float r[4] = { -q[1], q[0], -q[3], q[2] };
		float sclp = sinf(( 1.0f - t ) * 0.5f * kPiF );
		float sclq = sinf( t * 0.5f * kPiF );

		for( i = 0; i < 3; i++ )
			qt[i] = sclp * p[i] + sclq * r[i];

		qt[3] = r[3];
	}
}

void QuaternionMatrixLocal( const float q[4], float m[3][4] )
{
	m[0][0] = 1.0f - 2.0f * q[1] * q[1] - 2.0f * q[2] * q[2];
	m[1][0] = 2.0f * q[0] * q[1] + 2.0f * q[3] * q[2];
	m[2][0] = 2.0f * q[0] * q[2] - 2.0f * q[3] * q[1];

	m[0][1] = 2.0f * q[0] * q[1] - 2.0f * q[3] * q[2];
	m[1][1] = 1.0f - 2.0f * q[0] * q[0] - 2.0f * q[2] * q[2];
	m[2][1] = 2.0f * q[1] * q[2] + 2.0f * q[3] * q[0];

	m[0][2] = 2.0f * q[0] * q[2] + 2.0f * q[3] * q[1];
	m[1][2] = 2.0f * q[1] * q[2] - 2.0f * q[3] * q[0];
	m[2][2] = 1.0f - 2.0f * q[0] * q[0] - 2.0f * q[1] * q[1];
}

void ConcatTransformsLocal( const float in1[3][4], const float in2[3][4], float out[3][4] )
{
	for( int r = 0; r < 3; r++ )
	{
		out[r][0] = in1[r][0] * in2[0][0] + in1[r][1] * in2[1][0] + in1[r][2] * in2[2][0];
		out[r][1] = in1[r][0] * in2[0][1] + in1[r][1] * in2[1][1] + in1[r][2] * in2[2][1];
		out[r][2] = in1[r][0] * in2[0][2] + in1[r][1] * in2[1][2] + in1[r][2] * in2[2][2];
		out[r][3] = in1[r][0] * in2[0][3] + in1[r][1] * in2[1][3] + in1[r][2] * in2[2][3] + in1[r][3];
	}
}

// Entity rotation matrix from quake degrees (adapted from pm_math.cpp
// AngleMatrix; anglesDeg[0]=pitch, [1]=yaw, [2]=roll).
void AngleMatrixDeg( const float anglesDeg[3], float m[3][4] )
{
	float sy = sinf( anglesDeg[2] * kDegToRad ), cy = cosf( anglesDeg[2] * kDegToRad );	// roll
	float sp = sinf( anglesDeg[1] * kDegToRad ), cp = cosf( anglesDeg[1] * kDegToRad );	// yaw
	float sr = sinf( anglesDeg[0] * kDegToRad ), cr = cosf( anglesDeg[0] * kDegToRad );	// pitch

	m[0][0] = cr * cp;
	m[1][0] = cr * sp;
	m[2][0] = -sr;

	m[0][1] = ( sy * sr ) * cp - cy * sp;
	m[1][1] = ( sy * sr ) * sp + cy * cp;
	m[2][1] = sy * cr;

	m[0][2] = ( cy * sr ) * cp + sy * sp;
	m[1][2] = ( cy * sr ) * sp - sy * cp;
	m[2][2] = cy * cr;

	m[0][3] = 0.0f;
	m[1][3] = 0.0f;
	m[2][3] = 0.0f;
}

// ---------------------------------------------------------------------------
// RLE animation decode (adapted from StudioModelRenderer.cpp
// StudioCalcBoneQuaterion / StudioCalcBonePosition; bone controllers
// dropped on purpose, M1).
// ---------------------------------------------------------------------------
void CalcBoneQuaternion( int frame, float s, const mstudiobone_t *pbone,
	const mstudioanim_t *panim, float q[4] )
{
	float angle1[3], angle2[3];

	for( int j = 0; j < 3; j++ )
	{
		if( panim->offset[j + 3] == 0 )
		{
			angle2[j] = angle1[j] = pbone->value[j + 3];
			continue;
		}

		const mstudioanimvalue_t *pv = (const mstudioanimvalue_t *)((const byte *)panim + panim->offset[j + 3] );
		int k = frame;

		if( pv->num.total < pv->num.valid )
			k = 0;

		while( pv->num.total <= k )
		{
			k -= pv->num.total;
			pv += pv->num.valid + 1;

			if( pv->num.total < pv->num.valid )
				k = 0;
		}

		if( pv->num.valid > k )
		{
			angle1[j] = pv[k + 1].value;

			if( pv->num.valid > k + 1 )
			{
				angle2[j] = pv[k + 2].value;
			}
			else
			{
				if( pv->num.total > k + 1 )
					angle2[j] = angle1[j];
				else
					angle2[j] = pv[pv->num.valid + 2].value;
			}
		}
		else
		{
			angle1[j] = pv[pv->num.valid].value;

			if( pv->num.total > k + 1 )
				angle2[j] = angle1[j];
			else
				angle2[j] = pv[pv->num.valid + 2].value;
		}

		angle1[j] = pbone->value[j + 3] + angle1[j] * pbone->scale[j + 3];
		angle2[j] = pbone->value[j + 3] + angle2[j] * pbone->scale[j + 3];
	}

	if( angle1[0] != angle2[0] || angle1[1] != angle2[1] || angle1[2] != angle2[2] )
	{
		float q1[4], q2[4];

		AngleQuaternionRad( angle1, q1 );
		AngleQuaternionRad( angle2, q2 );
		QuaternionSlerpLocal( q1, q2, s, q );
	}
	else
	{
		AngleQuaternionRad( angle1, q );
	}
}

void CalcBonePosition( int frame, float s, const mstudiobone_t *pbone,
	const mstudioanim_t *panim, float pos[3] )
{
	for( int j = 0; j < 3; j++ )
	{
		pos[j] = pbone->value[j];

		if( panim->offset[j] == 0 )
			continue;

		const mstudioanimvalue_t *pv = (const mstudioanimvalue_t *)((const byte *)panim + panim->offset[j] );
		int k = frame;

		if( pv->num.total < pv->num.valid )
			k = 0;

		while( pv->num.total <= k )
		{
			k -= pv->num.total;
			pv += pv->num.valid + 1;

			if( pv->num.total < pv->num.valid )
				k = 0;
		}

		if( pv->num.valid > k )
		{
			if( pv->num.valid > k + 1 )
				pos[j] += ( pv[k + 1].value * ( 1.0f - s ) + s * pv[k + 2].value ) * pbone->scale[j];
			else
				pos[j] += pv[k + 1].value * pbone->scale[j];
		}
		else
		{
			if( pv->num.total <= k + 1 )
				pos[j] += ( pv[pv->num.valid].value * ( 1.0f - s ) + s * pv[pv->num.valid + 2].value ) * pbone->scale[j];
			else
				pos[j] += pv[pv->num.valid].value * pbone->scale[j];
		}
	}
}

int NumBonesClamped( const studiohdr_t *hdr )
{
	int n = hdr->numbones;

	if( n > kMaxGpuBones )
	{
		static bool s_warned;

		if( !s_warned )
		{
			s_warned = true;
			CSZ_LogWarn( "studio", "%s has %d bones; clamping to %d", hdr->name, n, kMaxGpuBones );
		}

		// .mdl bones are stored parent-first, so truncating drops leaves only.
		n = kMaxGpuBones;
	}

	return n;
}

// Whole-skeleton pose for ONE blend animation at fractional frame f
// (adapted StudioCalcRotations; controller adjustments dropped, dead linear
// movement term dropped).
void CalcRotations( const studiohdr_t *hdr, const mstudioseqdesc_t *pseqdesc,
	const mstudioanim_t *panim, float f, float pos[][3], float q[][4] )
{
	if( f > pseqdesc->numframes - 1 )
		f = 0.0f;
	else if( f < -0.01f )
		f = -0.01f;

	int frame = (int)f;
	float s = f - (float)frame;
	int numBones = NumBonesClamped( hdr );
	const mstudiobone_t *pbone = (const mstudiobone_t *)((const byte *)hdr + hdr->boneindex );

	for( int i = 0; i < numBones; i++, pbone++, panim++ )
	{
		CalcBoneQuaternion( frame, s, pbone, panim, q[i] );
		CalcBonePosition( frame, s, pbone, panim, pos[i] );
	}

	if( pseqdesc->motionbone >= 0 && pseqdesc->motionbone < numBones )
	{
		if( pseqdesc->motiontype & STUDIO_X )
			pos[pseqdesc->motionbone][0] = 0.0f;
		if( pseqdesc->motiontype & STUDIO_Y )
			pos[pseqdesc->motionbone][1] = 0.0f;
		if( pseqdesc->motiontype & STUDIO_Z )
			pos[pseqdesc->motionbone][2] = 0.0f;
	}
}

// Bind pose fallback (used when the sequence lives in a demand-loaded
// sequence group, which M1 does not support).
void CalcBindPose( const studiohdr_t *hdr, float pos[][3], float q[][4] )
{
	int numBones = NumBonesClamped( hdr );
	const mstudiobone_t *pbone = (const mstudiobone_t *)((const byte *)hdr + hdr->boneindex );

	for( int i = 0; i < numBones; i++, pbone++ )
	{
		float ang[3] = { pbone->value[3], pbone->value[4], pbone->value[5] };

		pos[i][0] = pbone->value[0];
		pos[i][1] = pbone->value[1];
		pos[i][2] = pbone->value[2];
		AngleQuaternionRad( ang, q[i] );
	}
}

void SlerpBones( int numBones, float q1[][4], float pos1[][3],
	const float q2[][4], const float pos2[][3], float s )
{
	s = ClampF( s, 0.0f, 1.0f );

	float s1 = 1.0f - s;

	for( int i = 0; i < numBones; i++ )
	{
		float q3[4];

		QuaternionSlerpLocal( q1[i], q2[i], s, q3 );
		q1[i][0] = q3[0];
		q1[i][1] = q3[1];
		q1[i][2] = q3[2];
		q1[i][3] = q3[3];
		pos1[i][0] = pos1[i][0] * s1 + pos2[i][0] * s;
		pos1[i][1] = pos1[i][1] * s1 + pos2[i][1] * s;
		pos1[i][2] = pos1[i][2] * s1 + pos2[i][2] * s;
	}
}

// Sequence group 0 only (all CS content); demand-loaded groups are an M2 gap
// (progress-t3.md decision 1). NULL means "use bind pose".
const mstudioanim_t *GetAnim( const studiohdr_t *hdr, const mstudioseqdesc_t *pseqdesc )
{
	if( pseqdesc->seqgroup != 0 )
	{
		static bool s_warned;

		if( !s_warned )
		{
			s_warned = true;
			CSZ_LogWarn( "studio", "%s uses demand-loaded seqgroup %d (unsupported in M1); bind pose",
				hdr->name, pseqdesc->seqgroup );
		}

		return NULL;
	}

	return (const mstudioanim_t *)((const byte *)hdr + pseqdesc->animindex );
}

// ---------------------------------------------------------------------------
// Player gait state, cszrender-private (never touches engine player_info_t
// or entity state). Slot = entity index (players are 1..maxclients).
// ---------------------------------------------------------------------------
struct GaitState
{
	bool valid;
	int frameStamp;		// engine framecount of last update (per-frame dedup)
	float yaw;		// smoothed movement yaw, degrees
	float frame;		// gait sequence frame cursor
	float prevOrigin[3];
	float yawBlend;		// 0..255 (blending[0] equivalent)
	float pitchBlend;	// 0..255 (blending[1] equivalent)
};

const int kMaxGaitSlots = 65;	// entity indices 1..64 cover any maxclients
GaitState s_gait[kMaxGaitSlots];

GaitState *GaitSlot( const cl_entity_s *ent )
{
	int idx = ClampI( ent->index, 0, kMaxGaitSlots - 1 );

	return &s_gait[idx];
}

// Gait estimation + yaw/pitch blend (adapted from this fork's
// CGameStudioModelRenderer::StudioEstimateGait / CalculateYawBlend /
// CalculatePitchBlend / StudioProcessGait, gait-estimation mode). Runs once
// per engine frame per player; later calls in the same frame reuse results.
GaitState *UpdateGait( const cl_entity_s *ent, const studiohdr_t *hdr )
{
	GaitState *gs = GaitSlot( ent );
	int framecount = 0;
	double clTime = 0.0, clOldTime = 0.0;

	IEngineStudio.GetTimes( &framecount, &clTime, &clOldTime );

	if( !gs->valid )
	{
		gs->valid = true;
		gs->frameStamp = -1;
		gs->yaw = ent->angles[1];
		gs->frame = 0.0f;
		gs->prevOrigin[0] = ent->origin[0];
		gs->prevOrigin[1] = ent->origin[1];
		gs->prevOrigin[2] = ent->origin[2];
		gs->yawBlend = 127.0f;
		gs->pitchBlend = 127.0f;
	}

	if( gs->frameStamp == framecount )
		return gs;

	float dt = ClampF((float)( clTime - clOldTime ), 0.0f, 1.0f );

	if( dt <= 0.0f )
		return gs;	// paused frame: keep previous gait state untouched

	gs->frameStamp = framecount;

	// --- movement estimation from origin deltas ---
	float est[3];

	est[0] = ent->origin[0] - gs->prevOrigin[0];
	est[1] = ent->origin[1] - gs->prevOrigin[1];
	est[2] = ent->origin[2] - gs->prevOrigin[2];
	gs->prevOrigin[0] = ent->origin[0];
	gs->prevOrigin[1] = ent->origin[1];
	gs->prevOrigin[2] = ent->origin[2];

	float movement = sqrtf( est[0] * est[0] + est[1] * est[1] + est[2] * est[2] );

	if( movement / dt < 5.0f )
	{
		movement = 0.0f;
		est[0] = 0.0f;
		est[1] = 0.0f;
	}

	if( est[0] == 0.0f && est[1] == 0.0f )
	{
		// standing: decay gait yaw toward the entity yaw
		float yawDiff = ent->angles[1] - gs->yaw;

		yawDiff = yawDiff - (int)( yawDiff / 360.0f ) * 360.0f;

		if( yawDiff > 180.0f )
			yawDiff -= 360.0f;
		if( yawDiff < -180.0f )
			yawDiff += 360.0f;

		if( dt < 0.25f )
			yawDiff *= dt * 4.0f;
		else
			yawDiff *= dt;

		gs->yaw += yawDiff;
		gs->yaw = gs->yaw - (int)( gs->yaw / 360.0f ) * 360.0f;
		movement = 0.0f;
	}
	else
	{
		gs->yaw = atan2f( est[1], est[0] ) * 180.0f / kPiF;

		if( gs->yaw > 180.0f )
			gs->yaw = 180.0f;
		if( gs->yaw < -180.0f )
			gs->yaw = -180.0f;
	}

	// --- yaw blend (run backwards when facing away from movement) ---
	float flYaw = fmodf( ent->angles[1] - gs->yaw, 360.0f );

	if( flYaw < -180.0f )
		flYaw += 360.0f;
	else if( flYaw > 180.0f )
		flYaw -= 360.0f;

	if( flYaw > 120.0f )
	{
		gs->yaw -= 180.0f;
		movement = -movement;
		flYaw -= 180.0f;
	}
	else if( flYaw < -120.0f )
	{
		gs->yaw += 180.0f;
		movement = -movement;
		flYaw += 180.0f;
	}

	float blendYaw = ( flYaw / 90.0f ) * 128.0f + 127.0f;

	gs->yawBlend = 255.0f - ClampF( blendYaw, 0.0f, 255.0f );

	// --- pitch blend (CS StudioPlayerBlend: range 45, pitch zeroed) ---
	float pb = ent->angles[0] * 3.0f;

	if( pb <= -45.0f )
		gs->pitchBlend = 255.0f;
	else if( pb >= 45.0f )
		gs->pitchBlend = 0.0f;
	else
		gs->pitchBlend = 255.0f * ( 45.0f - pb ) / 90.0f;

	// --- gait frame advance ---
	int gaitseq = ent->curstate.gaitsequence;

	if( gaitseq > 0 && gaitseq < hdr->numseq )
	{
		const mstudioseqdesc_t *pseqdesc = SeqDesc( hdr, gaitseq );

		if( pseqdesc->numframes > 0 )
		{
			if( pseqdesc->linearmovement[0] > 0.0f )
				gs->frame += ( movement / pseqdesc->linearmovement[0] ) * pseqdesc->numframes;
			else
				gs->frame += pseqdesc->fps * dt * ent->curstate.framerate;

			gs->frame = gs->frame - (int)( gs->frame / pseqdesc->numframes ) * pseqdesc->numframes;

			if( gs->frame < 0.0f )
				gs->frame += pseqdesc->numframes;
		}
	}

	return gs;
}

// Entity-space rotation angles for the model transform (adapted
// StudioSetUpTransform conventions). Players use the gait yaw with pitch
// folded into the blend axis; everything else uses entity angles with the
// quake pitch sign flip.
void TransformAngles( const cl_entity_s *ent, float anglesDeg[3] )
{
	if( ent->player )
	{
		const GaitState *gs = GaitSlot( ent );
		float yaw = ( gs->valid ) ? gs->yaw : ent->angles[1];

		if( yaw < 0.0f )
			yaw += 360.0f;

		anglesDeg[0] = 0.0f;
		anglesDeg[1] = yaw;
		anglesDeg[2] = ent->angles[2];
		return;
	}

	if( ent->curstate.movetype != MOVETYPE_NONE )
	{
		anglesDeg[0] = ent->angles[0];
		anglesDeg[1] = ent->angles[1];
		anglesDeg[2] = ent->angles[2];
	}
	else
	{
		anglesDeg[0] = ent->curstate.angles[0];
		anglesDeg[1] = ent->curstate.angles[1];
		anglesDeg[2] = ent->curstate.angles[2];
	}

	anglesDeg[0] = -anglesDeg[0];	// quake model pitch sign flip
}

// ---------------------------------------------------------------------------
// Pose evaluation scratch (single render thread, stock-renderer pattern).
// ---------------------------------------------------------------------------
float s_pos[kMaxGpuBones][3];
float s_q[kMaxGpuBones][4];
float s_pos2[kMaxGpuBones][3];
float s_q2[kMaxGpuBones][4];
float s_pos3[kMaxGpuBones][3];
float s_q3[kMaxGpuBones][4];
float s_pos4[kMaxGpuBones][3];
float s_q4[kMaxGpuBones][4];
float s_world[kMaxGpuBones][3][4];

// Blend-animation pointer for blend index i (panim is laid out as numblends
// consecutive bone tables).
const mstudioanim_t *BlendAnim( const studiohdr_t *hdr, const mstudioanim_t *panim, int blend )
{
	return panim + blend * hdr->numbones;
}

// Evaluates the local pose (s_pos/s_q) for the entity's current sequence,
// including the CS 9-way aim blend and the player gait leg overlay.
void EvaluatePose( cl_entity_s *ent, const studiohdr_t *hdr, float time )
{
	int numBones = NumBonesClamped( hdr );
	int seq = ent->curstate.sequence;

	// Out-of-range sequences reset to 0 -- engine parity (pinned
	// ref/gl/gl_studio.c StudioSetupBones and this fork's GameStudio
	// renderers all do `seq >= numseq -> 0`). Clamping to numseq-1 instead
	// picked the LAST sequence, which on CS player models is 110
	// "crouch_die": servers transiently network sequence=255 for live
	// players, and the clamp rendered them lying flat with the gait leg
	// overlay suppressed by the death-range gate below.
	if( seq < 0 || seq >= hdr->numseq )
		seq = 0;

	const mstudioseqdesc_t *pseqdesc = SeqDesc( hdr, seq );
	const mstudioanim_t *panim = GetAnim( hdr, pseqdesc );

	if( panim == NULL )
	{
		CalcBindPose( hdr, s_pos, s_q );
		return;
	}

	float f = EstimateFrame( pseqdesc, ent, time );

	// Blend axis values: players get locally computed gait yaw/pitch blends,
	// other entities use the networked blending bytes.
	const GaitState *gs = NULL;
	float blendS, blendT;

	if( ent->player )
	{
		gs = UpdateGait( ent, hdr );
		blendS = gs->yawBlend;
		blendT = gs->pitchBlend;

		// CS walk-gait yaw blend bias (GameStudioModelRenderer quirk).
		if( ent->curstate.gaitsequence == kAnimWalkSequence )
			blendS = ( blendS <= 26.0f ) ? 0.0f : ( blendS - 26.0f );
	}
	else
	{
		blendS = (float)ent->curstate.blending[0];
		blendT = (float)ent->curstate.blending[1];
	}

	if( pseqdesc->numblends == 9 )
	{
		// 4-corner bilinear pick from the 3x3 aim grid (adapted from this
		// fork's CGameStudioModelRenderer::StudioSetupBones).
		float s = blendS, t = blendT;
		int c0, c1, c2, c3;

		if( s <= 127.0f )
		{
			s = s * 2.0f;

			if( t <= 127.0f )
			{
				t = t * 2.0f;
				c0 = 0; c1 = 1; c2 = 3; c3 = 4;
			}
			else
			{
				t = 2.0f * ( t - 127.0f );
				c0 = 3; c1 = 4; c2 = 6; c3 = 7;
			}
		}
		else
		{
			s = 2.0f * ( s - 127.0f );

			if( t <= 127.0f )
			{
				t = t * 2.0f;
				c0 = 1; c1 = 2; c2 = 4; c3 = 5;
			}
			else
			{
				t = 2.0f * ( t - 127.0f );
				c0 = 4; c1 = 5; c2 = 7; c3 = 8;
			}
		}

		CalcRotations( hdr, pseqdesc, BlendAnim( hdr, panim, c0 ), f, s_pos, s_q );
		CalcRotations( hdr, pseqdesc, BlendAnim( hdr, panim, c1 ), f, s_pos2, s_q2 );
		CalcRotations( hdr, pseqdesc, BlendAnim( hdr, panim, c2 ), f, s_pos3, s_q3 );
		CalcRotations( hdr, pseqdesc, BlendAnim( hdr, panim, c3 ), f, s_pos4, s_q4 );

		s /= 255.0f;
		t /= 255.0f;

		SlerpBones( numBones, s_q, s_pos, s_q2, s_pos2, s );
		SlerpBones( numBones, s_q3, s_pos3, s_q4, s_pos4, s );
		SlerpBones( numBones, s_q, s_pos, s_q3, s_pos3, t );
	}
	else if( pseqdesc->numblends > 1 )
	{
		CalcRotations( hdr, pseqdesc, panim, f, s_pos, s_q );
		CalcRotations( hdr, pseqdesc, BlendAnim( hdr, panim, 1 ), f, s_pos2, s_q2 );
		SlerpBones( numBones, s_q, s_pos, s_q2, s_pos2, blendS / 255.0f );
	}
	else
	{
		CalcRotations( hdr, pseqdesc, panim, f, s_pos, s_q );
	}

	// --- player gait leg overlay ---
	if( gs == NULL )
		return;

	int gaitseq = ent->curstate.gaitsequence;

	if( gaitseq <= 0 || gaitseq >= hdr->numseq )
		return;

	// Death/emotion/swim sequences own the whole body (no leg overlay).
	if(( seq >= kAnimFirstDeathSequence && seq <= kAnimLastDeathSequence ) ||
		( seq >= kAnimFirstEmotionSequence && seq <= kAnimLastEmotionSequence ) ||
		seq == kAnimSwim1 || seq == kAnimSwim2 )
		return;

	const mstudioseqdesc_t *pgait = SeqDesc( hdr, gaitseq );
	const mstudioanim_t *pganim = GetAnim( hdr, pgait );

	if( pganim == NULL )
		return;

	CalcRotations( hdr, pgait, pganim, gs->frame, s_pos2, s_q2 );

	// Copy pelvis-and-below bones from the gait pose: copy until "Bip01
	// Spine" starts the upper body, re-enable for direct pelvis children
	// (legs) -- same bone-name boundary as the fork's GameStudio renderer.
	const mstudiobone_t *pbones = (const mstudiobone_t *)((const byte *)hdr + hdr->boneindex );
	int copy = 1;

	for( int i = 0; i < numBones; i++ )
	{
		if( NameEq( pbones[i].name, "Bip01 Spine" ))
			copy = 0;
		else if( pbones[i].parent >= 0 && pbones[i].parent < numBones &&
			NameEq( pbones[pbones[i].parent].name, "Bip01 Pelvis" ))
			copy = 1;

		if( copy )
		{
			s_pos[i][0] = s_pos2[i][0];
			s_pos[i][1] = s_pos2[i][1];
			s_pos[i][2] = s_pos2[i][2];
			s_q[i][0] = s_q2[i][0];
			s_q[i][1] = s_q2[i][1];
			s_q[i][2] = s_q2[i][2];
			s_q[i][3] = s_q2[i][3];
		}
	}
}

// Right-hand viewmodel mirror decision. CS v_ models are authored
// left-handed; with cl_righthand > 0 (the CS default) the stock client
// mirrors the viewmodel across the entity XZ plane by negating the Y row of
// the ROOT bone matrix (adapted from this fork's StudioModelRenderer.cpp
// StudioSetupBones, HLSDK lineage). Knife and shield viewmodels are authored
// the opposite way, so the stock client inverts the flip for them
// (StudioModelRenderer.cpp StudioDrawModel; the CZERO game-type exception is
// skipped here -- M1 assumes CS 1.6 content). The mirror reverses triangle
// winding; the draw side renders mirrored setups with culling off.
bool ViewModelMirror( const cl_entity_s *ent, const studiohdr_t *hdr )
{
	if( ent != gEngfuncs.GetViewModel())
		return false;

	static cvar_t *s_rightHand;
	static bool s_queried;

	if( !s_queried )
	{
		s_queried = true;
		s_rightHand = gEngfuncs.pfnGetCvarPointer( "cl_righthand" );
	}

	bool mirror = ( s_rightHand != NULL && s_rightHand->value > 0.0f );

	if( g_bHoldingKnife || strstr( hdr->name, "shield" ) != NULL )
		mirror = !mirror;

	return mirror;
}

// Local pose (s_pos/s_q) -> world 3x4 chain (s_world) -> BoneSetup rows.
void BuildWorldBones( const cl_entity_s *ent, const studiohdr_t *hdr, BoneSetup &out )
{
	int numBones = NumBonesClamped( hdr );
	const mstudiobone_t *pbones = (const mstudiobone_t *)((const byte *)hdr + hdr->boneindex );
	bool mirror = ViewModelMirror( ent, hdr );
	float anglesDeg[3];
	float rot[3][4];

	TransformAngles( ent, anglesDeg );
	AngleMatrixDeg( anglesDeg, rot );
	rot[0][3] = ent->origin[0];
	rot[1][3] = ent->origin[1];
	rot[2][3] = ent->origin[2];

	for( int i = 0; i < numBones; i++ )
	{
		float bonematrix[3][4];

		QuaternionMatrixLocal( s_q[i], bonematrix );
		bonematrix[0][3] = s_pos[i][0];
		bonematrix[1][3] = s_pos[i][1];
		bonematrix[2][3] = s_pos[i][2];

		if( pbones[i].parent < 0 || pbones[i].parent >= numBones )
		{
			if( mirror )
			{
				// Root-bone Y-row negation = mirror across the entity
				// XZ plane (right-hand flip, see ViewModelMirror).
				bonematrix[1][0] = -bonematrix[1][0];
				bonematrix[1][1] = -bonematrix[1][1];
				bonematrix[1][2] = -bonematrix[1][2];
				bonematrix[1][3] = -bonematrix[1][3];
			}

			ConcatTransformsLocal( rot, bonematrix, s_world[i] );
		}
		else
		{
			ConcatTransformsLocal( s_world[pbones[i].parent], bonematrix, s_world[i] );
		}
	}

	out.numBones = numBones;
	out.mirrored = mirror;

	for( int i = 0; i < numBones; i++ )
	{
		for( int r = 0; r < 3; r++ )
		{
			out.gpuBones[i][r * 4 + 0] = s_world[i][r][0];
			out.gpuBones[i][r * 4 + 1] = s_world[i][r][1];
			out.gpuBones[i][r * 4 + 2] = s_world[i][r][2];
			out.gpuBones[i][r * 4 + 3] = s_world[i][r][3];
		}
	}
}

// ---------------------------------------------------------------------------
// Per-frame bone cache: one evaluation per (entity, header) per frame;
// multi-pass callers (lit/depth passes, weapon merge) reuse the result.
// ---------------------------------------------------------------------------
const int kCacheEntries = 256;

struct CacheEntry
{
	const void *ent;
	const void *hdr;
	BoneSetup setup;
};

CacheEntry s_cache[kCacheEntries];
int s_cacheCount;

BoneSetup *CacheLookup( const void *ent, const void *hdr )
{
	for( int i = 0; i < s_cacheCount; i++ )
	{
		if( s_cache[i].ent == ent && s_cache[i].hdr == hdr )
			return &s_cache[i].setup;
	}

	return NULL;
}

BoneSetup *CacheInsert( const void *ent, const void *hdr )
{
	if( s_cacheCount >= kCacheEntries )
	{
		// Overflow: reuse the last slot uncached (correct picture, wasted
		// CPU). Dev-level so a pathological scene is visible in logs.
		static float s_nextWarn;
		float now = ClientTime();

		if( now >= s_nextWarn )
		{
			s_nextWarn = now + 1.0f;
			CSZ_LogDev( "studio", "bone cache full (%d entries); evaluating uncached", kCacheEntries );
		}

		return &s_cache[kCacheEntries - 1].setup;
	}

	CacheEntry &e = s_cache[s_cacheCount++];

	e.ent = ent;
	e.hdr = hdr;
	return &e.setup;
}

}

// ---------------------------------------------------------------------------
// Public sequence-frame helpers (declared in csz_studio_bones.h). The draw
// path (EvaluatePose, this file) and the viewmodel studio-event dispatch pass
// (csz_viewmodel.cpp) BOTH call these, so the event window is computed from the
// identical frame/seqdesc math that produces the drawn pose (W1, PT-02/G-P8).
// ---------------------------------------------------------------------------
const mstudioseqdesc_t *SeqDesc( const studiohdr_t *hdr, int seq )
{
	return (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex ) + seq;
}

float EstimateFrame( const mstudioseqdesc_t *pseqdesc, const cl_entity_s *ent, float time )
{
	double dfdt = 0.0;
	double f;

	if( time >= ent->curstate.animtime )
		dfdt = ( time - ent->curstate.animtime ) * ent->curstate.framerate * pseqdesc->fps;

	if( pseqdesc->numframes <= 1 )
		f = 0.0;
	else
		f = ( ent->curstate.frame * ( pseqdesc->numframes - 1 )) / 256.0;

	f += dfdt;

	if( pseqdesc->flags & STUDIO_LOOPING )
	{
		if( pseqdesc->numframes > 1 )
			f -= (int)( f / ( pseqdesc->numframes - 1 )) * ( pseqdesc->numframes - 1 );

		if( f < 0.0 )
			f += ( pseqdesc->numframes - 1 );
	}
	else
	{
		if( f >= pseqdesc->numframes - 1.001 )
			f = pseqdesc->numframes - 1.001;

		if( f < 0.0 )
			f = 0.0;
	}

	return (float)f;
}

void ResetBoneCache( float time )
{
	(void)time;
	s_cacheCount = 0;
}

bool SetupBones( cl_entity_s *ent, studiohdr_t *hdr, float time, const BoneSetup **out )
{
	*out = NULL;

	if( ent == NULL || hdr == NULL || hdr->numbones <= 0 || hdr->numseq <= 0 )
		return false;

	BoneSetup *cached = CacheLookup( ent, hdr );

	if( cached != NULL )
	{
		*out = cached;
		return true;
	}

	EvaluatePose( ent, hdr, time );

	BoneSetup *setup = CacheInsert( ent, hdr );

	BuildWorldBones( ent, hdr, *setup );
	*out = setup;
	return true;
}

bool SetupBonesMerged( cl_entity_s *ent, studiohdr_t *carrierHdr, const BoneSetup *carrierBones,
	studiohdr_t *weaponHdr, float time, const BoneSetup **out )
{
	*out = NULL;

	if( ent == NULL || carrierHdr == NULL || carrierBones == NULL ||
		weaponHdr == NULL || weaponHdr->numbones <= 0 || weaponHdr->numseq <= 0 )
		return false;

	BoneSetup *cached = CacheLookup( ent, weaponHdr );

	if( cached != NULL )
	{
		*out = cached;
		return true;
	}

	// Weapon's own pose for any bone that does not exist on the carrier
	// (adapted StudioMergeBones: carrier sequence index against the weapon's
	// own sequence table; out-of-range resets to 0, stock parity as above).
	int seq = ent->curstate.sequence;

	if( seq < 0 || seq >= weaponHdr->numseq )
		seq = 0;
	const mstudioseqdesc_t *pseqdesc = SeqDesc( weaponHdr, seq );
	const mstudioanim_t *panim = GetAnim( weaponHdr, pseqdesc );

	if( panim != NULL )
		CalcRotations( weaponHdr, pseqdesc, panim, EstimateFrame( pseqdesc, ent, time ), s_pos, s_q );
	else
		CalcBindPose( weaponHdr, s_pos, s_q );

	int numBones = NumBonesClamped( weaponHdr );
	int carrierCount = carrierBones->numBones;
	const mstudiobone_t *pbones = (const mstudiobone_t *)((const byte *)weaponHdr + weaponHdr->boneindex );
	const mstudiobone_t *pcarrier = (const mstudiobone_t *)((const byte *)carrierHdr + carrierHdr->boneindex );
	float anglesDeg[3];
	float rot[3][4];

	TransformAngles( ent, anglesDeg );
	AngleMatrixDeg( anglesDeg, rot );
	rot[0][3] = ent->origin[0];
	rot[1][3] = ent->origin[1];
	rot[2][3] = ent->origin[2];

	for( int i = 0; i < numBones; i++ )
	{
		int match = -1;

		for( int j = 0; j < carrierCount; j++ )
		{
			if( NameEq( pbones[i].name, pcarrier[j].name ))
			{
				match = j;
				break;
			}
		}

		if( match >= 0 )
		{
			for( int r = 0; r < 3; r++ )
			{
				s_world[i][r][0] = carrierBones->gpuBones[match][r * 4 + 0];
				s_world[i][r][1] = carrierBones->gpuBones[match][r * 4 + 1];
				s_world[i][r][2] = carrierBones->gpuBones[match][r * 4 + 2];
				s_world[i][r][3] = carrierBones->gpuBones[match][r * 4 + 3];
			}
		}
		else
		{
			float bonematrix[3][4];

			QuaternionMatrixLocal( s_q[i], bonematrix );
			bonematrix[0][3] = s_pos[i][0];
			bonematrix[1][3] = s_pos[i][1];
			bonematrix[2][3] = s_pos[i][2];

			if( pbones[i].parent < 0 || pbones[i].parent >= numBones )
				ConcatTransformsLocal( rot, bonematrix, s_world[i] );
			else
				ConcatTransformsLocal( s_world[pbones[i].parent], bonematrix, s_world[i] );
		}
	}

	BoneSetup *setup = CacheInsert( ent, weaponHdr );

	setup->numBones = numBones;
	setup->mirrored = false;	// p_ weapons ride world players, never mirrored

	for( int i = 0; i < numBones; i++ )
	{
		for( int r = 0; r < 3; r++ )
		{
			setup->gpuBones[i][r * 4 + 0] = s_world[i][r][0];
			setup->gpuBones[i][r * 4 + 1] = s_world[i][r][1];
			setup->gpuBones[i][r * 4 + 2] = s_world[i][r][2];
			setup->gpuBones[i][r * 4 + 3] = s_world[i][r][3];
		}
	}

	*out = setup;
	return true;
}

}
