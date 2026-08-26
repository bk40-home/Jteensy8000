// =============================================================================
// SlewedValue.cpp  –  Out-of-line configuration helpers.
// =============================================================================
// The hot path (tick / tickBlock) lives in the header so it inlines. This
// file holds the rarely-called setters that recompute the cached coefficient.
// =============================================================================

#include "core/dsp/SlewedValue.h"

namespace JT {

SlewedValue::SlewedValue()
    : _g(0.0f),
      _blockDecay(0.0f),
      _sampleRate(44100.0f),
      _blockSize(128),
      _timeMs(15.0f),
      _target(0.0f),
      _settled(true)
{
    _tpt1.s = 0.0f;
    recomputeG();
    recomputeDecay();
}

void SlewedValue::setSampleRate(float fs)
{
    // Defensive floor: divide-by-zero protection if a caller hasn't set this.
    _sampleRate = (fs > 1.0f) ? fs : 44100.0f;
    recomputeG();
    recomputeDecay();
}

void SlewedValue::setBlockSize(int n)
{
    _blockSize = (n > 0) ? n : 128;
    recomputeDecay();   // _g unchanged; only the cached N-step factor moves
}

void SlewedValue::setTimeMs(float ms)
{
    // 1 µs floor — effectively "snap immediately" without divide-by-zero in
    // the fc = 1/(2π·τ) step. Anything shorter than one sample is meaningless.
    _timeMs = (ms > 0.001f) ? ms : 0.001f;
    recomputeG();
    recomputeDecay();
}

void SlewedValue::setTarget(float t)
{
    _target = t;
    // If we're already at the target (within EPS), stay settled — avoids
    // burning CPU on a slew that has nowhere to go. Otherwise wake up.
    if (fabsf(_target - _tpt1.s) < kSlewEps) {
        _tpt1.s  = _target;
        _settled = true;
    } else {
        _settled = false;
    }
}

void SlewedValue::reset(float v)
{
    _tpt1.s  = v;
    _target  = v;
    _settled = true;
}

void SlewedValue::recomputeG()
{
    // τ in seconds  ↔  smoothing cutoff fc Hz  via  fc = 1/(2π·τ).
    // g via the bank's standard pre-warp — same function the filter cores use.
    const float tauSec = _timeMs * 0.001f;
    const float fcHz   = 1.0f / (2.0f * VA_PI * tauSec);
    _g = va_compute_g(fcHz, _sampleRate);
}

void SlewedValue::recomputeDecay()
{
    // ((1-g)/(1+g))^_blockSize — exact closed-form for TPT1's per-sample
    // recurrence over a block of constant target. See SlewedValue.h.
    const float perSample = (1.0f - _g) / (1.0f + _g);
    _blockDecay = powf(perSample, (float)_blockSize);
}

} // namespace JT
