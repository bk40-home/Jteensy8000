// =============================================================================
// FeedbackComb.h — JP-8000 feedback-oscillation comb for JT-8000 v2 (Pass 4)
// =============================================================================
//
// PROVENANCE (verified against v1 OscillatorBlock, lines 86–103 / 480–509)
//   v1 built this from three audio objects per oscillator:
//     combIn[n]  = source[n] · 1.0  +  delayOut[n] · feedbackGain
//     delayOut   = combIn delayed by FEEDBACK_DELAY_MS (5.0 ms)
//     output    += delayOut · feedbackMix          (alongside the dry path)
//   with feedbackGain clamped 0..0.99 and the whole path force-zeroed when
//   the amount is 0 — mix alone can NOT open the tap (setFeedbackAmount's
//   else-branch zeroes both gains; ported exactly).
//
//   The 5 ms recursion puts the first comb resonance at ~200 Hz with modes
//   every 200 Hz above — the JP-8000 "feedback osc" growl.  v2 uses a
//   221-sample line (5.0 ms × 44.1 kHz = 220.5, rounded to sample), i.e.
//   5.01 ms — 0.2% mode shift, far below the resonance bandwidth.
//
// LOOP SAFETY = V1 EQUIVALENCE
//   v1's loop ran through int16 mixers, which saturate at full scale — that
//   saturation is what bounded the loop at high gain and part of how hot
//   settings sound.  The float port hard-clamps the line WRITE to ±1.0 for
//   the same bound and the same flavour.
//
// DELIBERATE DIFFERENCES (flagged per the no-silent-change rule)
//   1. When inactive the line is NOT processed at all ("do not calculate if
//      not required"); v1's objects always ran, keeping the line warm with
//      dry signal.  On the inactive→active transition v2 clears the line,
//      so colour fades in over one 5 ms round instead of arriving from a
//      warm line.  Inaudible; saves 2 units × 8 voices of always-on writes.
//   2. v1's SYNC mode bypassed the whole OscillatorBlock (its own graph
//      limitation), so feedback never coexisted with sync.  v2 composes
//      them — strictly more capable, identical at defaults.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stddef.h>

namespace JT {

class FeedbackComb {
public:
    // 5.0 ms at 44.1 kHz, rounded to the nearest whole sample.  PUBLIC so
    // the storage owner can size the pool (see attachStorage).
    static constexpr size_t kDelaySamples = 221;

    // --- control plane (block boundaries) ---
    void setAmount(float amount01);   // loop gain, clamped 0..0.99 (v1)
    void setMix(float mix01);         // tap level into the unit output

    // Active only when the amount is non-zero — v1 semantics (see header).
    bool isActive() const { return _gain > 0.0f; }

    void reset();                     // zero the line (enable transition)

    // MEMORY PLACEMENT: the line lives OUTSIDE the object.  16 lines cost
    // 14.1 KB — too much DTCM at 8 voices — but a delay line is touched
    // once per sample SEQUENTIALLY, the ideal cached-OCRAM access pattern.
    // The platform owner allocates the pool (DMAMEM on Teensy, a plain
    // static on the host) and attaches one kDelaySamples slice per comb.
    // Unattached combs are silent no-ops — never a crash.
    void attachStorage(float* line);

    // --- audio plane ---
    // IN PLACE on the unit's buffer: buf becomes dry + delayed-comb × mix,
    // which is exactly the v1 output-mixer sum.  Everything downstream
    // (section mix, ring, cross-mod feed) then sees the coloured signal,
    // matching v1's post-comb tap points.
    void process(float* buf, size_t n);

private:
    float* _line = nullptr;           // owned by the platform pool, not us
    size_t _idx  = 0;
    float  _gain = 0.0f;
    float  _mix  = 0.0f;
};

} // namespace JT
