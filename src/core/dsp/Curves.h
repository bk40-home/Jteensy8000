// =============================================================================
// Curves.h — normalized <-> engineering-unit conversions for JT-8000 v2
// =============================================================================
//
// ROLE IN THE ARCHITECTURE (design brief §4.3)
//   Every parameter is stored as a NORMALIZED float t in 0..1.  Conversion to
//   engineering units (Hz, ms, semitones, ...) happens in exactly ONE place —
//   here — driven entirely by the curve metadata in the generated ParamTable.
//   This replaces v1's Mapping.h, which had a hand-written cc_to_x()/x_to_cc()
//   pair per parameter and required every subsystem to know CC encoding.
//
// CPU CONTRACT
//   These functions run at CONTROL RATE only: parameter dirty-application
//   (at most a handful per 2.9 ms block), patch load, UI display, MIDI I/O.
//   Nothing here is called per sample.  The audio engine caches engineering
//   values after each dirty application — it never converts in a sample loop.
//   That is why plain expf()/logf() calls are acceptable here and why no
//   lookup tables are needed.
//
// PRECISION CONTRACT
//   float end-to-end.  The whole project builds with -Wdouble-promotion
//   -Werror, so only the f-suffixed <cmath> functions are used.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "gen/ParamTable.h"

namespace JT {
namespace Curves {

// -----------------------------------------------------------------------------
// Core conversions.
//
// toEngineering(): normalized position t (clamped to 0..1) -> engineering
// units, using the parameter's curve:
//   Lin : min + t * (max - min)
//   Log : min * (max/min)^t          (equal ratios per step — perceptual
//                                     frequency and time sweeps; v1's
//                                     cc_to_cutoff_hz / cc_to_time_ms)
//   Seg2: two linear segments meeting at 'mid' when t == 0.5
//                                    (v1's envelope-slope cc_to_curve)
//
// toNorm() is the exact inverse (engineering value clamped to min..max).
// Select params: engineering value IS the option index (table: min 0,
// max count-1, Lin).  Toggle: 0.0 / 1.0.
// -----------------------------------------------------------------------------
float toEngineering(const Params::ParamDesc& d, float norm);
float toNorm(const Params::ParamDesc& d, float engineering);

// -----------------------------------------------------------------------------
// Select helpers — direct indices, replacing v1's bucket-midpoint encoding.
// toOptionIndex() rounds to the nearest option and clamps to a valid index;
// normFromOptionIndex() is its inverse.  Round-trip is exact for every
// option of every set (proven in test_curves.cpp).
// -----------------------------------------------------------------------------
int   toOptionIndex(const Params::ParamDesc& d, float norm);
float normFromOptionIndex(const Params::ParamDesc& d, int index);

// -----------------------------------------------------------------------------
// MIDI data-width quantisation.
//
// NRPN Data Entry carries 14 bits (MSB<<7 | LSB); a curated performance CC
// carries 7.  These four functions are the ONLY place the wire width meets
// the normalized domain, so every transport quantises identically.
// fromXbit(toXbit(...)) identity over all codes is proven in tests — the
// v2 equivalent of v1's Python bucket-midpoint simulation, now trivial.
// -----------------------------------------------------------------------------
uint16_t normTo14bit(float norm);          // 0..1 -> 0..16383 (round-nearest)
float    normFrom14bit(uint16_t v);        // 0..16383 -> 0..1
uint8_t  normTo7bit(float norm);           // 0..1 -> 0..127
float    normFrom7bit(uint8_t v);          // 0..127 -> 0..1

// Clamp helper shared by the store; public because transports need it too.
float clamp01(float v);

} // namespace Curves
} // namespace JT
