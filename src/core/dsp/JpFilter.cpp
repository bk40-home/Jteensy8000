// =============================================================================
// JpFilter.cpp — JP-8000 filter laws (provenance and model in the header)
// =============================================================================
// Every constant here is a least-squares fit to the JE-8086 scene 14 noise
// capture (capture_tools/jtclips/fitjp_final.py -> jpfilter_final.json),
// evaluated with THIS filter's own 48 kHz TPT response, so what was fitted is
// exactly what runs.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/JpFilter.h"

#include <math.h>   // exp2f, powf — control rate only

namespace JT {

namespace {

// Cutoff law: fc(64) and knob steps per octave.
constexpr float kF64Hz       = 565.3909f;
constexpr float kStepsPerOct = 11.74617f;

// HP/BP ceiling.  The JE's HPF -3 dB point saturates at ~13.3 kHz from code
// 120 up, with a +9.6 dB lift near the top from code 112; clamping fc here
// reproduces both.  LP has no ceiling of its own (the JE is open to 20 kHz),
// only the 0.49 fs stability margin.
constexpr float kCeilHPBPHz  = 12347.9f;
constexpr float kCeilLPFrac  = 0.49f;

// Resonance knots: log2(R) at reso codes 0, 32, 64, 96, 127.  log2 so the
// interpolation is exponential (Q climbs ~x1.5, x2.2, x3.9, x11.6 per
// segment — far from linear in the knob).
constexpr float kKnotCode[5] = { 0.0f, 32.0f, 64.0f, 96.0f, 127.0f };
constexpr float kKnotLog2R[5] = { -1.000000f, -1.548642f, -2.665412f,
                                  -4.612856f, -8.151481f };

constexpr float kR0          = 0.5f;    // R at reso 0 (Q = 1)
constexpr float kTrim12Exp   = 0.1f;    // -12 output: (R/R0)^0.1

inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

} // namespace

float JpFilter::cutoffHz(float code)
{
    return kF64Hz * exp2f((clampf(code, 0.0f, 127.0f) - 64.0f) * (1.0f / kStepsPerOct));
}

float JpFilter::ceilingHz(Mode m, float fs)
{
    const bool lp = (m == Mode::LP24 || m == Mode::LP12);
    const float hb = (kCeilHPBPHz < kCeilLPFrac * fs) ? kCeilHPBPHz : kCeilLPFrac * fs;
    return lp ? kCeilLPFrac * fs : hb;
}

float JpFilter::damping(float resoCode)
{
    const float c = clampf(resoCode, 0.0f, 127.0f);
    int i = 0;                                   // segment: knots i..i+1
    while (i < 3 && c > kKnotCode[i + 1]) ++i;
    const float t = (c - kKnotCode[i]) / (kKnotCode[i + 1] - kKnotCode[i]);
    return exp2f(kKnotLog2R[i] + t * (kKnotLog2R[i + 1] - kKnotLog2R[i]));
}

float JpFilter::outputTrim(Mode m, float R)
{
    const float r = R * (1.0f / kR0);           // 1 at reso 0, falls with Q
    return isCascade(m) ? r : powf(r, kTrim12Exp);
}

void JpFilter::setCoeffs(float g, float R)
{
    _g = g;
    _a = 2.0f * R + g;
    _d = 1.0f / (1.0f + 2.0f * R * g + g * g);
}

void JpFilter::reset()
{
    _s1a = _s2a = _s1b = _s2b = 0.0f;
}

} // namespace JT
