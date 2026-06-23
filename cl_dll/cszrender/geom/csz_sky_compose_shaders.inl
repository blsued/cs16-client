/*
 * csz_sky_compose_shaders.inl -- CSOZ renderer: HDR resolve GLSL sources
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
// Included ONLY by csz_sky_compose.cpp. Uniform names are this program's
// private contract. GLES3 equivalence assumption: nothing here beyond the
// GL3.3 core / GLES3 / WebGL2 intersection (code-standards section 7); the only
// notable feature is texelFetch (core in all three).

// Vertex stage: VAO-less fullscreen triangle via gl_VertexID (same pattern as
// csz_sky_shaders.inl kSkyVs). The third vertex is pushed to (3,-1)/(-1,3) so
// the clip triangle fully contains [-1,1]^2. clip z = 1 (depth disabled anyway).
static const char kComposeVs[] = R"GLSL(#version 330 core
void main()
{
	vec2 ndc = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0,
	                 ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( ndc, 1.0, 1.0 );
}
)GLSL";

// Fragment stage: the FULL resolve chain, identity-calibrated at default
// (red-team Design A -- display-space passthrough). Stages:
//   1. fetch HDR color (texelFetch, EXACT 1:1 texel copy -- red-team fix #8,
//      GL_LINEAR half-texel sampling would blur; account for the viewport
//      origin so a non-(0,0) viewport still maps 1:1).
//   2. exposure multiply (u_exposure; default 1.0 = identity).
//   3. purkinje() hook -- identity until C4 fills it.
//   4. tonemap: u_tonemap 0 = identity (Design A default, display-space pass-
//      through); 1 = ACES filmic (Narkowicz fit) which EXPECTS LINEAR radiance
//      from C2+ (do not enable in C1's display-space scene -- documented).
//   5. sRGB OETF: only when u_encode != 0 (Design A default 0 = no OETF, so we
//      never double-encode the already-display-space scene). Present for the
//      linear-pipeline future (paired with tonemap=1).
//   6. dither: TPDF in 8-bit space, only when u_dither != 0 (default 0 so the
//      C1 no-regression A/B is byte-clean -- red-team fix #7). Anti-banding tool
//      for C2+ smooth HDR gradients (rubric D9).
//   7. write opaque alpha 1.0 to the non-sRGB RGBA8 backbuffer (FRAMEBUFFER_SRGB
//      is asserted OFF on the CPU side before this pass runs).
static const char kComposeFs[] = R"GLSL(#version 330 core
uniform sampler2D u_hdr;        // HDR scene color (RGBA16F), bound on a sky unit
uniform ivec2 u_viewOrigin;     // viewport origin (rvp->viewport.xy) for the 1:1 fetch
uniform float u_exposure;       // csz_exposure (default 1.0)
uniform int u_tonemap;          // csz_tonemap: 0 = identity, 1 = ACES filmic
uniform int u_encode;           // csz_encode: 0 = no OETF (display passthrough), 1 = sRGB OETF
uniform int u_dither;           // csz_dither: 0 = off (byte-clean), 1 = TPDF dither
uniform float u_hlRolloff;      // csz_highlight_rolloff: identity-path overbright shoulder strength (0 = off/legacy, 1 = full)
uniform float u_hlKnee;         // csz_highlight_knee: shoulder onset; maxRGB <= knee returned bit-identical
// S4 night grade (REWORK-SPEC §S4 §4/§5 + codex finding8). The whole grade is GATED by
// u_nightness so the DAY image is bit-identical (u_nightness 0 -> the block is skipped).
uniform float u_nightness;      // phase night gate [0,1] (PublishLighting); 0 = day -> night grade skipped
uniform float u_nightExposure;  // csz_night_exposure: exposure mult at full night (1.0 = none; <1 darker mood)
uniform float u_nightToe;       // csz_night_toe: shadow-lift strength at full night (readability; 0 = off)
uniform float u_toeGamma;       // csz_night_toe_gamma: gamma for the shadow lift (>1 lifts shadows/midtones)
uniform float u_purkinje;       // csz_purkinje: scotopic desaturation + cool-shift strength (0 = off)
uniform float u_purkinjeKnee;   // csz_purkinje_knee: luma above which chroma is preserved (moon/sky/star anchor)
out vec4 fragColor;

// S4 §5 Purkinje / scotopic shift (REWORK-SPEC §S4 + codex finding8 = the SINGLE unified
// cool grade for world+studio+all, retiring the per-shader world-only cool grade). At low
// luminance human rod vision desaturates and shifts cool-blue. Desaturate toward luma and
// apply a cool scotopic tint, WEIGHTED by a mesopic mask so bright anchors (moon disc, stars,
// Milky Way, moonlit highlights) keep their color and only the dark scene cools -- i.e. the
// cool atmosphere comes from DESATURATION, not from lowering brightness (§7 ③). strength 0 ->
// EXACT identity (so the engage is continuous as nightness->0 and the day branch is skipped).
vec3 scotopicShift( vec3 c, float strength, float knee )
{
	const vec3 kScotopicTint = vec3( 0.85, 0.95, 1.15 );   // REWORK-SPEC §S4 cool rod tint (kept modest -- moonlight is already lightly cool)
	float lum = dot( c, vec3( 0.2126, 0.7152, 0.0722 ));
	float meso = ( 1.0 - smoothstep( 0.0, max( knee, 1e-4 ), lum )) * clamp( strength, 0.0, 1.0 );
	vec3 desat = mix( c, vec3( lum ), meso * 0.6 );          // partial desaturation toward luma
	return desat * mix( vec3( 1.0 ), kScotopicTint, meso );  // cool-blue shift, strongest in the dark end
}

// S4 §4 night shadow-lift (TOE). The display-space scene crushes dark detail at night
// ("整个地图很黑"); a gentle gamma lift raises shadows/midtones so silhouettes stay readable
// (competitive: dark must not become pay-to-win). amt 0 -> identity. The moon-highlight
// SHOULDER is the existing highlightShoulder() below (no separate night shoulder needed).
vec3 nightToe( vec3 c, float amt, float gamma )
{
	vec3 lifted = pow( max( c, vec3( 0.0 )), vec3( 1.0 / max( gamma, 1e-3 )));
	return mix( c, lifted, clamp( amt, 0.0, 1.0 ));
}

// ACES filmic tonemap, Narkowicz fitted rational approximation; numeric
// coefficients/formula only, no source copied. Rational form derived inline:
//   f(x) = ( x*(a*x + b) ) / ( x*(c*x + d) + e )
// with the standard published fit constants a=2.51, b=0.03, c=2.43, d=0.59,
// e=0.14. Operates per-channel on LINEAR radiance; result clamped to [0,1].
vec3 acesFilmic( vec3 x )
{
	const float a = 2.51;
	const float b = 0.03;
	const float c = 2.43;
	const float d = 0.59;
	const float e = 0.14;
	vec3 num = x * ( a * x + b );
	vec3 den = x * ( c * x + d ) + e;
	return clamp( num / den, 0.0, 1.0 );
}

// sRGB OETF (linear -> display). IEC 61966-2-1 piecewise curve, formula only.
vec3 srgbOetf( vec3 c )
{
	c = clamp( c, 0.0, 1.0 );
	vec3 lo = c * 12.92;
	vec3 hi = 1.055 * pow( c, vec3( 1.0 / 2.4 ) ) - 0.055;
	return mix( lo, hi, step( vec3( 0.0031308 ), c ) );
}

// L-polish A: identity-path highlight shoulder. Multiple additive flashlight beams
// overlapping one surface push the display-space color well past 1.0 and clip to flat
// RGB=255 ("dead white"). Under the identity tonemap (csz_tonemap 0) there is no rolloff
// to absorb that. This soft knee compresses ONLY the channel max above `knee`: any pixel
// whose maxRGB <= knee is returned BIT-IDENTICAL (the approved <0.95 look is preserved
// per-pixel), while >knee highlights roll smoothly toward 1.0 instead of clipping. The
// hue is preserved -- all channels scale by the same factor (rolled/m). ACES (tonemap 1)
// already rolls off, so this is never applied on that path.
vec3 highlightShoulder( vec3 c, float knee )
{
	float m = max( max( c.r, c.g ), c.b );
	if( m <= knee )
		return c;
	float x = m - knee;
	float rolled = knee + ( 1.0 - knee ) * x / ( x + ( 1.0 - knee ) );
	return c * ( rolled / max( m, 1e-4 ) );
}

void main()
{
	// 1. Exact 1:1 texel copy (texelFetch; account for the viewport origin).
	ivec2 texel = ivec2( gl_FragCoord.xy ) - u_viewOrigin;
	vec3 c = texelFetch( u_hdr, texel, 0 ).rgb;

	// 2. Exposure.
	c *= u_exposure;

	// 3. S4 night grade (REWORK-SPEC §S4 §4/§5 + finding8). ENTIRELY gated by u_nightness:
	//    at day (u_nightness == 0) the block is skipped -> the resolve is BIT-IDENTICAL to the
	//    pre-S4 pipeline (zero daytime regression). Each sub-op is ALSO identity as nightness->0
	//    so the engage is continuous across the branch. Order: night exposure -> shadow-lift toe
	//    -> unified Purkinje (world+studio+all). The moon-highlight shoulder is step 4 below.
	if( u_nightness > 0.0 )
	{
		float nn = clamp( u_nightness, 0.0, 1.0 );
		c *= mix( 1.0, u_nightExposure, nn );                       // §4 night exposure (mood; 1.0 = none)
		c = nightToe( c, nn * u_nightToe, u_toeGamma );             // §4 shadow lift (readability)
		c = scotopicShift( c, nn * u_purkinje, u_purkinjeKnee );    // §5 + finding8 unified Purkinje
	}

	// 4. Tonemap.
	if( u_tonemap == 1 )
		c = acesFilmic( c );   // ACES mode expects LINEAR radiance (C2+)
	else if( u_hlRolloff > 0.0 )
		// Identity path (display-space): soft-knee the overbright multi-beam highlights
		// only; rolloff scales between legacy (0) and full shoulder (1). (L-polish A)
		c = mix( c, highlightShoulder( c, u_hlKnee ), u_hlRolloff );

	// 5. sRGB OETF (off by default so we never double-encode the display-space
	//    scene; on for the linear pipeline future, paired with tonemap=1).
	if( u_encode != 0 )
		c = srgbOetf( c );

	// 6. Dither (off by default; TPDF +/-1 LSB in 8-bit space, two decorrelated
	//    interleaved-gradient-noise samples; screen-space, deterministic per
	//    pixel per frame -- no time term, clean A/B. Jimenez IGN base; Gjol/
	//    Playdead TPDF technique, reimplemented from the public math).
	if( u_dither != 0 )
	{
		float ign1 = fract( 52.9829189 * fract( dot( gl_FragCoord.xy,        vec2( 0.06711056, 0.00583715 ) ) ) );
		float ign2 = fract( 52.9829189 * fract( dot( gl_FragCoord.xy + 17.0, vec2( 0.06711056, 0.00583715 ) ) ) );
		c += ( ign1 + ign2 - 1.0 ) / 255.0;
	}

	// 7. Opaque write to the non-sRGB RGBA8 backbuffer.
	fragColor = vec4( c, 1.0 );
}
)GLSL";
