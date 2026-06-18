/*
 * csz_water_shaders.inl -- CSOZ renderer: turb/water surface shader sources
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
 * csoz docs/notes/primext-render-mechanisms-m2.md); implemented by an agent
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
// This file is #included from csz_water.cpp (single translation unit); it is
// intentionally NOT listed in CMakeLists.
//
// Turb/water surface shaders (GLSL 330 core). Single forward pass, NO FBO, NO
// extra textures, NO loops: all wave/reflection math is closed-form analytic.
//
// Realism model (cheap, forward-only):
//  * Per-pixel normal is rebuilt from TWO cross-domain scrolling sine wave layers
//    over world-XY (different directions/speeds/scales). The analytic gradient of
//    the height field gives a believable shimmering surface normal -- no normal
//    map, no derivatives, just sin/cos. This drives BOTH Fresnel and specular so
//    highlights move with the waves.
//  * View-dependent Fresnel (Schlick) mixes a DEEP-WATER body color (top-down,
//    refraction-ish dark teal) with a REFLECTION color at grazing angles. The
//    reflection color is an analytic SKY GRADIENT: the view ray is reflected about
//    the perturbed normal and its up-component selects horizon->zenith, with the
//    horizon/zenith/sun colors derived from u_fog/u_moonColor and the day/night
//    u_phase (0=nightfall .. 0.5=midnight .. 1=daylight, same convention as sky).
//  * A tight sun/moon SPECULAR glint along the perturbed normal gives moving
//    sparkle. Rain (u_rain>0) adds concentric ripple rings + high-freq normal
//    detail and broadens the specular lobe so the surface reads as disturbed.
//  * Night tint (u_ambTint) and exp2 fog are preserved exactly as before.

static const char *kWaterVs =
	"#version 330 core\n"
	"layout(location=0) in vec3 a_pos;\n"
	"layout(location=1) in vec2 a_uv;\n"
	"layout(location=2) in vec3 a_normal;\n"
	"uniform mat4 u_viewProj;\n"
	"out vec2 v_uv;\n"
	"out vec3 v_normal;\n"
	"out vec3 v_worldPos;\n"
	"void main()\n"
	"{\n"
	"	v_uv = a_uv;\n"
	"	v_normal = a_normal;\n"
	"	v_worldPos = a_pos;\n"
	"	gl_Position = u_viewProj * vec4( a_pos, 1.0 );\n"
	"}\n";

static const char *kWaterFs =
	"#version 330 core\n"
	"in vec2 v_uv;\n"
	"in vec3 v_normal;\n"
	"in vec3 v_worldPos;\n"
	"uniform sampler2D u_tex;\n"		// diffuse, TMU 0
	"uniform float u_time;\n"
	"uniform vec3 u_camPos;\n"
	"uniform vec4 u_fog;\n"			// rgb = fog color, w = exp2 density
	"uniform vec3 u_ambTint;\n"		// night tint multiplier (1,1,1 = neutral)
	"uniform vec3 u_moonDir;\n"		// surface -> moon (light L), normalized
	"uniform vec3 u_moonColor;\n"
	"uniform float u_rain;\n"		// 0..1 rain intensity
	"uniform float u_phase;\n"		// 0=nightfall .. 0.5=midnight .. 1=daylight
	"out vec4 fragColor;\n"
	"\n"
	"// One scrolling sine wave layer. Returns height and accumulates the analytic\n"
	"// XY gradient (dH/dx,dH/dy) into g so the caller can build a normal cheaply.\n"
	"float waveLayer( vec2 p, vec2 dir, float freq, float speed, float amp,\n"
	"                 float t, inout vec2 g )\n"
	"{\n"
	"	float ph = dot( p, dir ) * freq + t * speed;\n"
	"	g += dir * ( freq * amp * cos( ph ) );\n"
	"	return amp * sin( ph );\n"
	"}\n"
	"\n"
	"// Analytic sky gradient sampled along a reflected ray's up-component.\n"
	"// Horizon/zenith/sun colors are derived from fog + moon + day/night phase,\n"
	"// so the reflection tracks the procedural sky without a cubemap or FBO.\n"
	"vec3 skyReflect( vec3 R, vec3 sunDir, vec3 sunCol, float phase )\n"
	"{\n"
	"	float day = clamp( ( phase - 0.5 ) * 2.0, 0.0, 1.0 );\n"		// 0 at/below midnight, 1 at daylight
	"	// Day: bright blue zenith + pale horizon. Night: deep indigo + cool haze.\n"
	"	vec3 zenithDay   = vec3( 0.18, 0.40, 0.78 );\n"
	"	vec3 horizonDay  = vec3( 0.62, 0.74, 0.86 );\n"
	"	vec3 zenithNight = vec3( 0.02, 0.04, 0.10 );\n"
	"	vec3 horizonNight= mix( vec3( 0.05, 0.07, 0.14 ), u_fog.rgb, 0.5 );\n"
	"	vec3 zenith  = mix( zenithNight,  zenithDay,  day );\n"
	"	vec3 horizon = mix( horizonNight, horizonDay, day );\n"
	"	float up = clamp( R.z * 0.5 + 0.5, 0.0, 1.0 );\n"			// world up = +Z
	"	vec3 sky = mix( horizon, zenith, pow( up, 0.65 ) );\n"
	"	// Reflected sun/moon disc smear toward the light direction.\n"
	"	float sd = pow( max( dot( R, sunDir ), 0.0 ), 8.0 );\n"
	"	sky += sunCol * sd * ( 0.3 + 0.7 * day );\n"
	"	return sky;\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"	float day = clamp( ( u_phase - 0.5 ) * 2.0, 0.0, 1.0 );\n"
	"\n"
	"	// 1. PROCEDURAL NORMAL: two cross-domain scrolling wave layers over world\n"
	"	//    XY build a height gradient; rain adds a third high-freq layer + rings.\n"
	"	vec2 p = v_worldPos.xy * 0.06;\n"			// world units -> wave domain
	"	vec2 grad = vec2( 0.0 );\n"
	"	float h = 0.0;\n"
	"	h += waveLayer( p, normalize( vec2(  1.0,  0.30 ) ), 1.00, 1.20, 0.150, u_time, grad );\n"
	"	h += waveLayer( p, normalize( vec2( -0.40, 1.0  ) ), 1.70, 0.90, 0.095, u_time, grad );\n"
	"	h += waveLayer( p, normalize( vec2(  0.20, -0.9 ) ), 2.60, 1.50, 0.055, u_time, grad );\n"
	"	float wetness = u_rain;\n"
	"	if( wetness > 0.0 )\n"
	"	{\n"
	"		// Extra fine chop + expanding concentric ripple rings from rain impact.\n"
	"		h += waveLayer( p, normalize( vec2( 0.7, -0.7 ) ), 3.40, 2.20, 0.022 * wetness, u_time, grad );\n"
	"		// Concentric rain rings. ONE cell tiling (~25u, 0.04 freq) drives BOTH\n"
	"		// the ring radius and its outward direction so the rings stay centered\n"
	"		// per cell instead of reading as fine chop (radius and direction used to\n"
	"		// be on mismatched 25u vs 1u tilings -> off-center noise).\n"
	"		vec2  cc   = fract( v_worldPos.xy * 0.04 ) - 0.5;\n"
	"		float r    = length( cc );\n"
	"		float ring = sin( r * 28.0 - u_time * 7.0 );\n"
	"		float fade = 1.0 - smoothstep( 0.0, 0.5, r );\n"
	"		grad += normalize( cc + 1e-4 ) * ( ring * fade * 0.09 * wetness );\n"
	"	}\n"
	"	// Perturb the geometric (mostly +Z) normal by the height gradient. The flat\n"
	"	// turb faces are horizontal, so tangent space ~= world XY; this is exact enough.\n"
	"	vec3 N = normalize( v_normal );\n"
	"	N = normalize( N - vec3( grad, 0.0 ) );\n"
	"\n"
	"	vec3 V = normalize( u_camPos - v_worldPos );\n"
	"	// Reflected ray uses a MORE perturbed normal than the lighting normal so the\n"
	"	// mirrored sky/horizon visibly smears and wobbles with the ripples (moving\n"
	"	// reflection feel) without over-roughening the specular/Fresnel below.\n"
	"	vec3 Nref = normalize( N - vec3( grad, 0.0 ) * 1.6 );\n"
	"	vec3 R = reflect( -V, Nref );\n"
	"\n"
	"	// 2. BODY (refraction-ish) color: the lightly-warped diffuse tex tinted\n"
	"    //    toward a deep teal so top-down reads as water, not raw rock.\n"
	"	vec2 warp = grad * 0.5;\n"
	"	vec3 tex = texture( u_tex, v_uv + warp ).rgb;\n"
	"	vec3 deepDay   = vec3( 0.03, 0.20, 0.26 );\n"
	"	vec3 deepNight = vec3( 0.01, 0.05, 0.09 );\n"
	"	vec3 deep = mix( deepNight, deepDay, day );\n"
	"	// Lean harder on the water body color (less raw texture) so the surface\n"
	"	// reads as water rather than wet rock even from straight overhead.\n"
	"	vec3 body = mix( deep, tex * vec3( 0.40, 0.78, 0.92 ), 0.30 );\n"
	"\n"
	"	// 3. FRESNEL: grazing angles reflect the analytic sky, top-down shows body.\n"
	"	vec3 sunCol = ( day > 0.0 ) ? mix( u_moonColor, vec3( 1.0, 0.96, 0.85 ), day ) : u_moonColor;\n"
	"	vec3 refl = skyReflect( R, u_moonDir, sunCol, u_phase );\n"
	"	// f0 lifted well above physical 0.02 so the mirrored sky is visible even\n"
	"	// from above, and a softer Schlick exponent so the reflection ramps in\n"
	"	// earlier toward grazing -- water must read as reflective, not flat.\n"
	"	float f0 = 0.10;\n"
	"	float graze = 1.0 - max( dot( N, V ), 0.0 );\n"
	"	float fres = f0 + ( 1.0 - f0 ) * pow( graze, 3.5 );\n"
	"	vec3 col = mix( body, refl, clamp( fres, 0.0, 1.0 ) );\n"
	"	// Grazing-edge brightening: a strong bright wet-mirror rim toward the far\n"
	"	// edge so the water surface is unmistakable at shallow view angles.\n"
	"	col += refl * pow( graze, 3.0 ) * 0.65;\n"
	"\n"
	"	// 4. NIGHT INTEGRATION: cool the body toward the night tint + soft moon wash.\n"
	"	col *= mix( u_ambTint, vec3( 1.0 ), day );\n"
	"	col += body * u_moonColor * max( dot( N, u_moonDir ), 0.0 ) * 0.20 * ( 1.0 - day );\n"
	"\n"
	"	// 5. SUN/MOON SPECULAR GLINT along the perturbed normal. Rain broadens the\n"
	"	//    lobe (rougher surface) and lifts the diffuse light energy a touch.\n"
	"	vec3  H = normalize( u_moonDir + V );\n"
	"	float shin = mix( 140.0, 40.0, wetness );\n"			// rain -> broader, rougher
	"	float spec = pow( max( dot( N, H ), 0.0 ), shin );\n"
	"	// Bright, clearly visible sun/moon glint that sparkles as the waves move.\n"
	"	col += sunCol * spec * ( 2.2 + 1.0 * day );\n"
	"\n"
	"	// 6. FOG: same exp2 falloff as the world pass.\n"
	"	float d = gl_FragCoord.z / gl_FragCoord.w;\n"
	"	float fog = ( u_fog.w > 0.0 ) ? clamp( exp2( -u_fog.w * d ), 0.0, 1.0 ) : 1.0;\n"
	"	fragColor = vec4( mix( u_fog.rgb, col, fog ), 1.0 );\n"
	"}\n";
