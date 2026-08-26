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

// Thermal voltage Vt, in the SAME units as the audio signal (full scale = 1.0).
//
// The paper uses Vt ~ 26 mV with signals in volts (Fig. 3 caption), i.e. tanh
// arguments of V/(2*Vt). Ported literally to normalised +/-1 audio that would
// mean tanh arguments ~19x the signal, which pins the input node in permanent
// saturation and drops the self-oscillation ~40 dB below full scale. (Verified:
// at Vt = 0.026, fc = 1 kHz, k = 4 the tail lands at 0.008 - which is exactly
// Fig. 5's +/-0.008 V y-axis, so the model is behaving correctly; it is simply
// scaled for volts, not for normalised audio.)
//
// Vt is therefore the ONE free parameter when re-scaling this model, and it
// sets where the ladder transistors saturate relative to signal level.
// Measured self-oscillation tail at fc = 1 kHz, k = 4:
//     Vt 0.026 -> 0.008   (paper's own figure)
//     Vt 0.50  -> 0.151
//     Vt 0.75  -> 0.227   <- ship default, comparable to MoogLinear4's 0.27
//     Vt 1.00  -> 0.303   (peaks past 1.0 at the top of the range)
//
// SET THIS TO 0.026f TO REPRODUCE Fig. 5 and validate the implementation.
#ifndef JT_OPT_MOOGDV_VT
#define JT_OPT_MOOGDV_VT 0.75f
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
    // State for the paper's eqs. (12)/(13) driving the bilinear integrator (14).
    //   V  = ΔVi[n]                     — the stage outputs, in signal units
    //   Tp = tanh(ΔVi[n-1] / 2Vt)       — LAST sample's stage tanh, cached
    //   dp = dΔVi[n-1]                  — derivative history for eq. (14)
    //
    // Caching Tp is what makes the tanh count come out at the paper's FIVE and
    // not nine: Fig. 3(b) shows one tanh per stage feeding BOTH the forward
    // path and, through z^-1, that stage's own subtraction node. This sample's
    // tanh(ΔVi[n]) is next sample's tanh(ΔVi[n-1]), so it is computed once.
    float V[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
    float Tp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float dp[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // Block-rate coefficients: the paper's small-signal A (eq. 19), and the
    // integrator gain 2*Vt*A that eq. (14) folds in — see setCutoff.
    float A  = 0.0f;
    float G2 = 0.0f;

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

        // Integrator gain. eq. (13) gives dΔVi = (Ictl/2C)·[tanh - tanh]; the
        // bilinear integrator (14) contributes 1/(2·fs); and eq. (19)'s A is
        // defined as Ictl/(8·fs·C·Vt). Composing them:
        //     (Ictl/2C)/(2·fs) = Ictl/(4·fs·C) = 2·Vt·A
        // so the whole chain collapses to one multiply per stage. Nothing here
        // depends on fs beyond what A already carries.
        G2 = 2.0f * JT_OPT_MOOGDV_VT * A;
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
        // Scale factor for every tanh argument: the paper's 1/(2·Vt).
        constexpr float kS      = 0.5f / JT_OPT_MOOGDV_VT;   // 1/(2Vt)

        const float xin = qcomp ? x * (1.0f + k) : x;

        // ── tanh #1: global summing node, eq. (12) ───────────────────────────
        // Note the PLUS on k·ΔV4[n-1]: the paper's ladder inverts, and eq. (12)
        // carries an overall minus (the −1 block in Fig. 3(a)). The taps below
        // undo that inversion so this filter keeps the same output polarity as
        // every other type in the bank — the paper says the polarity choice
        // "does not make any difference in practice", but it very much does
        // when one filter in a bank is out of phase with the rest.
        const float tIn = moogdv_tanh((xin + k * V[3]) * kS);

        // ── Stage 1, eq. (12): dΔV1 = −[ tanh(ΔV1[n-1]) + tanh(in) ] ─────────
        const float d0 = -(Tp[0] + tIn);
        V[0] += G2 * (d0 + dp[0]);              // bilinear integrator, eq. (14)
        const float t0 = moogdv_tanh(V[0] * kS);              // tanh #2

        // ── Stages 2..4, eq. (13): dΔVi = tanh(ΔV(i-1)[n]) − tanh(ΔVi[n-1]) ──
        // THIS is the term the previous implementation was missing. It ran the
        // small-signal form of eq. (15) — which §3.2 introduces only to derive
        // the cutoff tuning, prefaced "when low amplitude signals are applied,
        // the hyperbolic tangent in each stage operates almost linearly" — and
        // applied a tanh to the stage INPUT instead. With a linear self-term
        // the stage damping is fixed, so loop gain falls monotonically with
        // amplitude and the limit cycle settles as soon as gain reaches 1:
        // early and quiet. With the tanh'd self-term the damping falls WITH the
        // state, which is what lets the ring grow to a musical level and hold.
        // Measured, fc = 1 kHz, k = 4: the two forms move in OPPOSITE
        // directions as Vt rises — the old one dies out entirely by Vt = 0.75,
        // this one reaches 0.227.
        const float d1 = t0 - Tp[1];
        V[1] += G2 * (d1 + dp[1]);
        const float t1 = moogdv_tanh(V[1] * kS);              // tanh #3

        const float d2 = t1 - Tp[2];
        V[2] += G2 * (d2 + dp[2]);
        const float t2 = moogdv_tanh(V[2] * kS);              // tanh #4

        const float d3 = t2 - Tp[3];
        V[3] += G2 * (d3 + dp[3]);
        const float t3 = moogdv_tanh(V[3] * kS);              // tanh #5
        // Fig. 3(c): stage 4 is the odd one out — its output is tapped BETWEEN
        // the integrator and the tanh (raw ΔV4, which is both the filter output
        // and what the global feedback carries), and the tanh sits only in its
        // own feedback branch. Stages 1..3 (Fig. 3(b)) pass the tanh'd value
        // forward instead, which is why t0..t2 feed the next stage while t3
        // feeds nothing but Tp.

        Tp[0] = t0; Tp[1] = t1; Tp[2] = t2; Tp[3] = t3;
        dp[0] = d0; dp[1] = d1; dp[2] = d2; dp[3] = d3;

        // ── Multimode taps ───────────────────────────────────────────────────
        // All from the RAW ΔVi (Fig. 3(c)), never the tanh'd forward value —
        // the tanh'd signal is dimensionless and would not share a scale with
        // the binomial residual below. Negated to undo the ladder's inversion.
        lp4 = -V[3];
        lp2 = -V[1];
        bp  =  V[3] - V[1];                     // = -(V[1] - V[3])

        // 24 dB HP by binomial residual of the four LP poles. The x term must be
        // the ladder input expressed in the SAME units as ΔVi. That is the
        // PRE-tanh sum, negated: at DC, stage 1 settles where d0 = 0, i.e.
        // tanh(V0·kS) = −tanh(xin·kS), so V0 = −xin exactly. Using the
        // post-tanh value instead leaves a residual (measured −0.002 at a 0.25
        // DC input) because tanh(a)·2Vt ≠ a once the argument is non-trivial.
        // With the pre-tanh form every stage sits at −xin and the residual is
        // −xin·(1 − 4 + 6 − 4 + 1) = 0 exactly, at any level.
        const float ladderIn = -(xin + k * V[3]);
        hp4 = -(ladderIn - 4.0f * V[0] + 6.0f * V[1] - 4.0f * V[2] + V[3]);
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
        V[0]  = V[1]  = V[2]  = V[3]  = 0.0f;
        Tp[0] = Tp[1] = Tp[2] = Tp[3] = 0.0f;
        dp[0] = dp[1] = dp[2] = dp[3] = 0.0f;
        lp4 = lp2 = hp4 = bp = 0.0f;
#if JT_OPT_MOOGDV_OVERSAMPLE >= 2
        osZ = 0.0f;
#endif
    }
};
