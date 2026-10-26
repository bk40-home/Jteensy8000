// =============================================================================
// JpXmodLaw.h — JP-8000 X-MOD depth by modulator waveform (JE-8086 measured)
// =============================================================================
//
// ROLE
//   JpXmod (mix.xmod_type = 1) deviates OSC1 by a FIXED number of Hz per knob
//   step × OSC2's output (OscSection: 69 Hz/step for a triangle modulator).
//   The 2026-10-25 X-MOD grid (xmod/xmod-grid-analysis-2026-10-25.md: 72 JE
//   cells, OSC2 TRI/SAW/SQR, sync off/on, osc shift 0/−1, MIDI 36..72) shows
//   the deviation also depends on WHICH wave OSC2 plays, and that SYNC only
//   scales the non-pulse waves:
//
//     OSC2 wave        Hz/step (sync off)   scale vs TRI   sync factor
//     triangle family        69                 1.00          0.623
//     saw family             34.5               0.50          0.623
//     pulse 50 %             20.4               0.296         1.00
//     pulse 79 %             19.1               0.277         1.00
//     anything else          (unmeasured)       1.00          0.623
//
//   Independent of note and osc shift; linear in the knob (16/44/88).  The
//   triangle row is the pre-existing law, so triangle-modulator patches are
//   unchanged.  The pulse modulator is AC-coupled on the JP (its DC never
//   moves OSC1's pitch) — that part lives in OscSection / OscCore::outputDc().
//
// COST
//   A switch and, for pulse waves, one fabsf — control rate, once per block.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include "core/dsp/OscCore.h"   // Wave

namespace JT {
namespace JpXmodLaw {

// Multiplier on the triangle-calibrated deviation for OSC2 wave `w2` playing
// at pulse width `pulseWidth` (0..1, only read for pulse waves), with the
// sync factor already folded in when `sync` is on.
float depthScale(Wave w2, float pulseWidth, bool sync);

} // namespace JpXmodLaw
} // namespace JT
