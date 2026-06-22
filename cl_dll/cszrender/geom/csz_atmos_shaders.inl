/*
 * csz_atmos_shaders.inl -- CSOZ renderer: physically-based atmosphere GLSL (C2)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). CLEAN-ROOM GLSL implementing the
 * Bruneton (2008) precomputed-transmittance and Hillaire (2020) multiple-
 * scattering + sky-view LUT MODELS from the PAPERS' mathematics only. No shader
 * source is copied or translated from their MIT/BSD reference code, from
 * Unreal's EULA tree, or from any other license-tainted source (see csoz
 * docs/provenance.md). The atmosphere coefficients are physical constants.
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
// Included ONLY by csz_atmos.cpp. GLES3/WebGL2 intersection: nothing beyond
// GL3.3 core (texture(), no compute, 2D LUTs only). The shared model code lives
// in ATMOS_GLSL_COMMON and is string-concatenated after the "#version" line of
// each fragment program. The physical constants below MUST match
// csz_atmos_math.h.

#define ATMOS_GLSL_COMMON R"GLSL(
// ---- physical constants (km, per-km cross sections); match csz_atmos_math.h --
const float Rg = 6360.0;
const float Rt = 6420.0;
const vec3  betaR = vec3( 5.802e-3, 13.558e-3, 33.100e-3 );  // Rayleigh scattering
const float HR = 8.0;
const float betaMs = 3.996e-3;                               // Mie scattering
const float betaMe = 4.440e-3;                               // Mie extinction
const float HM = 1.2;
const float mieG = 0.80;
const vec3  betaO = vec3( 0.650e-3, 1.881e-3, 0.085e-3 );    // ozone absorption
const float ozoneCenter = 25.0;
const float ozoneWidth = 15.0;
const float PI = 3.14159265358979;

float densR( float h ) { return exp( -h / HR ); }
float densM( float h ) { return exp( -h / HM ); }
float densO( float h ) { return max( 1.0 - abs( h - ozoneCenter ) / ozoneWidth, 0.0 ); }

vec3 extinction( float h )
{
	float dR = densR( h ), dM = densM( h ), dO = densO( h );
	return betaR * dR + vec3( betaMe ) * dM + betaO * dO;
}

// nearest positive ray-sphere distance (planet-centric, |rd|=1); -1 = miss.
float raySphere( vec3 ro, vec3 rd, float radius )
{
	float b = dot( ro, rd );
	float c = dot( ro, ro ) - radius * radius;
	float disc = b * b - c;
	if( disc < 0.0 ) return -1.0;
	float s = sqrt( disc );
	float t0 = -b - s, t1 = -b + s;
	if( t0 >= 0.0 ) return t0;
	if( t1 >= 0.0 ) return t1;
	return -1.0;
}

float distToTop( float r, float mu )
{
	float disc = r * r * ( mu * mu - 1.0 ) + Rt * Rt;
	disc = max( disc, 0.0 );
	return max( -r * mu + sqrt( disc ), 0.0 );
}

float rayleighPhase( float c ) { return ( 3.0 / ( 16.0 * PI ) ) * ( 1.0 + c * c ); }

float miePhase( float c )
{
	float g = mieG;
	float g2 = g * g;
	float d = max( 1.0 + g2 - 2.0 * g * c, 1e-4 );
	return ( 3.0 / ( 8.0 * PI ) ) * ( ( 1.0 - g2 ) * ( 1.0 + c * c ) ) / ( ( 2.0 + g2 ) * pow( d, 1.5 ) );
}

// ---- Bruneton transmittance-LUT parameterisation -----------------------------
vec2 transUv( float r, float mu )
{
	float H = sqrt( max( Rt * Rt - Rg * Rg, 0.0 ) );
	float rho = sqrt( max( r * r - Rg * Rg, 0.0 ) );
	float d = distToTop( r, mu );
	float dMin = Rt - r;
	float dMax = rho + H;
	float xMu = ( dMax - dMin > 1e-6 ) ? ( d - dMin ) / ( dMax - dMin ) : 0.0;
	float xR = ( H > 1e-6 ) ? rho / H : 0.0;
	return vec2( clamp( xMu, 0.0, 1.0 ), clamp( xR, 0.0, 1.0 ) );
}

void uvToTrans( vec2 uv, out float r, out float mu )
{
	float H = sqrt( max( Rt * Rt - Rg * Rg, 0.0 ) );
	float rho = H * uv.y;
	r = sqrt( max( rho * rho + Rg * Rg, 0.0 ) );
	float dMin = Rt - r;
	float dMax = rho + H;
	float d = dMin + uv.x * ( dMax - dMin );
	mu = ( d < 1e-6 ) ? 1.0 : ( H * H - rho * rho - d * d ) / ( 2.0 * r * d );
	mu = clamp( mu, -1.0, 1.0 );
}

vec3 sampleTransmittance( sampler2D lut, float r, float mu ) { return texture( lut, transUv( r, mu ) ).rgb; }

// ---- Hillaire multiple-scattering LUT parameterisation ------------------------
vec2 msUv( float r, float muSun )
{
	return vec2( clamp( muSun * 0.5 + 0.5, 0.0, 1.0 ), clamp( ( r - Rg ) / ( Rt - Rg ), 0.0, 1.0 ) );
}

vec3 sampleMS( sampler2D lut, float r, float muSun ) { return texture( lut, msUv( r, muSun ) ).rgb; }

// ---- Hillaire sky-view LUT parameterisation (horizon-dense, sun-relative) -----
// uv.x = azimuth/2pi around the sun meridian; uv.y = view zenith, with sqrt
// density toward the horizon (which sits at uv.y = 0.5).
void skyViewDecode( vec2 uv, float r, out vec3 dir )
{
	float thetaH = PI - asin( clamp( Rg / r, 0.0, 1.0 ) );
	float theta;
	if( uv.y < 0.5 ) { float c = 1.0 - 2.0 * uv.y; theta = thetaH * ( 1.0 - c * c ); }
	else { float c = 2.0 * uv.y - 1.0; theta = thetaH + ( PI - thetaH ) * ( c * c ); }
	float phi = uv.x * 2.0 * PI;
	float st = sin( theta ), ct = cos( theta );
	dir = vec3( st * cos( phi ), st * sin( phi ), ct );
}

float skyViewEncodeV( float theta, float r )
{
	float thetaH = PI - asin( clamp( Rg / r, 0.0, 1.0 ) );
	if( theta < thetaH ) { float c = sqrt( max( 1.0 - theta / thetaH, 0.0 ) ); return 0.5 * ( 1.0 - c ); }
	float c = sqrt( max( ( theta - thetaH ) / ( PI - thetaH ), 0.0 ) );
	return 0.5 * ( 1.0 + c );
}
)GLSL"

// =============================================================================
// Fullscreen VS for the three LUT-build passes (outputs the [0,1] texcoord).
// =============================================================================
static const char kAtmosLutVs[] = R"GLSL(#version 330 core
out vec2 v_uv;
void main()
{
	vec2 ndc = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0,
	                 ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	v_uv = ndc * 0.5 + 0.5;
	gl_Position = vec4( ndc, 1.0, 1.0 );
}
)GLSL";

// =============================================================================
// PASS 1 -- transmittance LUT: analytic optical depth to the atmosphere top.
// =============================================================================
static const char kAtmosTransmittanceFs[] =
"#version 330 core\n"
ATMOS_GLSL_COMMON
R"GLSL(
in vec2 v_uv;
out vec4 fragColor;
void main()
{
	float r, mu;
	uvToTrans( v_uv, r, mu );
	vec3 pos = vec3( 0.0, 0.0, r );
	vec3 dir = vec3( sqrt( max( 1.0 - mu * mu, 0.0 ) ), 0.0, mu );
	float dist = distToTop( r, mu );
	const int N = 40;
	float ds = dist / float( N );
	vec3 tau = vec3( 0.0 );
	for( int i = 0; i < N; i++ )
	{
		float t = ( float( i ) + 0.5 ) * ds;
		vec3 p = pos + dir * t;
		float h = length( p ) - Rg;
		tau += extinction( h ) * ds;
	}
	fragColor = vec4( exp( -tau ), 1.0 );
}
)GLSL";

// =============================================================================
// PASS 2 -- multiple-scattering LUT (Hillaire infinite-scattering series):
//   psi_ms = L_2nd / (1 - f_ms), sphere-averaged 2nd-order scattering + transfer.
// =============================================================================
static const char kAtmosMultiScatterFs[] =
"#version 330 core\n"
ATMOS_GLSL_COMMON
R"GLSL(
in vec2 v_uv;
out vec4 fragColor;
uniform sampler2D u_trans;
void main()
{
	float muSun = v_uv.x * 2.0 - 1.0;
	float r = clamp( Rg + v_uv.y * ( Rt - Rg ), Rg + 1e-3, Rt );
	vec3 pos = vec3( 0.0, 0.0, r );
	vec3 sunDir = vec3( sqrt( max( 1.0 - muSun * muSun, 0.0 ) ), 0.0, muSun );

	const int SQ = 8;                 // 8x8 = 64 sphere directions
	vec3 L2 = vec3( 0.0 );
	vec3 fms = vec3( 0.0 );
	float wsum = 0.0;
	for( int i = 0; i < SQ; i++ )
	for( int j = 0; j < SQ; j++ )
	{
		float theta = PI * ( float( i ) + 0.5 ) / float( SQ );
		float phi = 2.0 * PI * ( float( j ) + 0.5 ) / float( SQ );
		float st = sin( theta );
		vec3 wi = vec3( st * cos( phi ), st * sin( phi ), cos( theta ) );
		float w = st;                 // sin-weighted sphere average

		float mu = wi.z;              // pos is along +z, so cos(zenith) = wi.z
		float dGround = raySphere( pos, wi, Rg );
		float dist = distToTop( r, mu );
		if( dGround > 0.0 && dGround < dist ) dist = dGround;

		const int NS = 20;
		float ds = dist / float( NS );
		vec3 tput = vec3( 1.0 );
		vec3 l2 = vec3( 0.0 );
		vec3 f = vec3( 0.0 );
		for( int s = 0; s < NS; s++ )
		{
			float t = ( float( s ) + 0.5 ) * ds;
			vec3 p = pos + wi * t;
			float rr = length( p );
			float h = rr - Rg;
			float dR = densR( h ), dM = densM( h );
			vec3 scat = betaR * dR + vec3( betaMs ) * dM;
			vec3 ext = extinction( h );

			vec3 sunT = vec3( 0.0 );
			if( raySphere( p, sunDir, Rg ) <= 0.0 )
				sunT = sampleTransmittance( u_trans, rr, dot( normalize( p ), sunDir ) );

			vec3 stepT = exp( -ext * ds );
			vec3 seg = ( 1.0 - stepT ) / max( ext, vec3( 1e-7 ) );
			// 2nd-order in-scatter (sun light, isotropic phase 1/4pi):
			l2 += tput * ( scat * sunT * ( 1.0 / ( 4.0 * PI ) ) ) * seg;
			// transfer fraction for the geometric series (uniform unit radiance):
			f  += tput * ( scat * ( 1.0 / ( 4.0 * PI ) ) ) * seg;
			tput *= stepT;
		}
		L2 += l2 * w;
		fms += f * w;
		wsum += w;
	}
	L2 /= wsum;
	fms /= wsum;
	vec3 psi = L2 / max( vec3( 1.0 ) - fms, vec3( 1e-3 ) );
	fragColor = vec4( psi, 1.0 );
}
)GLSL";

// =============================================================================
// PASS 3 -- sky-view LUT: per view-ray in-scattered radiance (single + multi),
//   in the sun-relative frame (sun at azimuth phi=0, +x half-plane).
// =============================================================================
static const char kAtmosSkyViewFs[] =
"#version 330 core\n"
ATMOS_GLSL_COMMON
R"GLSL(
in vec2 v_uv;
out vec4 fragColor;
uniform sampler2D u_trans;
uniform sampler2D u_ms;
uniform float u_sunCosZenith;   // sun zenith cosine in the LUT frame (= sunDir.z)
uniform float u_viewR;          // camera radius (Rg + eye height)
uniform float u_msScale;        // multiple-scattering on/off (csz_atmos_ms)
void main()
{
	float r = u_viewR;
	vec3 pos = vec3( 0.0, 0.0, r );
	vec3 dir;
	skyViewDecode( v_uv, r, dir );
	float mz = u_sunCosZenith;
	vec3 sunDir = vec3( sqrt( max( 1.0 - mz * mz, 0.0 ) ), 0.0, mz );

	float mu = dir.z;
	float dGround = raySphere( pos, dir, Rg );
	float dist = distToTop( r, mu );
	if( dGround > 0.0 && dGround < dist ) dist = dGround;

	const int N = 32;
	float ds = dist / float( N );
	float cosVS = dot( dir, sunDir );
	float pR = rayleighPhase( cosVS );
	float pM = miePhase( cosVS );

	vec3 L = vec3( 0.0 );
	vec3 tput = vec3( 1.0 );
	for( int s = 0; s < N; s++ )
	{
		float t = ( float( s ) + 0.5 ) * ds;
		vec3 p = pos + dir * t;
		float rr = length( p );
		float h = rr - Rg;
		float dR = densR( h ), dM = densM( h );
		vec3 scatR = betaR * dR;
		float scatM = betaMs * dM;
		vec3 ext = extinction( h );
		float muS = dot( normalize( p ), sunDir );

		vec3 sunT = vec3( 0.0 );
		if( raySphere( p, sunDir, Rg ) <= 0.0 )
			sunT = sampleTransmittance( u_trans, rr, muS );

		vec3 single = ( scatR * pR + vec3( scatM ) * pM ) * sunT;
		vec3 ms = sampleMS( u_ms, rr, muS ) * u_msScale;
		vec3 multi = ( scatR + vec3( scatM ) ) * ms;
		vec3 S = single + multi;

		vec3 stepT = exp( -ext * ds );
		vec3 seg = ( 1.0 - stepT ) / max( ext, vec3( 1e-7 ) );
		L += tput * S * seg;
		tput *= stepT;
	}
	fragColor = vec4( max( L, vec3( 0.0 ) ), 1.0 );
}
)GLSL";

// =============================================================================
// SKY BACKGROUND DRAW -- full-screen, samples the sky-view LUT per view ray.
// VS rebuilds the world-space view ray from the pre-scaled camera basis (same
// convention as csz_sky_shaders.inl: u_camRight/Up already * tan(fov/2)).
// =============================================================================
static const char kAtmosSkyVs[] = R"GLSL(#version 330 core
uniform vec3 u_camFwd;
uniform vec3 u_camRight;   // pre-scaled by tan(fovX/2)
uniform vec3 u_camUp;      // pre-scaled by tan(fovY/2)
out vec3 v_dir;
void main()
{
	vec2 ndc = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0,
	                 ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	v_dir = u_camFwd + u_camRight * ndc.x + u_camUp * ndc.y;
	gl_Position = vec4( ndc, 1.0, 1.0 );
}
)GLSL";

static const char kAtmosSkyFs[] =
"#version 330 core\n"
ATMOS_GLSL_COMMON
R"GLSL(
in vec3 v_dir;
out vec4 fragColor;
uniform sampler2D u_skyView;
uniform vec3 u_sunDir;     // world sun direction (Quake Z-up), normalized
uniform float u_viewR;     // camera radius (Rg + eye height)
uniform float u_exposure;  // linear radiance -> display scale (csz_atmos_exposure)
	uniform float u_skyNavy;   // csz_sky_navy: night-navy-floor multiplier (regrade G2)
void main()
{
	vec3 dir = normalize( v_dir );
	float r = u_viewR;

	// view zenith -> LUT v.
	float theta = acos( clamp( dir.z, -1.0, 1.0 ) );
	float v = skyViewEncodeV( theta, r );

	// azimuth of the view ray RELATIVE to the sun azimuth -> LUT u (wraps, REPEAT).
	vec2 dh = vec2( dir.x, dir.y );
	vec2 sh = vec2( u_sunDir.x, u_sunDir.y );
	float dl = length( dh ), sl = length( sh );
	float phi = 0.0;
	if( dl > 1e-5 && sl > 1e-5 )
	{
		dh /= dl; sh /= sl;
		float cosA = clamp( dot( dh, sh ), -1.0, 1.0 );
		float sinA = sh.x * dh.y - sh.y * dh.x;
		phi = atan( sinA, cosA );
	}
	float u = fract( phi / ( 2.0 * PI ) );

	vec3 rad = texture( u_skyView, vec2( u, v ) ).rgb * u_exposure;

	// --- Night-gated deep-navy floor (R1b) -------------------------------------
	// With csz_atmos 1 the physical sky-view LUT owns the night sky, but at deep
	// night (sun far below the horizon) its in-scattered radiance falls to ~0, so
	// the zenith renders pure BLACK. Real moonless/moonlit night skies keep a deep
	// NAVY cast (integrated faint starlight + airglow + zodiacal light). Add a small
	// deep-navy FLOOR (max, not add: daytime/twilight LUT radiance >> navy is left
	// untouched), gated by deep-night so it never lifts day/twilight, with a gentle
	// airglow rise toward the horizon. Tuned to the reference astrophoto: zenith
	// ~(6,8,14)/255, lower sky ~(12,18,31)/255 (display-passthrough resolve -> the
	// linear value IS the 8-bit/255 target). ANTI-FOG: the floor sits BELOW the
	// faint-star level and the Milky Way band (both deposited additively in the C4
	// stars pass AFTER this background), so stars + the MW still clearly dominate --
	// it only lifts the EMPTY sky off true black, it must never read as a blue wash.
	float sunZ  = clamp( u_sunDir.z, -1.0, 1.0 );              // sun zenith cosine = sin(sunElev)
	float nightF = 1.0 - smoothstep( -0.17, 0.0, sunZ );       // 0 at/above horizon -> 1 by ~ -9.8 deg
	// Bias BLUE-dominant (keep R low like the reference's deep navy ~6/255) so the
	// floor still reads navy after the faint all-sky star carpet adds its ~neutral
	// pedestal (which lifts R most, graying a weaker navy). r1b capture: the prior
	// (.023,.031,.055) read near-neutral-black at the zenith; B pushed up, R held low.
	const vec3 kNavyZenith  = vec3( 0.040, 0.068, 0.140 );     // deep zenith navy, B-dominant (~10,17,36 / 255) -- regrade G2 lift (was 6,10,23 = read near-black)
	const vec3 kNavyHorizon = vec3( 0.072, 0.110, 0.195 );     // airglow lift toward horizon (~18,28,50 / 255) -- regrade G2 lift
	float horizonT  = 1.0 - smoothstep( 0.0, 0.35, clamp( dir.z, 0.0, 1.0 ) );  // 1 at horizon -> 0 above ~20 deg
	vec3  navyFloor = mix( kNavyZenith, kNavyHorizon, horizonT ) * nightF * u_skyNavy;
	rad = max( rad, navyFloor );

	// TPDF +/-1 LSB dither: the night/twilight gradient has very few 8-bit levels
	// (Jimenez interleaved-gradient base; Gjol/Playdead TPDF, public math).
	float ign1 = fract( 52.9829189 * fract( dot( gl_FragCoord.xy,        vec2( 0.06711056, 0.00583715 ) ) ) );
	float ign2 = fract( 52.9829189 * fract( dot( gl_FragCoord.xy + 17.0, vec2( 0.06711056, 0.00583715 ) ) ) );
	rad += ( ign1 + ign2 - 1.0 ) / 255.0;

	fragColor = vec4( max( rad, vec3( 0.0 ) ), 1.0 );
}
)GLSL";

#undef ATMOS_GLSL_COMMON
