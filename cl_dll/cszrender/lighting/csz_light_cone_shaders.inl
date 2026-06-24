/*
 * csz_light_cone_shaders.inl -- CSOZ renderer: world-space beam-volume GLSL (L6a / slot 13.4)
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
// Included ONLY by lighting/csz_light_cone.cpp. Both fragment programs PREPEND the
// shared fog Step-1 depth-reconstruct helpers (kFogDepthReconstructGlsl:
// u_zNear/u_zFar/u_invProj/u_invViewProj + linViewZ/worldPosFromDepth) at build
// time -- see EnsureBuilt(): the final FS = "#version 330 core\n" + reconstruct +
// body. GLES3/WebGL2 intersection only (code-standards section 7).
//
// === DESIGN-SPEC §V2 third-person air-cone rebuild (post-red-team, CANONICAL) ===
// The third-person (NON-LOCAL) flashlight air cone is now a TWO-PASS half-res path,
// replacing the old full-res-into-HDR mesh march:
//   Pass 1 (kConeFsBody, half-res accumulation buffer): every visible non-local cone
//     mesh proxy is drawn ADDITIVELY into a separate half-res RGBA16F buffer. Each
//     covered fragment runs an ENERGY-CONSERVING Beer-Lambert single-scatter march
//     (inScatter += T*(sigmaS/sigmaE)*(1-exp(-sigmaE*dt))*phase*spotAtten; T*=exp(-sigmaE*dt)),
//     a TWO-LOBE Henyey-Greenstein phase (forward g + dim halo g*0.5, weights w0+w1<=1),
//     a Frostbite squared angular falloff, a per-light radiance cap, and a STATIC
//     interleaved-gradient jitter (no temporal animation -> no boiling without TAA).
//     The march is bounded at BOTH ends (near plane .. min(cone far extent, opaque
//     scene depth)); against sky it still marches only the finite cone extent, so the
//     energy-conserving integral keeps it faint instead of a full-bright sky blowout.
//   Pass 2 (kConeUpFsBody, full-res composite): the half-res buffer is upsampled with
//     a depth-aware BILATERAL filter (ported from the first-person fog volume:
//     5x5 gaussian spatial x depth-agreement weights + a sharp nearest-depth fallback
//     at discontinuities -> no leak across silhouettes onto walls/sky), an
//     order-independent per-pixel soft-knee compression is applied ON THE VOLUME
//     BUFFER VALUE ONLY (bounds N-cone overlap wash; artistic clamp, NOT physical),
//     and the result is ADDED into the HDR scene.
// The LOCAL first-person beam is never drawn here (slot 13.5 owns it).

// -----------------------------------------------------------------------------
// VS -- procedural unit cone (no VBO; gl_VertexID), apex at u_apex, opening along
// u_axis (= dir * length). Each of u_segments wedges is one apex-rim-rim triangle
// (3*segments verts). The CPU folds the half-angle + length into the basis vectors
// (u_axis/u_right/u_up) so the SAME procedural mesh maps to any spot. Only the
// world position is interpolated; the fragment shader does the volume integral.
// -----------------------------------------------------------------------------
static const char kConeVs[] = R"GLSL(#version 330 core
uniform mat4  u_matViewProj;
uniform vec3  u_apex;      // cone tip (spot origin), world
uniform vec3  u_axis;      // dir * length      (tip -> base centre)
uniform vec3  u_right;     // right * (tanHalf*length)
uniform vec3  u_up;        // up    * (tanHalf*length)
uniform float u_segments;  // radial wedge count (matches the CPU draw call)
out vec3 vWorld;
void main()
{
	int tri    = gl_VertexID / 3;
	int corner = gl_VertexID - tri * 3;
	vec3 world;
	if( corner == 0 )
	{
		world = u_apex;                                   // shared tip
	}
	else
	{
		int idx = tri + ( corner - 1 );                   // corner1 -> i, corner2 -> i+1
		float ang = 6.28318530718 * float( idx ) / u_segments;
		world = u_apex + u_axis + cos( ang ) * u_right + sin( ang ) * u_up;
	}
	vWorld = world;
	gl_Position = u_matViewProj * vec4( world, 1.0 );
}
)GLSL";

// -----------------------------------------------------------------------------
// Pass 1 FS -- half-res energy-conserving single-scatter beam. Output = IN-SCATTER
// ONLY (linear-HDR radiance to ADD), accumulated additively (kBlendAddPremul) across
// every visible non-local cone into the half-res buffer. .a is unused (the upsample
// re-derives each tap's surface depth from the full-res depth texture).
//   * gl_FrontFacing discard => exactly ONE fragment per covered pixel (winding-
//     independent single coverage; no double-add, no rim seam).
//   * march extent = view-ray x cone bounding sphere, clamped near (u_zNear) and FAR
//     to min(sphere far, opaque scene depth) -> finite at BOTH ends. Sky pixels keep
//     the finite sphere-far bound (NO depth==far cull); the energy-conserving slice
//     keeps the sky-facing cone faint instead of blowing out.
//   * per sample: exact point-in-cone (axial s in [0,len] + angle), Frostbite squared
//     angular falloff (tight band), axial tip-bright falloff with a feathered base,
//     soft camera-side occlusion band, TWO-LOBE HG phase, and the Beer-Lambert slice.
//   * per-light radiance cap clamps a single cone's contribution; cross-cone overlap
//     is compressed by the soft-knee in Pass 2 (on the half-res buffer only).
// -----------------------------------------------------------------------------
static const char kConeFsBody[] = R"GLSL(
uniform sampler2D u_depthTex;   // FULL-RES scene depth (raw, compare-mode NONE); sky unit
uniform vec2  u_targetSize;     // HALF-RES accumulation buffer size in pixels (gl_FragCoord basis)
uniform vec3  u_camPos;         // view ray origin (world)
uniform vec3  u_apex;           // cone tip (spot origin)
uniform vec3  u_axisDir;        // normalized cone forward
uniform float u_len;            // beam length (world units; = min(radius, range))
uniform float u_cosInner;       // soft rim start (cos half-angle, inner)
uniform float u_cosOuter;       // cone cutoff   (cos half-angle, outer)
uniform vec3  u_color;          // linear warm-white spot tint (NOT premultiplied)
uniform float u_intensity;      // beam brightness scale (csz_flashlight_tp_intensity)
uniform float u_hgG;            // Henyey-Greenstein forward anisotropy (0.6..0.8)
uniform float u_halo;           // §V2 two-lobe weight w1 (0..1); forward lobe gets w0=1-w1
uniform float u_sigmaS;         // scattering coefficient (csz_flashlight_tp_sigmaS)
uniform float u_sigmaE;         // extinction coefficient (csz_flashlight_tp_sigmaE)
uniform float u_cap;            // per-light radiance cap (csz_flashlight_tp_cap)
uniform int   u_steps;          // bounded march sample count
uniform float u_surfFade;       // non-local surface-fade band scale (>0); shaft tapers into the pool
in vec3  vWorld;                // world-space cone-surface position (matches kConeVs `out vec3 vWorld`)
out vec4 fragColor;

// STATIC interleaved-gradient-noise dither (Jimenez). DESIGN-SPEC §V2/pitfall #7:
// the jitter MUST be static -- no per-frame animation. We have no TAA, so an animated
// dither would make the beam boil/crawl; the 5x5 bilateral upsample (Pass 2) launders
// the residual stationary grain into a smooth gradient instead.
float ignDither( vec2 p )
{
	return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) );
}

// Henyey-Greenstein phase, 1/4pi normalized (kept per pitfall #6: never drop the
// normalization or the HDR energy balance drifts).
float hgPhase( float c, float g )
{
	float g2 = g * g;
	float denom = 1.0 + g2 - 2.0 * g * c;
	return ( 1.0 - g2 ) / ( 4.0 * 3.14159265 * max( pow( denom, 1.5 ), 1e-4 ) );
}

// §V2 two-lobe beam phase: a tight forward lobe (g) + a dim wide halo lobe (g*0.5).
// The lobe weights are NORMALIZED so w0+w1<=1 (w1 = halo, w0 = 1-halo) -> energy is
// not double-counted (the old constant 0.55 isotropic pedestal is DELETED). g stays
// in 0.6..0.8 so the beam is visible side-on (the common third-person angle) yet not
// blinding head-on.
float beamPhase( float c, float g, float halo )
{
	float w1 = clamp( halo, 0.0, 1.0 );
	float w0 = 1.0 - w1;
	return w0 * hgPhase( c, g ) + w1 * hgPhase( c, g * 0.5 );
}

void main()
{
	if( !gl_FrontFacing )
		discard;                                  // single coverage (see header note)

	vec3 O = u_camPos;
	vec3 V = normalize( vWorld - O );

	// Bounding sphere of the finite cone (centre = apex, radius = slant length to the
	// rim = len/cosOuter) -> finite ray bounds without an exact cone solve.
	float sphR = u_len / max( u_cosOuter, 1e-3 );
	vec3  oc = O - u_apex;
	float b = dot( oc, V );
	float c = dot( oc, oc ) - sphR * sphR;
	float disc = b * b - c;
	if( disc <= 0.0 )
		discard;
	float sq = sqrt( disc );
	float tNear = max( -b - sq, u_zNear );
	float tFar  = -b + sq;                        // finite (sphere far) -- bounded vs sky too
	if( tFar <= tNear )
		discard;

	// FAR bound = min( cone far extent, opaque scene depth ) -> finite at BOTH ends.
	// Sky pixels (dscene==1) keep the finite sphere-far bound (NO depth==far cull,
	// per §V2/pitfall: skybox/water/sprite/precision would break that cull).
	vec2  uv = gl_FragCoord.xy / u_targetSize;
	float dscene = texture( u_depthTex, uv ).r;
	float tScene = -1.0;
	if( dscene < 1.0 )
	{
		vec3 sw = worldPosFromDepth( uv, dscene );
		tScene = dot( sw - O, V );
		if( tScene > 0.0 )
			tFar = min( tFar, tScene );
	}
	if( tFar <= tNear )
		discard;                                  // wholly behind a wall

	float segLen = tFar - tNear;
	float dt = segLen / float( u_steps );
	float jitter = ignDither( gl_FragCoord.xy );  // STATIC

	// Non-local surface-fade band (world units): the shaft tapers out over the last
	// stretch before the marched surface so it does NOT pile a bright shell ("floor
	// dome") exactly where the beam lands -- the separate full-res direct pool owns
	// "light on the floor". Tighter than the legacy patch band so the airborne shaft
	// still visibly reaches down toward the pool (they read as one phenomenon).
	float surfBand = max( ( 0.15 * u_len + 48.0 ) * max( u_surfFade, 1e-3 ), 1.0 );

	float stepTrans = exp( -u_sigmaE * dt );       // Beer-Lambert per-step transmittance
	float Tstart = 1.0;                            // transmittance from tNear to the step start
	float slice = ( u_sigmaS / max( u_sigmaE, 1e-6 ) ) * ( 1.0 - stepTrans );  // analytic in-scatter slice

	// Frostbite spot angular attenuation: saturate(cd*scale+offset)^2 (tight band).
	float angScale = 1.0 / max( u_cosInner - u_cosOuter, 1e-4 );
	float angOffset = -u_cosOuter * angScale;

	vec3 acc = vec3( 0.0 );
	for( int i = 0; i < u_steps; i++ )
	{
		float t = tNear + ( float( i ) + jitter ) * dt;
		vec3 P = O + V * t;

		float Tcur = Tstart;                      // transmittance to the start of this step
		Tstart *= stepTrans;                      // ...to the end (next step start)

		vec3  ap = P - u_apex;
		float s = dot( ap, u_axisDir );           // axial distance from the tip
		if( s <= 0.0 || s > u_len )
			continue;
		float r = length( ap );
		float cosAx = s / max( r, 1e-4 );         // cos( angle off axis )

		// Frostbite squared angular falloff (replaces the old smoothstep + 0.55 pedestal).
		float cone = clamp( cosAx * angScale + angOffset, 0.0, 1.0 );
		cone *= cone;
		if( cone <= 0.0 )
			continue;

		float atten = clamp( 1.0 - s / u_len, 0.0, 1.0 );  // tip bright, base feathered to 0
		atten *= atten;

		// Soft camera-side occlusion + depth-discontinuity taper: samples approaching
		// the scene surface fade out (no hard punch-through, shaft melts into the pool).
		float occ;
		if( tScene <= 0.0 )
			occ = 1.0;                            // sky behind this sample -> finite cone, no clamp
		else
			occ = smoothstep( 0.0, surfBand, tScene - t );

		// Two-lobe forward-scatter phase (brighter looking into the beam; halo broadens
		// the glow), 1/4pi normalized, no isotropic pedestal.
		float cosTheta = dot( V, normalize( u_apex - P ) );
		float phase = beamPhase( cosTheta, u_hgG, u_halo );

		float vis = cone * atten * occ;
		// Energy-conserving Beer-Lambert slice: bounded by construction, so neither a
		// long cone nor many overlapping cones can grow unbounded.
		acc += Tcur * phase * u_color * vis * slice;
	}

	acc *= u_intensity;

	// Per-light radiance cap (artistic clamp, NOT physical): bound a single cone's peak
	// so one near beam can never blow out on its own. Hue-preserving channel-max clamp.
	float m = max( max( acc.r, acc.g ), acc.b );
	if( m > u_cap )
		acc *= u_cap / max( m, 1e-4 );

	fragColor = vec4( acc, 0.0 );
}
)GLSL";

// -----------------------------------------------------------------------------
// Pass 2 VS -- fullscreen triangle (VAO-less, gl_VertexID); same idiom as the fog
// volume upsample / HDR resolve. uv is reconstructed from gl_FragCoord in the FS.
// -----------------------------------------------------------------------------
static const char kConeUpVs[] = R"GLSL(#version 330 core
void main()
{
	vec2 ndc = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0,
	                 ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( ndc, 1.0, 1.0 );
}
)GLSL";

// -----------------------------------------------------------------------------
// Pass 2 FS -- depth-aware (bilateral) upsample of the half-res in-scatter, ADDED
// into the full-res HDR buffer (kBlendAddPremul). PORTED from the first-person fog
// volume upsample (csz_fog_volume_shaders.inl): 5x5 gaussian spatial weights x a
// depth-agreement weight, with a SHARP nearest-depth fallback at large depth
// discontinuities so the soft volume never leaks across geometry silhouettes onto
// walls / sky (pitfall #6: soft depth-weight-only rejection still bleeds -> add the
// nearest pick). Each tap's surface depth is re-derived from the FULL-RES depth
// texture at the tap's uv (the same depth the half-res march bounded against), so no
// linViewZ needs to be stored in the half-res buffer.
//
// Before compositing, a per-pixel soft-knee compresses the upsampled VOLUME radiance
// ONLY (never the HDR scene): order-independent, it bounds the N-cone overlap peak so
// crowded beams roll off smoothly instead of clipping to a flat white plateau. This
// compression is an ARTISTIC clamp, not physical energy conservation -- the physical
// bound is the Beer-Lambert slice + per-light cap in Pass 1; this only tames the sum.
// -----------------------------------------------------------------------------
static const char kConeUpFsBody[] = R"GLSL(
uniform sampler2D u_inscatter;   // half-res march output (rgb = in-scatter); sky unit
uniform sampler2D u_depthTex;    // full-res scene depth (raw); sky unit
uniform vec2 u_fullSize;         // full-res target size in pixels
uniform vec2 u_halfSize;         // half-res source size in pixels
uniform float u_smoothSigma;     // gaussian spatial sigma in half-res texels
uniform float u_knee;            // soft-knee compression knee (volume buffer only)
out vec4 fragColor;

// Soft-knee: compress the channel max above `knee` toward 1.0 (hue-preserving). Any
// value with maxRGB <= knee is returned unchanged. Mirrors highlightShoulder() but is
// applied here to the VOLUME buffer only, pre-composite -- the HDR scene is untouched.
vec3 softKnee( vec3 c, float knee )
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
	vec2 uv = gl_FragCoord.xy / u_fullSize;
	float dC = linViewZ( texture( u_depthTex, uv ).r );

	// Continuous sample position in half-res texel space (texel centers at integers).
	vec2 t = uv * u_halfSize - 0.5;
	vec2 ctr = floor( t + 0.5 );
	float inv2s2 = 1.0 / ( 2.0 * u_smoothSigma * u_smoothSigma );

	vec3 sum = vec3( 0.0 );
	float wsum = 0.0;
	float bestDz = 1e30;
	vec3  bestRgb = vec3( 0.0 );

	for( int j = -2; j <= 2; j++ )
	{
		for( int i = -2; i <= 2; i++ )
		{
			vec2 idx = ctr + vec2( float( i ), float( j ) );
			vec2 tap = ( idx + 0.5 ) / u_halfSize;
			vec3 rgb = texture( u_inscatter, tap ).rgb;
			float dTap = linViewZ( texture( u_depthTex, tap ).r );

			vec2 off = idx - t;                                // offset from the continuous sample pos
			float sw = exp( -dot( off, off ) * inv2s2 );       // gaussian spatial weight
			float dz = abs( dC - dTap );
			// Depth-agreement weight, RELATIVE to distance (constant world-unit sigma
			// would over-blur near and over-sharpen far). 5% + 1 unit.
			float dw = exp( -dz / ( 0.05 * dC + 1.0 ) );
			float w = sw * dw + 1e-5;
			sum += rgb * w;
			wsum += w;

			if( dz < bestDz )                                  // track the nearest-depth tap
			{
				bestDz = dz;
				bestRgb = rgb;
			}
		}
	}

	vec3 result = sum / max( wsum, 1e-4 );

	// Sharp nearest-depth fallback: when even the closest tap disagrees in depth by
	// more than ~ (10% + 4u) of the center distance, ALL taps straddle a silhouette ->
	// lerp toward the single nearest-depth tap instead of the leaked bilinear blend.
	float thresh = 0.1 * dC + 4.0;
	float discFall = clamp( ( bestDz - thresh ) / thresh, 0.0, 1.0 );
	result = mix( result, bestRgb, discFall );

	// Soft-knee compress the VOLUME radiance only (artistic overlap clamp), then ADD.
	result = softKnee( result, u_knee );

	fragColor = vec4( result, 0.0 );
}
)GLSL";
