/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once
// =============================================================================
// FilterShape.h  –  Per-filter musical curves and limits (compile-time table)
// =============================================================================
//
// The synth/UI passes normalised 0..1 values to the VA filter bank via
// setCutoffNorm() / setResonanceNorm(). This table is the single source of
// truth for how each filter type translates a 0..1 knob into Hz and k:
//
//   fcMinHz, fcMaxHz : per-family musical cutoff range. Outside this band the
//                      filter sounds dull (LP fully open) or thin (HP at top).
//                      Used by an exponential map  Hz = fcMin·(fcMax/fcMin)^c
//                      so equal knob steps give equal octave steps.
//
//   resGamma         : exponent applied to the user knob BEFORE the existing
//                      mapResonance() step. γ < 1 lifts the early knob travel
//                      into the audible range so equal CC steps feel like equal
//                      resonance steps. γ = 1 disables shaping (TPT1, which has
//                      no resonance).
//
// Tuned from filter_response_analysis.py — see perceived_resonance.png and
// proposed_curves.png for the underlying small-signal analysis.
//
// Edit values in FilterShape.cpp only. Every caller goes through the bank's
// normalised API and inherits the change with no further code edits.
// =============================================================================

#include "AudioFilterVABank.h"   // for VAFilterType / FILTER_COUNT

struct FilterShape
{
    float fcMinHz;    // musical cutoff floor (Hz)
    float fcMaxHz;    // musical cutoff ceiling (Hz)
    float resGamma;   // user r → internal r exponent (γ ≤ 1; smaller = more lift)
};

// Indexed by VAFilterType. Defined in FilterShape.cpp, one row per filter.
extern const FilterShape kFilterShape[FILTER_COUNT];
