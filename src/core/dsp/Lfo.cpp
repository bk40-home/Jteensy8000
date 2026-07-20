// =============================================================================
// Lfo.cpp — implementation (model and rationale in Lfo.h)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/dsp/Lfo.h"

#include <cmath>   // fabsf only — float-suffixed per project rules

#include "core/dsp/FastMath.h"

namespace JT {

float Lfo::nextRandom01()
{
    // xorshift32 — identical construction to OscSection::nextNoise / OscCore's
    // noise source, so every per-block random draw in the engine comes from
    // the same cheap, well-understood family.
    uint32_t x = _rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _rng = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

float Lfo::tickBlock()
{
    // Delay-ramp advance (spec §3 decision #7): kBlockMs per block, no
    // millis() in core/ — the block period is the only clock this needs.
    if (_ramping) {
        _delayGain += kBlockMs / _delayMs;
        if (_delayGain >= 1.0f) { _delayGain = 1.0f; _ramping = false; }
    }

    // Phase accumulator, wraps at 1.0 (block-rate — see header UPDATE RATE).
    _phase += _rateHz / kBlocksPerSec;
    bool wrapped = false;
    if (_phase >= 1.0f) { _phase -= 1.0f; wrapped = true; }

    float value;
    switch (_wave) {
        case kSin: value = FastMath::fastSin01(_phase); break;
        case kTri: value = 1.0f - 4.0f * fabsf(_phase - 0.5f); break;
        case kSaw: value = 2.0f * _phase - 1.0f; break;
        case kSqr: value = (_phase < 0.5f) ? 1.0f : -1.0f; break;
        case kSampleHold:
            // Held for the whole cycle; only redrawn at the wrap.
            if (wrapped) _shValue = nextRandom01();
            value = _shValue;
            break;
        default:   // kNoise: fresh per block, not gated by phase at all.
            value = nextRandom01();
            break;
    }

    return value * _delayGain;
}

} // namespace JT
