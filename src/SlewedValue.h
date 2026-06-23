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
// SlewedValue.h  –  Parameter smoother built on the bank's own one-pole.
// =============================================================================
//
// The maths of "exponential smoothing"  y[n] = y[n-1] + a·(target − y[n-1])
// is identical to a one-pole low-pass driven by the target signal. Time
// constant τ and smoothing cutoff fc relate by:
//
//     fc = 1 / (2π · τ)
//
// So a 15 ms smoother is a 10.6 Hz low-pass on the control signal. This class
// is a thin shell over the bank's TPT1 struct (Zavalishin §3.1, p.45) and
// va_compute_g(fc, fs) — the same trapezoidal one-pole that runs in
// FILTER_TPT1_LP, just driven at control rate by parameter values.
//
// Used by AudioFilterVABank to smooth the cutoff, resonance and drive knobs so
// CC steps glide instead of clicking. Composition (not inheritance) keeps the
// DSP primitive untouched and obviously reused.
//
// API:
//   setSampleRate(fs)   → control-rate (call once at setup)
//   setBlockSize(n)     → cache size for tickBlock() fast path
//   setTimeMs(ms)       → τ in milliseconds
//   setTarget(t)        → control rate (CC handler, env, etc.)
//   reset(v)            → snap to v (note-on, init, patch change)
//   tick()              → per-sample advance (audio loop)
//   tickBlock()         → per-block advance (geometric, cheap)
//   current() / target() / isSettled()
//
// CPU:
//   tick()      : ~6 ops (one TPT1::processLP + settled check)
//   tickBlock() : 1 mul + 1 sub + 1 add + settled check (precomputed decay)
//   Both early-out when settled — no work for parameters that aren't moving.
//
// The block-rate fast path is mathematically exact for the per-sample TPT1
// recurrence. TPT1's per-sample update gives
//     (s' − target) = (s − target) · (1−g)/(1+g)
// so after N samples with constant target:
//     s_N = target + (s_0 − target) · ((1−g)/(1+g))^N
// We cache that factor as _blockDecay; one mul per block, exact result.
// =============================================================================

#include "VAFilterCore.h"   // TPT1, va_compute_g, VA_PI
#include <math.h>           // fabsf, powf

class SlewedValue
{
public:
    SlewedValue();

    // ── Configuration (control-rate; safe to call any time) ──────────────────
    void setSampleRate(float fs);
    void setBlockSize(int n);
    void setTimeMs(float ms);

    // ── Targeting ────────────────────────────────────────────────────────────
    void setTarget(float t);
    void reset(float v);

    // ── Advance the smoother ─────────────────────────────────────────────────
    // tick():      one sample.       Use in per-sample audio loops.
    // tickBlock(): _blockSize steps. Use once per AudioStream::update().
    inline float tick();
    inline float tickBlock();

    // ── Read without advancing ───────────────────────────────────────────────
    inline float current()   const { return _tpt1.s; }
    inline float target()    const { return _target; }
    inline bool  isSettled() const { return _settled; }

private:
    // The bank's own one-pole — composition, not inheritance, so the DSP
    // primitive stays untouched and obviously reused.
    TPT1  _tpt1;

    float _g;            // pre-warped integrator gain (= va_compute_g)
    float _blockDecay;   // ((1−g)/(1+g))^_blockSize, recomputed when fs/n/τ change
    float _sampleRate;
    int   _blockSize;
    float _timeMs;
    float _target;
    bool  _settled;

    // Treat values within this distance of the target as "arrived" — stops the
    // exponential tail from burning CPU forever. 1e-5 is ~ -100 dB relative to
    // a 0..1 parameter range; audibly indistinguishable.
    static constexpr float kSlewEps = 1.0e-5f;

    void recomputeG();
    void recomputeDecay();
};

// ── Inline definitions (hot path, kept in header for inlining) ───────────────

inline float SlewedValue::tick()
{
    if (_settled) return _tpt1.s;
    // Same one-pole the bank uses for FILTER_TPT1_LP — driven here by the
    // control target instead of an audio sample.
    _tpt1.processLP(_target, _g);
    if (fabsf(_tpt1.s - _target) < kSlewEps) {
        _tpt1.s  = _target;
        _settled = true;
    }
    return _tpt1.s;
}

inline float SlewedValue::tickBlock()
{
    if (_settled) return _tpt1.s;
    // Closed-form N-sample advance (see header derivation). One mul.
    _tpt1.s = _target + (_tpt1.s - _target) * _blockDecay;
    if (fabsf(_tpt1.s - _target) < kSlewEps) {
        _tpt1.s  = _target;
        _settled = true;
    }
    return _tpt1.s;
}
