/*
 * csz_glstate.h -- CSOZ renderer: thin GL state wrappers + takeover hygiene
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
#pragma once
struct ref_viewpass_s;
namespace csz
{
// kBlendAddPremul: premultiplied additive (glBlendFuncSeparate(ONE,ONE,ZERO,ONE)).
// Source RGB is already premultiplied (energy-conserving PSF), so it is added once
// with no per-source-alpha scaling; dst alpha is preserved. Used by the star PSF.
// kBlendPremulOver: premultiplied "over" (glBlendFunc(ONE, ONE_MINUS_SRC_ALPHA)).
// Source RGB is premultiplied radiance ADDED on top while the source ALPHA acts as
// an occlusion/coverage that ATTENUATES the destination behind it -- one mote both
// adds a faint speck AND slightly dims the light it floats in (L7 dust extinction).
// kBlendModulate: classic GoldSrc decal modulate (glBlendFunc(GL_DST_COLOR,
// GL_SRC_COLOR)) -> result = src*dst + dst*src = 2*src*dst, the engine's "2x
// overbright" decal blend. The blend identity (no visible effect) is src == 0.5.
// Used by csz_decal for the no-alpha bullet-hole class drawn after the light pass.
enum BlendMode { kBlendNone, kBlendAlpha, kBlendAdditive, kBlendAddPremul, kBlendPremulOver, kBlendModulate };
void EnterTakeover();   // baseline for CSZ passes: depth test LEQUAL + write on, blend/scissor/cull off
void ApplyMainViewport( const struct ref_viewpass_s *rvp, const float clearRgba[4] ); // bind FBO 0, glViewport(rvp->viewport), clear color+depth
void LeaveTakeover();   // restore-for-engine whitelist (calibrated in T1); ALWAYS the last call of a taken-over frame
// Thin wrappers with shadow state; never call raw gl* for these:
void SetBlend( BlendMode mode );
void SetDepthWrite( bool enable );
void SetDepthTest( bool enable );
void SetDepthRange( float zmin, float zmax );
void SetCull( bool enable );
void SetCullFront( bool cullFront );             // true = cull front faces (shadow acne trick)
void SetPolygonOffset( bool enable, float factor, float units );
void BindFbo( unsigned int fbo );                // 0 = default framebuffer
void UseProgram( unsigned int program );
void BindVao( unsigned int vao );
void BindTextureSlot( int tmu, int texSlot );    // via gRenderAPI.GL_Bind (ENGINE slot id, not raw GL name)
// Diagnostic: when `gate` cvar != 0, log a non-clean glGetError at a named step
// under `tag` (always clears the error -- probe semantics). Shared by the per-pass
// DbgErr wrappers (atmos/stars).
void DbgGlError( struct cvar_s *gate, const char *tag, const char *where );
}
