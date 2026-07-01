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
// MoogDVCore.h  –  D'Angelo–Välimäki nonlinear Moog ladder (single core)
// =============================================================================
//
// Reference: S. D'Angelo and V. Välimäki, "An Improved Virtual Analog Model of
//            the Moog Ladder Filter", ICASSP 2013, pp. 729–733.
//            Aalto University, Dept. of Signal Processing and Acoustics.
//
// WHY THIS CORE EXISTS (and what it is NOT replacing)
// ---------------------------------------------------
// This is a THIRD Moog-family core, added alongside the existing two — neither
// is touched:
//   • MoogLinear4 (Moog slots) — a LINEAR ladder whose "self-oscillation" is a
//     synthetic safe-k envelope limiter, not circuit physics.
//   • NLLadderNB  (JP slots)   — a per-stage-tanh nonlinear ladder that is
//     stable by construction but ITERATIVE (Newton-bisection, variable cost).
//
// MoogDV4 is the white-box circuit model from the ICASSP'13 paper. Its two
// advantages over the above, for the Moog slots specifically:
//   1. PHYSICAL self-oscillation. The paper shows the model locks into a
//      sustained ring at feedback k = 4.0 — the real Moog behaviour — rather
//      than the faked limiter in MoogLinear4. (Paper §4, Fig. 5.)
//   2. NON-ITERATIVE. The zero-delay self-dependencies are broken by inserting
//      a unit delay into the tanh arguments (paper eqs. 12–13), so there is NO
//      Newton solve and NO Gauss-Seidel relaxation. Cost is FIXED per sample:
//      exactly 5 tanh evaluations (paper §3.1) + cheap adds/mults. That makes
//      it lower-variance than NLLadderNB and only modestly dearer than the
//      linear MoogLinear4.
//
// STRUCTURE (paper §3, optimised block diagram Fig. 3)
// ----------------------------------------------------
// Four cascaded bilinear-integrator stages, each a SECOND-ORDER nonlinear
// section (the "A" coefficient is the per-stage one-pole gain). The global
// feedback −k·ΔV4 is summed at the input with one unit delay (the delay is what
// makes the whole thing computable in one pass). Each stage applies one tanh to
// its delayed self-state; the input summing node applies one tanh to
// (Vin + k·ΔV4[n−1]). Total = 5 tanh per output sample.
//
// The recurrence (paper eqs. 12–14, rearranged to the Fig. 3 stage form):
//   For stage i, with input wi and stored state si (= ΔVi[n−1]):
//       Vi[n]  = A·(tanh(wi) − tanh(si_delayed)) + si_integrator
//   implemented as a bilinear integrator (eq. 14) fed by the tanh difference.
//
// We use the linear-regime tuning of A from cutoff (paper eq. 19):
//       A = (π·fc/fs) · (1 − π·fc/fs) / (1 + π·fc/fs)
//   NOTE: this is the paper's own small-signal A. We keep the cutoff mapping
//   exactly as published so tuning matches the reference; the tanh nonlinearity
//   then warps amplitude/harmonics on top, as in the circuit.
//
// CPU (Teensy 4.1 / Cortex-M7, single precision):
//   5 tanh + ~13 adds + ~9 mults per sample, no division on the hot path, no
//   iteration. With va_tanh_bounded (~8 cyc) the tanh bill is ~40 cyc/sample;
//   with true tanhf (~30 cyc) it is ~150 cyc/sample. Default is the bounded
//   approximation; flip JT_OPT_MOOGDV_TRUE_TANH for the paper-faithful curve.
//
// AUTHENTICITY / TRADE-OFFS:
//   • Like every nonlinear ladder, this ALIASES at 1x on bright, resonant,
//     hard-driven input. The JT_OPT_MOOGDV_OVERSAMPLE flag mirrors the
//     NLLadderNB oversample pattern (default 1x = ship-and-measure).
//   • The paper notes residual tuning/phase-shift error (its §4) shared with
//     Huovilainen; not corrected here, matching the reference.
// =============================================================================

#include <math.h>
#include <stdint.h>
#include "VAFilterCore.h"   // VA_PI, va_clamp, va_tanh_bounded, tanhf wrappers

// ---------------------------------------------------------------------------
// Compile flags.
//
// AUTHORITATIVE settings live in JT8000_OptFlags.h. We pull that file in FIRST,
// before the fallback #ifndef guards below, so the user's chosen values are
// already defined and the fallbacks do nothing. This must happen here (not rely
// on the including TU) because #ifndef guards are order-sensitive: if this core
// were included before JT8000_OptFlags.h elsewhere, the fallback "1" would win
// and silently override the user's setting. (That bug shipped once — the flag
// appeared to have no effect. Including OptFlags here makes the core correct
// regardless of include order anywhere.)
//
// __has_include keeps the core usable standalone in the host harness, where
// JT8000_OptFlags.h may not be on the include path — there the fallbacks apply.
// ---------------------------------------------------------------------------
#if defined(__has_include)
#  if __has_include("JT8000_OptFlags.h")
#    include "JT8000_OptFlags.h"
#  endif
#endif

// Fallback defaults (only used if JT8000_OptFlags.h was not found above).
// 1 = use the cheap bounded Padé tanh (va_tanh_bounded, ~8 cyc).
// 0 = use true tanhf (paper-faithful, ~30 cyc on M7).
#ifndef JT_OPT_MOOGDV_TRUE_TANH
#define JT_OPT_MOOGDV_TRUE_TANH 0
#endif

// 1 = run the core at base rate (ship default).
// 2 = run at 2*fs with a light halfband decimator (less aliasing, ~2x cost).
#ifndef JT_OPT_MOOGDV_OVERSAMPLE
#define JT_OPT_MOOGDV_OVERSAMPLE 1
#endif

// ---------------------------------------------------------------------------
// Single tanh entry point for this core — switchable cheap vs faithful.
// Hoisted out of the sample loop by the compiler (the choice is compile-time).
// ---------------------------------------------------------------------------
inline float moogdv_tanh(float x)
{
#if JT_OPT_MOOGDV_TRUE_TANH
    return tanhf(x);
#else
    return va_tanh_bounded(x);   // bounded Padé [3/3], true saturation past |x|~4
#endif
}

// ---------------------------------------------------------------------------
// MoogDV4  –  one core, four multimode taps (LP4 / LP2 / BP / HP4)
//
// State layout matches the paper's four stage outputs ΔV1..ΔV4 plus the unit
// delays the discretisation requires. All POD floats for fast voice-array init.
// ---------------------------------------------------------------------------
struct MoogDV4
{
    // Stage outputs: ΔVi[n-1] (v) and ΔVi[n-2] (v2) for the second-order section
    // (eq. 16), plus w = the delayed (already tanh'd) stage input.
    float v[4]  = {0.0f, 0.0f, 0.0f, 0.0f};   // ΔVi[n-1]
    float v2[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // ΔVi[n-2]
    float w[4]  = {0.0f, 0.0f, 0.0f, 0.0f};   // tanh-input history in[n-1]

    // Global feedback unit delay: ΔV4[n-1] fed back to the input node.
    float v4Prev = 0.0f;

    // Block-rate coefficient: the paper's small-signal A (eq. 19). One per block.
    float A = 0.0f;

    // Multimode taps, refreshed every tick().
    float lp4 = 0.0f, lp2 = 0.0f, hp4 = 0.0f, bp = 0.0f;

#if JT_OPT_MOOGDV_OVERSAMPLE >= 2
    // Halfband decimation history for the primary (lp4) path.
    float osZ = 0.0f;
#endif

    // ── Maximum usable cutoff (Hz) for the current oversample setting ─────────
    // The paper's A coefficient (eq. 19) peaks at p = π·fc/fsInt ≈ 0.414 and
    // ZEROS at p = 1 (fc = fsInt/π). Past the peak, raising fc lowers the
    // effective cutoff (A falls), so the peak is the true usable maximum — like
    // any other filter in the bank, MoogDV simply has a lower max cutoff, and
    // it scales with the internal rate. The bank clamps against this so the
    // knob never drives the filter into the dead region above the zero.
    //
    //   1x: peak ≈ 0.414·fs/π  ≈ 5.8 kHz @ 44100
    //   2x: peak ≈ 0.414·2fs/π ≈ 11.6 kHz @ 44100
    //
    // p_peak solves d/dp [p(1-p)/(1+p)] = 0  ⇒  p_peak = √2 − 1 ≈ 0.41421356.
    static inline float maxCutoffHz(float fs)
    {
#if JT_OPT_MOOGDV_OVERSAMPLE >= 2
        const float fsInt = 2.0f * fs;
#else
        const float fsInt = fs;
#endif
        constexpr float P_PEAK = 0.41421356f;   // √2 − 1
        return P_PEAK * fsInt / VA_PI;
    }

    // ── Coefficient setter (paper eq. 19) ────────────────────────────────────
    // Computes A from cutoff in Hz. Called ONCE per block (control rate). The
    // tanf-free form here is a single divide — cheaper than va_compute_g.
    //   p = π·fc/fs ;  A = p·(1-p)/(1+p)
    // The bank clamps fcHz to maxCutoffHz() before calling, so p stays ≤ p_peak
    // and A stays on its rising, well-behaved branch. The internal guards below
    // are a backstop only (e.g. if the core is driven directly in a test).
    inline void setCutoff(float fcHz, float fs)
    {
#if JT_OPT_MOOGDV_OVERSAMPLE >= 2
        const float fsInt = 2.0f * fs;
#else
        const float fsInt = fs;
#endif
        float p = VA_PI * fcHz / fsInt;
        // Backstop: cap at the A peak (p = √2−1). Above this A would fall and
        // then go negative past p=1; the bank's maxCutoffHz() clamp normally
        // keeps us well below here, so this rarely engages.
        constexpr float P_PEAK = 0.41421356f;
        if (p > P_PEAK) p = P_PEAK;
        A = p * (1.0f - p) / (1.0f + p);
        if (A < 0.0f) A = 0.0f;
    }

    // ── Core single-rate sample step ─────────────────────────────────────────
    // x = input sample, k = global feedback (0..4; 4 = self-oscillation).
    // qcomp = Jupiter-style "stays loud" passband compensation (input *(1+k)).
    //
    // Per the paper, each stage is the second-order section (eq. 16):
    //     Vi[n] = A·(in[n] + in[n-1]) + (1-A)·Vi[n-1] - A·Vi[n-2]
    // where 'in' is the (tanh-shaped) stage input. The nonlinearity is the tanh
    // applied to each stage input (paper eqs. 12-13) and to the global feedback
    // summing node — exactly FIVE tanh per output sample, no iteration.
    inline void tickCore(float x, float k, bool qcomp)
    {
        const float xin = qcomp ? x * (1.0f + k) : x;

        // Input summing node with delayed global feedback (paper eq. 11/12).
        // tanh #1:
        const float in1 = moogdv_tanh(xin - k * v4Prev);

        // Stage 1: second-order section on the tanh-shaped input.
        // w[i] holds the previous (already tanh'd) input to stage i.
        const float y1 = A * (in1 + w[0]) + (1.0f - A) * v[0] - A * v2[0];
        v2[0] = v[0]; v[0] = y1; w[0] = in1;

        // Stage 2 driven by tanh of stage-1 output. tanh #2:
        const float in2 = moogdv_tanh(y1);
        const float y2  = A * (in2 + w[1]) + (1.0f - A) * v[1] - A * v2[1];
        v2[1] = v[1]; v[1] = y2; w[1] = in2;

        // Stage 3. tanh #3:
        const float in3 = moogdv_tanh(y2);
        const float y3  = A * (in3 + w[2]) + (1.0f - A) * v[2] - A * v2[2];
        v2[2] = v[2]; v[2] = y3; w[2] = in3;

        // Stage 4. tanh #4:
        const float in4 = moogdv_tanh(y3);
        const float y4  = A * (in4 + w[3]) + (1.0f - A) * v[3] - A * v2[3];
        v2[3] = v[3]; v[3] = y4; w[3] = in4;

        v4Prev = y4;

        // Multimode taps.
        lp4 = y4;
        lp2 = y2;
        // 24 dB HP via binomial residual of the four LP poles (same construction
        // NLLadderNB uses — keeps the HP tap phase-coherent with the LP taps).
        hp4 = in1 - 4.0f * y1 + 6.0f * y2 - 4.0f * y3 + y4;
        bp  = y2 - y4;
    }

    // ── Public per-sample entry ──────────────────────────────────────────────
    // 1x: just tickCore. 2x: run twice and halfband-decimate the taps.
    inline void tick(float x, float k, bool qcomp)
    {
#if JT_OPT_MOOGDV_OVERSAMPLE >= 2
        tickCore(x, k, qcomp); const float a4 = lp4, a2 = lp2, ah = hp4, ab = bp;
        tickCore(x, k, qcomp); const float b4 = lp4, b2 = lp2, bh = hp4, bb = bp;
        const float prev = osZ;
        lp4 = 0.5f * b4 + 0.25f * a4 + 0.25f * prev;
        lp2 = 0.5f * b2 + 0.25f * a2;
        hp4 = 0.5f * bh + 0.25f * ah;
        bp  = 0.5f * bb + 0.25f * ab;
        osZ = b4;
#else
        tickCore(x, k, qcomp);
#endif
    }

    inline void reset()
    {
        v[0] = v[1] = v[2] = v[3] = 0.0f;
        v2[0] = v2[1] = v2[2] = v2[3] = 0.0f;
        w[0] = w[1] = w[2] = w[3] = 0.0f;
        v4Prev = 0.0f;
        lp4 = lp2 = hp4 = bp = 0.0f;
#if JT_OPT_MOOGDV_OVERSAMPLE >= 2
        osZ = 0.0f;
#endif
    }
};
