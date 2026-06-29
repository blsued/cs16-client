/*
 * csz_sky_compose.h -- CSOZ renderer: HDR scene target + resolve + FROZEN sky ABI
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
#pragma once
// =============================================================================
// C1 -- HDR Compositing Foundation + FROZEN SKY ABI.
//
// This header is the FROZEN contract that C2 (atmosphere LUTs), C3 (sun/moon +
// fog-horizon coupling) and C4 (stars / purkinje) fan out on. Do not break the
// public layout/signatures below without an ABI version bump (SkyAtmosResources
// .version) and updating all consumers.
//
// Codebase-style match (verified against csz_sky.h / csz_shadowmap.h):
//   * free functions in the csz:: namespace + an extern g_* singleton for the
//     opaque module state.
//   * GLuint/GLenum come from the vendored csz_glcorearb.h (via csz_glfuncs.h);
//     callers include this header AFTER the engine/GL headers, exactly like the
//     other geom/lighting modules.
//   * GL object lifetime is keyed on the core GpuGeneration() counter: owners
//     FORGET (never glDelete) names from a foreign context and rebuild lazily.
//     Every typed GL handle carries its gpuGeneration so consumers re-validate
//     it like the FBO/sky/shadow code does (red-team fix #13).
// =============================================================================
#include "../core/csz_glfuncs.h"   // GLuint / GLenum / glcorearb typedefs

struct ref_viewpass_s;

namespace csz
{

// -----------------------------------------------------------------------------
// 3.2 / red-team fix #1,#2,#3 -- SAFE SKY TMU RANGE (binding contract for C2/C3/C4)
//
// The engine's ref_gl path tracks its own glState.activeTMU and cleans texture
// units DOWN from kMaxUsedTmu(=3) inside LeaveTakeover() (csz_glstate.cpp). A
// raw glActiveTexture that desyncs that tracker is UNRECOVERABLE (HUD/console/
// world break persistently, no vid_restart heal). Therefore:
//
//   * Sky subsystems (this module + C2/C3/C4) bind their OWN raw GL textures
//     (LUTs/cubemaps from glGenTextures) ONLY on units in
//     [kSkyTmuBase, kSkyTmuBase + kSkyTmuCount) -- ABOVE the engine's range so
//     they can never collide with the engine-tracked units 0..3.
//   * They bind/unbind ONLY through SkyComposeBindTex() / SkyComposeRestoreTmus()
//     -- NEVER raw glActiveTexture / glBindTexture directly.
//   * BAN on fixed-function texture enables (red-team fix #3): we are GL3.3 core,
//     but C2/C3/C4 MUST NOT call glEnable(GL_TEXTURE_2D/CUBE_MAP) on a sky unit.
//     The engine cleanup stops at unit 3 and would never disable a legacy enable
//     left on unit >=4, poisoning the engine renderer. Core sampling only.
//   * kMaxUsedTmu is NOT raised (engine ref arrays/wrappers are unverified above
//     3); instead SkyComposeRestoreTmus() unbinds every touched sky unit, then
//     restores the REAL active unit -- captured once via raw glGetIntegerv before
//     the cycle's first raw glActiveTexture -- with a raw glActiveTexture, never
//     touching the engine wrappers. The engine's tracked active unit was never
//     changed, so real==tracker stays coherent and LeaveTakeover's down-walk too.
// -----------------------------------------------------------------------------
enum
{
	kSkyTmuBase  = 4,   // first sky-reserved unit (engine uses 0..3)
	kSkyTmuCount = 4,   // units 4..7 reserved for the sky subsystem (C2/C3/C4)
};

// Bind a raw GL texture onto sky unit skyUnitIndex (absolute unit =
// kSkyTmuBase + skyUnitIndex). Records the (unit,target) so RestoreTmus can
// unbind it. skyUnitIndex must be in [0, kSkyTmuCount). The absolute GL unit
// the sampler uniform must reference is kSkyTmuBase + skyUnitIndex.
void SkyComposeBindTex( int skyUnitIndex, GLenum target, GLuint rawGlName );

// Unbind every sky unit touched since the last restore, then restore the REAL
// active unit to the value captured (via raw glGetIntegerv) at the start of the
// bind/restore cycle, using a raw glActiveTexture -- no engine wrappers, so the
// engine's tracked active unit stays coherent (red-team fix #1: avoids the core-
// profile 0x502 the old GL_SelectTexture resync raised). MUST be called before
// returning to any code that may use
// gRenderAPI.GL_Bind (not only before LeaveTakeover). SkyComposeResolve() calls
// it at the end of every resolve; sub-chunks that bind between frames must call
// it themselves (prefer an RAII scope at the call site).
void SkyComposeRestoreTmus();

// -----------------------------------------------------------------------------
// 3.3 / red-team fix #13 -- TYPED ATMOS RESOURCE ABI (C2 fills it later)
//
// Raw GLuint is insufficient: consumers need target/dimensions/format/space and
// the gpuGeneration to re-validate after context loss. C2 publishes real LUTs
// via SkyComposePublishAtmosResources(); until then SkyComposeAtmosResources()
// returns a default with ready=false and documented CPU fallbacks so C3 and the
// fog-horizon coupling build and run TODAY.
// -----------------------------------------------------------------------------
struct SkyTextureRef
{
	GLenum target;          // GL_TEXTURE_2D / GL_TEXTURE_3D / GL_TEXTURE_CUBE_MAP ...
	GLuint name;            // raw GL name; 0 = not allocated. Bind ONLY via SkyComposeBindTex.
	int    width;
	int    height;
	int    layers;          // depth/array layers (1 for 2D)
	GLenum internalFormat;  // e.g. GL_RGBA16F
	int    gpuGeneration;   // generation the name belongs to; consumers compare to GpuGeneration()
};

struct SkySamplerSpec
{
	GLenum filterMin;       // e.g. GL_LINEAR
	GLenum filterMag;       // e.g. GL_LINEAR
	GLenum wrapS;           // e.g. GL_CLAMP_TO_EDGE
	GLenum wrapT;
	GLenum wrapR;
};

struct SkyAtmosResources
{
	unsigned int version;          // ABI version, bump on layout change; C1 = 1, C2 = 2
	bool         ready;            // false until C2 publishes; consumers MUST check this

	SkyTextureRef transmittanceLut; // optical-depth / transmittance LUT (0 until ready)
	SkyTextureRef horizonLut;       // horizon/sky-radiance LUT (0 until ready; superseded by skyViewLut in C2 -- kept for the frozen C1 layout)

	SkySamplerSpec transmittanceSampler;
	SkySamplerSpec horizonSampler;

	// CPU-side fallbacks usable when !ready (defined defaults so C3 & the
	// fog-horizon coupling work before C2 lands). COLOR SPACE: these are LINEAR
	// radiance/transmittance values (the HDR-stage space C2+ produces), NOT
	// display/gamma. fallbackTransmittance {1,1,1} = no atmospheric extinction;
	// fallbackHorizonColor {0,0,0} = neutral no-contribution horizon (additive
	// identity -- adding it changes nothing, the safe default for an unfinished
	// coupling). C3 must treat !ready as "atmosphere contributes nothing".
	float fallbackTransmittance[3];
	float fallbackHorizonColor[3];

	// -------------------------------------------------------------------------
	// C2 ADDITIVE EXTENSION (version 2). The C1 review flagged "2 LUTs is too
	// few" for a Bruneton/Hillaire precomputed model -- the physically-based
	// atmosphere needs the multiple-scattering + sky-view LUTs in addition to
	// transmittance. These fields are APPENDED (existing offsets unchanged): a
	// cheap source-level recompile, NOT a binary ABI break. C2 is the ONLY
	// writer of this header after C1; C3/C4 only READ the fields below.
	//
	// All LUTs are GL_TEXTURE_2D RGBA16F holding LINEAR radiance/transfer values
	// (the loaded GL func table has no glTexImage3D, so no 3D aerial-perspective
	// froxel -- deferred, see csz_atmos.cpp). Bind ONLY via SkyComposeBindTex.
	// -------------------------------------------------------------------------
	SkyTextureRef  multiScatteringLut; // Hillaire 2nd-order infinite-scattering LUT (muSun, altitude)
	SkyTextureRef  skyViewLut;         // per-view-ray sky radiance LUT (azimuth-around-sun, view zenith)
	SkySamplerSpec multiScatteringSampler;
	SkySamplerSpec skyViewSampler;     // wrapS = GL_REPEAT (azimuth wraps at the sun meridian)

	// Low-frequency horizon CHROMA TARGET for the black-fog coupling (C3): the
	// representative LINEAR radiance of the sky at the horizon toward the sun
	// azimuth at the current phase. The fog reads this as a color/chroma target
	// (NOT a second fog-density term -- avoids double fog participation). {0,0,0}
	// until ready. Computed CPU-side (no glReadPixels in the loaded GL table).
	float horizonColor[3];

	// Current sun world direction (Quake Z-up, normalized) the LUTs were built
	// for, and the atmospheric transmittance from the ground toward the sun
	// (per-channel) -- C3 multiplies the sun disc radiance by this so the disc
	// reddens/dims as it sets. {0,0,1} / {1,1,1} until ready.
	float sunDirection[3];
	float sunTransmittance[3];
};

// Current resources (default-with-fallbacks until C2 publishes). Reference is
// stable for the process lifetime; the pointed-to struct is replaced wholesale
// by Publish. Consumers re-validate .ready and each SkyTextureRef.gpuGeneration.
const SkyAtmosResources &SkyComposeAtmosResources();

// C2 calls this once its LUTs are built (and again on gpuGeneration change).
void SkyComposePublishAtmosResources( const SkyAtmosResources &res );

// -----------------------------------------------------------------------------
// 3.4 -- STUB SEAMS for C2/C3/C4 (implemented as pure no-ops/identity in C1)
//
// These exist so each downstream chunk has a file/entry point to fill. After C1
// every body contributes NOTHING to the image (scene == C0 baseline).
//   * AtmosBuildLuts()  -- C2: build transmittance/horizon LUTs + Publish them.
//                          C1 stub: no-op (resources stay ready=false).
//   * SunMoonContribute()-- C3: any sun/moon HDR contribution into the scene
//                          target. C1 stub: no-op (csz_sky.cpp already draws the
//                          existing discs at slot 10.5; this seam is the C3 hook
//                          for the physically-based body, drawn into the HDR FBO).
//   * StarsContribute() -- C4: stars / purkinje hook. C1 stub: no-op (the
//                          existing hash star field stays in csz_sky's FS; the
//                          resolve purkinje() is identity until C4).
// Each takes the current view so C2/C3/C4 can place contributions in world space.
// -----------------------------------------------------------------------------
struct ViewSetup;   // core/csz_view.h (forward-declared; seams pass it by ref)

void AtmosBuildLuts();
void SunMoonContribute( const ViewSetup &view );
void StarsContribute( const ViewSetup &view );
// MW-rework (additive seam): sampled all-sky panorama backdrop, drawn at slot 10.5
// immediately BEFORE StarsContribute (behind the live twinkle stars + the moon).
// Additive into the HDR FBO, night-gated; emits the relocated moon outer sky-glow.
// Implemented in geom/csz_panorama.cpp.
void PanoramaContribute( const ViewSetup &view );

// SKY-REWORK-SPEC v3 Task C data contract (moon -> stars/MW). Returns the moon BODY
// occluder disc geometry (world dir + angular RADIUS in rad + limb-AA softness in
// rad) for the analytic T_moonBody mask the C4 star + Milky Way shaders apply, so
// the mask occludes layer1 EXACTLY under the disc the C3 body pass draws (same
// phase->dir, dev-aim override, csz_moon_size, horizon visibility). Returns false
// when the moon body is not contributing (below horizon / csz_moon 0); the caller
// then leaves T_moonBody == 1 (no occlusion). Implemented by C3 (csz_sunmoon.cpp).
// outGlowL receives the SKY-REWORK-SPEC v3 Task D moonlight-wash base luminance
// L_moon = I_moon(phase) * moonAltFactor (0 when the moon is down/new) -- the single
// source of the wash strength the star + Milky Way MoonSkyLum() shaders consume.
bool MoonBodyOccluder( const ViewSetup &view, float outDir[3], float &outAngR, float &outSoft, float &outGlowL );

// -----------------------------------------------------------------------------
// C2 ATMOSPHERE -- public entry points (csz_atmos.cpp). Additive to the C1 ABI.
//
//   * AtmosRegisterCvars() -- register csz_atmos / csz_atmos_exposure /
//       csz_atmos_ms / csz_atmos_timing. Call from Renderer::OnHudInit.
//   * AtmosActive() -- csz_atmos != 0 (read live). When false the renderer keeps
//       the legacy csz_sky monolithic background (the C1-identity baseline).
//   * AtmosReady() -- true once the transmittance + multiple-scattering +
//       sky-view LUTs are built and published for the live GpuGeneration().
//   * AtmosBuildLuts() (declared above) -- build the static LUTs once per
//       generation, refresh the sun-dependent sky-view LUT when the sun has
//       moved, and SkyComposePublishAtmosResources(). Does its OWN FBO/TMU
//       juggling and leaves FBO 0 bound + TMUs resynced, so it MUST be called
//       at slot 10 BEFORE SkyComposeBeginScene binds the HDR scene target.
//   * AtmosDrawSky() -- slot 10.5 full-screen sky background into the bound HDR
//       FBO (depth test/write OFF), sampling the sky-view LUT per view ray.
//       Returns false (drew nothing) when not active/ready so the caller can
//       fall back to the legacy sky.
//   * AtmosShutdown() -- generation-safe teardown. Call from Renderer::Shutdown.
// -----------------------------------------------------------------------------
void AtmosRegisterCvars();
bool AtmosActive();
bool AtmosReady();
bool AtmosDrawSky( const ViewSetup &view );
// Dev proof/showcase: when csz_sky_fullscreen != 0 (CSZ_DEV_TOOLS), repaint the
// atmosphere over the whole finished frame so the sky gradient is visible from
// any camera angle for the visual-acceptance capture (mirrors the legacy
// SkyRenderer::DrawDebugFullscreen). No-op when the cvar is absent/0.
void AtmosDrawDebugFullscreen( const ViewSetup &view );
void AtmosShutdown();

// -----------------------------------------------------------------------------
// 3.1 -- HDR SCENE TARGET LIFECYCLE
//
// SkyComposeBeginScene(): ensure the RGBA16F HDR FBO exists at the viewport size
//   (recreate on resize or GpuGeneration change), bind it, set the viewport and
//   clear color+depth with the SAME clearColor the engine would use, so the
//   identity resolve reproduces the baseline. Called at slot 10. When csz_hdr==0
//   it is a pure passthrough to ApplyMainViewport semantics (FBO 0).
//
// SkyComposeResolve(): when csz_hdr==1, bind backbuffer 0, clear it (fix #5),
//   set clean fullscreen state (fix #4), run the fullscreen resolve sampling the
//   HDR color texture via the SAFE sky-unit bind + texelFetch (fix #2,#8), then
//   SkyComposeRestoreTmus(). When csz_hdr==0 it is a no-op (scene already on FBO
//   0). Called just before slot 16 (LeaveTakeover).
//
// SkyComposeShutdown(): generation-safe teardown (delete only on a live context;
//   forget on a foreign one).
//
// SkyComposeRegisterCvars(): register csz_hdr/csz_exposure/csz_tonemap/
//   csz_encode/csz_dither/csz_hdr_timing (called from Renderer::OnHudInit).
//
// SkyComposeActive(): true iff the HDR path is engaged this frame (csz_hdr!=0).
//   The renderer queries this to decide the slot-10 clear ownership and whether
//   to run the resolve.
// -----------------------------------------------------------------------------
void SkyComposeRegisterCvars();
bool SkyComposeActive();   // csz_hdr != 0 (read live each call)
// VERIFICATION INFRA (csz_verify_freeze, default 0). NOT a gameplay feature: a
// machine-verification gate for TIME-VARYING render effects. When 1, every
// per-frame / time / RNG term that makes two captures of the SAME fixed instant
// differ run-to-run is pinned to a constant, so effect-diff (toggle one effect's
// own cvar) and A/B become byte-near-identical, grade-stable gates:
//   * resolve dither forced OFF (compose);
//   * sky adaptive eye-adaptation exposure pinned to a fixed value (atmos);
//   * fog-volume animated-IGN frame offset pinned to 0 (fog_volume);
//   * star twinkle u_time pinned to a constant (stars);
//   * dust temporal integration frozen (dt=0, dust).
// Read live each call so an in-session toggle takes effect next frame.
bool SkyComposeVerifyFreeze();
// S4 (REWORK-SPEC §S4): the renderer publishes the live phase nightness [0,1] each frame
// (after PublishLighting, before the resolve) so the resolve's night grade (exposure / shadow
// toe / Purkinje) follows the phase curve. Day (nightness 0) -> the resolve skips the whole
// grade -> bit-identical to the pre-S4 resolve. Clamped to [0,1] internally.
void SkyComposePublishNight( float nightness );
// Raw GL name of the sampleable scene depth texture (GL_DEPTH_COMPONENT24,
// compare-mode NONE) backing the HDR FBO's GL_DEPTH_ATTACHMENT; 0 when the HDR
// target is not valid. Bind ONLY via SkyComposeBindTex. (fog Step 1 prerequisite)
GLuint SkyComposeDepthTex();
// Raw GL name of the HDR scene FBO (color RGBA16F + sampleable D24 depth); 0 when
// the HDR target is not valid. The fog volume pass (Step 3) re-binds it to add its
// in-scatter into the scene at the kTmVolume seam. (fog Step 3)
GLuint SkyComposeHdrFbo();
void SkyComposeBeginScene( const struct ref_viewpass_s *rvp, const float clearRgba[4] );
void SkyComposeResolve( const struct ref_viewpass_s *rvp, const float clearRgba[4] );
void SkyComposeShutdown();

// GPU timer readout (red-team fix #11). Returns the most recently AVAILABLE
// GL_TIME_ELAPSED of the wrapped region (BeginScene clear .. Resolve) in
// milliseconds, or -1.0 if no result is ready yet. The wrapped region is ~the
// whole CSZ scene, NOT just HDR overhead -- the true HDR overhead is the
// whole-frame delta csz_hdr 1 vs 0 (measured by the orchestrator).
double SkyComposeLastGpuMs();

// L0 observability (csz_perf_dump, default 0). When armed, the orchestrator emits
// one parseable [csz_perf] line per sample window: the whole-frame in-scene GPU ms
// (SkyComposeLastGpuMs above) + per-pass CPU ms. Reading it also forces the single
// in-scene GL_TIME_ELAPSED query on (ComposeTimingActive) -- a passive scope that
// changes no draw, so the frame stays pixel-identical. NEVER nest another GPU timer
// inside the compose span (GL_TIME_ELAPSED cannot nest).
bool SkyComposePerfDumpEnabled();

}  // namespace csz
