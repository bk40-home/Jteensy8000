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
// C1 (2026-10-29): fitted to the JE's RING LINES at reso 124/127 (scene 16,
// codes 48..112, LP/HP/BP agreeing): at high Q the line sits exactly at the
// section's pole frequency, so it pins fc with no trade-off against damping
// (the JT's own lines reproduce this law to 0.03 Hz / 0.001 steps).  The
// previous law (565.39 Hz, 11.746 steps/oct) came from reso-0 -3 dB edges,
// where fc trades against R0, and ran up to 0.11 oct (1.3 semitones) sharp
// at the top.  Residual against the JE lines: 11 cents RMS, 24 max.
constexpr float kF64Hz       = 549.6588f;
constexpr float kStepsPerOct = 11.95908f;

// HP/BP ceiling.  The JE's HPF -3 dB point saturates at ~13.3 kHz from code
// 120 up, with a +9.6 dB lift near the top from code 112; clamping fc here
// reproduces both.  LP has no ceiling of its own (the JE is open to 20 kHz),
// only the 0.49 fs stability margin.
constexpr float kCeilHPBPHz  = 12403.5f;   // C1 refit with the line law
constexpr float kCeilLPFrac  = 0.49f;

// ONE resonance table, log2-interpolated over the reso code (the law is
// exponential: Q climbs x1.5 .. x5 per segment).  It sets the section
// damping (Q, ring time, resonant peak) AND the JP's level law (the -24
// output trim is R/R0; the -12 trim (R/R0)^0.1).
//
// DAMPING (R = 1/2Q).  0..96 from
// the scene 14 spectral fit; 104..127 (R4, 2026-10-27) calibrated to the
// JE's DECAY in scene 15: this filter was run through the same sequence
// (1.5 s noise, 0.25 s gap, 1.5 s silent cell) and measured with the same
// line-bin slope fit, and R bisected until its dB/s matched the JE's (LP/HP/
// BP -24 mean: -556, -249, -186, -90, -37, -20 dB/s).  Matching the method,
// not a formula, matters: a -24 cascade is a repeated pole whose free ring
// is t*e^(-at), so a naive Q = 27.29 f / slope is biased.  The spectral fit
// had the top Q 3..4x too low (its -40 dB floor cannot see a sub-Hz-wide
// line); the JE at 127 is a LINEAR Q ~590 resonator, not a self-oscillator
// (syx-import/scene15-reso127-analysis-2026-10-27.md).
constexpr int   kNumQ = 10;
constexpr float kQCode[kNumQ]   = {   0.0f,  32.0f,  64.0f,  96.0f, 104.0f,
                                    112.0f, 116.0f, 120.0f, 124.0f, 127.0f };
// 0..96 refitted for C1 (scene 14, cutoff law fixed to the lines): R0 0.4802,
// R32 0.3297, R64 0.1503, R96 0.03845.  104..127 unchanged (R4 decay).
constexpr float kQLog2R[kNumQ]  = { -1.058188f, -1.600755f, -2.733652f, -4.701040f,
                                    -5.689170f, -6.837051f, -7.250299f, -8.231000f,
                                    -9.400329f, -10.196505f };

// The trim = R/R0 law was checked against the JE's LEVEL with noise in
// (scene 15 B, LPF-24 cut 64, relative to reso 0): the trim that matches the
// JE is within 0..2 dB of R/R0 at reso 104..120 and 2..4 dB at 124/127 (the
// JE slightly quieter) — so one table carries both, as it does 0..96.

// R5a level compensation (scene 16, syx-import/scene16-reso-cutoff-analysis-
// 2026-10-29.md).  At reso 120..127 the JE's noise-excited resonance stays
// near -16..-21 dBFS whatever the cutoff, while this filter (linear, trim
// R/R0) gets louder with cutoff - 5..12 dB too loud at cut 96..112, enough
// to drive the H2b shoulder into audible clipping.  The JE is NOT limiting
// (harmonics < -94 dB, normal noise crest), so this is a linear gain.
// Fitted per reso as a slope in dB per 48 codes above cut 64 (the JT's
// linear level from the exact model, calibrated on its unclipped cells
// within 1.8 dB; fit residual 0.5..1.3 dB):
//     reso   116    120    124    127
//     dB      0   -8.17  -9.17  -9.95     (per cut 64 -> 112, linear in cut)
// Linear in reso between knots, 0 at or below 116; extrapolated in cut to
// 127 (x63/48).  Applied to the -24 modes only - the -12 modes were not
// measured and their resonance is a single section.
constexpr int   kNumLc = 4;
constexpr float kLcReso[kNumLc]  = { 116.0f, 120.0f, 124.0f, 127.0f };
constexpr float kLcSlope[kNumLc] = { 0.0f, -8.17f, -9.17f, -9.95f };   // dB / 48 codes
constexpr float kLcCut0          = 64.0f;
constexpr float kDbToLog2        = 0.16609640474f;   // log2(10)/20: dB -> exp2f arg

constexpr float kR0          = 0.480235f;   // R at reso 0 (Q 1.04): trim is 1 there
constexpr float kTrim12Exp   = 0.1f;    // -12 output: (R/R0)^0.1

inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

// Piecewise-linear interpolation of log2(R) between knots, then exp2f.
// Control rate only (one call per knob change).
float knotR(float code, const float* kc, const float* kl, int n)
{
    const float c = clampf(code, 0.0f, 127.0f);
    int i = 0;                                   // segment: knots i..i+1
    while (i < n - 2 && c > kc[i + 1]) ++i;
    const float t = (c - kc[i]) / (kc[i + 1] - kc[i]);
    return exp2f(kl[i] + t * (kl[i + 1] - kl[i]));
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
    return knotR(resoCode, kQCode, kQLog2R, kNumQ);
}

float JpFilter::outputTrim(Mode m, float resoCode)
{
    const float r = damping(resoCode) * (1.0f / kR0);  // 1 at reso 0
    return isCascade(m) ? r : powf(r, kTrim12Exp);
}

float JpFilter::levelComp(Mode m, float resoCode, float cutCode)
{
    // Not calculated where it is 1 (the common case): -12 modes, low reso,
    // low cutoff.
    if (!isCascade(m) || resoCode <= kLcReso[0] || cutCode <= kLcCut0) return 1.0f;
    const float r = clampf(resoCode, kLcReso[0], kLcReso[kNumLc - 1]);
    int i = 0;
    while (i < kNumLc - 2 && r > kLcReso[i + 1]) ++i;
    const float t     = (r - kLcReso[i]) / (kLcReso[i + 1] - kLcReso[i]);
    const float slope = kLcSlope[i] + t * (kLcSlope[i + 1] - kLcSlope[i]);
    const float dB    = slope * (clampf(cutCode, kLcCut0, 127.0f) - kLcCut0) * (1.0f / 48.0f);
    return exp2f(dB * kDbToLog2);
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
