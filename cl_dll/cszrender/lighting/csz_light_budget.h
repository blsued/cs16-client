/*
 * csz_light_budget.h -- CSOZ renderer: multi-flashlight hard-cap budgeter (L6b)
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
namespace csz
{
struct ViewSetup;

// L6b hard cap: under 32-bot load several players can hold lit flashlights at
// once; rendering every beam at full quality (cone-mesh march + spot direct +
// shadow) would blow the frame budget. The L4D rule (research
// flashlight-thirdperson-gating.md) is to keep the cost bounded by quality
// degradation, not by hiding lights: the few most prominent beams stay full,
// the next band drops to a cheaper cone, the rest are culled.
//
// Tier values are stored in ActiveLight.budgetTier. 0 == full so a registry that
// never ran the budgeter (or a single beam) behaves exactly like L6a.
enum LightBudgetTier
{
	kBudgetFull  = 0,   // cone-mesh full steps + spot direct + (the one) shadow map
	kBudgetCheap = 1,   // cone-mesh reduced steps + spot direct, shadowless
	kBudgetCull  = 2,   // not rendered (off-screen, or beyond the full+cheap cap)
};

void LightBudgetRegisterCvars();                    // csz_flashlight_max_full / _max_cheap
void LightBudgetCompute( const ViewSetup &view );   // per-frame: rank visible spots, fill budgetTier
int  LightBudgetCheapSteps();                       // cone march steps for the cheap tier
}
