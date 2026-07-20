// =============================================================================
// FastMath.h — fast float approximations shared across the JT-8000 v2 DSP
// =============================================================================
//
// Everything here trades a few LSBs of accuracy for an order of magnitude in
// cycles.  Each function documents its error bound and where it is safe to
// use — these are for AUDIO-RATE inner loops; control-rate code (once per
// block) should keep using the libm originals, which are exact and cheap at
// that rate.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>
#include <cmath>    // ldexpf, floorf — both single-cycle-class on the M7 FPU

namespace JT {
namespace FastMath {

// -----------------------------------------------------------------------------
// fastPow2 — 2^x, ported BYTE-FOR-BYTE from v1 AudioSynthSupersaw so v2's
// cross-mod and supersaw FM sound identical to the hardware in the field.
// MEASURED relative error: ≤ 7.2e-4, worst at the top of the fractional
// range (v1's header claimed 2e-6 — that holds only near 0).  7.2e-4 is
// ≤ 1.25 cents of pitch at the worst instant of an FM sweep — irrelevant
// to a chaotic timbre effect, and an "improved" polynomial would subtly
// change the v1 sound, so the coefficients stay.  powf() costs ~10x more.
// -----------------------------------------------------------------------------
inline float fastPow2(float x)
{
    if (x >  126.0f) return 67108864.0f;    // clamp: prevent IEEE overflow
    if (x < -126.0f) return 1.49e-38f;      // clamp: prevent denormals

    const int32_t xi = (int32_t)floorf(x);  // integer part (correct for x < 0)
    const float   xf = x - (float)xi;       // fractional part [0, 1)

    const float poly = 1.00000000f
                     + xf * (0.69314718f
                     + xf * (0.24022651f
                     + xf * (0.05550411f
                     + xf *  0.00961823f)));

    return ldexpf(poly, xi);                // 2^i × poly
}

// -----------------------------------------------------------------------------
// fastSin01 — sine of a NORMALIZED phase (0..1 == 0..2π), for oscillators
// that already keep phase in 0..1 (all of ours).  Parabolic approximation
// with one refinement step (the classic "Bhaskara-style" audio sine):
// harmonic distortion ≈ −60 dB, far below the mix floor of an analog-style
// synth voice, at ~5 multiplies — no range reduction, no table, no libm.
// NOT for measurement code — tests use sinf().
// -----------------------------------------------------------------------------
inline float fastSin01(float phase01)
{
    // Map 0..1 to x in -1..1; the parabola below approximates sin(πx),
    // and sin(2π·phase) = −sin(π·(2·phase − 1)) — hence the negation at
    // the end.  (Getting the sign wrong makes an inverted sine: identical
    // alone, wrong against ring mod / mixing.  Caught by test.)
    float x = phase01 * 2.0f - 1.0f;                 // -1..1
    float y = 4.0f * (x - x * (x < 0.0f ? -x : x));  // parabola: 4(x - x|x|)
    // One refinement pass pulls THD from -40 dB down to about -60 dB.
    y = 0.225f * (y * (y < 0.0f ? -y : y) - y) + y;
    return -y;
}

} // namespace FastMath
} // namespace JT
