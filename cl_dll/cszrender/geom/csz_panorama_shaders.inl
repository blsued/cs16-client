/*
 * csz_panorama_shaders.inl -- CSOZ renderer: sampled all-sky panorama GLSL (MW rework)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original clean-room GLSL: the
 * equirectangular view-dir -> galactic (l,b) -> equirect (u,v) mapping with a
 * textureGrad analytic-longitude seam fix, and the relocated moon sky-glow
 * (MoonSkyLum/MoonSkyGlow, moved here from the deleted procedural Milky Way
 * shader so the moon outer halo survives the rewrite). No shader source is copied
 * or translated from any license-tainted source. The sampled panorama is a
 * SEPARATELY-licensed data asset (NASA/GSFC SVS Deep Star Maps 2020, galactic
 * variant, Public Domain; Gaia DR2 star data; see CREDITS-sky-assets.md) loaded at
 * runtime -- NOT compiled into this source.
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
// Included ONLY by csz_panorama.cpp. GL3.3 core. An attrib-less fullscreen
// triangle (gl_VertexID quad-gen, salvaged verbatim from the deleted kStarsMwVs)
// reconstructs the per-fragment WORLD view direction from the camera basis
// (u_camFwd + u_camRight*ndc.x + u_camUp*ndc.y, pre-scaled by the half-FOV
// tangents host-side -- identical to the procedural MW pass it replaces, so the
// alignment is bit-for-bit the same frame). The fragment maps that direction into
// the EXISTING galactic basis and samples the equirect panorama, depositing
// PREMULTIPLIED linear-HDR radiance with alpha 0 via glBlendFunc(GL_ONE, GL_ONE)
// into the RGBA16F scene FBO, behind the live twinkle stars and the moon.

static const char kPanoramaVs[] = R"GLSL(#version 330 core
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

static const char kPanoramaFs[] = R"GLSL(#version 330 core
in vec3 v_dir;
out vec4 fragColor;

uniform sampler2D u_panorama;   // equirect RGB8 backdrop on sky unit 4 (kSkyTmuBase+0)
uniform float u_panoValid;      // 1 when the CSZP texture loaded, else 0 (sample -> 0; moon glow still emits)
uniform float u_panoIntensity;  // csz_pano_intensity: single master radiance scalar
uniform float u_panoLonOffset;  // csz_pano_lon_offset: longitude alignment nudge in UV (wraps via GL_REPEAT)
uniform float u_panoSat;        // csz_pano_saturation: runtime post-saturation (1.0 = bake default)
uniform vec3  u_panoTint;       // runtime warm/cool tint multiply (derived from csz_pano_warm; (1,1,1) = neutral)
uniform float u_nightFactor;    // 0..1 CHANNEL-2 backdrop night gate (field+MW band; fades in -9 -> -16 deg)
uniform float u_nightMoon;      // 0..1 moon-glow night gate (0 -> -7 deg, decoupled from the backdrop fade)

// --- galactic frame (orthonormalised host-side; the SAME basis the baked catalog +
//     the live bright-star gold-core path use, so band/stars/moon stay consistent) --
uniform vec3 u_galPole;         // world dir of galactic +Z (north galactic pole)
uniform vec3 u_galCenter;       // world dir of galactic +X (toward galactic centre, l=0)
uniform vec3 u_galY;            // world dir of galactic +Y (= cross(pole,center))

// --- T_moonBody occlusion (the opaque moon disc occludes the backdrop behind it) ---
uniform vec3  u_moonDir;        // moon disc centre (world unit)
uniform float u_moonAngR;       // moon angular RADIUS (rad); < 0 when the moon is not up -> mask T==1
uniform float u_moonSoft;       // limb-AA feather (rad) just outside the disc

// --- MoonSkyLum / MoonSkyGlow moon background wash (RELOCATED here from the deleted
//     procedural Milky Way FS so the moon outer halo + all-sky glow survive). This pass
//     now EMITS the moon background glow ONCE (single ownership); the star-points pass
//     keeps its OWN MoonSkyLum copy for per-star dimming (unchanged). -------------------
uniform float u_moonGlowL;      // L_moon = I_moon(phase) * moonAltFactor (0 = no wash)
uniform vec4  u_moonGlowCoef;   // (kA aureole, kM mie, kR rayleigh-pedestal, rho0 bounded-core deg)
uniform float u_moonGlowMax;    // GLOW_MAX cap on the f(rho) sum (anti overflow / NaN)
uniform float u_moonGlowEmit;   // emit gain: f(rho) sky-luminance -> background HDR radiance
uniform vec3  u_moonGlowWarm;   // WARM_INNER: moon disc + tight aureole, neutral-warm
uniform vec3  u_moonGlowCool;   // COOL_BLUE: far wide-angle Rayleigh pedestal (blue)

const float PI = 3.14159265358979;

// MoonSkyLum / MoonSkyGlow -- IDENTICAL f(rho) to the star VS so the relocated glow and
// the per-star dimming share one energy curve (single ownership). Bounded aureole core
// (rho0) so rho->0 cannot blow up; returns 0 when the moon is down/new (u_moonGlowL<=0).
float MoonSkyLum( vec3 viewDir )
{
	if( u_moonGlowL <= 0.0 )
		return 0.0;
	float cd   = clamp( dot( viewDir, u_moonDir ), -1.0, 1.0 );
	float rho  = degrees( acos( cd ) );
	float rho0 = u_moonGlowCoef.w;
	float aureole = u_moonGlowCoef.x / ( rho * rho + rho0 * rho0 );
	float mie     = u_moonGlowCoef.y * exp( -rho / 40.0 );
	float rayl    = u_moonGlowCoef.z * ( 1.06 + cd * cd );
	return min( u_moonGlowL * ( rayl + mie + aureole ), u_moonGlowMax );
}
vec3 MoonSkyGlow( vec3 viewDir )
{
	float s = MoonSkyLum( viewDir );
	if( s <= 0.0 )
		return vec3( 0.0 );
	float cd  = clamp( dot( viewDir, u_moonDir ), -1.0, 1.0 );
	float rho = degrees( acos( cd ) );
	vec3 glowCol = mix( u_moonGlowWarm, u_moonGlowCool, smoothstep( 2.0, 30.0, rho ) );
	return s * glowCol;
}

void main()
{
	vec3 dir = normalize( v_dir );

	// --- view dir -> galactic (l,b) -> equirect (u,v) ---------------------------------
	float x = dot( dir, u_galCenter );   // toward galactic centre (l=0)
	float y = dot( dir, u_galY );        // in-plane, 90deg from centre
	float z = dot( dir, u_galPole );     // galactic north
	float l = atan( y, x );              // galactic longitude [-pi,pi]
	float b = asin( clamp( z, -1.0, 1.0 ) ); // galactic latitude [-pi/2,pi/2]
	vec2  uv = vec2( l * ( 0.5 / PI ) + 0.5 + u_panoLonOffset, 0.5 - b / PI ); // band on v=0.5

	// Seam fix: atan(y,x) is C0-discontinuous at l=+/-pi, so dFdx(uv.u) spikes at the
	// wrap and the auto-LOD jumps -> a sharp mip seam. Sample with explicit CONTINUOUS
	// gradients: derive d(l) analytically from the continuous (x,y) field (the b axis is
	// already continuous). With WRAP_S=GL_REPEAT the UV itself wraps cleanly.
	float r2  = x * x + y * y;
	float dlx = ( x * dFdx( y ) - y * dFdx( x ) ) / max( r2, 1e-8 );
	float dly = ( x * dFdy( y ) - y * dFdy( x ) ) / max( r2, 1e-8 );
	vec2 dUVdx = vec2( dlx * ( 0.5 / PI ), -dFdx( b ) / PI );
	vec2 dUVdy = vec2( dly * ( 0.5 / PI ), -dFdy( b ) / PI );
	vec3 srgb = textureGrad( u_panorama, uv, dUVdx, dUVdy ).rgb;

	// COLOR SPACE (codex fix #5 -- explicit single choice): the offline bake stores the
	// backdrop sRGB-ENCODED RGB8 (sRGB packs the code range toward the darks, giving the
	// best 8-bit precision exactly where banding shows -- the deep navy dust lanes / faint
	// outskirts). Decode sRGB -> LINEAR here so the additive deposit into the LINEAR RGBA16F
	// HDR FBO is physically-correct radiance. This is NOT "sample sRGB as radiance" and NOT
	// an sRGB internal format; it is shader sRGB-decode on sample. ~2.2 gamma is the standard
	// fast approximation (exact for an additive deep-space backdrop is overkill).
	vec3 texel = pow( max( srgb, vec3( 0.0 ) ), vec3( 2.2 ) );

	// Runtime fine-tune (the bake carries the primary colour grade; just TWO knobs here to
	// avoid the FBM "forest of knobs" the brief warns against): post-saturation about the
	// per-texel luma, then a warm/cool tint multiply. Defaults (sat 1, tint (1,1,1)) are no-ops.
	float luma = dot( texel, vec3( 0.2126, 0.7152, 0.0722 ) );
	texel = max( mix( vec3( luma ), texel, u_panoSat ), vec3( 0.0 ) ) * u_panoTint;

	// Backdrop radiance: night-gated on the SAME curve as the twinkle stars. u_panoValid 0
	// (texture missing OR csz_pano master off) -> the backdrop contributes nothing, but the
	// moon glow below STILL emits (the moon halo is independent of panorama availability --
	// codex fix #7).
	vec3 radiance = texel * ( u_panoIntensity * u_nightFactor * u_panoValid );

	// T_moonBody: the opaque moon disc occludes the backdrop behind it (band must not
	// shine through the disc). PHASE-INDEPENDENT analytic mask; u_moonAngR < 0 -> T==1.
	float mAng      = acos( clamp( dot( dir, u_moonDir ), -1.0, 1.0 ) );
	float TmoonBody = smoothstep( u_moonAngR, u_moonAngR + max( u_moonSoft, 1e-5 ), mAng );
	radiance *= TmoonBody;

	// Moon background wash (atmospheric scatter IN FRONT of the moon -> NOT occluded by
	// T_moonBody). Emitted ONCE here; this is the moon's visible outer halo (the rho->0
	// inner segment of the glow) relocated out of the deleted procedural MW shader.
	vec3 moonGlow = MoonSkyGlow( dir ) * ( u_moonGlowEmit * u_nightMoon );
	vec3 col = radiance + moonGlow;

	// Signal-gated TPDF +/-1 LSB dither (Jimenez IGN + Gjol/Playdead TPDF): the default
	// resolve does not dither; gate by signal so faint backdrop edges decay to genuine
	// black (no positive pedestal -> no faint uniform fog -- the anti-fog red line).
	float ign1 = fract( 52.9829189 * fract( dot( gl_FragCoord.xy,        vec2( 0.06711056, 0.00583715 ) ) ) );
	float ign2 = fract( 52.9829189 * fract( dot( gl_FragCoord.xy + 17.0, vec2( 0.06711056, 0.00583715 ) ) ) );
	float gate = smoothstep( 0.0, 2.0 / 255.0, max( col.r, max( col.g, col.b ) ) );
	col += vec3( ( ign1 + ign2 - 1.0 ) / 255.0 ) * gate;

	// Premultiplied additive with alpha 0 (same blend contract as the stars).
	fragColor = vec4( max( col, vec3( 0.0 ) ), 0.0 );
}
)GLSL";
