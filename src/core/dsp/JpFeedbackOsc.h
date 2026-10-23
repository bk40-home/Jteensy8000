// =============================================================================
// JpFeedbackOsc.h — JP-8000 "FEEDBACK OSC", JP-flavoured (wave "JFBK")
// =============================================================================
//
// ROLE
//   A separate oscillator class (owned per unit by OscSection, like
//   SupersawOsc) that recreates the JP-8000 OSC1 FEEDBACK OSC from JE-8086
//   measurements.  Controls follow the JP: Ctrl1 = HARMONICS (osc<n>.shape_dc,
//   the shared SHAPE route), Ctrl2 = FEEDBACK AMOUNT (osc<n>.jfbk_feedback).
//   The existing FeedbackComb is a different effect and is untouched.
//
// WHAT IS EXACT, WHAT IS "JP-FLAVOURED" (sign-off option 1, 2026-10-23)
//   * EXACT: at zero feedback the JP wave is a plain saw through a 1-pole
//     high-pass with a FIXED time constant (1.92 ms, corner ≈ 83 Hz), measured
//     identical at C3, C5 and C7.  That is reproduced sample-for-sample.
//   * CHARACTER: with feedback the JP grows an inner two-level "block"
//     oscillation whose blocks-per-cycle depends on Harmonics, Feedback and
//     pitch; it hard-locks in some settings, drifts slowly in others, and is
//     unsettled in a few.  No closed form was found (three loop models were
//     rejected against the captures — see project doc osc-shapes/jp-feedback-
//     osc-findings), so this class drives an inner oscillator from MEASURED
//     maps: the block count at C3 and C5 (fresh-note grids; the nearest of
//     the two by pitch, split at F#4 — D3a), plus a per-cell lock strength
//     and chaos amount.  It matches the
//     JE's block counts in 72/72 C3 and 68/72 C5 grid cells and its wander-vs-
//     locked behaviour in 72/72 C3 cells; the block LAYOUT
//     inside a cycle is not reproduced (the JP spaces its blocks unevenly).
//     Constants are the starting point for the by-ear tuning pass (design
//     doc osc-shapes/jfbk-design-proposal-2026-10-23).
//
// SIGNAL FLOW (per sample)
//     master  φ += inc                                  (naive saw — the JP
//                                                         aliases here too)
//     on each master wrap: ψ is pulled toward a whole inner cycle by `pull`
//                          (1 = hard lock) and nudged by a leaky random walk
//                          `wander` in the cells where the JE pattern keeps
//                          rearranging (block count held, layout wandering)
//     inner   ψ += N · inc · (1 + chaos · y[n−1])       (two-level blocks)
//     mix     v = (1 − m) · kSawLevel · saw(φ) + m · block(ψ)
//     out     y = HPF_1pole(v),  τ = 1.92 ms             (exact, fixed in time)
//
// CPU DISCIPLINE
//   All law work (a handful of bilinear table reads, one exp2f) runs ONCE
//   per actual change of pitch / Harmonics / Feedback (dirty flag), never per
//   sample.  Per sample: two phase accumulators, one compare, one 1-pole and a
//   handful of multiply-adds — cheaper than SupersawOsc's seven voices.  No
//   delay line, no wavetable.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "core/AudioConfig.h"

namespace JT {

class JpFeedbackOsc {
public:
    // --- control plane (block boundaries; all dirty-checked) ---
    void setFrequency(float hz);      // also selects the C3 or C5 count map
    void setHarmonics(float k01);     // JP Ctrl1, 0..1 (OscSection's SHAPE value)
    void setFeedback(float fb01);     // JP Ctrl2, 0..1 (osc<n>.jfbk_feedback)

    // Note trigger: master phase from the caller's per-voice randomisation
    // (same value OscCore receives), inner phase aligned to it, HPF and the
    // chaos feedback cleared — every note starts fresh, as on the JP.
    void noteOn(float phase01);
    // Wander randomness: seeded per voice by OscSection (like SupersawOsc) so
    // voices decorrelate yet renders and tests stay deterministic.
    void seedNoise(uint32_t seed) { _rng = seed | 1u; }

    // --- audio plane ---
    // WRITES one block (replaces).  fmBuf optional (nullptr = off): ±1-ish
    // modulator, pitch × 2^(fm × fmOctaves) — the same exponential FM contract
    // as SupersawOsc (pitch envelope, JtXmod).  Both phases follow it, so the
    // block count per cycle is preserved under pitch modulation.
    void render(float* out, size_t n, const float* fmBuf, float fmOctaves);

    // Test/diagnostic access to the current law (no audio effect).
    float blocksPerCycle() const { return _blocks; }
    float lockPull() const       { return _pull; }

private:
    void updateLaw();                 // N, pull, chaos, wander, mix from the tables
    float nextNoise();                // xorshift32, −1..1

    // parameters (dirty-checked)
    float _freq     = 440.0f;
    float _harm     = 0.0f;
    float _fb       = 0.0f;
    bool  _lawDirty = true;

    // derived per-change law
    float _inc      = 440.0f / kSampleRate;   // master increment
    float _blocks   = 1.0f;                   // inner cycles per master cycle
    float _innerInc = 440.0f / kSampleRate;   // _blocks × _inc, Nyquist-guarded
    float _pull     = 1.0f;
    float _chaos    = 0.0f;
    float _mix      = 0.0f;                   // 0 = pure saw, 1 = pure blocks
    float _wander   = 0.0f;                   // random-walk step per master wrap

    // state
    float _phase    = 0.0f;           // master φ, 0..1
    float _inner    = 0.0f;           // inner ψ, 0..1
    float _hpPrevIn = 0.0f;
    float _hpOut    = 0.0f;
    float _walk     = 0.0f;           // accumulated wander offset (leaky)
    uint32_t _rng   = 0x2545F491u;
};

} // namespace JT
