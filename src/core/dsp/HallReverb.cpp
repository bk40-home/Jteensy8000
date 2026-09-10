// =============================================================================
// HallReverb.cpp — see HallReverb.h for the topology and the cost accounting.
// =============================================================================

#include "core/dsp/HallReverb.h"

#include <string.h>   // memset

namespace JT {

namespace {
inline float clamp01(float x) { return (x < 0.0f) ? 0.0f : (x > 1.0f) ? 1.0f : x; }
}

JT_COLD void HallReverb::begin(float* pool)
{
    _pool = pool;
    if (!_pool) return;                  // inert; processBlock bails on null

    memset(_pool, 0, kPoolFloats * sizeof(float));
    memset(_apBuf, 0, sizeof(_apBuf));

    float* p = _pool;
    for (uint8_t i = 0; i < kLines; ++i) {
        _line[i].buf      = p;
        _line[i].len      = kLineLen[i];
        _line[i].writeIdx = 0;
        p += kLineLen[i];

        _lineLP[i].clear(); _lineLP[i].coeff = _hiDampCoeff;
        _lineHP[i].clear(); _lineHP[i].coeff = _loDampCoeff;
        _fb[i] = 0.0f;
    }

    float* d = _apBuf;
    for (uint8_t i = 0; i < 4; ++i) {
        _ap[i].dl.buf      = d;
        _ap[i].dl.len      = kApLen[i];
        _ap[i].dl.writeIdx = 0;
        _ap[i].gain        = kApGain;
        d += kApLen[i];
    }

    for (uint8_t i = 0; i < 2; ++i) {
        _masterLP[i].clear(); _masterLP[i].coeff = 0.0f;
        _masterHP[i].clear(); _masterHP[i].coeff = 0.0f;
    }
}

void HallReverb::clearTail()
{
    // Same contract as the other algorithms: drop the RECIRCULATING state,
    // never memset the pool — 57 KB of PSRAM would overrun a block.
    for (uint8_t i = 0; i < kLines; ++i) {
        _fb[i] = 0.0f;
        _lineLP[i].clear();
        _lineHP[i].clear();
    }
    for (uint8_t i = 0; i < 4; ++i) _ap[i].clear();
    for (uint8_t i = 0; i < 2; ++i) { _masterLP[i].clear(); _masterHP[i].clear(); }
}

// ---------------------------------------------------------------------------
// size -> feedback gain.
//
// Because the Householder matrix is orthogonal (lossless), this gain is the
// ONLY thing setting the decay — nothing else in the loop discards energy.
// That is what makes the control predictable here in a way it is not on a
// plate, where the tank allpasses and taps also bleed.
//
// Ceiling of 0.93: the longest line is 5011 samples (114 ms), so 0.93 per lap
// is about -0.63 dB per 114 ms, or roughly a 10 s RT60.  Above that the tail
// outlasts any musical use and freeze() is the honest way to hold forever.
// ---------------------------------------------------------------------------
void HallReverb::setSize(float n)
{
    if (_frozen) return;                     // freeze owns the decay
    _decay = 0.35f + 0.58f * clamp01(n);     // 0.35 .. 0.93
}

void HallReverb::setHiDamp(float n)
{
    // Same 0.85 ceiling as the plate and room, so the knob feels identical
    // across all three algorithms.
    _hiDampCoeff = clamp01(n) * 0.85f;
    for (uint8_t i = 0; i < kLines; ++i) _lineLP[i].coeff = _hiDampCoeff;
}

void HallReverb::setLoDamp(float n)
{
    _loDampCoeff = clamp01(n) * 0.85f;
    for (uint8_t i = 0; i < kLines; ++i) _lineHP[i].coeff = _loDampCoeff;
}

void HallReverb::setLowpass(float n)
{
    _masterLpCoeff = clamp01(n) * 0.95f;
    _masterLP[0].coeff = _masterLpCoeff;
    _masterLP[1].coeff = _masterLpCoeff;
}

void HallReverb::setHipass(float n)
{
    _masterHpCoeff = clamp01(n) * 0.95f;
    _masterHP[0].coeff = _masterHpCoeff;
    _masterHP[1].coeff = _masterHpCoeff;
}

void HallReverb::setFreeze(bool on)
{
    if (_frozen == on) return;
    _frozen = on;
    if (on) { _savedDecay = _decay; _decay = 1.0f; }
    else    { _decay = _savedDecay; }
}

// ---------------------------------------------------------------------------
// processBlock — the FDN loop.
//
// PSRAM traffic is 8 accesses per sample: four line reads, four line writes.
// The four input allpasses, eight damping filters and the whole mixing matrix
// are DTCM or register work.
// ---------------------------------------------------------------------------
void HallReverb::processBlock(float* left, float* right, size_t n,
                              float mixStart, float mixEnd)
{
    if (!_pool) return;

    // Block-constant caches — member loads hoisted out of the loop.
    const float decay      = _decay;
    const float inputGain  = _frozen ? 0.02f : 1.0f;   // freeze bleed
    const bool  doMasterLP = (_masterLpCoeff > 0.001f);
    const bool  doMasterHP = (_masterHpCoeff > 0.001f);

    float       mix    = mixStart;
    const float mixInc = (n > 0) ? ((mixEnd - mixStart) / (float)n) : 0.0f;

    for (size_t i = 0; i < n; ++i) {
        const float inL = left[i];
        const float inR = right[i];
        const float monoIn = (inL + inR) * 0.5f * inputGain;

        // Input diffusion (DTCM).  Four lines alone are sparse in the first
        // few milliseconds; this fills the early density in.
        float diffused = monoIn;
        for (uint8_t a = 0; a < 4; ++a) diffused = _ap[a].process(diffused);

        // Read every line, damping INSIDE the loop so the tail darkens
        // progressively instead of being filtered once on the way out.
        float d[kLines];
        for (uint8_t k = 0; k < kLines; ++k) {
            const float raw = _line[k].read(_line[k].len - 1);
            d[k] = _lineHP[k].process(_lineLP[k].process(raw));
        }

        // Householder mixing: out_i = d_i - (2/N) * sum(d).  Three adds, one
        // multiply, four subtracts — the entire 4x4 matrix.
        const float s = (d[0] + d[1] + d[2] + d[3]) * kHouseholder;

        for (uint8_t k = 0; k < kLines; ++k) {
            // Alternating input polarity across the lines decorrelates the
            // injection; feeding all four in phase makes the onset sound like
            // a single flam rather than a diffuse wash.
            const float inj = (k & 1u) ? -diffused : diffused;
            _line[k].write(inj + (d[k] - s) * decay);
            _fb[k] = d[k];
        }

        // Stereo tap.  SUBTRACTIVE pairs, not additive: summing the lines
        // pulls the image to the centre, differencing them opens it out.
        float wetL = (d[0] - d[2]) * kWetScale;
        float wetR = (d[1] - d[3]) * kWetScale;

        if (doMasterLP) { wetL = _masterLP[0].process(wetL); wetR = _masterLP[1].process(wetR); }
        if (doMasterHP) { wetL = _masterHP[0].process(wetL); wetR = _masterHP[1].process(wetR); }

        float outL = inL + mix * wetL;
        float outR = inR + mix * wetR;

        // Soft clip — free unless we actually overshoot (same form as plate/room).
        if (outL > 1.0f || outL < -1.0f) outL = outL / (1.0f + fabsf(outL));
        if (outR > 1.0f || outR < -1.0f) outR = outR / (1.0f + fabsf(outR));

        left[i]  = outL;
        right[i] = outR;
        mix += mixInc;
    }
}

} // namespace JT
