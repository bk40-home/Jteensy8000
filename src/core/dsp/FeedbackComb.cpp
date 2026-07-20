// =============================================================================
// FeedbackComb.cpp — implementation (topology and provenance in the header)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/FeedbackComb.h"

namespace JT {

void FeedbackComb::setAmount(float amount01)
{
    // v1: constrain(amount, 0.0f, 0.99f) — the 0.99 ceiling is the loop's
    // primary stability guarantee (unity feedback never decays).
    if (amount01 < 0.0f)  amount01 = 0.0f;
    if (amount01 > 0.99f) amount01 = 0.99f;

    // Inactive -> active: start from a silent line, not 5 ms of stale
    // audio from whenever feedback was last on (difference #1 in header).
    const bool wasActive = (_gain > 0.0f);
    _gain = amount01;
    if (!wasActive && _gain > 0.0f) reset();
}

void FeedbackComb::setMix(float mix01)
{
    if (mix01 < 0.0f) mix01 = 0.0f;
    if (mix01 > 1.0f) mix01 = 1.0f;
    _mix = mix01;
}

void FeedbackComb::attachStorage(float* line)
{
    _line = line;
    reset();                          // a fresh slice starts silent
}

void FeedbackComb::reset()
{
    if (_line == nullptr) return;
    for (size_t i = 0; i < kDelaySamples; ++i) _line[i] = 0.0f;
    _idx = 0;
}

void FeedbackComb::process(float* buf, size_t n)
{
    if (_line == nullptr) return;     // storage never attached: silent no-op

    // Local copies for the hot loop (register allocation, as elsewhere).
    size_t idx = _idx;
    const float g = _gain;
    const float m = _mix;

    for (size_t i = 0; i < n; ++i) {
        // Reading before writing at the same index makes the circular
        // buffer exactly kDelaySamples deep — the tap IS the comb signal
        // from one full round earlier.
        const float tap = _line[idx];

        // Comb input: dry + feedback return.  The ±1 clamp on the WRITE is
        // the int16-mixer saturation of v1's loop (see header) — remove it
        // and gains near 0.99 ring far hotter than the hardware did.
        float combIn = buf[i] + tap * g;
        if (combIn >  1.0f) combIn =  1.0f;
        if (combIn < -1.0f) combIn = -1.0f;
        _line[idx] = combIn;

        if (++idx == kDelaySamples) idx = 0;

        // Unit output: dry stays as-is, coloured tap added at mix level —
        // v1's output mixer, collapsed to one add.
        buf[i] += tap * m;
    }

    _idx = idx;
}

} // namespace JT
