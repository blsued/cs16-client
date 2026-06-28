/*
 * csz_cloudvol.cpp -- CSOZ renderer: volumetric cloud REBUILD v2 (Phase 0 LOOK slice)
 *
 * Copyright (c) 2026 CSOZ project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of CSOZ (cs16-client fork). Clean-room implementation written
 * from PUBLISHED physical/algorithm descriptions ONLY (Beer-Lambert, Henyey-
 * Greenstein, Perlin/Worley noise, the Nubis density-remap model). No code is copied
 * or translated from any license-tainted source (Shadertoy/iQ, Unreal/Unity/Frostbite/
 * Hillaire samples, GPU-Gems/GPU-Pro snippets, PrimeXT, Paranoia, Trinity, retail/
 * leaked). See the header of csz_cloudvol_shaders.inl.
 *
 * This program is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * In addition, as a special exception, the author gives permission to link the code
 * of this program with the Half-Life Game Engine ("HL Engine") and Modified Game
 * Libraries ("MODs") developed by Valve, L.L.C ("Valve"). You must obey the GNU
 * General Public License in all respects for all of the code used other than the HL
 * Engine and MODs from Valve. If you modify this file, you may extend this exception
 * to your version of the file, but you are not obligated to do so. If you do not wish
 * to do so, delete this exception statement from your version.
 */
// Render order: csz_renderer.cpp inserts g_cloudvol.Contribute(view) at the kTmVolume
// seam -- AFTER opaque world geometry + the fog volume / god-rays / dust passes, with
// the scene depth populated and the HDR FBO bound -- so terrain OCCLUDES the clouds and
// the cloud SIDES are visible. csz_clouds 0 early-outs on the first line (production
// byte-identical). This is the depth-composited counterpart to the REJECTED, default-off
// geom/csz_volcloud (which still composites at the sky seam when csz_volcloud is on).
#include "csz_cloudvol.h"
#include "../csz_sky.h"          // g_sky.ComputePhase()
#include "../csz_sky_math.h"     // skymath::SunDir
#include "../csz_sky_compose.h"  // kSkyTmuBase, SkyCompose{BindTex,RestoreTmus,DepthTex,HdrFbo,Active,PerfDumpEnabled}
#include "../../core/csz_engine.h"
#include "../../core/csz_glfuncs.h"
#include "../../core/csz_glstate.h"
#include "../../core/csz_glcaps.h"
#include "../../core/csz_log.h"
#include "../../core/csz_math.h"
#include "../../core/csz_shader.h"
#include "../../core/csz_view.h"

#include <math.h>
#include <string.h>
#include <vector>

namespace csz
{

#include "csz_cloudvol_shaders.inl"

namespace
{

const float kDegToRad = 3.14159265358979323846f / 180.0f;

float ReadCvar( cvar_t *cv, float fallback ) { return ( cv != NULL ) ? cv->value : fallback; }
float clampf( float v, float lo, float hi ) { return ( v < lo ) ? lo : ( v > hi ) ? hi : v; }
int   clampi( int v, int lo, int hi )       { return ( v < lo ) ? lo : ( v > hi ) ? hi : v; }
float mixf( float a, float b, float t )     { return a + ( b - a ) * t; }
int   ifloor( float v )                     { return (int)floorf( v ); }

// ---- cvars (FCVAR_CLIENTDLL, registered eagerly, read live each frame) ----
bool    s_cvarsReady = false;
cvar_t *s_cvMaster;   // csz_clouds        "0"  master on/off (0 = production byte-identical)
cvar_t *s_cvTod;      // csz_clouds_tod    "0"  0 live / 1 day / 2 sunset / 3 full-moon night
cvar_t *s_cvRes;      // csz_clouds_res    "4"  resolution divisor (quarter-res)
cvar_t *s_cvPerf;     // csz_clouds_perf   "0"  0 off / 1 per-frame GPU-ms timer log
cvar_t *s_cvGlDebug;  // csz_gl_debug      "0"  A1: 0 off / 1 per-stage glGetError checkpoints in the cloud frame path (dev; pinpoints the 0x502 at runtime)
// TEST-ONLY judgeability knob (NOT a production/look cvar): relocate the hero box to an
// ABSOLUTE WORLD position so a clamped/headless capture camera (which cannot freecam/setpos
// in the rig) can place the cloud wherever the fixed sky-vantage camera is already looking,
// NO camera movement required. 0 = real world-fixed placement (kOffX east of spawn, unchanged).
// 1 = ABSOLUTE WORLD: box centered at (box_x,box_y,box_z) with half-extent box_radius, IGNORING
// the spawn-relative offset logic (the prior spawn-forward placement aimed the box into a WALL).
// Does NOT touch density/coverage/lighting -- placement + footprint only. See the dbgNear
// branch in Contribute(). Resolved AABB min/max is logged to engine.log on enable.
cvar_t *s_cvDbgNear;    // csz_clouds_dbg_nearbox  "0"  0 off / 1 absolute-world (TEST ONLY)
cvar_t *s_cvDbgBoxX;    // csz_clouds_dbg_box_x       world center X (default: open sky over de_dust2)
cvar_t *s_cvDbgBoxY;    // csz_clouds_dbg_box_y       world center Y
cvar_t *s_cvDbgBoxZ;    // csz_clouds_dbg_box_z       world center Z (high in open sky)
cvar_t *s_cvDbgBoxRad;  // csz_clouds_dbg_box_radius  X/Y half-extent (storm footprint width)
cvar_t *s_cvDbgBoxZRad; // csz_clouds_dbg_box_zrad    Z (vertical) half-extent; <0 => cube (use radius). Tall storm build-up.
// iter4 grazing-angle rework knobs (R1 domain-warp / R2 elevation fade / R5 debug viz).
cvar_t *s_cvDomainWarp;    // csz_clouds_domainwarp      "1"   R1 SECONDARY: low-freq noise domain-warp on/off (per-step dither is the PRIMARY fan killer)
cvar_t *s_cvDomainWarpAmp; // csz_clouds_domainwarp_amp  "220" R1 domain-warp world-space amplitude (live tune)
cvar_t *s_cvHorizonFadeLo; // csz_clouds_horizon_fade_lo "0.01" R2 screen-elevation fade LOW threshold (sin elev)
cvar_t *s_cvHorizonFadeHi; // csz_clouds_horizon_fade_hi "0.07" R2 screen-elevation fade HIGH threshold (sin elev)
cvar_t *s_cvDbgMode;       // csz_clouds_dbg_mode        "0"   R5 debug viz: 0 off / 1 density / 2 transmittance / 3 stepcount / 4 first-hit / 5 scatter / 6 raw-nearest-upsample
// REBUILD v2: single nightness luminance authority + half-res sharpness pipeline (bilateral upscale + CAS).
cvar_t *s_cvNightLum;      // csz_clouds_night_lum    "0.06" [0.01..0.5] midnight cloud luma fraction of day (the ONE night dimming dial)
cvar_t *s_cvFineDiv;       // csz_clouds_fine_div     "2"    [2..8]      ESS in-cloud fine-step divisor (dtFine = dtCoarse/fine_div); PERF default 4->2 (halves in-cloud iters, preserves dtCoarse/sharpness)
cvar_t *s_cvCas;           // csz_clouds_cas          "0.5"  [0..1]      CAS sharpen amount in the upsample pass
cvar_t *s_cvDepthSigma;    // csz_clouds_depth_sigma  "0.01"             bilateral DEPTH edge-stop falloff
cvar_t *s_cvAlphaSigma;    // csz_clouds_alpha_sigma  "8.0"              bilateral cloud-ALPHA edge-stop falloff

// ---- LOOK hot cvars (csz_clouds_*): the form/lighting controls, READ LIVE each frame so
// the look can be swept WITHOUT a rebuild. Each is clamped to a sane [min,max] on read;
// the resolved set is logged to engine.log ONLY when it changes (no per-frame spam). These
// drive the box-silhouette kill (low coverage + face falloff + erosion) and the lighting form.
cvar_t *s_cvCoverage;    // csz_clouds_coverage    0.42  [0..1]      coverage gate (LOW => organic blobs, not a full box)
cvar_t *s_cvDensity;     // csz_clouds_density     1.10  [0.05..6]   density multiplier feeding extinction
cvar_t *s_cvSigma;       // csz_clouds_sigma       0.006 [5e-4..0.05] extinction coeff (1/world-units)
cvar_t *s_cvBaseScale;   // csz_clouds_basescale   4500  [500..20000] base-noise tiling PERIOD in world-u (freq=1/scale); tune for several lumps
cvar_t *s_cvDetail;      // csz_clouds_detail      0.70  [0..1]      high-freq Worley edge-erosion strength (cauliflower edges)
cvar_t *s_cvDetailScale; // csz_clouds_detailscale 700   [50..4000]  detail-noise tiling period in world-u
cvar_t *s_cvHBase;       // csz_clouds_hbase       0.12  [0.01..0.6] height-gradient: base taper fraction (feathered flat-ish base)
cvar_t *s_cvHTop;        // csz_clouds_htop        0.50  [0.15..0.99] height-gradient: where the rounded top begins
cvar_t *s_cvFalloff;     // csz_clouds_falloff     0.35  [0..0.9]    X/Y face-falloff window (density->0 BEFORE the box faces; kills box silhouette)
cvar_t *s_cvSilver;      // csz_clouds_silver      2.50  [0..8]      silver-lining rim strength (BACKLIT forward-scatter edges)
cvar_t *s_cvSilverWidth; // csz_clouds_silver_width 0.50 [0.05..8]  rim band width (LOW=broad glow inward, HIGH=razor edge only)
cvar_t *s_cvPowder;      // csz_clouds_powder      1.00  [0..3]      powder dark-edge strength (self-shadowed near faces)
cvar_t *s_cvAmbient;     // csz_clouds_ambient     1.00  [0..4]      ambient skylight multiplier (undersides not black)
cvar_t *s_cvSun;         // csz_clouds_sun         3.20  [0..12]     day/sun lit intensity
cvar_t *s_cvSunElev;     // csz_clouds_sun_elev    -1    [-1..90]    CLOUD-ONLY light elevation override (deg, 0=horizon..90=zenith); -1=follow tod/skymath sun
cvar_t *s_cvSunAzim;     // csz_clouds_sun_azim    -1    [-1..360]   CLOUD-ONLY light azimuth override (deg); used only when sun_elev>=0
cvar_t *s_cvMoon;        // csz_clouds_moon        2.40  [0..12]     moon lit intensity (dim BUT visible -- not a black void)
cvar_t *s_cvMoonTint;    // csz_clouds_moontint    1.00  [0..2]      moon cool-tint amount (0=white, 1=cool, 2=very cool)
// STRUCTURE hot cvars (iter-3): turn the smooth slab into stacked cauliflower turrets with
// shadowed valleys + an eroded, irregular (non-flat) underside. All live/hot (no rebuild).
cvar_t *s_cvBillow;      // csz_clouds_billow      0.55  [0..1]      cauliflower lobe separation (deep valleys between turrets)
cvar_t *s_cvErodeDepth;  // csz_clouds_erode_depth 1.00  [0..3]      multi-octave erosion valley DEPTH (deeper notches between lobes)
cvar_t *s_cvErodeOct;    // csz_clouds_erode_oct   4     [1..4]      erosion octave count (coarse turret-scale valleys + fine fray)
cvar_t *s_cvSelfShadow;  // csz_clouds_selfshadow  1.60  [0..5]      cone-march self-shadow strength (internal shadow pockets)
cvar_t *s_cvBaseIrreg;   // csz_clouds_base_irreg  0.60  [0..1]      underside irregularity (bumpy/mammatus base, not a flat plane)
cvar_t *s_cvTowerVar;    // csz_clouds_tower_var   0.28  [0..0.6]    DEPRECATED (v3 re-arch: shape now from CloudShapeEnvelope lobes)
cvar_t *s_cvEnvWarp;     // csz_clouds_envwarp     0.12  [0..0.6]    organic silhouette perturbation (low-freq noise warp of the lobe shell)
// STORM-CELL hot cvars (v4 codex-compare iter): mid-frequency cauliflower + direct-sun tonal range + virga.
cvar_t *s_cvMid;         // csz_clouds_mid         0.45  [0..1]      MID-freq cauliflower strength (rounded packed bumps on the OUTER half)
cvar_t *s_cvMidScale;    // csz_clouds_midscale    2200  [800..6000] mid-freq tiling PERIOD in world-u (freq=1/scale)
cvar_t *s_cvVirga;       // csz_clouds_virga       0.0   [0..1]      faint rain/virga shaft under the darkest core (0=off)
cvar_t *s_cvSunFwd;      // csz_clouds_sunfwd      1.6   [0..5]      DIRECT-sun forward-scatter strength (near-white sun-lit caps)
cvar_t *s_cvSunG;        // csz_clouds_sung        0.82  [0..0.95]   direct-sun forward HG anisotropy g
// CAP-LIGHT / BASE hot cvars (codex compare2 iter): broad sun-facing cap faces + storm-base shelf/mammatus.
cvar_t *s_cvCapLight;    // csz_clouds_caplight    1.0   [0..4]      BROAD sun-facing cap light (density-gradient normal); 0=off (skips gradient taps)
cvar_t *s_cvShelf;       // csz_clouds_shelf       1.0   [0.1..4]    storm-base flatten/shelf band width scale (DEPRECATED v5: layer pivot)
cvar_t *s_cvMammatus;    // csz_clouds_mammatus    0.35  [0..1]      small downward mammatus lobes under the base (DEPRECATED v5: layer pivot)
// LAYER + WEATHER + ANIMATION hot cvars (v5 PIVOT: world-space coverage-driven cloud DECK).
// csz_clouds_weather is the headline knob: switching it pushes a 3-state PRESET into the
// individual look cvars (so each stays HOT for tuning). The layer/wind/evolve cvars place and
// animate the deck. The old single-hero-box STORM cvars (shelf/mammatus/virga/envwarp/falloff)
// are now inert (the layer has no sculpted envelope); kept registered so old configs don't error.
cvar_t *s_cvWeather;     // csz_clouds_weather       0     0=normal scattered cumulus / 1=rain overcast / 2=snow overcast
cvar_t *s_cvLayerHeight; // csz_clouds_layer_height  2600  [200..12000] cloud-BASE altitude above the spawn anchor (world-u)
cvar_t *s_cvLayerThick;  // csz_clouds_layer_thick   850   [120..6000]  cloud-DECK thickness (world-u)
cvar_t *s_cvWindDir;     // csz_clouds_wind_dir      45    [0..360]     wind azimuth (deg) for the horizontal drift
cvar_t *s_cvWindSpeed;   // csz_clouds_wind_speed    60    [0..600]     horizontal drift speed (world-u/sec)
cvar_t *s_cvEvolve;      // csz_clouds_evolve        35    [0..400]     volume-EVOLVE (morph) rate
// PATH A (macro cloud distribution) hot cvars: the low-freq world-XY coverage FIELD that gives
// "clouds in some regions, clear sky in others" + the incommensurate base de-tile.
cvar_t *s_cvCovScale;    // csz_clouds_cov_scale     42000 [8000..120000] coverage-FIELD world period (freq=1/scale); >> footprint => no visible repeat
cvar_t *s_cvCovContrast; // csz_clouds_cov_contrast  0.85  [0..2]         field spread around the preset coverage level (per-weather preset; HIGH=scattered gaps, LOW=mild overcast variation)
cvar_t *s_cvCovDrift;    // csz_clouds_cov_drift      0.0   [0..2]         weather-system world drift fraction of wind (0 = world-static field; existing cloud drift/evolve unchanged)
cvar_t *s_cvDetile;      // csz_clouds_detile         0.5   [0..0.5]       blend weight of the incommensurate (0.73x) second base tap (de-repeats within-region shape; 0 = old single tap)

void RegisterCvarsImpl()
{
	if( s_cvarsReady )
		return;
	s_cvMaster  = gEngfuncs.pfnRegisterVariable( "csz_clouds",     "0", FCVAR_CLIENTDLL );
	s_cvTod     = gEngfuncs.pfnRegisterVariable( "csz_clouds_tod", "0", FCVAR_CLIENTDLL );
	// REBUILD v2: quarter-res(4) -> HALF-res(2) primary march = the headline de-blur (still a quality
	// cvar: 1=full/screenshot, 2=default, 3-4=low-end). The plain bilinear upsample is replaced by a
	// joint depth+alpha bilateral + CAS sharpen (see kCloudUpsampleFs).
	s_cvRes     = gEngfuncs.pfnRegisterVariable( "csz_clouds_res", "2", FCVAR_CLIENTDLL );
	s_cvPerf    = gEngfuncs.pfnRegisterVariable( "csz_clouds_perf","0", FCVAR_CLIENTDLL );
	s_cvGlDebug = gEngfuncs.pfnRegisterVariable( "csz_gl_debug",   "0", FCVAR_CLIENTDLL );  // A1: per-stage glGetError checkpoints in the cloud frame path (dev, default off)
	s_cvDbgNear   = gEngfuncs.pfnRegisterVariable( "csz_clouds_dbg_nearbox",    "0",    FCVAR_CLIENTDLL );
	// Absolute-world hero-box placement (active only when csz_clouds_dbg_nearbox 1). Defaults
	// frame a big cumulus HIGH in open sky over the de_dust2 playable area, sited to land in the
	// fixed sky-vantage capture camera's view (see recommended capture args in the handoff).
	s_cvDbgBoxX   = gEngfuncs.pfnRegisterVariable( "csz_clouds_dbg_box_x",      "150",  FCVAR_CLIENTDLL );
	s_cvDbgBoxY   = gEngfuncs.pfnRegisterVariable( "csz_clouds_dbg_box_y",      "2850", FCVAR_CLIENTDLL );
	// STORM-SCALE default debug box (codex compare2 #7): a WIDE WEATHER SYSTEM over the map, not a
	// prop or a plume. The plume read came partly from a box that was TALLER than wide -- so widen
	// the X/Y footprint to 6000 (12000u across, ~2x the old) while keeping a tall vertical extent
	// (zrad 5000), so width > height and the silhouette reads as a storm cell across the background.
	// box_z lowered so the flat base sits closer to the horizon (still above terrain at the vantage).
	s_cvDbgBoxZ    = gEngfuncs.pfnRegisterVariable( "csz_clouds_dbg_box_z",      "3000", FCVAR_CLIENTDLL );
	s_cvDbgBoxRad  = gEngfuncs.pfnRegisterVariable( "csz_clouds_dbg_box_radius", "6000", FCVAR_CLIENTDLL );
	s_cvDbgBoxZRad = gEngfuncs.pfnRegisterVariable( "csz_clouds_dbg_box_zrad",   "5000", FCVAR_CLIENTDLL );
	// iter4 grazing-angle rework: R1 domain-warp toggle+amp, R2 elevation-fade thresholds, R5 debug viz.
	s_cvDomainWarp    = gEngfuncs.pfnRegisterVariable( "csz_clouds_domainwarp",      "1",    FCVAR_CLIENTDLL );
	s_cvDomainWarpAmp = gEngfuncs.pfnRegisterVariable( "csz_clouds_domainwarp_amp",  "220",  FCVAR_CLIENTDLL );
	s_cvHorizonFadeLo = gEngfuncs.pfnRegisterVariable( "csz_clouds_horizon_fade_lo", "0.01", FCVAR_CLIENTDLL );
	s_cvHorizonFadeHi = gEngfuncs.pfnRegisterVariable( "csz_clouds_horizon_fade_hi", "0.03", FCVAR_CLIENTDLL );  // REBUILD: half-res shrank the grazing fan => recover far deck
	s_cvDbgMode       = gEngfuncs.pfnRegisterVariable( "csz_clouds_dbg_mode",        "0",    FCVAR_CLIENTDLL );
	// REBUILD v2 NEW cvars: the single nightness luminance authority + the half-res sharpness pipeline.
	s_cvNightLum    = gEngfuncs.pfnRegisterVariable( "csz_clouds_night_lum",    "0.06", FCVAR_CLIENTDLL );  // midnight cloud luma as a fraction of day (set EQUAL to the world day-for-night floor). Single night dimming authority.
	s_cvFineDiv     = gEngfuncs.pfnRegisterVariable( "csz_clouds_fine_div",     "2",    FCVAR_CLIENTDLL );  // PERF: in-cloud fine-step divisor (dtFine = dtCoarse / fine_div). 4->2 HALVES in-cloud iters (the looking-up 9-10ms peak driver) while leaving dtCoarse -- hence first-hit/silhouette precision + thin-wisp catching (SHARPNESS) -- untouched. dtFine 4.4->8.9u stays ~34x oversampled vs the 300u detail field; energy-conserving Beer-Lambert keeps density/transmittance. Live cvar: A/B 2/3/4 without a rebuild.
	s_cvCas         = gEngfuncs.pfnRegisterVariable( "csz_clouds_cas",          "0.2",  FCVAR_CLIENTDLL );  // M4: 0.5->0.2 -- CAS crisps the high-freq strands we are REMOVING, so back it off (upsample pass; 0 = bilateral only)
	s_cvDepthSigma  = gEngfuncs.pfnRegisterVariable( "csz_clouds_depth_sigma",  "0.01", FCVAR_CLIENTDLL );  // joint-bilateral DEPTH edge-stop falloff (1/world-u of linear depth) -> crisp vs terrain
	s_cvAlphaSigma  = gEngfuncs.pfnRegisterVariable( "csz_clouds_alpha_sigma",  "8.0",  FCVAR_CLIENTDLL );  // joint-bilateral cloud-ALPHA edge-stop falloff -> crisp cloud-vs-sky silhouette
	// LOOK hot cvars (swept live, no rebuild). Defaults = the iter-2 SUBSTANCE-LOCK target:
	// coverage 0.6 + density 2.5 + basescale 2200 gave a substantial, rounded, billowy cumulus
	// (confirmed from the oblique vantage); X/Y face falloff + erosion keep the AABB silhouette GONE.
	// v3 RE-ARCHITECTURE defaults (codex re-arch starting set): organic CloudShapeEnvelope lobes +
	// Nubis coverage-dilation body + edge-only erosion + Hillaire multi-scatter / sky-vis lighting.
	// LARGE baseScale (8000) is the popcorn killer (small kernels were the old 2200); low coverage +
	// thin sigma + soft silver = a soft billowy cumulus, not a cube of popcorn.
	// v4 STORM defaults (codex-compare iter): higher coverage/density/sigma + strong direct-sun tonal
	// range + low ambient + sharper thin-shell erosion + mid-freq cauliflower = a tall, dark-based,
	// high-contrast STORM cell, not a soft isolated puff. All still HOT (swept without a rebuild).
	// v5 PIVOT defaults = the NORMAL scattered-cumulus preset (csz_clouds_weather 0): low coverage
	// (isolated puffs over blue sky), moderate density, smaller base cells (a FIELD of clouds, not
	// one mass), brighter warm ambient. csz_clouds_weather writeback re-applies these on switch.
	// REBUILD v2 (weather-0 scattered cumulus, the ONLY path now): defaults ARE the live look (the
	// weather-preset push system was retired with rain/snow). Values fold the former NORMAL preset
	// with the rebuild changes: basescale 3000->2000 (distinct puffs, de-tile hides repeat),
	// detailscale 1400->300 (fine crisp wisps, not coarse 43u lumps), hbase 0.15->0.12 (feathered
	// flat base), htop 0.55->0.45 (rounded dome), erode_depth->0.55 (edge bite), sunfwd 1.7->0.6
	// (bound day forward beam), moon 1.9->1.0 (RELATIVE dial; csz_clouds_night_lum owns dimming).
	s_cvCoverage    = gEngfuncs.pfnRegisterVariable( "csz_clouds_coverage",    "0.55",  FCVAR_CLIENTDLL );  // M4: 0.40->0.55 -- scattered-but-substantial cumulus
	s_cvDensity     = gEngfuncs.pfnRegisterVariable( "csz_clouds_density",     "1.6",   FCVAR_CLIENTDLL );  // M4: 1.15->1.6 -- raise optical opacity (on top of M1, not a substitute)
	s_cvSigma       = gEngfuncs.pfnRegisterVariable( "csz_clouds_sigma",       "0.006", FCVAR_CLIENTDLL );  // M4: 0.0045->0.006 -- extinction coeff
	s_cvBaseScale   = gEngfuncs.pfnRegisterVariable( "csz_clouds_basescale",   "2000",  FCVAR_CLIENTDLL );  // REBUILD: distinct cumulus across deck; de-tile hides the 2000u repeat
	s_cvDetail      = gEngfuncs.pfnRegisterVariable( "csz_clouds_detail",      "0.35",  FCVAR_CLIENTDLL );  // M4: 0.85->0.35 -- subtle edge erosion (R4 #4)
	s_cvDetailScale = gEngfuncs.pfnRegisterVariable( "csz_clouds_detailscale", "1000",  FCVAR_CLIENTDLL );  // M4: 300->1000 -- smoke-strand (~20-75u) -> cauliflower-lobe scale
	s_cvHBase       = gEngfuncs.pfnRegisterVariable( "csz_clouds_hbase",       "0.16",  FCVAR_CLIENTDLL );  // M3: 0.12->0.16 -- smoothstep flat base end (R1 §1b)
	s_cvHTop        = gEngfuncs.pfnRegisterVariable( "csz_clouds_htop",        "0.88",  FCVAR_CLIENTDLL );  // M3: 0.45->0.88 -- rounded dome fade-out start (R1 §1b)
	s_cvFalloff     = gEngfuncs.pfnRegisterVariable( "csz_clouds_falloff",     "0.12",  FCVAR_CLIENTDLL );  // inert now (slab faces beyond marchFar); kept for config-compat
	s_cvSilver      = gEngfuncs.pfnRegisterVariable( "csz_clouds_silver",      "0.80",  FCVAR_CLIENTDLL );
	s_cvSilverWidth = gEngfuncs.pfnRegisterVariable( "csz_clouds_silver_width","0.9",   FCVAR_CLIENTDLL );
	s_cvPowder      = gEngfuncs.pfnRegisterVariable( "csz_clouds_powder",      "0.40",  FCVAR_CLIENTDLL );
	s_cvAmbient     = gEngfuncs.pfnRegisterVariable( "csz_clouds_ambient",     "1.00",  FCVAR_CLIENTDLL );
	s_cvSun         = gEngfuncs.pfnRegisterVariable( "csz_clouds_sun",         "3.4",   FCVAR_CLIENTDLL );
	// CLOUD-ONLY sun-direction override (does NOT touch the engine sun or any other system):
	// -1 = follow the tod/skymath sun (production behavior UNCHANGED). When sun_elev>=0 the cloud's
	// light direction is rebuilt from (sun_elev,sun_azim) so the lit cauliflower tops can face up/camera.
	// codex compare2 #7: DEFAULT the cloud-only sun so the broad sunlit caps FACE the camera. The
	// elev55 capture hid the lit faces (sun behind/over the top); elev~35 + azim~180 lights the
	// camera-facing side so the warm-white cap faces show. Still HOT/overridable (-1 = follow tod).
	// P1: defaults UNFROZEN to -1 (follow the per-tod direction below) so day/sunset/night move the
	// lit side. The old frozen 35/180 made tod NEVER change the light direction. -1 => use the
	// per-tod elev/azim defaults (still camera-facing lit caps); a user value >=0 still hard-overrides.
	s_cvSunElev     = gEngfuncs.pfnRegisterVariable( "csz_clouds_sun_elev",    "-1",    FCVAR_CLIENTDLL );
	s_cvSunAzim     = gEngfuncs.pfnRegisterVariable( "csz_clouds_sun_azim",    "-1",    FCVAR_CLIENTDLL );
	s_cvMoon        = gEngfuncs.pfnRegisterVariable( "csz_clouds_moon",        "1.0",   FCVAR_CLIENTDLL );  // REBUILD: RELATIVE dial only (moon chroma at ~day magnitude); ALL night dimming is owned by csz_clouds_night_lum
	s_cvMoonTint    = gEngfuncs.pfnRegisterVariable( "csz_clouds_moontint",    "1.0",   FCVAR_CLIENTDLL );
	// STRUCTURE hot cvars (iter-3): stacked cauliflower turrets + shadowed valleys + irregular base.
	// Defaults already show clearly separated lobes with internal shadow pockets out of the box.
	s_cvBillow      = gEngfuncs.pfnRegisterVariable( "csz_clouds_billow",      "0.10",  FCVAR_CLIENTDLL );  // DEPRECATED (v3): turret hard-carve removed
	s_cvErodeDepth  = gEngfuncs.pfnRegisterVariable( "csz_clouds_erode_depth", "0.25",  FCVAR_CLIENTDLL );  // M4: 0.55->0.25 -- shallower canonical remap-subtract erosion bite
	s_cvErodeOct    = gEngfuncs.pfnRegisterVariable( "csz_clouds_erode_oct",   "3",     FCVAR_CLIENTDLL );
	// codex compare2: LOWER selfshadow (1.6 -> 0.85) so cores read DEEP GREY, not the black-smoke
	// charcoal the prior 1.6 produced; the cap-light + tonal range keep the lit/shadow contrast.
	s_cvSelfShadow  = gEngfuncs.pfnRegisterVariable( "csz_clouds_selfshadow",  "1.4",   FCVAR_CLIENTDLL );  // P1: 0.85->1.4 deeper cone shadow = real lit-top/shadowed-underside contrast (paired w/ lower day ambient)
	s_cvBaseIrreg   = gEngfuncs.pfnRegisterVariable( "csz_clouds_base_irreg",  "0.6",   FCVAR_CLIENTDLL );  // DEPRECATED (v3)
	s_cvTowerVar    = gEngfuncs.pfnRegisterVariable( "csz_clouds_tower_var",   "0.28",  FCVAR_CLIENTDLL );  // DEPRECATED (v3)
	s_cvEnvWarp     = gEngfuncs.pfnRegisterVariable( "csz_clouds_envwarp",     "0.12",  FCVAR_CLIENTDLL );
	// STORM-CELL hot cvars (v4): mid-freq cauliflower + direct-sun tonal range + virga toggle.
	s_cvMid         = gEngfuncs.pfnRegisterVariable( "csz_clouds_mid",         "0.45",  FCVAR_CLIENTDLL );
	s_cvMidScale    = gEngfuncs.pfnRegisterVariable( "csz_clouds_midscale",    "2200",  FCVAR_CLIENTDLL );
	s_cvVirga       = gEngfuncs.pfnRegisterVariable( "csz_clouds_virga",       "0.0",   FCVAR_CLIENTDLL );
	s_cvSunFwd      = gEngfuncs.pfnRegisterVariable( "csz_clouds_sunfwd",      "1.6",   FCVAR_CLIENTDLL );
	s_cvSunG        = gEngfuncs.pfnRegisterVariable( "csz_clouds_sung",        "0.82",  FCVAR_CLIENTDLL );
	// CAP-LIGHT / BASE hot cvars (codex compare2): broad sun-facing cap faces + storm base shelf/mammatus.
	s_cvCapLight    = gEngfuncs.pfnRegisterVariable( "csz_clouds_caplight",    "1.0",   FCVAR_CLIENTDLL );
	s_cvShelf       = gEngfuncs.pfnRegisterVariable( "csz_clouds_shelf",       "1.0",   FCVAR_CLIENTDLL );
	s_cvMammatus    = gEngfuncs.pfnRegisterVariable( "csz_clouds_mammatus",    "0.35",  FCVAR_CLIENTDLL );
	// LAYER + WEATHER + ANIMATION (v5 PIVOT). Defaults = NORMAL scattered cumulus; weather 0/1/2
	// pushes the matching preset (see ApplyWeatherPreset) into the look cvars on change.
	s_cvWeather     = gEngfuncs.pfnRegisterVariable( "csz_clouds_weather",      "0",    FCVAR_CLIENTDLL );
	s_cvLayerHeight = gEngfuncs.pfnRegisterVariable( "csz_clouds_layer_height", "2600", FCVAR_CLIENTDLL );
	s_cvLayerThick  = gEngfuncs.pfnRegisterVariable( "csz_clouds_layer_thick",  "850",  FCVAR_CLIENTDLL );
	s_cvWindDir     = gEngfuncs.pfnRegisterVariable( "csz_clouds_wind_dir",     "45",   FCVAR_CLIENTDLL );
	s_cvWindSpeed   = gEngfuncs.pfnRegisterVariable( "csz_clouds_wind_speed",   "60",   FCVAR_CLIENTDLL );
	s_cvEvolve      = gEngfuncs.pfnRegisterVariable( "csz_clouds_evolve",       "35",   FCVAR_CLIENTDLL );
	// PATH A (macro cloud distribution): coverage-FIELD scale/contrast/drift + base de-tile.
	// cov_contrast is re-pushed per weather preset (scattered=high gaps, overcast=mild variation).
	s_cvCovScale    = gEngfuncs.pfnRegisterVariable( "csz_clouds_cov_scale",    "42000", FCVAR_CLIENTDLL );
	s_cvCovContrast = gEngfuncs.pfnRegisterVariable( "csz_clouds_cov_contrast", "0.85",  FCVAR_CLIENTDLL );
	s_cvCovDrift    = gEngfuncs.pfnRegisterVariable( "csz_clouds_cov_drift",    "0.0",   FCVAR_CLIENTDLL );
	s_cvDetile      = gEngfuncs.pfnRegisterVariable( "csz_clouds_detile",       "0.5",   FCVAR_CLIENTDLL );
	s_cvarsReady = true;
	CSZ_LogDev( "cloudvol", "cvars registered (csz_clouds + _tod/_res/_perf/_dbg_* + LOOK: coverage/density/sigma/basescale/detail/detailscale/hbase/htop/falloff/silver/silver_width/powder/ambient/sun/sun_elev/sun_azim/moon/moontint + STRUCTURE: billow/erode_depth/erode_oct/selfshadow/base_irreg/tower_var)" );
}

// =============================================================================
// CPU NOISE for the IN-PROCESS density bake (Phase 0). TILEABLE value-noise (Perlin
// substitute) + tileable inverted Worley (cellular billow). Clean-room: standard
// non-proprietary constructions. Phase 1 replaces this with an OFFLINE baker + real
// weather-map asset pipeline (TODO -- do NOT extend the in-process bake further).
// =============================================================================
unsigned HashU( int x, int y, int z, unsigned seed )
{
	unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)z * 2147483647u + seed * 362437u;
	h = ( h ^ ( h >> 13 ) ) * 1274126177u;
	h ^= ( h >> 16 );
	return h;
}
float HashF( int x, int y, int z, unsigned seed )
{
	return (float)( HashU( x, y, z, seed ) & 0xFFFFFFu ) / (float)0xFFFFFFu;
}
int Wrap( int v, int p )
{
	int m = v % p;
	return ( m < 0 ) ? m + p : m;
}

// Tileable trilinear value noise. nx/ny/nz in [0,1); P = lattice cells across the
// (tileable) texture; cell indices wrap mod P so f(0) == f(1) along every axis.
float ValNoise( float nx, float ny, float nz, int P, unsigned seed )
{
	float x = nx * (float)P, y = ny * (float)P, z = nz * (float)P;
	int x0 = ifloor( x ), y0 = ifloor( y ), z0 = ifloor( z );
	float fx = x - (float)x0, fy = y - (float)y0, fz = z - (float)z0;
	fx = fx * fx * ( 3.0f - 2.0f * fx );
	fy = fy * fy * ( 3.0f - 2.0f * fy );
	fz = fz * fz * ( 3.0f - 2.0f * fz );
	int xa = Wrap( x0, P ), xb = Wrap( x0 + 1, P );
	int ya = Wrap( y0, P ), yb = Wrap( y0 + 1, P );
	int za = Wrap( z0, P ), zb = Wrap( z0 + 1, P );
	float n000 = HashF( xa, ya, za, seed ), n100 = HashF( xb, ya, za, seed );
	float n010 = HashF( xa, yb, za, seed ), n110 = HashF( xb, yb, za, seed );
	float n001 = HashF( xa, ya, zb, seed ), n101 = HashF( xb, ya, zb, seed );
	float n011 = HashF( xa, yb, zb, seed ), n111 = HashF( xb, yb, zb, seed );
	float nx00 = mixf( n000, n100, fx ), nx10 = mixf( n010, n110, fx );
	float nx01 = mixf( n001, n101, fx ), nx11 = mixf( n011, n111, fx );
	return mixf( mixf( nx00, nx10, fy ), mixf( nx01, nx11, fy ), fz );
}

// Multi-octave tileable value FBM (the low-frequency "Perlin" base). Each octave
// doubles frequency AND period so every octave stays tileable.
float ValFBM( float nx, float ny, float nz, int P, unsigned seed, int oct )
{
	float s = 0.0f, amp = 0.5f, norm = 0.0f;
	int p = P;
	for( int o = 0; o < oct; o++ )
	{
		s    += amp * ValNoise( nx, ny, nz, p, seed + (unsigned)o * 101u );
		norm += amp;
		amp  *= 0.5f;
		p    *= 2;
	}
	return ( norm > 0.0f ) ? ( s / norm ) : 0.0f;
}

// Tileable inverted Worley (cellular billow): 1 - F1 distance. High near feature points
// -> puffy cauliflower lumps. Cell indices wrap mod P so the field tiles seamlessly.
float InvWorley( float nx, float ny, float nz, int P, unsigned seed )
{
	float x = nx * (float)P, y = ny * (float)P, z = nz * (float)P;
	int xi = ifloor( x ), yi = ifloor( y ), zi = ifloor( z );
	float fx = x - (float)xi, fy = y - (float)yi, fz = z - (float)zi;
	float f1 = 1.0e9f;
	for( int dz = -1; dz <= 1; dz++ )
	for( int dy = -1; dy <= 1; dy++ )
	for( int dx = -1; dx <= 1; dx++ )
	{
		int wx = Wrap( xi + dx, P ), wy = Wrap( yi + dy, P ), wz = Wrap( zi + dz, P );
		float ox = HashF( wx, wy, wz, seed * 3u + 1u );
		float oy = HashF( wx, wy, wz, seed * 3u + 2u );
		float oz = HashF( wx, wy, wz, seed * 3u + 3u );
		float rx = ( (float)dx + ox ) - fx;
		float ry = ( (float)dy + oy ) - fy;
		float rz = ( (float)dz + oz ) - fz;
		float dd = rx * rx + ry * ry + rz * rz;
		if( dd < f1 ) f1 = dd;
	}
	float dF = sqrtf( f1 );
	return clampf( 1.0f - dF, 0.0f, 1.0f );
}

float Remap( float v, float a, float b, float c, float d )
{
	float t = ( v - a ) / ( ( b - a != 0.0f ) ? ( b - a ) : 1e-4f );
	t = clampf( t, 0.0f, 1.0f );
	return c + t * ( d - c );
}

// ---- baked 3D textures (generation-keyed) ----------------------------------------
const int kBaseN   = 128;   // 128^3 RGBA8 Perlin-Worley base
const int kDetailN = 32;    // 32^3  RGBA8 high-freq Worley detail

// dynamically-loaded glTexImage3D (NOT in the static csz GL func table).
typedef void ( APIENTRY *CSZ_PFNTEXIMAGE3D )( GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void * );
CSZ_PFNTEXIMAGE3D s_glTexImage3D = NULL;
bool s_triedLoad3d = false;

unsigned char FloatToU8( float v ) { return (unsigned char)( clampf( v, 0.0f, 1.0f ) * 255.0f + 0.5f ); }

// Bake the 128^3 Perlin-Worley base: R = low-freq Perlin FBM dilated by inverted Worley
// (the cauliflower base shape), G/B/A = single-octave inverted Worley at increasing
// frequency (the billow-dilation FBM the shader rebuilds as 0.625G+0.25B+0.125A).
void BakeBase( std::vector<unsigned char> &out )
{
	const int N = kBaseN;
	out.resize( (size_t)N * N * N * 4 );
	const int Pp = 4;    // perlin base cells across the (tileable) volume
	const int Pw = 6;    // worley base cells
	for( int z = 0; z < N; z++ )
	for( int y = 0; y < N; y++ )
	for( int x = 0; x < N; x++ )
	{
		float nx = (float)x / (float)N, ny = (float)y / (float)N, nz = (float)z / (float)N;
		float perlin = ValFBM( nx, ny, nz, Pp, 1311u, 3 );
		float wLow   = InvWorley( nx, ny, nz, Pw, 2207u );
		// classic Perlin-Worley: dilate the Perlin shape by the low-freq Worley billow.
		float pw = Remap( perlin, wLow - 1.0f, 1.0f, 0.0f, 1.0f );
		float w1 = InvWorley( nx, ny, nz, Pw,     5101u );
		float w2 = InvWorley( nx, ny, nz, Pw * 2, 5102u );
		float w3 = InvWorley( nx, ny, nz, Pw * 4, 5103u );
		size_t idx = ( ( (size_t)z * N + y ) * N + x ) * 4;
		out[idx + 0] = FloatToU8( pw );
		out[idx + 1] = FloatToU8( w1 );
		out[idx + 2] = FloatToU8( w2 );
		out[idx + 3] = FloatToU8( w3 );
	}
}

// Bake the 32^3 high-freq Worley detail: RGB = inverted Worley at three increasing
// frequencies (the shader rebuilds 0.625R+0.25G+0.125B to erode the silhouette).
void BakeDetail( std::vector<unsigned char> &out )
{
	const int N = kDetailN;
	out.resize( (size_t)N * N * N * 4 );
	const int Pd = 4;
	for( int z = 0; z < N; z++ )
	for( int y = 0; y < N; y++ )
	for( int x = 0; x < N; x++ )
	{
		float nx = (float)x / (float)N, ny = (float)y / (float)N, nz = (float)z / (float)N;
		float w1 = InvWorley( nx, ny, nz, Pd,     7001u );
		float w2 = InvWorley( nx, ny, nz, Pd * 2, 7002u );
		float w3 = InvWorley( nx, ny, nz, Pd * 4, 7003u );
		size_t idx = ( ( (size_t)z * N + y ) * N + x ) * 4;
		out[idx + 0] = FloatToU8( w1 );
		out[idx + 1] = FloatToU8( w2 );
		out[idx + 2] = FloatToU8( w3 );
		out[idx + 3] = 255;
	}
}

// ---- quarter-res RGBA16F march target (generation-keyed) --------------------------
struct VolTarget
{
	GLuint fbo;
	GLuint colorTex;
	int    width, height;
	int    gpuGeneration;
	bool   valid;
	bool   failedThisGen;
};
VolTarget s_tgt;

// ---- march + upsample programs + 3D textures + timer ring -------------------------
const int kRing = 3;   // 3-deep GL_TIME_ELAPSED ring (non-blocking readback)

struct VolGpu
{
	ShaderProgram march;
	ShaderProgram upsample;
	GLuint vao;

	// march uniforms
	int mCamFwd, mCamRight, mCamUp, mCamPos, mLightDir, mLightColor, mAmbGround, mAmbSky;
	int mBoxMin, mBoxMax, mTime, mFrame, mDensity, mCoverage, mSilver, mSigmaT;
	int mBaseFreq, mDetailFreq, mDetailAmt, mLightReach, mMarchFar, mTargetSize, mSteps, mLightSteps;
	int mFalloff, mHBase, mHTop, mPowder, mSilverWidth;
	int mBillow, mErodeDepth, mErodeOct, mSelfShadow, mBaseIrreg, mTowerVar, mEnvWarp;
	int mMid, mMidFreq, mVirga, mSunForward, mSunG;
	int mCapLight, mShelf, mMammatus, mCapEps;
	int mStepLenMax, mSkyVisFloor;   // P0 grazing-ray step-length cap + P3 weather-gated skyVis floor
	int mWindVec, mEvolveRate;   // v5 layer animation: wind drift + volume evolve
	int mDomainWarp, mHorizonFadeLo, mHorizonFadeHi, mDbgMode;   // iter4: R1 warp / R2 elevation fade / R5 debug
	int mWeatherKind, mOvercastVar, mLowHaze;   // iter5: weather hard-gate + P1 overcast billow + P2 cold low-band haze
	int mCovFreq, mCovContrast, mCovDrift, mDetile;   // PATH A: macro coverage field (freq/contrast/drift) + base de-tile
	int mDepthTex, mZNear, mZFar, mInvViewProj, mBase3d, mDetail3d;
	int mNightLum, mFineDiv;   // REBUILD: single nightness luminance authority + ESS fine-step divisor
	// upsample uniforms
	int uCloudTex, uFullSize, uUpDbgMode;
	int uDepthTex, uZNear, uZFar, uDepthSigma, uAlphaSigma, uCasAmount;   // REBUILD: joint depth+alpha bilateral + CAS sharpen

	// baked 3D textures
	GLuint base3d, detail3d;
	bool   baked;

	// GPU timer ring
	GLuint   query[kRing];
	bool     qInFlight[kRing];
	unsigned qFrame[kRing];
	int      ringHead;

	int  gpuGeneration;
	bool built;
};
VolGpu s_gpu;

unsigned s_frame = 0;          // monotonic Contribute frame counter
bool     s_tmuProbed = false;  // GL_MAX_TEXTURE_IMAGE_UNITS query (codex #10) done once
bool     s_tmuOk     = false;
bool     s_anchored  = false;  // hero-box world anchor latched (per generation)
float    s_anchor[3] = { 0.0f, 0.0f, 0.0f };
float    s_anchorFwd[2] = { 1.0f, 0.0f };  // spawn-frame horizontal view-forward (XY, normalized) for the nearbox knob

void ForgetGpuTimers()
{
	for( int i = 0; i < kRing; i++ ) { s_gpu.query[i] = 0; s_gpu.qInFlight[i] = false; s_gpu.qFrame[i] = 0; }
	s_gpu.ringHead = 0;
}

void ForgetTarget()
{
	s_tgt.fbo = 0; s_tgt.colorTex = 0; s_tgt.width = 0; s_tgt.height = 0;
	s_tgt.valid = false; s_tgt.failedThisGen = false;
}

void DestroyTargetSameContext()
{
	if( s_tgt.fbo )      glDeleteFramebuffers( 1, &s_tgt.fbo );
	if( s_tgt.colorTex ) glDeleteTextures( 1, &s_tgt.colorTex );
	ForgetTarget();
}

bool EnsureTarget( int w, int h )
{
	if( w < 1 ) w = 1;
	if( h < 1 ) h = 1;
	if( s_tgt.gpuGeneration != GpuGeneration() )
	{
		ForgetTarget();
		s_tgt.gpuGeneration = GpuGeneration();
	}
	if( s_tgt.valid && s_tgt.width == w && s_tgt.height == h )
		return true;
	if( s_tgt.failedThisGen )
		return false;
	if( s_tgt.fbo != 0 || s_tgt.colorTex != 0 )
		DestroyTargetSameContext();
	s_tgt.gpuGeneration = GpuGeneration();

	glGenTextures( 1, &s_tgt.colorTex );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_tgt.colorTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	SkyComposeRestoreTmus();

	glGenFramebuffers( 1, &s_tgt.fbo );
	BindFbo( s_tgt.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tgt.colorTex, 0 );
	GLenum drawBuf = GL_COLOR_ATTACHMENT0;
	glDrawBuffers( 1, &drawBuf );
	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	BindFbo( 0 );
	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		DestroyTargetSameContext();
		s_tgt.failedThisGen = true;
		CSZ_LogError( "cloudvol", "quarter-res FBO incomplete (0x%x); clouds disabled this generation", (unsigned)status );
		return false;
	}
	s_tgt.width = w; s_tgt.height = h; s_tgt.valid = true;
	CSZ_LogInfo( "cloudvol", "quarter-res march target ready (%dx%d RGBA16F, gpu gen %d)", w, h, s_tgt.gpuGeneration );
	return true;
}

GLuint UploadVolume3d( int N, const std::vector<unsigned char> &px, int skyUnit )
{
	GLuint tex = 0;
	glGenTextures( 1, &tex );
	SkyComposeBindTex( skyUnit, GL_TEXTURE_3D, tex );
	while( glGetError() != GL_NO_ERROR ) { }
	s_glTexImage3D( GL_TEXTURE_3D, 0, GL_RGBA8, N, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px[0] );
	GLenum upErr = glGetError();
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );  // level 0 only (no mips: glGenerateMipmap unwired)
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT );
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT );
	GLenum parErr = glGetError();
	SkyComposeRestoreTmus();
	if( upErr != GL_NO_ERROR || parErr != GL_NO_ERROR )
	{
		glDeleteTextures( 1, &tex );
		CSZ_LogError( "cloudvol", "[csz_clouds] 3D upload failed N=%d up=0x%x par=0x%x", N, (unsigned)upErr, (unsigned)parErr );
		return 0;
	}
	return tex;
}

// CPU-bake + upload the 128^3 base + 32^3 detail volumes once per generation. Phase 0
// in-process bake (blocking, logged); Phase 1 = offline baker + .bin asset loader (TODO).
bool EnsureBake()
{
	if( s_gpu.baked && s_gpu.base3d && s_gpu.detail3d )
		return true;

	if( !s_triedLoad3d )
	{
		s_triedLoad3d = true;
		if( gRenderAPI.GL_GetProcAddress != NULL )
			s_glTexImage3D = (CSZ_PFNTEXIMAGE3D)gRenderAPI.GL_GetProcAddress( "glTexImage3D" );
	}
	if( s_glTexImage3D == NULL )
	{
		CSZ_LogError( "cloudvol", "[csz_clouds] glTexImage3D unavailable; clouds disabled this generation" );
		return false;
	}

	float t0 = ClientTime();
	std::vector<unsigned char> base, detail;
	BakeBase( base );
	BakeDetail( detail );
	float bakeMs = ( ClientTime() - t0 ) * 1000.0f;

	s_gpu.base3d   = UploadVolume3d( kBaseN,   base,   1 );   // sky unit 1 for setup
	s_gpu.detail3d = UploadVolume3d( kDetailN, detail, 2 );   // sky unit 2 for setup
	if( s_gpu.base3d == 0 || s_gpu.detail3d == 0 )
	{
		if( s_gpu.base3d )   { glDeleteTextures( 1, &s_gpu.base3d );   s_gpu.base3d = 0; }
		if( s_gpu.detail3d ) { glDeleteTextures( 1, &s_gpu.detail3d ); s_gpu.detail3d = 0; }
		return false;
	}
	s_gpu.baked = true;
	CSZ_LogInfo( "cloudvol", "[csz_clouds] density baked in-process: base %d^3 + detail %d^3 RGBA8 (bake %.1f ms, gpu gen %d)",
		kBaseN, kDetailN, bakeMs, s_gpu.gpuGeneration );
	return true;
}

void RunTmuProbe()
{
	if( s_tmuProbed )
		return;
	s_tmuProbed = true;
	while( glGetError() != GL_NO_ERROR ) { }
	GLint maxUnits = 0;
	glGetIntegerv( GL_MAX_TEXTURE_IMAGE_UNITS, &maxUnits );
	while( glGetError() != GL_NO_ERROR ) { }
	// units used: depth = kSkyTmuBase+0, base3d = +1, detail3d = +2 (3 within the sky
	// reserved range [kSkyTmuBase, kSkyTmuBase+kSkyTmuCount) = [4,8)).
	int needAbs = kSkyTmuBase + 2;   // highest absolute unit index touched
	s_tmuOk = ( (int)maxUnits > needAbs ) && ( 3 <= kSkyTmuCount );
	CSZ_LogInfo( "cloudvol", "[csz_clouds] GL_MAX_TEXTURE_IMAGE_UNITS=%d; using units depth=%d base3d=%d detail3d=%d (need>%d, skyReserve=%d) -> %s",
		(int)maxUnits, kSkyTmuBase + 0, kSkyTmuBase + 1, kSkyTmuBase + 2, needAbs, kSkyTmuCount, s_tmuOk ? "OK" : "INSUFFICIENT" );
	if( !s_tmuOk )
		CSZ_LogError( "cloudvol", "[csz_clouds] insufficient texture image units; clouds disabled" );
}

void BuildPrograms()
{
	if( s_gpu.built && s_gpu.gpuGeneration == GpuGeneration() )
		return;
	if( s_gpu.gpuGeneration != GpuGeneration() )
	{
		// foreign generation: forget all GL names (never glDelete a dead context)
		s_gpu.march.program = 0; s_gpu.upsample.program = 0; s_gpu.vao = 0;
		s_gpu.base3d = 0; s_gpu.detail3d = 0; s_gpu.baked = false;
		ForgetGpuTimers();
		s_gpu.gpuGeneration = GpuGeneration();
	}
	if( !s_gpu.vao )
		glGenVertexArrays( 1, &s_gpu.vao );
	// A compile failure disables the pass this generation (NOT fatal -- production is OFF
	// by default, so a broken cloud shader must never brick the game).
	if( !BuildProgram( "csz_cloudvol_march", kCloudVs, kCloudMarchFs, false, s_gpu.march ) ||
	    !BuildProgram( "csz_cloudvol_upsample", kCloudVs, kCloudUpsampleFs, false, s_gpu.upsample ) )
	{
		CSZ_LogError( "cloudvol", "shader build failed; volumetric clouds disabled this generation" );
		s_gpu.built = false;
		return;
	}

	s_gpu.mCamFwd      = UniformLoc( s_gpu.march, "u_camFwd" );
	s_gpu.mCamRight    = UniformLoc( s_gpu.march, "u_camRight" );
	s_gpu.mCamUp       = UniformLoc( s_gpu.march, "u_camUp" );
	s_gpu.mCamPos      = UniformLoc( s_gpu.march, "u_camPos" );
	s_gpu.mLightDir    = UniformLoc( s_gpu.march, "u_lightDir" );
	s_gpu.mLightColor  = UniformLoc( s_gpu.march, "u_lightColor" );
	s_gpu.mAmbGround   = UniformLoc( s_gpu.march, "u_ambGround" );
	s_gpu.mAmbSky      = UniformLoc( s_gpu.march, "u_ambSky" );
	s_gpu.mBoxMin      = UniformLoc( s_gpu.march, "u_boxMin" );
	s_gpu.mBoxMax      = UniformLoc( s_gpu.march, "u_boxMax" );
	s_gpu.mTime        = UniformLoc( s_gpu.march, "u_time" );
	s_gpu.mFrame       = UniformLoc( s_gpu.march, "u_frame" );
	s_gpu.mDensity     = UniformLoc( s_gpu.march, "u_density" );
	s_gpu.mCoverage    = UniformLoc( s_gpu.march, "u_coverage" );
	s_gpu.mSilver      = UniformLoc( s_gpu.march, "u_silver" );
	s_gpu.mSilverWidth = UniformLoc( s_gpu.march, "u_silverWidth" );
	s_gpu.mSigmaT      = UniformLoc( s_gpu.march, "u_sigmaT" );
	s_gpu.mBaseFreq    = UniformLoc( s_gpu.march, "u_baseFreq" );
	s_gpu.mDetailFreq  = UniformLoc( s_gpu.march, "u_detailFreq" );
	s_gpu.mDetailAmt   = UniformLoc( s_gpu.march, "u_detailAmt" );
	s_gpu.mFalloff     = UniformLoc( s_gpu.march, "u_falloff" );
	s_gpu.mHBase       = UniformLoc( s_gpu.march, "u_hBase" );
	s_gpu.mHTop        = UniformLoc( s_gpu.march, "u_hTop" );
	s_gpu.mPowder      = UniformLoc( s_gpu.march, "u_powder" );
	s_gpu.mBillow      = UniformLoc( s_gpu.march, "u_billow" );
	s_gpu.mErodeDepth  = UniformLoc( s_gpu.march, "u_erodeDepth" );
	s_gpu.mErodeOct    = UniformLoc( s_gpu.march, "u_erodeOct" );
	s_gpu.mSelfShadow  = UniformLoc( s_gpu.march, "u_selfShadow" );
	s_gpu.mBaseIrreg   = UniformLoc( s_gpu.march, "u_baseIrreg" );
	s_gpu.mTowerVar    = UniformLoc( s_gpu.march, "u_towerVar" );
	s_gpu.mEnvWarp     = UniformLoc( s_gpu.march, "u_envWarp" );
	s_gpu.mMid         = UniformLoc( s_gpu.march, "u_mid" );
	s_gpu.mMidFreq     = UniformLoc( s_gpu.march, "u_midFreq" );
	s_gpu.mVirga       = UniformLoc( s_gpu.march, "u_virga" );
	s_gpu.mSunForward  = UniformLoc( s_gpu.march, "u_sunForward" );
	s_gpu.mSunG        = UniformLoc( s_gpu.march, "u_sunG" );
	s_gpu.mCapLight    = UniformLoc( s_gpu.march, "u_capLight" );
	s_gpu.mShelf       = UniformLoc( s_gpu.march, "u_shelf" );
	s_gpu.mMammatus    = UniformLoc( s_gpu.march, "u_mammatus" );
	s_gpu.mCapEps      = UniformLoc( s_gpu.march, "u_capEps" );
	s_gpu.mStepLenMax  = UniformLoc( s_gpu.march, "u_stepLenMax" );   // P0
	s_gpu.mSkyVisFloor = UniformLoc( s_gpu.march, "u_skyVisFloor" );  // P3
	s_gpu.mWindVec     = UniformLoc( s_gpu.march, "u_windVec" );
	s_gpu.mEvolveRate  = UniformLoc( s_gpu.march, "u_evolveRate" );
	s_gpu.mDomainWarp    = UniformLoc( s_gpu.march, "u_domainWarp" );      // R1
	s_gpu.mHorizonFadeLo = UniformLoc( s_gpu.march, "u_horizonFadeLo" );  // R2
	s_gpu.mHorizonFadeHi = UniformLoc( s_gpu.march, "u_horizonFadeHi" );  // R2
	s_gpu.mDbgMode       = UniformLoc( s_gpu.march, "u_dbgMode" );        // R5
	s_gpu.mWeatherKind   = UniformLoc( s_gpu.march, "u_weatherKind" );    // iter5 hard-gate
	s_gpu.mOvercastVar   = UniformLoc( s_gpu.march, "u_overcastVar" );    // iter5 P1
	s_gpu.mLowHaze       = UniformLoc( s_gpu.march, "u_lowHaze" );        // iter5 P2
	s_gpu.mCovFreq       = UniformLoc( s_gpu.march, "u_covFreq" );        // PATH A coverage field
	s_gpu.mCovContrast   = UniformLoc( s_gpu.march, "u_covContrast" );    // PATH A
	s_gpu.mCovDrift      = UniformLoc( s_gpu.march, "u_covDrift" );       // PATH A
	s_gpu.mDetile        = UniformLoc( s_gpu.march, "u_detile" );         // CHANGE 2 base de-tile
	s_gpu.mLightReach  = UniformLoc( s_gpu.march, "u_lightReach" );
	s_gpu.mMarchFar    = UniformLoc( s_gpu.march, "u_marchFar" );
	s_gpu.mTargetSize  = UniformLoc( s_gpu.march, "u_targetSize" );
	s_gpu.mSteps       = UniformLoc( s_gpu.march, "u_steps" );
	s_gpu.mLightSteps  = UniformLoc( s_gpu.march, "u_lightSteps" );
	s_gpu.mDepthTex    = UniformLoc( s_gpu.march, "u_depthTex" );
	s_gpu.mZNear       = UniformLoc( s_gpu.march, "u_zNear" );
	s_gpu.mZFar        = UniformLoc( s_gpu.march, "u_zFar" );
	s_gpu.mInvViewProj = UniformLoc( s_gpu.march, "u_invViewProj" );
	s_gpu.mBase3d      = UniformLoc( s_gpu.march, "u_base3d" );
	s_gpu.mDetail3d    = UniformLoc( s_gpu.march, "u_detail3d" );
	s_gpu.mNightLum    = UniformLoc( s_gpu.march, "u_nightLum" );   // REBUILD: single nightness luminance authority
	s_gpu.mFineDiv     = UniformLoc( s_gpu.march, "u_fineDiv" );    // REBUILD: ESS in-cloud fine-step divisor

	s_gpu.uCloudTex    = UniformLoc( s_gpu.upsample, "u_cloudTex" );
	s_gpu.uFullSize    = UniformLoc( s_gpu.upsample, "u_fullSize" );
	s_gpu.uUpDbgMode   = UniformLoc( s_gpu.upsample, "u_dbgMode" );   // R5 mode 6: raw nearest upsample
	s_gpu.uDepthTex    = UniformLoc( s_gpu.upsample, "u_depthTex" );   // REBUILD: full-res scene depth for the depth edge-stop
	s_gpu.uZNear       = UniformLoc( s_gpu.upsample, "u_zNear" );
	s_gpu.uZFar        = UniformLoc( s_gpu.upsample, "u_zFar" );
	s_gpu.uDepthSigma  = UniformLoc( s_gpu.upsample, "u_depthSigma" );
	s_gpu.uAlphaSigma  = UniformLoc( s_gpu.upsample, "u_alphaSigma" );
	s_gpu.uCasAmount   = UniformLoc( s_gpu.upsample, "u_casAmount" );

	glGenQueries( kRing, s_gpu.query );
	for( int i = 0; i < kRing; i++ ) { s_gpu.qInFlight[i] = false; s_gpu.qFrame[i] = 0; }
	s_gpu.ringHead = 0;

	s_gpu.built = true;
	CSZ_LogDev( "cloudvol", "march + upsample programs + timer ring built (gpu gen %d)", s_gpu.gpuGeneration );
}

// ---- celestial light: sun by day, moon by night, blended by SMOOTH nightness -------
// Replaces the rejected module's HARD night>0.5 step with a continuous nightness lerp
// (codex #1: smooth twilight). SAME scattering math; night = lower intensity + cool
// Purkinje tint. nightness in [0,1] (0 day/sunset -> 1 midnight).
struct CelLight
{
	float dir[3];
	float color[3];
	float ambGround[3];
	float ambSky[3];
};
void DeriveCelestial( float phase, float nightness, float sunI, float moonI, float ambientMul, float moonTint,
                      const float *moonDirReal, const float *moonColReal, CelLight &out )
{
	float sun[3];
	skymath::SunDir( phase, sun );
	// N2: light the night clouds by the REAL published moon, not the sun antipode. When the scene
	// publishes a valid moonlightDir (surface->moon, same TOWARD-the-body convention as SunDir), use
	// it so the lit face / silver rim lands on the moon-facing edge; else fall back to -sun.
	float moon[3] = { -sun[0], -sun[1], -sun[2] };
	if( moonDirReal )
	{
		float ml = sqrtf( moonDirReal[0]*moonDirReal[0] + moonDirReal[1]*moonDirReal[1] + moonDirReal[2]*moonDirReal[2] );
		if( ml > 0.1f )
		{
			moon[0] = moonDirReal[0] / ml; moon[1] = moonDirReal[1] / ml; moon[2] = moonDirReal[2] / ml;
		}
	}

	// Direction: lerp sun->moon, guarded against the exact-antipode cancellation at the
	// twilight crossover (pick the dominant body if the blend nears zero length).
	float d[3];
	for( int i = 0; i < 3; i++ ) d[i] = mixf( sun[i], moon[i], nightness );
	float len = sqrtf( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] );
	if( len < 0.05f )
	{
		const float *b = ( nightness >= 0.5f ) ? moon : sun;
		out.dir[0] = b[0]; out.dir[1] = b[1]; out.dir[2] = b[2];
	}
	else
	{
		out.dir[0] = d[0] / len; out.dir[1] = d[1] / len; out.dir[2] = d[2] / len;
	}

	// REBUILD v2 NIGHT AUTHORITY (§3.E): DECOUPLE chroma from luminance. The moon emits a
	// DAY-MAGNITUDE cool-white chroma (NOT a dim absolute intensity); the relative dial `moonI`
	// (csz_clouds_moon, default 1.0) only trims that magnitude. ALL night dimming is owned by the
	// single shader multiply u_nightLum (applied to the marched radiance before the soft-knee), so
	// direct + ambient + rim can NEVER desync and night L stays far below the knee (never whitens).
	// The retired `moonI=1.9` absolute and the ngS/ngG night-ambient floor were the night-too-bright
	// defect -- both deleted. moonTint is chroma-only (0 neutral .. 2 very cool).
	const float dayC[3]   = { 1.00f, 0.97f, 0.90f };
	// N2: cool-white moon CHROMA target = the real published moonColor (~{0.54,0.64,0.95}), normalized
	// to max-channel 1.0 so it stays DAY-MAGNITUDE (a chroma, not a dim absolute -- u_nightLum owns the
	// dimming) and the cool blue-silver scotopic tint is the moon's actual hue. Falls back to a fixed
	// cool-white if no valid moonColor is published.
	float coolC[3] = { 0.74f, 0.86f, 1.00f };
	if( moonColReal )
	{
		float mx = moonColReal[0];
		if( moonColReal[1] > mx ) mx = moonColReal[1];
		if( moonColReal[2] > mx ) mx = moonColReal[2];
		if( mx > 1e-3f )
		{
			coolC[0] = moonColReal[0] / mx; coolC[1] = moonColReal[1] / mx; coolC[2] = moonColReal[2] / mx;
		}
	}
	float nightC[3];
	for( int i = 0; i < 3; i++ )
		nightC[i] = mixf( 1.0f, coolC[i], clampf( moonTint, 0.0f, 2.0f ) );
	for( int i = 0; i < 3; i++ )
		out.color[i] = mixf( dayC[i] * sunI, nightC[i] * sunI * moonI, nightness );   // both ends ~day magnitude; u_nightLum dims night

	// Height-aware ambient skylight (cool sky zenith vs darker ground bounce). NIGHT = the SAME day
	// ambient cooled by a near-luminance-preserving chroma bias (NO separate dim floor); u_nightLum
	// does the dimming downstream. This is the single-authority design (§7 conflict resolutions 2-4).
	const float dayG[3] = { 0.13f, 0.15f, 0.18f }; const float dayS[3] = { 0.34f, 0.42f, 0.56f };
	const float coolBias[3] = { 0.82f, 0.95f, 1.18f };   // cool chroma shift, mean ~0.98 (does not brighten)
	for( int i = 0; i < 3; i++ )
	{
		out.ambGround[i] = mixf( dayG[i], dayG[i] * coolBias[i], nightness ) * ambientMul;
		out.ambSky[i]    = mixf( dayS[i], dayS[i] * coolBias[i], nightness ) * ambientMul;
	}
}

// =============================================================================
// REBUILD v2 SCOPE: weather 0 (scattered cumulus) is the ONLY render path. The rain (w1) and
// snow (w2) overcast presets + the chroma-tint pass were DELETED with the overcast shader
// branches -- the cloud look is now driven directly by the registered cvar defaults (no preset
// writeback). csz_clouds_weather is left registered but inert for config-compatibility.
// =============================================================================
#if 0   // retired weather-preset machinery (rain/snow out of scope for this rebuild)
void ApplyWeatherPreset( int w )
{
	// iter5: detail/erode/selfsh added so P5 (NORMAL edge crispen) + P1-FIX-5 (per-weather selfShadow)
	// are wired PER-PRESET WITH RESET (BLOCKER 5): every weather change re-pushes ALL preset cvars, so
	// a NORMAL-only edge tune cannot leak into rain/snow and vice-versa.
	struct P { float cov, dens, sigma, amb, sun, sunfwd, cap, silver, powder, height, thick, detail, erode, selfsh, covctr; };
	// PATH A covctr (coverage-FIELD contrast): the per-weather spread of the macro coverage field
	// around `cov` (the field LEVEL). NORMAL = HIGH (0.85) so columns swing from clear to puffy =>
	// scattered cumulus over open blue sky. RAIN/SNOW = MILD (0.22/0.30) so the deck stays mostly
	// overcast but keeps large-scale thick/thin variation (not a dead-flat ceiling).
	const P presets[3] = {
		// cov   dens   sigma    amb    sun    sunfwd cap    silver powder height  thick   detail erode  selfsh covctr
		// P2: NORMAL thick 850->1500 so the height gradient produces REAL rounded vertical form (a
		//     850u deck was too thin for the gradient to read => flat sheet). capEps is now derived
		//     from the SMALLEST box dim (below) so the cap-light normal still resolves at this thickness.
		// P5 iter5: NORMAL silver 0.50->0.80, detail 0.70->0.85, erode 0.30->0.55 = crisper torn cumulus
		//     edges (per-preset; overcast keeps smooth-stratiform 0.70/0.30). selfShadow 1.40 (was global).
		{ 0.40f, 1.15f, 0.0045f, 1.00f, 3.40f, 1.70f, 1.10f, 0.80f, 0.40f, 2600.f, 1500.f, 0.85f, 0.55f, 1.40f, 0.85f },  // 0 NORMAL
		// P3 RAIN: was near-BLACK (sigma .011 + amb .42 + sun .95 + skyVis->0). Rebalanced to a
		//     legible dark blue-GREY rainy DAY: sigma .011->.0060 (not an opaque void), amb .42->1.00
		//     + weather-gated skyVis floor (undersides read), sun .95->1.40 / cap .35->.60 (lighter
		//     top than base = form), dens 2.10->1.80 (not pitch-opaque). Cool-neutral tint via ApplyWeatherTint.
		// R4 (iter4): readable dark blue-GREY rainy DAY (not a black ceiling). amb 1.00->1.30 +
		//     sigma 0.0060->0.0050 (less opaque, structure stays visible); paired with the shader's
		//     overcast underside-lift floor so a LOW upward vantage reads grey, not black.
		// P1-FIX-5 iter5: RAIN selfShadow 1.40->1.90 = deeper cone shadow => darker carved turbulent
		//     undersides on the (now structured) deck. detail/erode kept smooth-stratiform 0.70/0.30.
		// P1-FIX iter6: RAIN selfShadow 1.90->1.40 ONLY (NOT 0.95 -- codex: 0.95 ~halves cone extinction =>
		//     brightens rain toward snow). 1.40 lets sunVis=exp(-tauL*selfShadow) VARY (cores stay darker
		//     than breaks) instead of flat-zeroing the whole cov=0.95 deck => lit/shadow contrast returns
		//     WITHOUT raising the mean. cap (0.60) / sunfwd (0.30) / sun (1.40) / amb (1.30) deliberately
		//     UNCHANGED -- those raise mean; the goal is VARIANCE (skyLumaSD 4.3->~20), rain STAYS DARK.
		// P1 iter7 (1): RAIN selfShadow 1.40->1.10 (selfsh field, RAIN row ONLY). The physical lit-top/
		//     shadowed-underside contrast lever: lower cone self-shadow => sunVis=exp(-tauL*selfShadow)
		//     varies MORE across the deck => real volumetric lit/shadow structure (variance up). Small
		//     mean rise acceptable per directive (w1 must stay <=~70). NORMAL/SNOW rows untouched.
		{ 0.95f, 1.80f, 0.0050f, 1.30f, 1.40f, 0.30f, 0.60f, 0.10f, 0.55f, 1700.f, 1150.f, 0.70f, 0.30f, 1.10f, 0.22f },  // 1 RAIN
		// P3 SNOW: bright cold cool-WHITE. amb 1.35->1.70, sun 1.80->2.40, cap .70->1.00, sigma
		//     .0075->.0055 (light penetrates => the deck glows). Cool-white tint via ApplyWeatherTint.
		// R4 (iter4): brighter COLD cool-WHITE. amb 1.70->2.10, sun 2.40->2.80, cap 1.00->1.30,
		//     sigma 0.0055->0.0048 (light penetrates => the deck glows white, not dull mid-grey).
		// P3 iter5: SNOW amb 2.10->2.50, cap 1.30->1.50 = brighter cold high-albedo deck (paired with
		//     P4 tonemap to hold highlight detail + the ApplyWeatherTint cool ambient/base below).
		// P1-FIX-5: SNOW selfShadow 1.40->1.20 = shallower cone shadow => bright diffuse undersides
		//     (not carved-dark). detail/erode kept smooth 0.70/0.30.
		{ 0.90f, 1.55f, 0.0048f, 2.50f, 2.80f, 0.70f, 1.50f, 0.25f, 0.45f, 2100.f, 1000.f, 0.70f, 0.30f, 1.20f, 0.30f },  // 2 SNOW
	};
	const P &p = presets[ clampi( w, 0, 2 ) ];
	gEngfuncs.Cvar_SetValue( "csz_clouds_coverage",     p.cov );
	gEngfuncs.Cvar_SetValue( "csz_clouds_density",      p.dens );
	gEngfuncs.Cvar_SetValue( "csz_clouds_sigma",        p.sigma );
	gEngfuncs.Cvar_SetValue( "csz_clouds_ambient",      p.amb );
	gEngfuncs.Cvar_SetValue( "csz_clouds_sun",          p.sun );
	gEngfuncs.Cvar_SetValue( "csz_clouds_sunfwd",       p.sunfwd );
	gEngfuncs.Cvar_SetValue( "csz_clouds_caplight",     p.cap );
	gEngfuncs.Cvar_SetValue( "csz_clouds_silver",       p.silver );
	gEngfuncs.Cvar_SetValue( "csz_clouds_powder",       p.powder );
	gEngfuncs.Cvar_SetValue( "csz_clouds_layer_height", p.height );
	gEngfuncs.Cvar_SetValue( "csz_clouds_layer_thick",  p.thick );
	// iter5 P5 / P1-FIX-5: per-preset edge + self-shadow (reset on every weather change => no leak).
	gEngfuncs.Cvar_SetValue( "csz_clouds_detail",       p.detail );
	gEngfuncs.Cvar_SetValue( "csz_clouds_erode_depth",  p.erode );
	gEngfuncs.Cvar_SetValue( "csz_clouds_selfshadow",   p.selfsh );
	// PATH A: per-weather coverage-FIELD contrast (reset on every weather change => no leak).
	gEngfuncs.Cvar_SetValue( "csz_clouds_cov_contrast", p.covctr );
	CSZ_LogInfo( "cloudvol",
		"[csz_clouds] weather preset %d applied (cov=%.2f dens=%.2f sigma=%.4f amb=%.2f sun=%.2f sunfwd=%.2f cap=%.2f height=%.0f thick=%.0f)",
		w, p.cov, p.dens, p.sigma, p.amb, p.sun, p.sunfwd, p.cap, p.height, p.thick );
}

// Weather CHROMA tint (hue only; brightness is set by the sun/ambient preset scalars above so
// they stay hot). NORMAL keeps the warm sun + blue-sky ambient. RAIN neutral-greys the lit
// response and desaturates the ambient (shadows read grey, not blue). SNOW shifts the lit
// response toward a cool luminous white. Applied AFTER DeriveCelestial + the sun-dir override.
void ApplyWeatherTint( int w, CelLight &c )
{
	if( w == 0 )
		return;
	float lum = c.color[0] * 0.30f + c.color[1] * 0.59f + c.color[2] * 0.11f;
	const float coolRain[3] = { 0.97f, 1.00f, 1.05f };
	const float coolSnow[3] = { 0.90f, 1.00f, 1.16f };  // P3 iter5: bluer chroma (was {0.95,1.00,1.10})
	const float *cool   = ( w == 1 ) ? coolRain : coolSnow;
	float        kColor = ( w == 1 ) ? 0.72f : 0.78f;   // P3 iter5: SNOW 0.68->0.78 = reads distinctly cold-blue, not neutral
	for( int i = 0; i < 3; i++ )
		c.color[i] = mixf( c.color[i], lum * cool[i], kColor );
	if( w == 1 )   // RAIN: desaturate the ambient toward neutral grey.
	{
		float ag = ( c.ambGround[0] + c.ambGround[1] + c.ambGround[2] ) / 3.0f;
		float as = ( c.ambSky[0]    + c.ambSky[1]    + c.ambSky[2] )    / 3.0f;
		for( int i = 0; i < 3; i++ )
		{
			c.ambGround[i] = mixf( c.ambGround[i], ag, 0.55f );
			c.ambSky[i]    = mixf( c.ambSky[i],    as, 0.45f );
		}
	}
	else if( w == 2 )   // P3 iter5 SNOW: cool the AMBIENT/BASE too (not only the direct-light tint).
	{
		// Shift the ambient chroma toward blue-white at ~constant luminance (coolAmb mean ~1.0 => the
		// snow deck goes COLDER without dimming -- the warm-ish dayS base was leaving it mid-grey).
		const float coolAmb[3] = { 0.90f, 1.00f, 1.16f };
		float ag = ( c.ambGround[0] + c.ambGround[1] + c.ambGround[2] ) / 3.0f;
		float as = ( c.ambSky[0]    + c.ambSky[1]    + c.ambSky[2] )    / 3.0f;
		for( int i = 0; i < 3; i++ )
		{
			c.ambGround[i] = mixf( c.ambGround[i], ag * coolAmb[i], 0.45f );
			c.ambSky[i]    = mixf( c.ambSky[i],    as * coolAmb[i], 0.45f );
		}
	}
}
#endif   // retired weather-preset machinery

// A1: cvar-gated per-stage glGetError checkpoint for the cloud frame path. When csz_gl_debug is OFF
// (default) it is a single cvar compare + early return -- it does NOT call glGetError, so it never
// drains an error the surrounding code is responsible for, and adds ~zero cost. When ON it drains +
// logs every pending GL error tagged with `tag`, so the main window's TEST agent can localize exactly
// which cloud-pass GL op (or which prior stage) leaves the residual 0x502. Returns the first error.
GLenum CloudGlCheck( const char *tag )
{
	if( ReadCvar( s_cvGlDebug, 0.0f ) < 0.5f )
		return GL_NO_ERROR;
	GLenum first = GL_NO_ERROR, e; int n = 0;
	while( ( e = glGetError() ) != GL_NO_ERROR && n < 8 )
	{
		if( first == GL_NO_ERROR ) first = e;
		CSZ_LogError( "cloudglcheck", "[csz_gl_debug] GL error 0x%x at %s (frame=%u)", (unsigned)e, tag, s_frame );
		n++;
	}
	return first;
}

}  // anonymous namespace

CloudVolRenderer g_cloudvol;

void CloudVolRenderer::RegisterCvars()
{
	RegisterCvarsImpl();
}

void CloudVolRenderer::Shutdown()
{
	bool live = ( s_tgt.gpuGeneration == GpuGeneration() );
	if( live ) DestroyTargetSameContext(); else ForgetTarget();

	if( s_gpu.gpuGeneration == GpuGeneration() )
	{
		if( s_gpu.vao )              glDeleteVertexArrays( 1, &s_gpu.vao );
		if( s_gpu.march.program )    DestroyProgram( s_gpu.march );
		if( s_gpu.upsample.program ) DestroyProgram( s_gpu.upsample );
		if( s_gpu.built )            glDeleteQueries( kRing, s_gpu.query );
		if( s_gpu.base3d )           glDeleteTextures( 1, &s_gpu.base3d );
		if( s_gpu.detail3d )         glDeleteTextures( 1, &s_gpu.detail3d );
	}
	s_gpu.vao = 0; s_gpu.march.program = 0; s_gpu.upsample.program = 0;
	s_gpu.base3d = 0; s_gpu.detail3d = 0; s_gpu.baked = false;
	s_gpu.built = false;
	s_anchored = false;
	ForgetGpuTimers();
}

// =============================================================================
// CONTRIBUTE -- quarter-res WORLD-SPACE depth-bounded ray-box march -> bilinear
// upsample -> premultiplied composite into the HDR scene FBO at the kTmVolume seam.
// csz_clouds 0 early-outs on the first line (production byte-identical). Full GL state
// guard + pre/post GL-error attribution + GPU timer wrapping only the cloud work.
// =============================================================================
void CloudVolRenderer::Contribute( const ViewSetup &view )
{
	RegisterCvarsImpl();

	// --- master early-out: production byte-identical when off ------------------------
	if( ReadCvar( s_cvMaster, 0.0f ) < 0.5f )
		return;

	// --- gate: need the HDR path (RGBA16F target to composite into) + sampleable scene
	//     depth (terrain occlusion bound). A pure no-op otherwise. --------------------
	if( !SkyComposeActive() )
		return;
	GLuint depthTex = SkyComposeDepthTex();
	GLuint hdrFbo   = SkyComposeHdrFbo();
	if( depthTex == 0 || hdrFbo == 0 )
		return;

	BuildPrograms();
	if( !s_gpu.built )
		return;
	RunTmuProbe();
	if( !s_tmuOk )
		return;
	if( !EnsureBake() )
		return;

	s_frame++;
	int perf = (int)( ReadCvar( s_cvPerf, 0.0f ) + 0.5f );

	// --- resolved parameters --------------------------------------------------------
	int res = clampi( (int)( ReadCvar( s_cvRes, 4.0f ) + 0.5f ), 1, 8 );
	int tod = clampi( (int)( ReadCvar( s_cvTod, 0.0f ) + 0.5f ), 0, 3 );

	// REBUILD v2: weather 0 (scattered cumulus) is the ONLY path; the preset writeback + rain/snow
	// overcast branches were retired. `weather` is pinned 0 so every weather-gated term below takes
	// the clear-NORMAL path (the dead u_weatherKind/overcast uniforms are skipped via their -1 locs).
	const int weather = 0;

	// time-of-day: live (engine phase + ambience nightness) or forced for capture.
	// P1: each tod also sets a per-tod light ELEVATION/AZIMUTH (todElev/todAzim) so day/sunset/
	// night light a DISTINCT, camera-facing side => the lit side MOVES with tod (the old frozen
	// 35/180 never moved it). tod 0 follows the map sun (-1 => use the skymath dir already in
	// cel.dir). A user csz_clouds_sun_elev/azim >=0 still hard-overrides this below.
	float phase, nightness, todElev, todAzim;
	switch( tod )
	{
		case 1:  phase = 0.92f; nightness = 0.0f;  todElev = 58.0f; todAzim = 180.0f; break;  // day: high sun, top-lit caps face camera
		case 2:  phase = 0.02f; nightness = 0.12f; todElev = 14.0f; todAzim = 165.0f; break;  // sunset: low warm sidelight
		case 3:  phase = 0.50f; nightness = 1.0f;  todElev = 42.0f; todAzim = 200.0f; break;  // full-moon night: moon mid-high, opposite side
		default: phase = g_sky.ComputePhase(); nightness = clampf( view.ambience.nightness, 0.0f, 1.0f ); todElev = -1.0f; todAzim = -1.0f; break;  // follow map sun
	}
	// --- LOOK hot params: read live + clamp; logged on change (no per-frame spam). These are
	//     the swept-without-rebuild controls. The defaults are the iter-1 STRUCTURE-first target.
	float coverage    = clampf( ReadCvar( s_cvCoverage,    0.50f  ), 0.0f,    1.0f    );
	float density     = clampf( ReadCvar( s_cvDensity,     1.4f   ), 0.05f,   6.0f    );
	float sigmaT      = clampf( ReadCvar( s_cvSigma,       0.0035f), 0.0005f, 0.05f   );
	float baseScale   = clampf( ReadCvar( s_cvBaseScale,   8000.0f), 500.0f,  20000.0f);
	float detailScale = clampf( ReadCvar( s_cvDetailScale, 1200.0f), 50.0f,   4000.0f );
	float detailAmt   = clampf( ReadCvar( s_cvDetail,      0.30f  ), 0.0f,    1.0f    );
	float hBase       = clampf( ReadCvar( s_cvHBase,       0.12f  ), 0.01f,   0.6f    );  // DEPRECATED (v3)
	float hTop        = clampf( ReadCvar( s_cvHTop,        0.5f   ), 0.15f,   0.99f   );  // DEPRECATED (v3)
	float falloff     = clampf( ReadCvar( s_cvFalloff,     0.12f  ), 0.0f,    0.9f    );
	float silver      = clampf( ReadCvar( s_cvSilver,      0.6f   ), 0.0f,    8.0f    );
	float silverWidth = clampf( ReadCvar( s_cvSilverWidth, 2.0f   ), 0.05f,   8.0f    );
	float powder      = clampf( ReadCvar( s_cvPowder,      0.4f   ), 0.0f,    3.0f    );
	float ambientMul  = clampf( ReadCvar( s_cvAmbient,     0.9f   ), 0.0f,    4.0f    );
	float sunI        = clampf( ReadCvar( s_cvSun,         3.2f   ), 0.0f,    12.0f   );
	float moonI       = clampf( ReadCvar( s_cvMoon,        1.9f   ), 0.0f,    12.0f   );
	float moonTint    = clampf( ReadCvar( s_cvMoonTint,    1.0f   ), 0.0f,    2.0f    );
	// STRUCTURE hot params (iter-3): stacked turrets / multi-octave valleys / self-shadow pockets /
	// irregular base / ragged crown. Read live + clamp; folded into the LOOK change-log below.
	float billow      = clampf( ReadCvar( s_cvBillow,      0.10f  ), 0.0f,    1.0f    );  // DEPRECATED (v3)
	float erodeDepth  = clampf( ReadCvar( s_cvErodeDepth,  0.35f  ), 0.0f,    3.0f    );
	int   erodeOct    = clampi( (int)( ReadCvar( s_cvErodeOct, 3.0f ) + 0.5f ), 1, 3 );
	float selfShadow  = clampf( ReadCvar( s_cvSelfShadow,  0.9f   ), 0.0f,    5.0f    );
	float baseIrreg   = clampf( ReadCvar( s_cvBaseIrreg,   0.6f   ), 0.0f,    1.0f    );  // DEPRECATED (v3)
	float towerVar    = clampf( ReadCvar( s_cvTowerVar,    0.28f  ), 0.0f,    0.6f    );  // DEPRECATED (v3)
	float envWarp     = clampf( ReadCvar( s_cvEnvWarp,     0.12f  ), 0.0f,    0.6f    );
	// STORM-CELL hot params (v4): mid-freq cauliflower + direct-sun tonal range + virga toggle.
	float midAmt      = clampf( ReadCvar( s_cvMid,         0.45f  ), 0.0f,    1.0f    );
	float midScale    = clampf( ReadCvar( s_cvMidScale,    2200.0f), 800.0f,  6000.0f );
	float virga       = clampf( ReadCvar( s_cvVirga,       0.0f   ), 0.0f,    1.0f    );
	float sunFwd      = clampf( ReadCvar( s_cvSunFwd,      1.6f   ), 0.0f,    5.0f    );
	float sunG        = clampf( ReadCvar( s_cvSunG,        0.82f  ), 0.0f,    0.95f   );
	// CAP-LIGHT / BASE hot params (codex compare2): broad sun-facing cap faces + base shelf/mammatus.
	float capLight    = clampf( ReadCvar( s_cvCapLight,    1.0f   ), 0.0f,    4.0f    );
	float shelf       = clampf( ReadCvar( s_cvShelf,       1.0f   ), 0.1f,    4.0f    );
	float mammatus    = clampf( ReadCvar( s_cvMammatus,    0.35f  ), 0.0f,    1.0f    );
	// LAYER + ANIMATION hot params (v5 PIVOT): cloud-deck placement + wind drift + volume evolve.
	float layerHeight = clampf( ReadCvar( s_cvLayerHeight, 2600.0f ), 200.0f, 12000.0f );
	float layerThick  = clampf( ReadCvar( s_cvLayerThick,  850.0f  ), 120.0f, 6000.0f  );
	float windDirDeg  = ReadCvar( s_cvWindDir,   45.0f );
	float windSpeed   = clampf( ReadCvar( s_cvWindSpeed, 60.0f ), 0.0f, 600.0f );
	float evolveRate  = clampf( ReadCvar( s_cvEvolve,    35.0f ), 0.0f, 400.0f );
	float windAz      = windDirDeg * kDegToRad;
	float windVec[3]  = { cosf( windAz ) * windSpeed, sinf( windAz ) * windSpeed, 0.0f };
	float midFreq     = 1.0f / midScale;
	// PATH A (macro cloud distribution): coverage-FIELD freq/contrast/drift + base de-tile (all hot).
	float covScale    = clampf( ReadCvar( s_cvCovScale,    42000.0f ), 8000.0f, 120000.0f );
	float covFreq     = 1.0f / covScale;
	float covContrast = clampf( ReadCvar( s_cvCovContrast, 0.85f ), 0.0f, 2.0f );
	float covDrift    = clampf( ReadCvar( s_cvCovDrift,    0.0f  ), 0.0f, 2.0f );
	float detile      = clampf( ReadCvar( s_cvDetile,      0.5f  ), 0.0f, 0.5f );
	// REBUILD v2 NEW live params: the single nightness luminance authority + the half-res sharpness
	// pipeline (ESS fine divisor; joint-bilateral depth/alpha edge-stops + CAS sharpen on the upscale).
	float kMoonLum    = clampf( ReadCvar( s_cvNightLum,    0.06f ), 0.01f, 0.5f );
	float nightLum    = mixf( 1.0f, kMoonLum, nightness );   // mix(1, ~0.06, nightness); dims TOTAL cloud radiance (direct+ambient+rim) before the day soft-knee
	float fineDiv     = clampf( ReadCvar( s_cvFineDiv,     4.0f  ), 2.0f, 8.0f );
	float casAmount   = clampf( ReadCvar( s_cvCas,         0.5f  ), 0.0f, 1.0f );
	float depthSigma  = clampf( ReadCvar( s_cvDepthSigma,  0.01f ), 0.0f, 1000.0f );
	float alphaSigma  = clampf( ReadCvar( s_cvAlphaSigma,  8.0f  ), 0.0f, 1000.0f );
	// CLOUD-ONLY sun-direction override: -1 = follow the tod/skymath sun (production unchanged).
	// elev>=0 rebuilds the cloud light dir from (elev,azim); azim<0 falls back to a fixed azimuth.
	float sunElevOvr  = ReadCvar( s_cvSunElev, -1.0f );
	float sunAzimOvr  = ReadCvar( s_cvSunAzim, -1.0f );
	if( hTop <= hBase + 0.05f )                 // keep a body between base taper and top round
		hTop = clampf( hBase + 0.05f, 0.15f, 0.99f );
	float baseFreq    = 1.0f / baseScale;
	float detailFreq  = 1.0f / detailScale;
	{
		static bool  s_lookLogged = false;
		static float s_last[30] = { 0 };
		float cur[30] = { coverage, density, sigmaT, baseScale, detailScale, detailAmt,
		                  hBase, hTop, falloff, silver, silverWidth, powder, ambientMul, sunI, moonI, moonTint,
		                  sunElevOvr, sunAzimOvr,
		                  billow, erodeDepth, (float)erodeOct, selfShadow, baseIrreg, towerVar, envWarp,
		                  midAmt, midScale, virga, sunFwd, sunG };
		bool changed = !s_lookLogged;
		for( int i = 0; i < 30 && !changed; i++ ) if( s_last[i] != cur[i] ) changed = true;
		if( changed )
		{
			CSZ_LogInfo( "cloudvol",
				"[csz_clouds] LOOK resolved: cov=%.2f dens=%.2f sigma=%.4f baseScale=%.0f detScale=%.0f detail=%.2f hBase=%.2f hTop=%.2f falloff=%.2f silver=%.2f silverW=%.2f powder=%.2f amb=%.2f sun=%.2f moon=%.2f moonTint=%.2f sunElevOvr=%.1f sunAzimOvr=%.1f | STRUCT billow=%.2f erodeDepth=%.2f erodeOct=%d selfShadow=%.2f baseIrreg=%.2f towerVar=%.2f envWarp=%.2f | STORM mid=%.2f midScale=%.0f virga=%.2f sunFwd=%.2f sunG=%.2f",
				coverage, density, sigmaT, baseScale, detailScale, detailAmt, hBase, hTop, falloff, silver, silverWidth, powder, ambientMul, sunI, moonI, moonTint, sunElevOvr, sunAzimOvr,
				billow, erodeDepth, erodeOct, selfShadow, baseIrreg, towerVar, envWarp,
				midAmt, midScale, virga, sunFwd, sunG );
			for( int i = 0; i < 30; i++ ) s_last[i] = cur[i];
			s_lookLogged = true;
		}
	}

	CelLight cel;
	// N2: hand DeriveCelestial the REAL published moon so the night clouds are lit BY the moon (dir +
	// cool chroma), not the sun antipode. Only pass them when the scene actually has a moon/moonlight
	// up (else null => fall back to -sun / fixed cool-white). todElev/todAzim stay -1 at tod 0 (live)
	// so no synthetic ElevYawDir override fires and moonlightDir flows straight through to cel.dir.
	const float *moonDirReal = view.ambience.moonlightEnabled ? view.ambience.moonlightDir : nullptr;
	const float *moonColReal = view.ambience.moonEnabled      ? view.ambience.moonColor    : nullptr;
	DeriveCelestial( phase, nightness, sunI, moonI, ambientMul, moonTint, moonDirReal, moonColReal, cel );

	// --- CLOUD-ONLY sun-direction override (art/debug, the key missing lever) -----------
	// When sun_elev>=0, rebuild ONLY this cloud pass's light DIRECTION from (elev,azim) using
	// the SAME skymath elev/yaw->unit-dir convention the sky uses (Z-up, pointing TOWARD the
	// body). This does NOT touch g_sky, the engine sun, or any other system -- it only steers
	// where THIS cloud is lit from, so the lit cauliflower tops can face up/toward the camera
	// instead of being backlit by a near-horizon golden-hour sun. The light COLOR/intensity and
	// ambient are unchanged (still the tod-derived warm sun / cool moon). azim<0 => a fixed
	// default azimuth (the skymath east rise node).
	// P1: user cvar override wins (>=0); otherwise fall back to the per-tod elevation/azimuth so the
	// cloud is lit from a DISTINCT, camera-facing side per tod (the lit side MOVES with tod). When
	// elevSel<0 (tod 0 with no user override) cel.dir is left at the skymath map-sun direction.
	float elevSel = ( sunElevOvr >= 0.0f ) ? sunElevOvr : todElev;
	float azimSel = ( sunAzimOvr >= 0.0f ) ? sunAzimOvr : todAzim;
	if( elevSel >= 0.0f )
	{
		float elev = clampf( elevSel, 0.0f, 90.0f );
		float azim = ( azimSel < 0.0f ) ? skymath::kNodeYawDeg : azimSel;
		skymath::ElevYawDir( elev, azim, cel.dir );
	}

	// REBUILD v2: weather-chroma tint (rain/snow) retired; NORMAL keeps the warm sun + blue-sky ambient.

	// --- hero AABB volume: latch a world anchor at the player's first-frame position
	//     (per generation), so the box is WORLD-FIXED (real parallax + terrain occlusion
	//     as the player moves) and reliably overhead/mid-range from spawn. -------------
	if( !s_anchored )
	{
		s_anchor[0] = view.origin[0]; s_anchor[1] = view.origin[1]; s_anchor[2] = view.origin[2];
		// latch the spawn-frame horizontal view-forward so the nearbox knob can place the hero
		// mass directly in front of wherever the (often clamped/headless) camera first looks.
		float lf[3], lr[3], lu[3];
		AngleVectors( view.angles, lf, lr, lu );
		float fl = sqrtf( lf[0] * lf[0] + lf[1] * lf[1] );
		if( fl > 1.0e-3f ) { s_anchorFwd[0] = lf[0] / fl; s_anchorFwd[1] = lf[1] / fl; }
		else               { s_anchorFwd[0] = 1.0f;       s_anchorFwd[1] = 0.0f; }
		s_anchored = true;
		CSZ_LogInfo( "cloudvol", "[csz_clouds] hero-box anchor latched at (%.0f %.0f %.0f) fwd(%.2f %.2f)",
			s_anchor[0], s_anchor[1], s_anchor[2], s_anchorFwd[0], s_anchorFwd[1] );
	}

	int dbgNear = clampi( (int)( ReadCvar( s_cvDbgNear, 0.0f ) + 0.5f ), 0, 1 );
	float boxMin[3], boxMax[3];
	if( dbgNear >= 1 )
	{
		// TEST-ONLY judgeability placement (csz_clouds_dbg_nearbox 1): a large cumulus mass at an
		// ABSOLUTE WORLD center (box_x,box_y,box_z) with half-extent box_radius, IGNORING the
		// spawn-relative offset (the prior spawn-forward placement aimed the box into a WALL). This
		// lets a capture pass drop the cloud wherever the fixed sky-vantage camera is already looking
		// -- no freecam/setpos needed. Coords clamped to the engine world box; radius to a sane span.
		// Density/coverage/lighting are UNCHANGED -- placement + footprint only.
		const float kWorldLim = 16384.0f;                   // GoldSrc engine world half-extent
		float cx = clampf( ReadCvar( s_cvDbgBoxX,   150.0f  ), -kWorldLim, kWorldLim );
		float cy = clampf( ReadCvar( s_cvDbgBoxY,   2850.0f ), -kWorldLim, kWorldLim );
		float cz = clampf( ReadCvar( s_cvDbgBoxZ,   1200.0f ), -kWorldLim, kWorldLim );
		float rad = clampf( ReadCvar( s_cvDbgBoxRad, 1500.0f ), 16.0f, 8000.0f );
		// Vertical (Z) half-extent decoupled from the X/Y footprint so the storm builds UP
		// (taller than wide). <0 = legacy cube (use rad on all axes). Clamped to a sane span.
		float zrad = ReadCvar( s_cvDbgBoxZRad, -1.0f );
		zrad = ( zrad < 0.0f ) ? rad : clampf( zrad, 16.0f, 12000.0f );
		boxMin[0] = cx - rad; boxMin[1] = cy - rad; boxMin[2] = cz - zrad;
		boxMax[0] = cx + rad; boxMax[1] = cy + rad; boxMax[2] = cz + zrad;

		// Log the resolved AABB once on enable, and again whenever the box is retuned via cvar mid-run,
		// so a capture pass can confirm placement from engine.log without spamming every frame.
		static bool  s_dbgLogged = false;
		static float s_dbgLast[6] = { 0, 0, 0, 0, 0, 0 };
		bool changed = !s_dbgLogged ||
			s_dbgLast[0] != boxMin[0] || s_dbgLast[1] != boxMin[1] || s_dbgLast[2] != boxMin[2] ||
			s_dbgLast[3] != boxMax[0] || s_dbgLast[4] != boxMax[1] || s_dbgLast[5] != boxMax[2];
		if( changed )
		{
			CSZ_LogInfo( "cloudvol",
				"[csz_clouds] dbg_nearbox 1: ABSOLUTE-WORLD hero box center(%.0f %.0f %.0f) radius=%.0f -> AABB min(%.0f %.0f %.0f) max(%.0f %.0f %.0f)",
				cx, cy, cz, rad, boxMin[0], boxMin[1], boxMin[2], boxMax[0], boxMax[1], boxMax[2] );
			s_dbgLast[0] = boxMin[0]; s_dbgLast[1] = boxMin[1]; s_dbgLast[2] = boxMin[2];
			s_dbgLast[3] = boxMax[0]; s_dbgLast[4] = boxMax[1]; s_dbgLast[5] = boxMax[2];
			s_dbgLogged = true;
		}
	}
	else
	{
		// WORLD-SPACE CLOUD DECK (v5 PIVOT, the DEFAULT csz_clouds 1 render): a thin horizontal
		// slab at cloud altitude spanning the whole sky. XY extent is huge and CAMERA-CENTERED so
		// the deck always fills the sky AND its XY faces sit far beyond u_marchFar (= never hit =>
		// no box edge); the density noise is sampled in WORLD space (only TIME drifts it) so the
		// deck is world-anchored with correct parallax. Z (Quake-up) = [anchor.z + layerHeight,
		// + layerThick]; density -> 0 at both Z faces via the height gradient, so top/bottom of the
		// slab are never a visible edge either. Vertical altitude is world-fixed (anchor.z based);
		// only XY follows the player. NO single dbg box -- the layer is the sky.
		const float kHalfXY = 60000.0f;   // >> the layer u_marchFar (45000): XY walls are never reached
		float baseZ = s_anchor[2] + layerHeight;
		boxMin[0] = view.origin[0] - kHalfXY; boxMin[1] = view.origin[1] - kHalfXY; boxMin[2] = baseZ;
		boxMax[0] = view.origin[0] + kHalfXY; boxMax[1] = view.origin[1] + kHalfXY; boxMax[2] = baseZ + layerThick;
	}

	// P2 FIX: world-space epsilon for the broad cap-light density-gradient normal. It MUST be a
	// CLOUD-FEATURE scale (turret/lobe face), NOT the huge XY march-bound extent: the old
	// `largest dim * 0.012` gave ~1440u in LAYER mode (XY=120000) = ~ the WHOLE deck, so the Z
	// gradient tap fell OUTSIDE the slab and the cap normal had no vertical component (no lit
	// top / shadowed underside). Derive it from the SMALLEST box dimension (the deck thickness in
	// LAYER mode) and clamp to a sane feature-scale band so the central differences resolve real
	// face orientation within the deck.
	float boxMinExt = boxMax[0] - boxMin[0];
	if( boxMax[1] - boxMin[1] < boxMinExt ) boxMinExt = boxMax[1] - boxMin[1];
	if( boxMax[2] - boxMin[2] < boxMinExt ) boxMinExt = boxMax[2] - boxMin[2];
	float capEps = clampf( boxMinExt * 0.20f, 60.0f, 400.0f );

	// --- march constants (NOT look cvars: cost/quality knobs, fixed this phase) -----------
	// iter-3: longer cone reach + more taps so the self-shadow spans whole turrets and lands
	// real shadow in the valleys between stacked lobes (paired with the u_selfShadow weight).
	const float lightReach = 3200.0f;          // cone self-shadow over several feature-diameters
	// LAYER mode marches a thin deck toward the horizon. The view step COUNT is ADAPTIVE (the shader
	// derives it from stepLenMax, floored at `steps`, capped at MAX_STEPS) so grazing rays stay
	// sampled without the fixed-940u/step horizon singularity. R3 (iter4): far cap 33000->22000 (the
	// deck is sub-pixel well before 22000u at gameplay altitude AND the R2 elevation fade now hides
	// the far horizon) + stepLenMax 400->220 (resolve the mid/detail cells at grazing) + MAX_STEPS
	// 80->128 (shader). Worst case (grazing): ceil(22000/220)=100 -> <=128, ~220u/step, bounded. The
	// R1 per-step dither + domain-warp convert any residual coarse-step banding into noise.
	// PERF iter5 (STAGED, codex): marchFar 22000->20000, stepLenMax 220->260 (NOT 300/18000 yet).
	// Worst case grazing: ceil(20000/260)=77 steps (<=128). The R1 per-step dither + domain-warp are
	// step-count-INDEPENDENT, so they keep carrying the anti-fan at the coarser step. marchFar also
	// feeds the shader far-fade at 0.70*u_marchFar (inl) -- kept coherent (reads u_marchFar). A capture
	// must verify worst-case cloud_pass_ms (<5ms) + R5 mode1/mode3 show NO fan/rib return before any
	// further coarsening (260/20000 -> 300/18000 is a later capture-gated pass).
	const float marchFar   = ( dbgNear >= 1 ) ? 12000.0f : 20000.0f;
	const int   steps      = 48;               // P0: now the MIN COARSE step count (quality floor for steep / short rays)
	const float stepLenMax = 260.0f;           // R3/iter5: target MAX world-space COARSE step length (= dtCoarse; ESS fine = dtCoarse/fineDiv)
	const int   lightSteps = 6;                // REBUILD: 6 EXPONENTIALLY-spaced cone taps (was 8 uniform) -- same shadow quality, cheaper inner loop
	// iter4 grazing-angle knobs (read live). R1 domain-warp amplitude = toggle ? amp : 0 (0 => the
	// shader disables the warp, for capture A/B against the per-step dither). R2 elevation fade band.
	const bool  domainWarpOn  = ( ReadCvar( s_cvDomainWarp, 1.0f ) >= 0.5f );
	const float domainWarpAmp = clampf( ReadCvar( s_cvDomainWarpAmp, 220.0f ), 0.0f, 2000.0f );
	const float domainWarp    = domainWarpOn ? domainWarpAmp : 0.0f;
	const float horizonFadeLo = clampf( ReadCvar( s_cvHorizonFadeLo, 0.01f ), -1.0f, 1.0f );
	const float horizonFadeHi = clampf( ReadCvar( s_cvHorizonFadeHi, 0.07f ), horizonFadeLo + 1e-3f, 1.0f );
	// REBUILD v2: half-res shrank the grazing fan, so the horizon fade is relaxed by default
	// (csz_clouds_horizon_fade_hi 0.07->0.03) to recover the far deck. No weather override now.
	const float hFadeHi = horizonFadeHi;
	const int   dbgMode       = clampi( (int)( ReadCvar( s_cvDbgMode, 0.0f ) + 0.5f ), 0, 6 );
	// P3: overcast skylight floor for skyVis -- applied ONLY for rain/snow (weather != 0) so a dense
	// overcast deck's undersides never collapse to black; clear NORMAL keeps 0 (no contrast flattening).
	const float skyVisFloor = ( weather != 0 ) ? 0.40f : 0.0f;
	// iter5 P1/P2 overcast-structure controls. CPU-gated to 0 for clear NORMAL (belt-and-suspenders
	// alongside the shader's u_weatherKind hard-gate) so weather==0 takes the byte-identical old path.
	const float overcastVar = ( weather != 0 ) ? 0.22f : 0.0f;   // P1: low-freq billow into the coverage field
	const float lowHaze     = ( weather != 0 ) ? 0.85f : 0.0f;   // P4-FIX iter6: 0.6->0.85 cool the residual warm engine-sky strip under the overcast deck; weather==0 stays 0 (day/w0 horizon = engine-sky scope, untouched). P2-orig: cold low-band horizon haze amplitude

	float t = fmodf( ClientTime(), 3600.0f );
	float frame = (float)( s_frame & 1023u );

	// inverse view-proj for depth->world reconstruction (terrain occlusion bound).
	Mat4 invViewProj;
	if( !Mat4Inverse( view.matViewProj, invViewProj ) )
		return;

	// camera basis (Quake Z-up), right/up pre-scaled by the half-FOV tangents.
	float fwd[3], right[3], up[3];
	AngleVectors( view.angles, fwd, right, up );
	float tanX = tanf( view.fovX * kDegToRad * 0.5f );
	float tanY = tanf( view.fovY * kDegToRad * 0.5f );
	float rightS[3] = { right[0] * tanX, right[1] * tanX, right[2] * tanX };
	float upS[3]    = { up[0] * tanY,    up[1] * tanY,    up[2] * tanY };

	// --- viewport / target sizing ---------------------------------------------------
	int fullW = view.viewport[2]; if( fullW < 1 ) fullW = 1;
	int fullH = view.viewport[3]; if( fullH < 1 ) fullH = 1;
	int qW = fullW / res; if( qW < 1 ) qW = 1;
	int qH = fullH / res; if( qH < 1 ) qH = 1;
	if( !EnsureTarget( qW, qH ) )
		return;

	// ================================================================================
	// FULL STATE GUARD -- snapshot every state the two-pass detour perturbs, restore on
	// exit so the following transparent/viewmodel passes are unaffected. (Mirrors the
	// volcloud / fog-volume guards.)
	// ================================================================================
	GLint  prevFbo = 0;          glGetIntegerv( GL_FRAMEBUFFER_BINDING, &prevFbo );
	GLint  prevViewport[4];      glGetIntegerv( GL_VIEWPORT, prevViewport );
	GLint  prevColorMask[4];     glGetIntegerv( GL_COLOR_WRITEMASK, prevColorMask );
	GLboolean prevScissor = glIsEnabled( GL_SCISSOR_TEST );
	GLboolean prevSrgb    = glIsEnabled( GL_FRAMEBUFFER_SRGB );
	GLboolean prevDepthTest = glIsEnabled( GL_DEPTH_TEST );
	GLboolean prevBlend     = glIsEnabled( GL_BLEND );
	GLboolean prevCull      = glIsEnabled( GL_CULL_FACE );
	GLint     prevDepthMask = GL_TRUE; glGetIntegerv( GL_DEPTH_WRITEMASK, &prevDepthMask );

	// --- pre-pass GL-error drain + attribution marker -------------------------------
	// A1/A2: ALWAYS drain so the cloud pass starts from a clean GL state, but only LOG under
	// csz_gl_debug (NOT perf -- the old `perf>0` gate flooded -log with one line/frame whenever the
	// GPU timer was on). Honest wording: an error PRESENT at cloud-pass entry was generated by an
	// EARLIER stage THIS or last frame (the cloud post-pass check below proves the cloud draws add
	// none); the prior "predates the cloud pass" phrasing falsely implied stock-engine origin. The
	// most likely generator is the engine's compat-profile GL_SelectTexture under -glcore in
	// LeaveTakeover / the 2D-HUD path (see csz_sky_compose.cpp RestoreTmus notes); run csz_gl_debug
	// with the per-stage CloudGlCheck()s to localize it precisely.
	{
		bool logDrain = ( ReadCvar( s_cvGlDebug, 0.0f ) >= 0.5f );
		GLenum e; int drained = 0;
		while( ( e = glGetError() ) != GL_NO_ERROR && drained < 8 )
		{
			if( logDrain )
				CSZ_LogError( "cloudglcheck", "[csz_gl_debug] GL error 0x%x present at cloud-pass entry (generated by an earlier stage; drained)", (unsigned)e );
			drained++;
		}
	}

	bool timerOn = ( perf > 0 );
	// GL_TIME_ELAPSED cannot NEST: skip our timer if the compose-span timer is active.
	if( timerOn && SkyComposePerfDumpEnabled() )
	{
		static bool warned = false;
		if( !warned ) { warned = true; CSZ_LogInfo( "cloudvol", "[csz_clouds] timer SKIPPED: csz_perf_dump active (GL_TIME_ELAPSED cannot nest)" ); }
		timerOn = false;
	}

	int slot = s_gpu.ringHead;
	double readMs = -1.0; unsigned readAge = 0; bool gotSample = false;
	if( timerOn && s_gpu.qInFlight[slot] )
	{
		GLint avail = 0;
		glGetQueryObjectiv( s_gpu.query[slot], GL_QUERY_RESULT_AVAILABLE, &avail );
		if( avail )
		{
			GLuint64 ns = 0;
			glGetQueryObjectui64v( s_gpu.query[slot], GL_QUERY_RESULT, &ns );
			readMs = (double)ns / 1.0e6;
			readAge = s_frame - s_gpu.qFrame[slot];
			s_gpu.qInFlight[slot] = false;
			gotSample = true;
		}
		else
		{
			timerOn = false;   // slot still busy: skip issuing this frame (don't clobber)
		}
	}
	bool issuing = ( timerOn && !s_gpu.qInFlight[slot] );
	if( issuing )
		glBeginQuery( GL_TIME_ELAPSED, s_gpu.query[slot] );

	// ================================ Pass 1: march =================================
	BindFbo( s_tgt.fbo );
	glViewport( 0, 0, qW, qH );
	glDisable( GL_FRAMEBUFFER_SRGB );
	glDisable( GL_SCISSOR_TEST );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendNone );
	SetCull( false );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	UseProgram( s_gpu.march.program );
	BindVao( s_gpu.vao );
	// scene depth on sky unit 0, base 3D on unit 1, detail 3D on unit 2.
	SkyComposeBindTex( 0, GL_TEXTURE_2D, depthTex );
	SkyComposeBindTex( 1, GL_TEXTURE_3D, s_gpu.base3d );
	SkyComposeBindTex( 2, GL_TEXTURE_3D, s_gpu.detail3d );

	float fTarget[2] = { (float)qW, (float)qH };
	if( s_gpu.mCamFwd >= 0 )      glUniform3fv( s_gpu.mCamFwd, 1, fwd );
	if( s_gpu.mCamRight >= 0 )    glUniform3fv( s_gpu.mCamRight, 1, rightS );
	if( s_gpu.mCamUp >= 0 )       glUniform3fv( s_gpu.mCamUp, 1, upS );
	if( s_gpu.mCamPos >= 0 )      glUniform3fv( s_gpu.mCamPos, 1, view.origin );
	if( s_gpu.mLightDir >= 0 )    glUniform3fv( s_gpu.mLightDir, 1, cel.dir );
	if( s_gpu.mLightColor >= 0 )  glUniform3fv( s_gpu.mLightColor, 1, cel.color );
	if( s_gpu.mAmbGround >= 0 )   glUniform3fv( s_gpu.mAmbGround, 1, cel.ambGround );
	if( s_gpu.mAmbSky >= 0 )      glUniform3fv( s_gpu.mAmbSky, 1, cel.ambSky );
	if( s_gpu.mBoxMin >= 0 )      glUniform3fv( s_gpu.mBoxMin, 1, boxMin );
	if( s_gpu.mBoxMax >= 0 )      glUniform3fv( s_gpu.mBoxMax, 1, boxMax );
	if( s_gpu.mTime >= 0 )        glUniform1f( s_gpu.mTime, t );
	if( s_gpu.mFrame >= 0 )       glUniform1f( s_gpu.mFrame, frame );
	if( s_gpu.mDensity >= 0 )     glUniform1f( s_gpu.mDensity, density );
	if( s_gpu.mCoverage >= 0 )    glUniform1f( s_gpu.mCoverage, coverage );
	if( s_gpu.mSilver >= 0 )      glUniform1f( s_gpu.mSilver, silver );
	if( s_gpu.mSilverWidth >= 0 ) glUniform1f( s_gpu.mSilverWidth, silverWidth );
	if( s_gpu.mSigmaT >= 0 )      glUniform1f( s_gpu.mSigmaT, sigmaT );
	if( s_gpu.mBaseFreq >= 0 )    glUniform1f( s_gpu.mBaseFreq, baseFreq );
	if( s_gpu.mDetailFreq >= 0 )  glUniform1f( s_gpu.mDetailFreq, detailFreq );
	if( s_gpu.mDetailAmt >= 0 )   glUniform1f( s_gpu.mDetailAmt, detailAmt );
	if( s_gpu.mFalloff >= 0 )     glUniform1f( s_gpu.mFalloff, falloff );
	if( s_gpu.mHBase >= 0 )       glUniform1f( s_gpu.mHBase, hBase );
	if( s_gpu.mHTop >= 0 )        glUniform1f( s_gpu.mHTop, hTop );
	if( s_gpu.mPowder >= 0 )      glUniform1f( s_gpu.mPowder, powder );
	if( s_gpu.mBillow >= 0 )      glUniform1f( s_gpu.mBillow, billow );
	if( s_gpu.mErodeDepth >= 0 )  glUniform1f( s_gpu.mErodeDepth, erodeDepth );
	if( s_gpu.mErodeOct >= 0 )    glUniform1i( s_gpu.mErodeOct, erodeOct );
	if( s_gpu.mSelfShadow >= 0 )  glUniform1f( s_gpu.mSelfShadow, selfShadow );
	if( s_gpu.mBaseIrreg >= 0 )   glUniform1f( s_gpu.mBaseIrreg, baseIrreg );
	if( s_gpu.mTowerVar >= 0 )    glUniform1f( s_gpu.mTowerVar, towerVar );
	if( s_gpu.mEnvWarp >= 0 )     glUniform1f( s_gpu.mEnvWarp, envWarp );
	if( s_gpu.mMid >= 0 )         glUniform1f( s_gpu.mMid, midAmt );
	if( s_gpu.mMidFreq >= 0 )     glUniform1f( s_gpu.mMidFreq, midFreq );
	if( s_gpu.mVirga >= 0 )       glUniform1f( s_gpu.mVirga, virga );
	if( s_gpu.mSunForward >= 0 )  glUniform1f( s_gpu.mSunForward, sunFwd );
	if( s_gpu.mSunG >= 0 )        glUniform1f( s_gpu.mSunG, sunG );
	if( s_gpu.mCapLight >= 0 )    glUniform1f( s_gpu.mCapLight, capLight );
	if( s_gpu.mShelf >= 0 )       glUniform1f( s_gpu.mShelf, shelf );
	if( s_gpu.mMammatus >= 0 )    glUniform1f( s_gpu.mMammatus, mammatus );
	if( s_gpu.mCapEps >= 0 )      glUniform1f( s_gpu.mCapEps, capEps );
	if( s_gpu.mWindVec >= 0 )     glUniform3fv( s_gpu.mWindVec, 1, windVec );
	if( s_gpu.mEvolveRate >= 0 )  glUniform1f( s_gpu.mEvolveRate, evolveRate );
	if( s_gpu.mLightReach >= 0 )  glUniform1f( s_gpu.mLightReach, lightReach );
	if( s_gpu.mMarchFar >= 0 )    glUniform1f( s_gpu.mMarchFar, marchFar );
	if( s_gpu.mStepLenMax >= 0 )  glUniform1f( s_gpu.mStepLenMax, stepLenMax );    // P0
	if( s_gpu.mSkyVisFloor >= 0 ) glUniform1f( s_gpu.mSkyVisFloor, skyVisFloor );  // P3
	if( s_gpu.mDomainWarp >= 0 )    glUniform1f( s_gpu.mDomainWarp, domainWarp );       // R1
	if( s_gpu.mHorizonFadeLo >= 0 ) glUniform1f( s_gpu.mHorizonFadeLo, horizonFadeLo ); // R2
	if( s_gpu.mHorizonFadeHi >= 0 ) glUniform1f( s_gpu.mHorizonFadeHi, hFadeHi );       // R2 / iter5 P2 (overcast => 0.03)
	if( s_gpu.mDbgMode >= 0 )       glUniform1i( s_gpu.mDbgMode, dbgMode );             // R5
	if( s_gpu.mWeatherKind >= 0 )   glUniform1i( s_gpu.mWeatherKind, weather );         // iter5 hard-gate
	if( s_gpu.mOvercastVar >= 0 )   glUniform1f( s_gpu.mOvercastVar, overcastVar );     // iter5 P1
	if( s_gpu.mLowHaze >= 0 )       glUniform1f( s_gpu.mLowHaze, lowHaze );             // iter5 P2
	if( s_gpu.mCovFreq >= 0 )       glUniform1f( s_gpu.mCovFreq, covFreq );            // PATH A coverage field
	if( s_gpu.mCovContrast >= 0 )   glUniform1f( s_gpu.mCovContrast, covContrast );    // PATH A
	if( s_gpu.mCovDrift >= 0 )      glUniform1f( s_gpu.mCovDrift, covDrift );          // PATH A
	if( s_gpu.mDetile >= 0 )        glUniform1f( s_gpu.mDetile, detile );              // CHANGE 2 base de-tile
	if( s_gpu.mNightLum >= 0 )      glUniform1f( s_gpu.mNightLum, nightLum );          // REBUILD: night luminance authority
	if( s_gpu.mFineDiv >= 0 )       glUniform1f( s_gpu.mFineDiv, fineDiv );            // REBUILD: ESS fine-step divisor
	if( s_gpu.mTargetSize >= 0 )  glUniform2fv( s_gpu.mTargetSize, 1, fTarget );
	if( s_gpu.mSteps >= 0 )       glUniform1i( s_gpu.mSteps, steps );
	if( s_gpu.mLightSteps >= 0 )  glUniform1i( s_gpu.mLightSteps, lightSteps );
	if( s_gpu.mZNear >= 0 )       glUniform1f( s_gpu.mZNear, view.zNear );
	if( s_gpu.mZFar >= 0 )        glUniform1f( s_gpu.mZFar, view.zFar );
	if( s_gpu.mInvViewProj >= 0 ) glUniformMatrix4fv( s_gpu.mInvViewProj, 1, GL_FALSE, invViewProj.m );
	if( s_gpu.mDepthTex >= 0 )    glUniform1i( s_gpu.mDepthTex, kSkyTmuBase + 0 );
	if( s_gpu.mBase3d >= 0 )      glUniform1i( s_gpu.mBase3d, kSkyTmuBase + 1 );
	if( s_gpu.mDetail3d >= 0 )    glUniform1i( s_gpu.mDetail3d, kSkyTmuBase + 2 );

	CloudGlCheck( "march uniforms (pre-draw)" );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	CloudGlCheck( "march glDrawArrays" );
	BindVao( 0 );
	SkyComposeRestoreTmus();
	CloudGlCheck( "march SkyComposeRestoreTmus" );

	// ============== Pass 2: JOINT BILATERAL upsample + CAS sharpen + premultiplied composite ====
	// REBUILD v2: replaces the plain bilinear tap with a depth-AND-alpha joint bilateral (clouds do
	// NOT write depth, so the cloud-ALPHA edge-stop is what sharpens the dominant cloud-vs-sky
	// silhouette; the scene-DEPTH edge-stop sharpens cloud-vs-terrain) + a clamped CAS sharpen.
	BindFbo( hdrFbo );
	glViewport( view.viewport[0], view.viewport[1], view.viewport[2], view.viewport[3] );
	SetDepthTest( false );
	SetDepthWrite( false );
	SetBlend( kBlendPremulOver );   // dst = src.rgb + dst*(1-src.a) -- premultiplied over
	SetCull( false );

	CloudGlCheck( "upsample BindFbo+state (hdrFbo)" );
	UseProgram( s_gpu.upsample.program );
	BindVao( s_gpu.vao );
	SkyComposeBindTex( 0, GL_TEXTURE_2D, s_tgt.colorTex );   // low-res cloud result on sky unit 0
	SkyComposeBindTex( 1, GL_TEXTURE_2D, depthTex );         // full-res scene depth on sky unit 1
	// NOTE (A1 candidate): depthTex is the hdrFbo's OWN GL_DEPTH_ATTACHMENT (SkyComposeDepthTex ==
	// s_hdr.depthTex), so this draw samples a texture attached to the bound draw FBO -- a read-only
	// feedback loop (depth test+write are OFF, so the attachment is never written). GL 4.x permits an
	// unwritten attachment to be sampled; on a strict 3.3-core driver it is UB and can raise 0x502. The
	// post-pass check below currently reports zero, i.e. THIS driver tolerates it -- the per-stage
	// CloudGlCheck()s confirm at runtime whether any residual originates here vs the engine takeover.
	float fSize[2] = { (float)fullW, (float)fullH };
	if( s_gpu.uCloudTex >= 0 )   glUniform1i( s_gpu.uCloudTex, kSkyTmuBase + 0 );
	if( s_gpu.uFullSize >= 0 )   glUniform2fv( s_gpu.uFullSize, 1, fSize );
	if( s_gpu.uUpDbgMode >= 0 )  glUniform1i( s_gpu.uUpDbgMode, dbgMode );   // R5 mode 6: raw nearest upsample
	if( s_gpu.uDepthTex >= 0 )   glUniform1i( s_gpu.uDepthTex, kSkyTmuBase + 1 );
	if( s_gpu.uZNear >= 0 )      glUniform1f( s_gpu.uZNear, view.zNear );
	if( s_gpu.uZFar >= 0 )       glUniform1f( s_gpu.uZFar, view.zFar );
	if( s_gpu.uDepthSigma >= 0 ) glUniform1f( s_gpu.uDepthSigma, depthSigma );
	if( s_gpu.uAlphaSigma >= 0 ) glUniform1f( s_gpu.uAlphaSigma, alphaSigma );
	if( s_gpu.uCasAmount >= 0 )  glUniform1f( s_gpu.uCasAmount, casAmount );

	CloudGlCheck( "upsample uniforms (pre-draw)" );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	CloudGlCheck( "upsample glDrawArrays" );
	BindVao( 0 );
	SkyComposeRestoreTmus();
	CloudGlCheck( "upsample SkyComposeRestoreTmus" );

	if( issuing )
	{
		glEndQuery( GL_TIME_ELAPSED );
		s_gpu.qInFlight[slot] = true;
		s_gpu.qFrame[slot] = s_frame;
		s_gpu.ringHead = ( slot + 1 ) % kRing;
	}

	// --- restore the snapshotted entry state ----------------------------------------
	UseProgram( 0 );
	SetBlend( prevBlend ? kBlendAlpha : kBlendNone );
	SetDepthTest( prevDepthTest != GL_FALSE );
	SetDepthWrite( prevDepthMask != GL_FALSE );
	SetCull( prevCull != GL_FALSE );
	BindFbo( (GLuint)prevFbo );
	glViewport( prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3] );
	glColorMask( (GLboolean)prevColorMask[0], (GLboolean)prevColorMask[1], (GLboolean)prevColorMask[2], (GLboolean)prevColorMask[3] );
	if( prevScissor ) glEnable( GL_SCISSOR_TEST ); else glDisable( GL_SCISSOR_TEST );
	if( prevSrgb )    glEnable( GL_FRAMEBUFFER_SRGB ); else glDisable( GL_FRAMEBUFFER_SRGB );

	// --- post-pass GL-error check: the cloud pass MUST contribute zero ---------------
	// A2: ALWAYS drain (never swallow a real error), but LOG-ONCE so a genuine regression surfaces
	// without flooding -log every frame. A persistent cloud-pass error is a build regression, not the
	// upstream-takeover residual the entry drain handles; one logged line is enough to flag it.
	{
		static bool s_postErrLogged = false;
		GLenum e; int n = 0;
		while( ( e = glGetError() ) != GL_NO_ERROR && n < 8 )
		{
			if( !s_postErrLogged )
			{
				s_postErrLogged = true;
				CSZ_LogError( "cloudvol", "[csz_clouds] CLOUD-PASS GL ERROR 0x%x (frame=%u) -- the cloud pass MUST contribute zero; logged once, run csz_gl_debug to localize", (unsigned)e, s_frame );
			}
			n++;
		}
	}

	if( gotSample && perf >= 1 )
		CSZ_LogInfo( "cloudvol",
			"[csz_clouds] frame=%u qidx=%d cloud_pass_ms=%.4f res=%d steps=%d light=%d tod=%d nightness=%.2f avail_age=%u",
			s_frame, slot, readMs, res, steps, lightSteps, tod, nightness, readAge );
}

}  // namespace csz
