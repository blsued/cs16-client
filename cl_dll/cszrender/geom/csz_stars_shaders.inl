/*
 * csz_stars_shaders.inl -- CSOZ renderer: star field + Milky Way GLSL (C4)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Original clean-room GLSL: the
 * energy-normalised Gaussian star PSF, the B-V -> Teff (Ballesteros 2012) ->
 * Planckian-locus colour model, and the procedural Milky Way band are our own
 * work, written from published physical/empirical FORMULAS (facts, not
 * copyrightable). No shader source is copied or translated from any
 * license-tainted source (see csoz docs/provenance.md). The Milky Way is fully
 * procedural (zero licensing surface); star positions / magnitudes / B-V come
 * from BSC5 numeric facts baked into csz_stars_catalog.inl (CDS/VizieR
 * acknowledgement in CREDITS-stars.md).
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
// Included ONLY by csz_stars.cpp. GL3.3 core (GLES3/WebGL2 intersection: no
// geometry/compute/instancing). Each star is drawn as a screen-aligned QUAD: the
// VBO repeats every star for its 6 quad corners and a plain
// glDrawArrays(GL_TRIANGLES, 0, starCount*6) renders them, so only GL entry
// points already in the renderer's loader table are used (no
// glDrawArraysInstanced / glVertexAttribDivisor). Both the stars and the Milky
// Way deposit PREMULTIPLIED linear-HDR radiance with alpha 0 via
// glBlendFunc(GL_ONE, GL_ONE) into the RGBA16F scene FBO; the single resolve does
// the display conversion (default = identity passthrough).

// =============================================================================
// STAR POINTS -- conserved-total-flux, energy-normalised Gaussian PSF.
//
// Model (csz_stars_math + STAR-REWRITE-DESIGN spec): the conserved quantity is a
// star's TOTAL flux F(m) = Fref * 10^(-0.4*beta*(m-mref)); the PSF only spreads F
// over pixels, so the deposited energy is independent of the spot size AND of the
// output resolution. The centre pixel value is the DERIVED quantity
// center = F/(2*pi*sigma^2). This replaces the old "fix the centre peak, clip at
// 255" model whose magnitude->peak clamp drove ~10k stars to an identical ~255
// (the "TV snow"). Now a handful of bright stars dominate and the faint majority
// deposits its true small energy down to the noise floor -- a real log-magnitude
// distribution. Colour comes from B-V via the Planckian locus with a
// brightness-coupled saturation (faint stars near white, only heroes tinted).
// =============================================================================
static const char kStarsVs[] = R"GLSL(#version 330 core
// Per-vertex attributes from a 5-float-per-star VBO, each star repeated for its 6
// quad corners (corner index = gl_VertexID % 6).
layout(location = 0) in vec3  a_dir;    // world unit direction (Quake Z-up)
layout(location = 1) in float a_bv;     // B-V colour index
layout(location = 2) in float a_vmag;   // visual magnitude

uniform mat4  u_viewProj;
uniform vec3  u_viewportPx;    // (w,h,_) in pixels (.z unused; vec3 for glUniform3fv)
uniform float u_time;
uniform float u_nightFactor;   // 0..1 star visibility (sun elevation driven)
uniform float u_intensity;     // csz_stars_intensity: master flux scale
uniform float u_sizeMul;       // csz_stars_size: master sigma scale (look only)
uniform float u_colorSat;      // csz_stars_color_sat: chroma strength
uniform float u_twAmp;         // csz_stars_twinkle * base amplitude
uniform float u_twSpeed;       // csz_stars_twinkle_speed: Hz multiplier on the per-star 1.5-6 Hz base
uniform float u_twChroma;      // csz_stars_twinkle_chroma: luminance-preserving colour-flash strength (horizon + bright)
uniform float u_twElev;        // csz_stars_twinkle_elev: airmass exponent (ampAir = X^elev)
uniform float u_twBrightBias;  // csz_stars_twinkle_brightbias: faint-star amplitude floor brightW=mix(this,1,brightT)
uniform float u_twClampLo;     // strobe guard, lower multiplier clamp (never-extinguish floor)
uniform float u_twClampHi;     // strobe guard, upper multiplier clamp
uniform float u_resScale;      // viewportH / 1080 (sigma scales with resolution)
uniform float u_magLimit;      // csz_pano_twinkle_maglimit: the live layer renders ONLY stars with vmag <= this (the rest live in the sampled panorama)
// --- T_moonBody occlusion (SKY-REWORK-SPEC v3 Task C) ---------------------------
uniform vec3  u_moonDir;       // moon disc centre (world unit). The opaque moon BODY occludes layer1 behind it.
uniform float u_moonAngR;      // moon angular RADIUS (rad). < 0 when the moon is not up -> mask disabled (T==1).
uniform float u_moonSoft;      // limb-AA feather (rad) just OUTSIDE the disc
// --- MoonSkyLum moonlight wash (SKY-REWORK-SPEC v3 Task D) -----------------------
// Single-ownership f(rho) sky-luminance the moon adds along a view direction. Here it
// drives only the per-star photometric DIMMING (it does NOT emit radiance in this
// pass -- the background glow is emitted ONCE in the Milky Way FS). u_moonGlowL = 0
// when the moon is down / new -> MoonSkyLum() == 0 -> the dark-sky field is untouched.
uniform float u_moonGlowL;     // L_moon = I_moon(phase) * moonAltFactor (0 = no wash)
uniform vec4  u_moonGlowCoef;  // (kA aureole, kM mie, kR rayleigh-pedestal, rho0 bounded-core deg)
uniform float u_moonGlowMax;   // GLOW_MAX cap on the f(rho) sum (anti overflow / NaN)
uniform vec2  u_starDim;       // (c, k) moonlight star-dimming: threshold = moonLum*c .. moonLum*c*k

flat out vec3  v_color;
flat out float v_flux;         // TOTAL deposited energy; FS centre = v_flux/(2*pi*sigma^2)
flat out float v_sigmaPx;
out vec2 v_deltaPx;            // this fragment's pixel offset from the star centre

// ---- baked-in realistic defaults (calibrated to the default passthrough resolve) ----
const float kFluxRef   = 20.0;         // F at m=0. CALIBRATED (2026-06-19) so a typical
                                       // open-sky bright star (vmag 1-2) lands ~120-180/255
                                       // and heroes (vmag<=0) saturate to a punchy core at
                                       // 1080p identity-passthrough resolve. This is a pure
                                       // LINEAR flux scale: the log-magnitude distribution is
                                       // unchanged (faint majority stays sub-visible), so it
                                       // cannot reintroduce the old clamp "TV snow".
                                       // csz_stars_intensity (default 1.0) is the user's live
                                       // multiplier around this calibrated anchor.
const float kBeta      = 0.85;         // gentle compression of the Pogson range
const float kMagRef    = 0.0;
const float kLog2_10   = 3.321928095;  // log2(10): 10^x = exp2(x*log2(10))
const float kSigmaMin  = 0.70;         // faint-fill spot sigma (px)
const float kSigmaMax  = 1.70;         // hero spot sigma ceiling (px)
const float kSigmaGrow = 1.00;
const float kMBright   = -1.0;         // brightT = 1 at/above this magnitude
const float kMFaint    =  5.0;         // brightT = 0 at/below this magnitude
const float kClipSig   = 3.0;          // quad half-extent = 3 sigma (<1.2% energy clipped)
const float kSatMin    = 0.0;
const float kSatMax    = 0.85;
const float kSatCap    = 0.65;         // saturation ceiling (no Christmas lights)
const float kExagMul   = 1.6;          // hue exaggeration (capped by kSatCap)
const float kFSatLo    = 0.12;         // intrinsic flux ~vmag 5: colour begins to read.
const float kFSatHi    = 1.85;         // intrinsic flux ~vmag 1.5: full colour reached.
                                       // CALIBRATED (2026-06-19) DOWN from 0.574/6.0 so the
                                       // typical in-frame bright stars (vmag 1-2.5, not just
                                       // the rare vmag<=0 heroes) carry visible chroma --
                                       // hot blue stars read blue-white, cool stars orange.
                                       // The gate stays on INTRINSIC flux (no night/intensity/
                                       // twinkle drift); brighter heroes still wash toward
                                       // white at the clipped core but keep hue in the wings.

const vec2 kCorners[6] = vec2[6](
	vec2( -1.0, -1.0 ), vec2( 1.0, -1.0 ), vec2( -1.0, 1.0 ),
	vec2( -1.0,  1.0 ), vec2( 1.0, -1.0 ), vec2(  1.0, 1.0 ) );

// 0 at the faint end (m=kMFaint), 1 at the bright end (m=kMBright < kMFaint).
// Written as an explicit monotone curve so the edges are never reversed (a
// reversed GLSL smoothstep(edge0>edge1,..) is undefined).
float brightT( float m )
{
	float t = clamp( ( kMFaint - m ) / ( kMFaint - kMBright ), 0.0, 1.0 );
	return t * t * ( 3.0 - 2.0 * t );
}

// B-V colour index -> blackbody temperature (Ballesteros 2012; published fact).
float bvToTemp( float bv )
{
	bv = clamp( bv, -0.4, 2.0 );
	return 4600.0 * ( 1.0 / ( 0.92 * bv + 1.7 ) + 1.0 / ( 0.92 * bv + 0.62 ) );
}

// Blackbody temperature -> chroma RGB on the Planckian locus (Tanner-Helland
// piecewise empirical fit; public formula). Normalised to max channel = 1 so it
// carries HUE only. The locus runs blue-white <-> yellow <-> orange-red; green
// and magenta are physically impossible here, so the result never goes rainbow.
// HONESTY (not a physical SPD): Tanner-Helland is a *display-space* (approx sRGB)
// empirical fit; we use it directly as an HDR hue tint. It is NOT a strict
// physically-linear stellar spectrum -- it is a plausible chroma ramp, not a
// radiometric colour. (No claim of strict physical linearity is made.)
vec3 planckianRGB( float t )
{
	t = clamp( t, 1000.0, 40000.0 ) / 100.0;
	float r, g, b;
	if( t <= 66.0 ) r = 255.0;
	else            r = 329.698727446 * pow( t - 60.0, -0.1332047592 );
	if( t <= 66.0 ) g = 99.4708025861 * log( t ) - 161.1195681661;
	else            g = 288.1221695283 * pow( t - 60.0, -0.0755148492 );
	if( t >= 66.0 )      b = 255.0;
	else if( t <= 19.0 ) b = 0.0;
	else                 b = 138.5177312231 * log( t - 10.0 ) - 305.0447927307;
	vec3 c = clamp( vec3( r, g, b ), 0.0, 255.0 );
	float m = max( c.r, max( c.g, c.b ) );
	return c / max( m, 1.0 );
}

// MoonSkyLum (SKY-REWORK-SPEC v3 Task D, single-ownership f(rho)) -- the scalar sky
// luminance the moon adds at viewDir. K&S-style aureole + Mie + Rayleigh pedestal:
//   aureole = kA/(rho^2 + rho0^2)  -- BOUNDED CORE (rho0~1.5deg) replaces a bare rho^-2
//                                     so rho->0 cannot blow up to white / NaN.
//   mie     = kM*exp(-rho/40deg)   -- mid-angle aerosol forward scatter.
//   rayl    = kR*(1.06 + cos^2)    -- all-sky pedestal (never zero).
// Capped by GLOW_MAX. Returns 0 when the moon is down/new (u_moonGlowL<=0). The SAME
// scalar drives the per-star dimming here AND the background glow in the MW FS, so the
// f(rho) energy is computed once (spec single-ownership: no double-count with Task C).
float MoonSkyLum( vec3 viewDir )
{
	if( u_moonGlowL <= 0.0 )
		return 0.0;
	float cd   = clamp( dot( viewDir, u_moonDir ), -1.0, 1.0 );
	float rho  = degrees( acos( cd ) );                 // 0..180 deg
	float rho0 = u_moonGlowCoef.w;
	float aureole = u_moonGlowCoef.x / ( rho * rho + rho0 * rho0 );
	float mie     = u_moonGlowCoef.y * exp( -rho / 40.0 );
	float rayl    = u_moonGlowCoef.z * ( 1.06 + cd * cd );
	return min( u_moonGlowL * ( rayl + mie + aureole ), u_moonGlowMax );
}
)GLSL"
// SPLIT (2026-06-20, Task D): the kStarsVs source grew past the MSVC ~16KB single-
// string-literal limit (C2026) when Task D added MoonSkyLum + the wash uniforms.
// Adjacent string literals concatenate at translation -> the GLSL source is byte-
// identical; this is a pure compile-seam, NOT a behaviour change.
R"GLSL(
void main()
{
	// Live magnitude cutoff (codex fix #8): the sampled panorama supplies the dense dim
	// starfield, so the live twinkle layer renders ONLY stars brighter than the cutoff
	// (vmag <= u_magLimit, default 3.8). Honored PER-FRAME off the live uniform -- the VBO
	// holds all BSC5 stars, so raising csz_pano_twinkle_maglimit reveals more live-twinkle
	// stars WITHOUT a VBO rebuild (the cvar is not ignored by a build-time VBO).
	if( a_vmag > u_magLimit )
	{
		gl_Position = vec4( 2.0, 2.0, 2.0, 1.0 );   // culled: outside the clip volume
		v_deltaPx = vec2( 0.0 ); v_color = vec3( 0.0 ); v_flux = 0.0; v_sigmaPx = 1.0;
		return;
	}

	// Project the direction at infinity (w=0 drops camera translation).
	vec4 clip = u_viewProj * vec4( a_dir, 0.0 );
	if( clip.w <= 0.0 )
	{
		gl_Position = vec4( 2.0, 2.0, 2.0, 1.0 );   // behind camera: clip away
		v_deltaPx = vec2( 0.0 ); v_color = vec3( 0.0 ); v_flux = 0.0; v_sigmaPx = 1.0;
		return;
	}
	vec2 ndc = clip.xy / clip.w;

	// Intrinsic TOTAL flux (conserved). exp2 form avoids a pow() with a non-const base.
	float flux = kFluxRef * exp2( -0.4 * kBeta * ( a_vmag - kMagRef ) * kLog2_10 );

	// PSF size (px): grows gently with brightness; the energy norm keeps the centre
	// value = F/(2*pi*sigma^2) -- size and brightness stay DECOUPLED. resScale keeps
	// the spot a constant screen fraction across resolutions (so total energy, not
	// the pixel footprint, is what is invariant -- the cross-resolution stability test).
	float sigmaPx = clamp( kSigmaMin + kSigmaGrow * brightT( a_vmag ), kSigmaMin, kSigmaMax )
	              * u_resScale * u_sizeMul;

	// Colour from B-V (Planckian) with brightness-coupled saturation, gated on the
	// INTRINSIC flux so the hue does not drift with night factor / intensity / twinkle.
	vec3 hueRGB = planckianRGB( bvToTemp( a_bv ) );
	float sat = kSatMin + ( kSatMax - kSatMin ) * smoothstep( kFSatLo, kFSatHi, flux );
	sat = clamp( sat * u_colorSat, 0.0, kSatCap );

	// MW REWORK (2026-06-21): the gold-core band-star saturation FLOOR was DELETED here
	// (it floored the chroma of near-galactic-centre BAND-fill stars -- a population that
	// no longer exists; the Milky Way warm core now lives in the sampled panorama). The
	// kept real BSC5 stars use the plain brightness-coupled saturation above.
	vec3 hueExag = clamp( vec3( 1.0 ) + ( hueRGB - vec3( 1.0 ) ) * kExagMul, 0.0, 1.0 );
	v_color = mix( vec3( 1.0 ), hueExag, sat );

	// ========================================================================
	// Per-star twinkle -- atmospheric scintillation. FULL REWRITE to SKY-REWORK
	// -SPEC v3 Task B frozen numeric contract (Section 4 + STAR-SCINTILLATION
	// -RESEARCH). Every star's PHASE, FREQUENCY and AMPLITUDE is driven by its
	// own hash seed, so the field is fully DECORRELATED -- there is NO global
	// sin(time) term lacking a per-star frequency+phase, hence it never breathes
	// as one. The boil noise is continuous, zero-mean and bounded [-1,1];
	// amplitude follows the airmass law amp ~ X^0.875 with a hard cap, is right-
	// skewed (skew = s^1.5, softened from s*s on 2026-06-20 so the obviously-twinkling
	// share lands in spec's 20-40% rather than the ~0.3% the s*s skew produced) and is
	// damped on the faint majority (brightW floor). tw = clamp(1 + amp*n) is SYMMETRIC
	// (E[n]~0 -> E[tw]~1) so mean sky luminance is preserved -- NOT a log-normal
	// exp() (which biases up). All terms are per-star (a_dir/a_vmag are constant
	// across the 6 quad verts -> flat-correct), ADDITIVE/ORTHOGONAL to the bold-MW
	// band terms below. The Moon and bright planets are NOT star vertices in this
	// pass (the Moon is a separate sunmoon pass; the BSC5 catalogue carries no
	// planets), so "amp_i = 0 for Moon/planets" holds by construction here.

	// Decorrelated per-star seeds: two PCG32 hashes of the star index give six
	// independent streams (amp-skew, base frequency, three octave phases, chroma
	// phase) -- better distributed than sin(dot) variants, which collinearly-
	// correlate for nearby a_dir -> patchy synchronised blinking. Derived in-shader
	// from gl_VertexID/6, so NOTHING is baked into the VBO (the 5-float layout
	// {dir.xyz,bv,vmag} stays byte-identical -- spec Section 8 seam).
	uint  h1 = uint( gl_VertexID / 6 ) * 747796405u + 2891336453u;
	h1 ^= h1 >> 16; h1 *= 2246822519u; h1 ^= h1 >> 13; h1 *= 3266489917u; h1 ^= h1 >> 16;
	uint  h2 = ( uint( gl_VertexID / 6 ) ^ 0x9E3779B9u ) * 747796405u + 2891336453u;
	h2 ^= h2 >> 16; h2 *= 2246822519u; h2 ^= h2 >> 13; h2 *= 3266489917u; h2 ^= h2 >> 16;
	float sAmp = float(   h1         & 0xFFFFu ) * ( 1.0 / 65535.0 );        // amp right-skew seed
	float sFrq = float( ( h1 >> 16 ) & 0xFFFFu ) * ( 1.0 / 65535.0 );        // base frequency seed
	float ph0  = float(   h2         & 0xFFFFu ) * ( 6.2831853 / 65535.0 );  // octave-1 phase
	float ph1  = float( ( h2 >> 16 ) & 0xFFFFu ) * ( 6.2831853 / 65535.0 );  // octave-2 phase
	float ph2  = float( ( h1 ^ h2 )  & 0xFFFFu ) * ( 6.2831853 / 65535.0 );  // octave-3 phase
	float phC  = float( ( h1 + h2 )  & 0xFFFFu ) * ( 6.2831853 / 65535.0 );  // chroma phase

	// Airmass amplitude law (spec Section 4, frozen). sinAlt floored at 0.05
	// (~2.9 deg) -> X capped at 20; ampAir = X^u_twElev (default exponent 0.875).
	// a_dir.z is sin(altitude) in the Z-up world.
	float sinAlt  = clamp( a_dir.z, 0.05, 1.0 );
	float X       = 1.0 / sinAlt;
	float ampAir  = pow( X, u_twElev );

	float bt      = brightT( a_vmag );                  // 0 (faint) .. 1 (bright)
	float skew_i  = pow( sAmp, 1.5 );                   // right-skew (SOFTENED s*s -> s^1.5, 2026-06-20):
	                                                    // still right-skewed (most calm) but lifts the
	                                                    // mid-skew majority ~1.4-1.8x so more stars reach
	                                                    // a visible amp, per the amplitude-calibration retune.
	float brightW = mix( u_twBrightBias, 1.0, bt );     // faint stars near-steady (floor default 0.15)
	float amp_i   = clamp( u_twAmp * skew_i * ampAir * brightW, 0.0, 0.88 ); // hard cap 0.88 (anti-strobe:
	                                                    // |n|max~1.0 -> tw in [0.12,1.88], strictly inside
	                                                    // the [0.10,2.0] clamps, never squares off)

	// Visible flicker frequency 1.5-6 Hz per star, scaled by u_twSpeed (NOT the
	// physical 50-100 Hz: human flicker-fusion only resolves a few Hz -- research A5).
	float f_i = mix( 1.5, 6.0, sFrq ) * u_twSpeed;     // Hz
	float w_i = 6.2831853 * f_i;                       // rad/s

	// Continuous, zero-mean, bounded [-1,1] boil: 3 incommensurate (non-harmonic)
	// octaves (ratios 1 : 2.3 : 4.0), each with its OWN per-star phase -> organic,
	// never re-periodises. Peak 0.60+0.30+0.15 = 1.05 -> renormalise by 0.952 to ~[-1,1].
	float n = 0.60 * sin( w_i        * u_time + ph0 )
	        + 0.30 * sin( w_i * 2.3  * u_time + ph1 )
	        + 0.15 * sin( w_i * 4.0  * u_time + ph2 );
	n *= 0.952;
	float tw = clamp( 1.0 + amp_i * n, u_twClampLo, u_twClampHi );  // anti-strobe, never-extinguish; default [0.10,2.0]

	// Chromatic flicker -- LUMINANCE-PRESERVING hue jitter on the BRIGHT + LOW
	// stars only (prism dispersion, the Sirius "flashing red/green/blue" cue).
	// Three decorrelated phases (0/120/240 deg), blue-weighted (blue twinkles most).
	// The tinted colour is renormalised back to the ORIGINAL luminance, so it shifts
	// HUE only and adds/removes no energy (research B: not RGB fairy-light noise).
	// ~0 at zenith, ~0 on the faint majority.
	float am01    = clamp( 1.0 - a_dir.z, 0.0, 1.0 );  // ~0 zenith .. ~1 horizon
	float chromaW = u_twChroma * bt * smoothstep( 0.0, 0.6, am01 );
	float wc      = w_i * 1.2;                          // chroma slightly faster than the boil
	vec3  cN = vec3( sin( wc * u_time + phC + 0.0   ),
	                 sin( wc * u_time + phC + 2.094 ),  // +120 deg
	                 sin( wc * u_time + phC + 4.188 ) );// +240 deg
	vec3  cGain = vec3( 0.6, 0.8, 1.0 );               // R < G < B
	vec3  cCol  = v_color * clamp( vec3( 1.0 ) + chromaW * cGain * cN, 0.0, 2.0 );
	float lum0  = dot( v_color, vec3( 0.2126, 0.7152, 0.0722 ) );
	float lum1  = dot( cCol,    vec3( 0.2126, 0.7152, 0.0722 ) );
	v_color = cCol * ( lum0 / max( lum1, 1e-4 ) );     // luminance-preserving hue shift

	// MW REWORK (2026-06-21): the galactic-band FILL-star flux multiplier (u_mwStarBoost /
	// isBand) was DELETED -- the dense procedural band-fill population no longer exists (the
	// Milky Way body is now the sampled equirect panorama). Every live star is a real BSC5
	// star at its conserved catalogue flux; there is no per-star band boost.

	// === Task D moonlight wash -- per-star photometric dimming (spec §6 step ②) ===
	// The moon raises the LOCAL sky luminance (MoonSkyLum at THIS star's direction);
	// a star survives while its RENDERED flux clears the local sky by a margin, else
	// it sinks into the wash. The threshold is MOON-RELATIVE: when the moon is down
	// (moonLum==0) thLo==0 and starVis==1 for every rendered star (kWashEps keeps
	// thHi>thLo), so the approved dark-sky field -- including the dim MW band carpet --
	// is byte-IDENTICAL (no absolute limiting-magnitude cull; the dark-sky limit is the
	// emergent energy-floor already approved). As the moon brightens, faint stars near
	// the moon sink first (aureole), the brightest survive, and far from the moon only
	// the broad Rayleigh pedestal lifts -> a gentle global thinning (spec V4). Every live
	// star is now a real BSC5 star, so effFlux is just its conserved catalogue flux.
	const float kWashEps = 1.0e-4;
	float effFlux = flux;
	float moonLum = MoonSkyLum( a_dir );
	float thLo    = moonLum * u_starDim.x;                                  // c
	float thHi    = moonLum * u_starDim.x * u_starDim.y + kWashEps;         // c*k (+eps -> thHi>thLo at moonLum==0)
	float starVis = smoothstep( thLo, thHi, effFlux );

	// Deposited energy term (this is what the FS integrates): the moonlight wash
	// (starVis) gates the survivor, then twinkle modulates ONLY survivors (spec §6
	// step ③), then master intensity + night-visibility scale the conserved flux.
	float energy = max( flux * starVis * tw, 0.0 ) * u_intensity * u_nightFactor;

	// === T_moonBody (SKY-REWORK-SPEC v3 Task C) =============================
	// The moon BODY is an opaque geometric disc that occludes ALL layer1 behind it
	// -- the dark side too (PHASE-INDEPENDENT: u_moonAngR/Dir carry no phase). A star
	// inside the disc must not peek through the unlit limb. mAng = this star's angular
	// separation from the moon centre; smoothstep gives T=0 across the WHOLE disc
	// (mAng <= angR) and feathers to 1 over the thin limb band [angR, angR+soft]. When
	// the moon is down the host passes u_moonAngR < 0 -> mAng (>=0) > edge1 -> T==1
	// everywhere. The glow/halo is a SEPARATE additive term in the sunmoon pass and
	// never participates in occlusion (spec §5: moonBodyOccMask vs moonHaloRadiance).
	float mAng      = acos( clamp( dot( a_dir, u_moonDir ), -1.0, 1.0 ) );
	float TmoonBody = smoothstep( u_moonAngR, u_moonAngR + max( u_moonSoft, 1e-5 ), mAng );
	energy *= TmoonBody;

	v_flux    = energy;
	v_sigmaPx = sigmaPx;

	// Expand the quad corner in PIXELS, convert to an NDC offset, emit. z at the far
	// plane (depth test is off for this pass).
	vec2 cornerPx = kCorners[gl_VertexID % 6] * ( kClipSig * sigmaPx );
	v_deltaPx = cornerPx;
	vec2 ndcPos = ndc + cornerPx / ( 0.5 * u_viewportPx.xy );
	gl_Position = vec4( ndcPos, 0.99999, 1.0 );
}
)GLSL";

static const char kStarsFs[] = R"GLSL(#version 330 core
flat in vec3  v_color;
flat in float v_flux;
flat in float v_sigmaPx;
in vec2 v_deltaPx;
out vec4 fragColor;
void main()
{
	// Analytic energy-normalised Gaussian PSF in pixel space. norm = 1/(2*pi*sigma^2)
	// makes the integral of the spot equal to v_flux for ANY sigma; the centre
	// fragment (r=0) deposits v_color * v_flux/(2*pi*sigma^2). Clip at 3 sigma (the
	// quad's extent) -- the lost tail is <1.2% of the energy.
	float r2 = dot( v_deltaPx, v_deltaPx );
	float sigma2 = v_sigmaPx * v_sigmaPx;
	if( r2 > 9.0 * sigma2 ) discard;
	float psf  = exp( -0.5 * r2 / sigma2 );
	float norm = 1.0 / ( 6.28318530718 * sigma2 );
	vec3 premul = v_color * v_flux * psf * norm;
	// Premultiplied additive (glBlendFunc(GL_ONE, GL_ONE)); alpha 0 leaves the scene
	// FBO alpha intact.
	fragColor = vec4( premul, 0.0 );
}
)GLSL";

// =============================================================================
// DIAG PREFLIGHT (csz_stars_diag 1, default OFF): one fixed ~16x16 pure-white
// quad at the viewport centre, drawn with blending OFF (replace) into the scene
// HDR FBO. The single dev probe (STAR-REWRITE-DESIGN spec H.2): if THIS appears in
// a capture the draw -> scene-FBO -> resolve -> screenshot chain is sound, so any
// "no stars" symptom is the per-star energy model, not the pipeline. This is the
// ONLY debug path retained; it is NEVER valid as success evidence for the real
// star field (that is the live PSF path under default cvars).
// =============================================================================
static const char kStarsDiagVs[] = R"GLSL(#version 330 core
uniform vec3 u_viewportPx;   // (w,h,_) -- .z unused
const vec2 kCorners[6] = vec2[6](
	vec2( -1.0, -1.0 ), vec2( 1.0, -1.0 ), vec2( -1.0, 1.0 ),
	vec2( -1.0,  1.0 ), vec2( 1.0, -1.0 ), vec2(  1.0, 1.0 ) );
void main()
{
	vec2 cornerPx = kCorners[gl_VertexID] * 8.0;        // 16x16 px total
	vec2 ndc = cornerPx / ( 0.5 * u_viewportPx.xy );    // centred at NDC origin
	gl_Position = vec4( ndc, 0.0, 1.0 );
}
)GLSL";

static const char kStarsDiagFs[] = R"GLSL(#version 330 core
out vec4 fragColor;
void main() { fragColor = vec4( 1.0, 1.0, 1.0, 1.0 ); }
)GLSL";

// MW REWORK (2026-06-21): the per-pixel procedural Milky Way shaders kStarsMwVs /
// kStarsMwFs (FBM band-field, de-stripe warp, diffuse underglow, nebula, dust) were
// DELETED here. The Milky Way is now a sampled equirect panorama (geom/csz_panorama_
// shaders.inl + csz_panorama.cpp). SALVAGED before deletion: the attrib-less quad-gen
// vertex shader -> kPanoramaVs; the MoonSkyLum/MoonSkyGlow moon sky-glow (so the moon
// halo survives) -> the panorama FS. The star-points pass above keeps its OWN MoonSkyLum
// copy for per-star moonlight dimming (unchanged).
