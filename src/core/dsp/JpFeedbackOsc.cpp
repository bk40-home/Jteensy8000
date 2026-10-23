// =============================================================================
// JpFeedbackOsc.cpp — implementation (design, provenance and limits in the .h)
// =============================================================================
// The four 8 × 9 tables below are MEASURED from JE-8086 (JP-8000 DSP
// emulation) fresh-note grids, 2026-10-22: Feedback rows 0/32/48/64/80/96/112/
// 127, Harmonics columns 0/16/32/48/64/80/96/112/127 (knob bytes), each cell a
// 1.5 s note analysed once settled (0.6–0.9 s).  They are the starting point
// for the by-ear tuning pass; edit the numbers, not the structure.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/JpFeedbackOsc.h"

#include <cmath>   // exp2f, floorf

#include "core/dsp/FastMath.h"

namespace JT {

namespace {

constexpr int kFbRows   = 8;
constexpr int kHarmCols = 9;

// Grid axes in knob units (byte / 127).  Non-uniform on Feedback (the grid
// skipped 1..31 where the JP stays a plain saw → first doubling).
const float kFbAxis[kFbRows] JT_FLASH_DATA = {
    0.0f / 127.0f,  32.0f / 127.0f,  48.0f / 127.0f,  64.0f / 127.0f,
    80.0f / 127.0f, 96.0f / 127.0f, 112.0f / 127.0f, 127.0f / 127.0f };
const float kHarmAxis[kHarmCols] JT_FLASH_DATA = {
    0.0f / 127.0f,  16.0f / 127.0f,  32.0f / 127.0f,  48.0f / 127.0f, 64.0f / 127.0f,
    80.0f / 127.0f, 96.0f / 127.0f, 112.0f / 127.0f, 127.0f / 127.0f };

// log2(blocks per master cycle), C3 = 130.81 Hz fresh notes (feedback_grid.wav, settled 0.6-0.9 s)
const float kLog2BlocksC3[kFbRows][kHarmCols] JT_FLASH_DATA = {
    {  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f },   // FB   0
    {  1.0000f,  0.9814f,  1.4425f,  1.4695f,  1.3585f,  1.4150f,  0.8643f,  0.0000f,  0.0000f },   // FB  32
    {  2.3870f,  1.5850f,  2.8231f,  1.5850f,  2.0000f,  2.3219f,  2.9386f,  2.4357f,  3.5123f },   // FB  48
    {  1.5972f,  3.6542f,  3.0000f,  2.6155f,  2.0000f,  4.4374f,  3.0000f,  3.5757f,  1.0184f },   // FB  64
    {  2.5850f,  4.0000f,  3.0046f,  4.5538f,  2.0000f,  4.4561f,  3.4594f,  3.9767f,  0.9044f },   // FB  80
    {  2.0000f,  4.0000f,  3.0046f,  3.9482f,  2.0455f,  3.9362f,  2.8895f,  5.0677f,  1.5850f },   // FB  96
    {  1.0000f,  3.9977f,  4.0000f,  3.9790f,  2.0000f,  2.3145f,  2.9338f,  4.2730f,  2.3219f },   // FB 112
    {  1.0000f,  4.0000f,  3.9977f,  3.9930f,  2.0000f,  3.3621f,  2.9289f,  4.9506f,  1.5972f },   // FB 127
};
// log2(blocks per master cycle), C5 = 523.25 Hz fresh notes (feedback_grid_C5.wav, settled 0.6-0.9 s)
const float kLog2BlocksC5[kFbRows][kHarmCols] JT_FLASH_DATA = {
    {  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f },   // FB   0
    {  1.0000f,  1.0000f,  1.1112f,  1.0589f,  1.0000f,  0.9673f,  0.0000f,  0.0000f,  0.0000f },   // FB  32
    {  1.0000f,  1.8047f,  2.2024f,  1.3033f,  2.0000f,  2.1637f,  0.6155f,  0.0000f,  0.0000f },   // FB  48
    {  1.0000f,  3.8833f,  3.0000f,  2.4271f,  2.0000f,  2.1842f,  2.9988f,  0.2920f,  0.0000f },   // FB  64
    {  1.0000f,  3.9884f,  2.9988f,  1.5632f,  2.1176f,  3.4331f,  2.6079f,  1.8643f,  0.0000f },   // FB  80
    {  1.0000f,  3.0589f,  2.9988f,  1.3476f,  1.9977f,  3.4636f,  2.9802f,  1.7753f,  0.0000f },   // FB  96
    {  1.0000f,  3.0809f,  2.9988f,  1.3071f,  1.9977f,  2.1112f,  3.0161f,  1.7861f,  0.0000f },   // FB 112
    {  1.0000f,  3.0994f,  2.9988f,  1.3145f,  1.9977f,  2.1261f,  3.0092f,  1.6483f,  0.0000f },   // FB 127
};
// inner-phase pull toward the master per master wrap: 1 = hard lock where the JE pattern holds
// still, 0 where it wanders (0.05 in the unsettled/chaotic cells)
const float kLockPull[kFbRows][kHarmCols] JT_FLASH_DATA = {
    {  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f },   // FB   0
    {  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f },   // FB  32
    {  0.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  0.0000f },   // FB  48
    {  0.0000f,  1.0000f,  1.0000f,  1.0000f,  1.0000f,  0.0000f,  1.0000f,  1.0000f,  0.0000f },   // FB  64
    {  0.0000f,  0.0000f,  0.0000f,  0.0500f,  1.0000f,  0.0500f,  0.0000f,  0.0000f,  0.0000f },   // FB  80
    {  0.0000f,  0.0000f,  0.0000f,  0.0500f,  1.0000f,  0.0500f,  0.0000f,  0.0500f,  0.0000f },   // FB  96
    {  0.0000f,  0.0000f,  0.0000f,  0.0500f,  0.0000f,  0.0500f,  0.0000f,  0.0000f,  0.0000f },   // FB 112
    {  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0500f,  0.0000f,  0.0500f,  0.0000f },   // FB 127
};
// self-modulation of the inner rate by the output: non-zero only where the JP never settles cycle-to-cycle
const float kChaos[kFbRows][kHarmCols] JT_FLASH_DATA = {
    {  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f },   // FB   0
    {  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f },   // FB  32
    {  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f },   // FB  48
    {  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f },   // FB  64
    {  0.0000f,  0.0000f,  0.0000f,  0.3000f,  0.0000f,  0.3000f,  0.0000f,  0.0000f,  0.0000f },   // FB  80
    {  0.0000f,  0.0000f,  0.0000f,  0.3000f,  0.0000f,  0.3000f,  0.0000f,  0.3000f,  0.0000f },   // FB  96
    {  0.0000f,  0.0000f,  0.0000f,  0.3000f,  0.0000f,  0.3000f,  0.0000f,  0.0000f,  0.0000f },   // FB 112
    {  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.3000f,  0.0000f,  0.3000f,  0.0000f },   // FB 127
};
// random-walk amount of the inner phase (x kWanderStep per master wrap): the JE shape change
// between 0.6-0.7 s and 1.2-1.3 s of a held note (0 where < 0.15, capped at 1.6)
const float kWander[kFbRows][kHarmCols] JT_FLASH_DATA = {
    {  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f },   // FB   0
    {  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f },   // FB  32
    {  0.6366f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  1.0392f },   // FB  48
    {  1.1920f,  0.0000f,  0.0000f,  0.0000f,  0.0000f,  0.4418f,  0.0000f,  0.0000f,  1.0770f },   // FB  64
    {  0.8877f,  0.7704f,  0.3512f,  1.6000f,  0.0000f,  1.3340f,  0.4895f,  0.4085f,  1.0142f },   // FB  80
    {  0.8989f,  0.8869f,  0.8842f,  1.3991f,  0.0000f,  1.3806f,  1.4718f,  0.3835f,  0.8695f },   // FB  96
    {  0.6051f,  0.9121f,  1.5614f,  1.2113f,  0.7811f,  0.9952f,  1.0078f,  0.3685f,  0.9715f },   // FB 112
    {  0.6068f,  0.9024f,  1.5018f,  0.7251f,  0.9175f,  1.0272f,  0.9512f,  0.8828f,  0.8601f },   // FB 127
};

// --- fixed constants (measured or derived; provenance in the header) -------
// Pitch law (D3a, 2026-10-23): NEAREST ANCHOR, no interpolation.  The block
// count map is taken from the C3 grid below kAnchorSplitHz and from the C5
// grid at or above it, and is never extrapolated outside the two captures.
// Why (pitch-spread capture at 65/131/262/523 Hz, project doc osc-shapes/
// jfbk-pitch-spread-analysis-2026-10-23):
//   * the JE locks to whole counts (4 / 8 / 16 ...).  At C4 it takes the C3 or
//     the C5 count — never a value in between — so the v0 log-interpolation
//     produced counts (6, 11.3, 11.7) the JE never shows;
//   * one octave below C3 the JE stays near its C3 counts; the v0
//     extrapolation gave 17-23 blocks where the JE gives 5.5-16.
// Hits within 1/3 octave over the 48 test cells: v0 34 -> D3a 40.
// The split sits 1.5 octaves above C3 (F#4, 370 Hz): C4 cells matched the
// C3 grid more often than the C5 grid (scored at splits C4 and F#4; F#4 won).
// Trade-off (accepted): a cell whose C3 and C5 counts differ changes count
// abruptly at the split.  The JE also switches abruptly, at a cell-dependent
// pitch that has not been measured.
constexpr float kAnchorSplitHz = 369.9944f;   // C3 (130.8128 Hz) x 2^1.5 = F#4

// Base wave level relative to the blocks.  JE: zero-feedback saw jump ≈ 0.20
// pk-pk vs block swing ≈ 0.28 → blocks are 1.4× the saw; blocks are ±1, so
// the saw is ±1/1.4.  Keeps the whole wave inside ±1 nominal before the HPF.
constexpr float kSawLevel = 1.0f / 1.4f;

// Saw → blocks crossfade over Feedback: plain saw below ~10 %, fully blocks by
// ~40 % (JE: FB 32 shows saw + extra steps, FB 48 is already block-shaped).
constexpr float kMixFbStart = 0.10f;
constexpr float kMixFbSpan  = 0.30f;

// The measured fixed-time high-pass: τ = 1.92 ms (C3 and C5 fits agree to
// 0.3 %), as a 1-pole y = a·(y + x − x_prev), a = τ / (τ + 1/fs).
constexpr float kHpTau = 1.92e-3f;
constexpr float kHpA   = kHpTau / (kHpTau + 1.0f / kSampleRate);

// Inner oscillator Nyquist guard: the chaotic cells carry 20–34 blocks per
// cycle at C3, which would pass Nyquist on high notes.  The JP aliases, but a
// rate past Nyquist would fold to a LOWER pitch — clamp instead.
constexpr float kMaxInnerInc = 0.45f;

// Pattern wander (JE: at most high-feedback settings a held note keeps its
// block COUNT but keeps rearranging the blocks — shape change ≈ 0.9 between
// windows 0.6 s apart, with almost no net slide).  Modelled as a leaky random
// walk added to the inner phase once per master cycle: step = kWander(cell) ×
// kWanderStep × U(−1, 1), leak pulls the walk back toward zero so it wanders
// rather than drifts away.  kWanderStep calibrated ON THIS C++ CODE against the
// C3 grid, each cell averaged over 4 voice seeds (sweep 0.08..0.17): at 0.11
// the model/JE shape-change ratio has median 1.03 (IQR 0.91-1.37) across the
// JE's wandering cells, wander/locked class matches in 72/72 cells, and block
// counts are unaffected (72/72 C3, 68/72 C5).
constexpr float kWanderStep = 0.11f;
constexpr float kWanderLeak = 0.01f;

inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// Bilinear read of one 8 × 9 table at (fb, k), both 0..1.  Axis searches are
// a few compares on 8/9-entry arrays; this runs once per law change only.
float bilinear(const float (&t)[kFbRows][kHarmCols], float fb, float k)
{
    int r = 0;
    while (r < kFbRows - 2 && fb > kFbAxis[r + 1]) ++r;
    int c = 0;
    while (c < kHarmCols - 2 && k > kHarmAxis[c + 1]) ++c;
    const float fr = clampf((fb - kFbAxis[r]) / (kFbAxis[r + 1] - kFbAxis[r]), 0.0f, 1.0f);
    const float fc = clampf((k - kHarmAxis[c]) / (kHarmAxis[c + 1] - kHarmAxis[c]), 0.0f, 1.0f);
    const float top = t[r][c]     + (t[r][c + 1]     - t[r][c])     * fc;
    const float bot = t[r + 1][c] + (t[r + 1][c + 1] - t[r + 1][c]) * fc;
    return top + (bot - top) * fr;
}

} // namespace

// -----------------------------------------------------------------------------
// Control plane — setters only mark the law dirty; updateLaw() runs once.
// -----------------------------------------------------------------------------
void JpFeedbackOsc::setFrequency(float hz)
{
    if (hz == _freq) return;
    _freq = hz;
    _lawDirty = true;
}

void JpFeedbackOsc::setHarmonics(float k01)
{
    k01 = clampf(k01, 0.0f, 1.0f);
    if (k01 == _harm) return;
    _harm = k01;
    _lawDirty = true;
}

void JpFeedbackOsc::setFeedback(float fb01)
{
    fb01 = clampf(fb01, 0.0f, 1.0f);
    if (fb01 == _fb) return;
    _fb = fb01;
    _lawDirty = true;
}

void JpFeedbackOsc::noteOn(float phase01)
{
    _phase    = phase01 - floorf(phase01);
    _inner    = 0.0f;          // inner falling edge aligned to the master wrap
    _hpPrevIn = 0.0f;
    _hpOut    = 0.0f;          // also clears the chaos feedback term
    _walk     = 0.0f;          // pattern wander restarts from the aligned state
}

float JpFeedbackOsc::nextNoise()
{
    uint32_t x = _rng;         // xorshift32 — deterministic under seedNoise()
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _rng = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);   // −1..1
}

void JpFeedbackOsc::updateLaw()
{
    _lawDirty = false;
    const float hz = clampf(_freq, 0.0f, kSampleRate * 0.45f);
    _inc = hz / kSampleRate;

    if (_fb <= 0.0f) {
        // Zero feedback: the exact JP base — plain saw → HPF, Harmonics inert.
        _blocks = 1.0f; _pull = 1.0f; _chaos = 0.0f; _mix = 0.0f; _wander = 0.0f;
        _innerInc = _inc;
        return;
    }

    // Blocks per master cycle from the NEAREST anchor map (D3a — see
    // kAnchorSplitHz): one compare + one bilinear read, no log2f.
    const bool  useC5 = (hz >= kAnchorSplitHz);
    float l2 = bilinear(useC5 ? kLog2BlocksC5 : kLog2BlocksC3, _fb, _harm);
    if (l2 < 0.0f) l2 = 0.0f;                 // never fewer than 1 block
    _blocks = exp2f(l2);

    _innerInc = _blocks * _inc;
    if (_innerInc > kMaxInnerInc) _innerInc = kMaxInnerInc;

    _pull  = clampf(bilinear(kLockPull, _fb, _harm), 0.0f, 1.0f);
    _chaos = bilinear(kChaos, _fb, _harm);
    _wander = bilinear(kWander, _fb, _harm) * kWanderStep;
    _mix   = clampf((_fb - kMixFbStart) / kMixFbSpan, 0.0f, 1.0f);
}

// -----------------------------------------------------------------------------
// Audio plane
// -----------------------------------------------------------------------------
void JpFeedbackOsc::render(float* out, size_t n, const float* fmBuf, float fmOctaves)
{
    if (_lawDirty) updateLaw();

    const float sawG  = (1.0f - _mix) * kSawLevel;
    const float blkG  = _mix;
    const float pull  = _pull;
    const float chaos = _chaos;
    const float wander = _wander;

    float ph   = _phase;
    float psi  = _inner;
    float hpIn = _hpPrevIn;
    float hp   = _hpOut;

    for (size_t i = 0; i < n; ++i) {
        // Exponential FM (pitch env / JtXmod): one fastPow2 shared by both
        // phases, so the block count per cycle survives pitch modulation.
        float mul = 1.0f;
        if (fmBuf != nullptr) mul = FastMath::fastPow2(fmBuf[i] * fmOctaves);

        ph += _inc * mul;
        if (ph >= 1.0f) {
            ph -= floorf(ph);
            // Master wrap: pull the inner phase toward a whole inner cycle.
            // e is the signed distance to the nearest cycle boundary.
            const float e = (psi >= 0.5f) ? (psi - 1.0f) : psi;
            psi -= pull * e;
            // Pattern wander (drift cells only — 0 elsewhere costs a compare).
            if (wander > 0.0f) {
                const float d = wander * nextNoise() - kWanderLeak * _walk;
                _walk += d;
                psi   += d;
            }
            if (psi < 0.0f) psi -= floorf(psi);
            if (psi >= 1.0f) psi -= floorf(psi);
        }

        // Inner block oscillator; chaos cells modulate its rate by the output.
        psi += _innerInc * mul * (1.0f + chaos * hp);
        if (psi >= 1.0f || psi < 0.0f) psi -= floorf(psi);

        // Two-level block: falls at the inner wrap (lined up with the master
        // wrap when locked, so the saw reset and a block edge coincide — the
        // JE's reset counts depend on that alignment).
        const float blk = (psi < 0.5f) ? -1.0f : 1.0f;
        const float v   = sawG * (2.0f * ph - 1.0f) + blkG * blk;

        hp   = kHpA * (hp + v - hpIn);         // fixed-time 1-pole HPF
        hpIn = v;
        out[i] = hp;
    }

    _phase    = ph;
    _inner    = psi;
    _hpPrevIn = hpIn;
    _hpOut    = hp;
}

} // namespace JT
