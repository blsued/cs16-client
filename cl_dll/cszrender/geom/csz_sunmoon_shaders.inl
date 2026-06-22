/*
 * csz_sunmoon_shaders.inl -- CSOZ renderer: sun/moon body GLSL (C3)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). CLEAN-ROOM GLSL. The lunar
 * disc/phase/limb math is first-principles spherical geometry; the Bruneton
 * transmittance-LUT parameterisation re-typed below is published mathematics
 * (physical constants + a coordinate change, not copyrightable expression) and
 * MUST stay numerically identical to geom/csz_atmos_shaders.inl so the same C2
 * LUT samples correctly. No shader source is copied or translated from Unreal's
 * EULA tree, sebh/UnrealEngineSkyAtmosphere, Bruneton's repo, or any other
 * license-tainted source (see csoz docs/provenance.md).
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
// Included ONLY by csz_sunmoon.cpp. GL3.3 core / GLES3 intersection (texture(),
// fwidth, no compute). The bodies are drawn with a full-screen triangle into the
// HDR scene target with ADDITIVE blend (depth off): each fragment evaluates the
// ray vs the moon disc (the moon's outer glow now lives in MoonSkyLum, Task D) and
// the sun disc + aureole and accumulates HDR radiance. Most of the screen early-outs
// of the per-body work.

// ------------------------------------------------------------------ vertex ----
static const char kSunMoonVs[] = R"GLSL(#version 330 core
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

// ----------------------------------------------------------------- fragment ---
static const char kSunMoonFs[] = R"GLSL(#version 330 core
in vec3 v_dir;
out vec4 fragColor;

// --- body geometry (world, Z up; viewer->body) ---
uniform vec3  u_moonDir;
uniform vec3  u_moonRight;     // lunar east  (face frame, world-up aligned)
uniform vec3  u_moonUp;        // lunar north
uniform vec3  u_sunDir;
uniform vec3  u_moonLightDir;   // moon phase-light dir (full-moon == u_sunDir). Decoupled
                               // from the antipodal disc PLACEMENT so a real crescent/
                               // gibbous/full terminator appears without moving the disc.
uniform vec3  u_moonExtinctDir; // real celestial moon dir for the extinction lookup
uniform vec3  u_sunExtinctDir;  // real celestial sun dir for the extinction lookup
uniform float u_moonAngR;      // moon angular RADIUS (radians)
uniform float u_sunAngR;       // sun  angular RADIUS (radians)

// --- look ---
// NOTE (SKY-REWORK-SPEC v3 Task D): the old standalone moon HALO (u_moonHalo*) was
// REMOVED. The moon's outer glow is now the rho->0 inner segment of MoonSkyLum() in
// csz_stars_shaders.inl (single ownership -- emitted once, no double-count). This pass
// produces ONLY the moon BODY disc (+ T_moonBody occluder geometry, reported separately).
uniform vec3  u_moonColor;     // cold blue-white disc tint
uniform vec3  u_sunColor;      // warm sun tint
uniform float u_moonGain;      // disc radiance scale
uniform float u_sunGain;
uniform float u_sunAureoleGain;
uniform float u_aureoleK;      // aureole power-law exponent (~0.9)
uniform float u_bloodMoon;     // 0 = normal, 1 = ominous deep red
uniform float u_moonVis;       // 0..1 horizon visibility
uniform float u_sunVis;

// --- atmospheric extinction (C2) ---
uniform sampler2D u_transLut;  // C2 transmittance LUT (bound when u_hasTrans!=0)
uniform int       u_hasTrans;
uniform vec3      u_fallbackTransmit;
uniform float     u_viewR;     // ground radius the LUT was built for (km)

// --- surface ---
uniform sampler2D u_moonTex;
uniform int       u_hasMoonTex;

const float PI = 3.14159265358979;
// Bruneton transmittance-LUT geometry constants -- MUST match csz_atmos*.
const float Rg = 6360.0;
const float Rt = 6420.0;

float distToTop( float r, float mu )
{
	float disc = r * r * ( mu * mu - 1.0 ) + Rt * Rt;
	return max( -r * mu + sqrt( max( disc, 0.0 ) ), 0.0 );
}

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

// Per-channel atmospheric transmittance from the ground toward a body direction.
// mu = cos(zenith) = dir.z (Z up). Below the horizon the LUT clamps -> heavy
// extinction, which naturally extinguishes/reddens a setting body.
vec3 transmitToward( vec3 dir )
{
	if( u_hasTrans == 0 )
		return u_fallbackTransmit;
	return texture( u_transLut, transUv( u_viewR, clamp( dir.z, -1.0, 1.0 ) ) ).rgb;
}

// Cheap hash-value noise for the procedural fallback moon (only when the NASA
// texture is missing -- gives the disc some maria-like variance so it is not flat).
float hash21( vec2 p )
{
	p = fract( p * vec2( 123.34, 345.45 ) );
	p += dot( p, p + 34.345 );
	return fract( p.x * p.y );
}
float vnoise( vec2 p )
{
	vec2 i = floor( p ), f = fract( p );
	f = f * f * ( 3.0 - 2.0 * f );
	float a = hash21( i );
	float b = hash21( i + vec2( 1.0, 0.0 ) );
	float c = hash21( i + vec2( 0.0, 1.0 ) );
	float d = hash21( i + vec2( 1.0, 1.0 ) );
	return mix( mix( a, b, f.x ), mix( c, d, f.x ), f.y );
}
vec3 proceduralMoon( vec2 uv )
{
	// Large dark maria blotches + fine speckle; near-grey, low saturation.
	float m = 0.0;
	m += smoothstep( 0.55, 0.05, length( uv - vec2( 0.40, 0.55 ) ) ) * 0.55;
	m += smoothstep( 0.35, 0.03, length( uv - vec2( 0.62, 0.40 ) ) ) * 0.45;
	m += smoothstep( 0.28, 0.02, length( uv - vec2( 0.50, 0.70 ) ) ) * 0.40;
	float fine = vnoise( uv * 28.0 ) * 0.18 + vnoise( uv * 80.0 ) * 0.08;
	float g = clamp( 0.78 - m * 0.42 + ( fine - 0.13 ), 0.18, 1.0 );
	return vec3( g, g * 0.99, g * 0.97 );   // faintly warm near-grey regolith
}

void main()
{
	vec3 ray = normalize( v_dir );
	vec3 col = vec3( 0.0 );

	vec3 bloodCol = vec3( 0.62, 0.055, 0.025 );   // ominous deep red

	// ============================== MOON =================================
	if( u_moonVis > 0.0 )
	{
		float cosA = dot( ray, u_moonDir );
		float ang  = acos( clamp( cosA, -1.0, 1.0 ) );
		vec3  mTint = mix( u_moonColor, bloodCol, u_bloodMoon );
		vec3  mTrans = transmitToward( u_moonExtinctDir );

		// --- disc + surface. The disc-plane radius r and its limb-AA term are
		// computed in UNIFORM control flow (OUTSIDE the ang<1.5R early-out). THIS IS
		// THE FIX FOR THE DASHED RING: the previous code took fwidth(r) INSIDE that
		// branch, where screen-space derivatives are UNDEFINED (GLSL: derivatives in
		// non-uniform control flow) for the 2x2 quads straddling the boundary. The
		// garbage fwidth made `aa` spuriously non-zero in a 1px-wide ring at exactly
		// ~1.5*moonAngR, leaking ~aa*disc radiance (~35 LSB over the night sky) where
		// aa should be exactly 0 -> the faint dashed/dotted circle that tracked the
		// moon. Evaluated for every fragment, fwidth(r) is well-defined, so aa is
		// non-zero ONLY across the true limb (r in ~[1-2px,1]) and the ring vanishes.
		// du/dv/r are a few cheap ALU ops. The spherical shading stays behind the
		// ang<1.5R early-out, but the disc-surface uv AND its screen-space gradients are
		// ALSO computed here in uniform flow so the moon texture is fetched with EXPLICIT
		// gradients (textureGrad) inside the branch. An implicit-LOD texture() there would
		// take derivatives in non-uniform control flow -- spec-undefined (possibly NaN, and
		// 0*NaN=NaN, so aa==0 is NOT a safe mask), and at small moon radius / wide FOV the
		// limb quads are not guaranteed uniform. textureGrad with uniform-flow gradients is
		// spec-safe: on interior quads the gradients equal what the HW would derive
		// implicitly -> the maria sample IDENTICALLY; at the limb/boundary quads they stay
		// well-defined (finite) -> trilinear minification stays crisp, never an undefined fetch.
		float sinR = max( sin( u_moonAngR ), 1e-5 );
		float du = dot( ray, u_moonRight ) / sinR;
		float dv = dot( ray, u_moonUp ) / sinR;
		float rr = du * du + dv * dv;
		float r  = sqrt( rr );
		float aa = 1.0 - smoothstep( 1.0 - max( 2.0 * fwidth( r ), 1e-5 ), 1.0, r );  // limb AA (derivative valid; width clamped > 0)

		// disc-surface normal + near-side equirect uv (lon 0 at sub-viewer point) + their
		// screen-space gradients, ALL in UNIFORM control flow so the texture fetch in the
		// branch below uses textureGrad (explicit, spec-safe LOD). lon=atan(du,w) stays in
		// [-PI/2,PI/2] over the visible near-side hemisphere (no atan wrap seam), so
		// dFdx/dFdy(uv) are smooth on the disc; out-of-disc quads (w=0) yield finite grads
		// that are only ever consumed where aa>0, so they cannot produce an undefined fetch.
		float w   = sqrt( max( 1.0 - min( rr, 1.0 ), 0.0 ) );
		vec3  N   = du * u_moonRight + dv * u_moonUp + w * ( -u_moonDir );
		float lat = asin( clamp( dot( N, u_moonUp ), -1.0, 1.0 ) );
		float lon = atan( dot( N, u_moonRight ), dot( N, -u_moonDir ) );
		vec2  uv  = vec2( lon / PI * 0.5 + 0.5, 0.5 - lat / PI );
		vec2  duvdx = dFdx( uv );
		vec2  duvdy = dFdy( uv );
		if( ang < u_moonAngR * 1.5 )
		{
			vec3  albedo;
			if( u_hasMoonTex != 0 )
				albedo = textureGrad( u_moonTex, uv, duvdx, duvdy ).rgb;  // explicit grads == HW implicit on interior quads -> maria identical
			else
				albedo = proceduralMoon( vec2( du, dv ) * 0.5 + 0.5 );

			// phase from sun-moon geometry: lit where the surface faces the moon's
			// LIGHT direction. u_moonLightDir is decoupled from the antipodal disc
			// placement (full-moon == u_sunDir), so as csz_moon_phase sweeps the
			// terminator moves across a FIXED-size disc -> real crescent/gibbous/full.
			float ndl = dot( N, u_moonLightDir );
			float lit = smoothstep( -0.10, 0.12, ndl );      // crisp terminator (phase)
			float earthshine = 0.022;                        // faint earthshine on the dark limb (RESTRAINED, Task C)
			float shade = max( lit, earthshine * ( 1.0 - lit ) );

			// NEW-MOON ZERO RETURN (SKY-REWORK-SPEC v3 Task D finish, codex visual flag):
			// at csz_moon_phase 0 the whole near side is dark, so the flat earthshine pedestal
			// (+ the sub-pixel grazing-limb terminator sliver) paints the disc as a faint grey
			// blob over the dark sky. Gate the ENTIRE body disc by the visible LIT FRACTION so a
			// NEW moon is invisible (dark-sky zero return) while every crescent/quarter/gibbous/
			// full -- AND the legacy always-full moon (litFrac==1) AND the dbg==1 capture
			// (moonLightDir==-moonDir) -- stay byte-identical. litFrac = 0.5*(1+cos(phaseAngle)),
			// phaseAngle = angle(moonLightDir, viewer dir -moonDir): full light==-moonDir(==sunDir)
			// ->1, new light==moonDir->0, quarter light==moonRight->0.5. Mirrors the host litFrac in
			// MoonBodyOccluder() (csz_sunmoon.cpp) that already zeroes the wash GLOW at new moon, so
			// BODY + GLOW now vanish together. Knee 0.02 (litFrac at csz_moon_phase~0.09) reaches 1
			// by a thin crescent -> no real crescent is dimmed; only the new-moon neighbourhood
			// fades. Gates RADIANCE (size-independent), so it holds at any csz_moon_size.
			float litFrac = 0.5 * ( 1.0 + dot( u_moonLightDir, -u_moonDir ) );
			float bodyVis = smoothstep( 0.0, 0.02, litFrac );
			shade *= bodyVis;

			// EXPLICIT limb darkening (SKY-REWORK-SPEC v3 Task C "体积"): the disc must read
			// as a 3D SPHERE, not a flat decal. mu = w = the view-facing normal component
			// (1 at the disc centre, 0 at the limb) = cos(emergent angle). A classic
			// (a + b*mu^p) law darkens the rim on BOTH the lit face AND the earthshine dark
			// side, so the moon looks round even on the procedural-fallback disc (no maria
			// asset). Combined with the N.L phase terminator this gives volume/curvature.
			float mu       = clamp( w, 0.0, 1.0 );
			float limbDark = mix( 0.42, 1.0, pow( mu, 0.55 ) );   // rim ~0.42x centre, smooth toward edge
			shade *= limbDark;

			col += albedo * mTint * ( u_moonGain * shade * aa * u_moonVis ) * mTrans;
		}

		// NOTE (SKY-REWORK-SPEC v3 Task D): the standalone moon halo that used to live here
		// (exp(-ang/(tau+R)) glow) was REMOVED. The moon's outer atmospheric glow is now the
		// rho->0 inner segment of MoonSkyLum() (csz_stars_shaders.inl, emitted ONCE in the
		// Milky Way pass) -- single ownership, so the aureole energy is never double-counted.
		// This pass now contributes ONLY the moon BODY disc above.
	}
)GLSL"
// SPLIT (2026-06-20): the kSunMoonFs source grew past the MSVC ~16KB single-string-
// literal limit (C2026) when Task C added explicit limb darkening. Adjacent string
// literals concatenate at translation -> the GLSL source is byte-identical; this is
// a pure compile-seam, NOT a behaviour change.
R"GLSL(
	// =============================== SUN =================================
	if( u_sunVis > 0.0 )
	{
		float cosA = dot( ray, u_sunDir );
		float ang  = acos( clamp( cosA, -1.0, 1.0 ) );
		vec3  sTrans = transmitToward( u_sunExtinctDir );

		vec3 sunRad = vec3( 0.0 );

		// --- crisp disc: AA'd HARD limb + a brighter limb-darkened CORE, so the sun
		// reads as a solid disc with internal structure, not a flat clamped plateau.
		// rSun and its limb-AA are computed in UNIFORM control flow (outside the
		// ang<1.5R early-out) for the SAME reason as the moon disc above: fwidth(rSun)
		// taken INSIDE the branch is an undefined derivative on the boundary quads and
		// paints a 1px dashed ring at ~1.5*sunAngR (a sun-ring at round-end). Hoisted
		// out, the derivative is well-defined and the ring cannot form. ---
		float rSun  = ang / max( u_sunAngR, 1e-5 );                          // 0 centre .. 1 limb
		float aaSun = 1.0 - smoothstep( 1.0 - max( 1.5 * fwidth( rSun ), 1e-5 ), 1.0, rSun ); // hard, AA'd limb (width clamped > 0)
		if( ang < u_sunAngR * 1.5 )
		{
			float core = 1.0 + 0.8 * ( 1.0 - smoothstep( 0.0, 1.0, rSun ) ); // brighter centre, dims to the limb
			sunRad += u_sunColor * ( u_sunGain * core * aaSun );
		}

		// --- TIGHT Mie forward-scatter aureole. It now STARTS at the limb (the
		// smoothstep ramps it up from the disc centre) so there is no interior
		// plateau / derivative kink (the old r^-k was flat ==1 for ang<=R), and a
		// steep exp(-ang/0.10) (~5.7deg) keeps it a tight halo instead of the old
		// exp(-ang/0.6) (~34deg) white smear that swallowed the disc. ---
		float aur = pow( u_sunAngR / max( ang, u_sunAngR ), u_aureoleK );
		aur *= exp( -ang / 0.10 );                       // tight outer cutoff (was 0.6)
		aur *= smoothstep( 0.0, u_sunAngR, ang );        // ramp up from centre -> limb (kills the plateau)
		sunRad += u_sunColor * ( u_sunAureoleGain * aur );

		// atmospheric extinction (reddens + dims a low / horizon sun)
		sunRad *= sTrans;

		// --- body-LOCAL soft-knee (extended Reinhard, per channel, white point Lw
		// mapped to 1.0): compress the bright core SMOOTHLY toward white instead of
		// hard-clamping to a flat white plateau at the (tonemap-off) HDR resolve.
		// LOCAL only -- the global ACES tonemap stays OFF (csz_tonemap 0). ---
		const float Lw = 2.2;
		sunRad = sunRad * ( 1.0 + sunRad / ( Lw * Lw ) ) / ( 1.0 + sunRad );

		col += sunRad * u_sunVis;
	}

	// Body-LOCAL anti-banding + alpha. The steep moon-disc limb / sun-aureole tails on
	// the near-black night sky quantize into concentric 8-bit contour rings at the
	// RGBA16F->RGBA8 resolve (csz_dither is OFF, csz_exposure==1, csz_tonemap/encode
	// 0 -> the final 8-bit value is quantize(scene_linear), so +/-1/255 HERE is
	// exactly +/-1 output LSB; THIS is the single dither point for the bodies).
	// This pass is a FULLSCREEN triangle drawn additively over the whole atmosphere,
	// so the dither + contribution MUST stay confined to the body footprint, else it
	// peppers +/-1 LSB noise across the entire sky whenever a body is up.
	//
	// Two defects of the previous version produced the residual dashed ring that
	// tracked a body, and are fixed here (they applied to the now-removed moon halo and
	// still apply to the sun aureole + moon-disc limb tails):
	//   (1) the dither was RAMPED DOWN (smoothstep 0.5->2 LSB) exactly in the faint
	//       0.5-5 LSB tail where the OUTERMOST bands live -- at luma 1/255 it ran at
	//       only ~26% amplitude, far too weak to dither the 0<->1 / 1<->2 LSB steps,
	//       so those faint rings survived. The TPDF now runs at FULL +/-1 LSB across
	//       the entire VISIBLE tail (mask saturates by ~0.6 LSB, under visibility).
	//   (2) the hard `lum<=0.5/255 -> discard` cut the footprint at a FIXED radius =
	//       a clean circular iso-luma edge with zero dither across it = a faint clean
	//       ring. The cutoff is now STOCHASTIC (a per-pixel IGN-jittered ~0.15-0.55
	//       LSB floor) and the radiance is feathered to zero approaching it, so the
	//       footprint edge is sub-LSB noise at a per-pixel-varying radius -- it can
	//       never resolve into a clean circle. Below the floor we contribute nothing
	//       (alpha 0): the additive blend leaves scene RGB + HDR alpha untouched.
	// This block is SHARED by the moon disc limb and the sun aureole (both accumulate
	// into col), so the same fix removes a sun-aureole ring at round-end as well.
	float lum = max( col.r, max( col.g, col.b ) );

	// Two decorrelated interleaved-gradient-noise samples (Jimenez IGN base;
	// Gjol/Playdead TPDF technique) -> a triangular-PDF dither, screen-space and
	// deterministic per pixel (no time term -> clean A/B), mirroring the resolve and
	// csz_sky_shaders.inl noise.
	float ign1 = fract( 52.9829189 * fract( dot( gl_FragCoord.xy,        vec2( 0.06711056, 0.00583715 ) ) ) );
	float ign2 = fract( 52.9829189 * fract( dot( gl_FragCoord.xy + 17.0, vec2( 0.06711056, 0.00583715 ) ) ) );
	float tpdf = ( ign1 + ign2 - 1.0 ) / 255.0;   // +/-1 LSB, ~triangular PDF

	// Stochastic footprint cutoff: a per-pixel jittered floor (~0.15..0.55 LSB),
	// well under visibility, instead of the old fixed 0.5/255 step. The boundary
	// radius varies pixel to pixel -> no clean circle. Below it: contribute nothing.
	float floorLsb = ( 0.15 + 0.40 * ign1 ) / 255.0;
	if( lum < floorLsb )
	{
		fragColor = vec4( 0.0 );   // no-op region: contribute nothing, touch no HDR alpha
		return;
	}

	// Feather the radiance itself to zero across the deep sub-LSB tail so even the
	// MEAN contribution has no step at the footprint edge (the bright glow, lum well
	// above ~0.7 LSB, is multiplied by 1.0 -> never dimmed; disc/phase/size/texture
	// and blood-moon behaviour are all untouched).
	col *= smoothstep( 0.0, 0.7 / 255.0, lum );

	// FULL-amplitude TPDF across the visible halo (mask == 1 for lum >= ~0.6 LSB, i.e.
	// the entire faint banding zone), feathered out only in the deep sub-LSB tail so
	// there is no hard dither edge.
	col += tpdf * smoothstep( 0.0, 0.6 / 255.0, lum );

	fragColor = vec4( col, 1.0 );   // additive (GL_SRC_ALPHA, GL_ONE) with a=1 over the body
}
)GLSL";
