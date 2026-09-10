// =============================================================================
// ReverbRack.cpp — see ReverbRack.h for the design rationale.
// =============================================================================

#include "core/dsp/ReverbRack.h"

#include "core/AudioConfig.h"   // JT_COLD

namespace JT {

IReverb* ReverbRack::instance(int algo)
{
    switch (algo) {
        case kPlate:   return &_plate;
        case kShimmer: return &_shimmer;
        case kRoom:    return &_room;
        case kHall:    return &_hall;
        default:       return nullptr;
    }
}

const char* ReverbRack::activeName() const
{
    switch (_active) {
        case kPlate:   return _plate.name();
        case kShimmer: return _shimmer.name();
        case kRoom:    return _room.name();
        case kHall:    return _hall.name();
        default:       return "none";
    }
}

// ---------------------------------------------------------------------------
// begin — slice one region across every algorithm.
// ---------------------------------------------------------------------------
JT_COLD void ReverbRack::begin(float* base)
{
    _failed = 0;

    // Slice in Algo order.  A null base propagates to every algorithm, which
    // then reports itself inert via its own begin(nullptr) contract.
    float* p = base;
    for (int a = 0; a < kNumAlgos; ++a) {
        IReverb* r = instance(a);
        if (!r) continue;
        const uint32_t need = r->poolFloats();
        if (base) { r->begin(p); p += need; }
        else      { r->begin(nullptr); ++_failed; }
    }

    _ready   = (base != nullptr);
    _active  = kPlate;
    _pending = kPlate;
    _fade    = 0;
    _fadeOut = false;
    _lastMix = 0.0f;
}

// ---------------------------------------------------------------------------
// setAlgorithm — arm the crossfade.
//
// Re-selecting the algorithm already running is a no-op rather than a restart:
// an editor doing a full resync re-sends every parameter, and a resync that
// silently muted the reverb for 23 ms each time would be a mystifying bug.
// ---------------------------------------------------------------------------
void ReverbRack::setAlgorithm(int algo)
{
    if (algo < 0 || algo >= kNumAlgos) return;     // stale editor; ignore
    if (algo == _pending) return;                  // already there or heading there

    _pending = algo;
    _fadeOut = true;
    _fade    = kFadeBlocks;
}

// ---------------------------------------------------------------------------
// Parameter fan-out.  Every instance gets every value, so a switch lands on an
// algorithm that already matches the patch instead of on boot defaults.
// Control plane only — a few float stores per parameter change, and the cost
// is proportional to the number of ALGORITHMS, not to block rate.
// ---------------------------------------------------------------------------
void ReverbRack::setSize(float n)    { _plate.setSize(n);    _shimmer.setSize(n);     _room.setSize(n);  _hall.setSize(n); }
void ReverbRack::setHiDamp(float n)  { _plate.setHiDamp(n);  _shimmer.setHiDamp(n);   _room.setHiDamp(n);  _hall.setHiDamp(n); }
void ReverbRack::setLoDamp(float n)  { _plate.setLoDamp(n);  _shimmer.setLoDamp(n);   _room.setLoDamp(n);  _hall.setLoDamp(n); }
void ReverbRack::setLowpass(float n) { _plate.setLowpass(n); _shimmer.setLowpass(n);  _room.setLowpass(n);  _hall.setLowpass(n); }
void ReverbRack::setHipass(float n)  { _plate.setHipass(n);  _shimmer.setHipass(n);   _room.setHipass(n);  _hall.setHipass(n); }
void ReverbRack::setShimmer(float n) { _plate.setShimmer(n); _shimmer.setShimmer(n);  _room.setShimmer(n);  _hall.setShimmer(n); }
void ReverbRack::setFreeze(bool on)  { _plate.setFreeze(on); _shimmer.setFreeze(on); _room.setFreeze(on);  _hall.setFreeze(on); }

// ---------------------------------------------------------------------------
// processBlock — run exactly one algorithm, ramping the mix through a switch.
//
// The ramp is expressed as (mixStart, mixEnd) handed to the algorithm, which
// interpolates per sample.  At steady state both are `mix`, the algorithm's
// per-sample increment is exactly 0.0f, and the arithmetic is bit-identical to
// the pre-rack scalar-mix call.
// ---------------------------------------------------------------------------
void ReverbRack::processBlock(float* left, float* right, size_t n, float mix)
{
    IReverb* active = instance(_active);
    if (!active) return;

    if (_fade == 0) {
        // Steady state — the overwhelmingly common path, and the one that has
        // to stay bit-exact.
        active->processBlock(left, right, n, mix, mix);
        _lastMix = mix;
        return;
    }

    // Fade in progress.  The ramp target is derived from how many blocks are
    // left, so the gain is continuous across the block boundary even if the
    // caller's `mix` changed mid-fade (a knob moving during a switch).
    const float stepsLeft = (float)_fade;
    const float endScale  = _fadeOut ? ((stepsLeft - 1.0f) / (float)kFadeBlocks)
                                     : (1.0f - (stepsLeft - 1.0f) / (float)kFadeBlocks);

    const float mixStart = _lastMix;
    const float mixEnd   = mix * endScale;

    active->processBlock(left, right, n, mixStart, mixEnd);
    _lastMix = mixEnd;

    if (--_fade == 0) {
        if (_fadeOut) {
            // Silent now: drop the outgoing tail and hand over.  clearTail is
            // deliberately not a full begin() — re-carving would memset ~90 KB
            // of PSRAM inside the audio path.
            active->clearTail();
            _active  = _pending;
            _fadeOut = false;
            _fade    = kFadeBlocks;   // second half: ramp the new one up
            _lastMix = 0.0f;
        }
        // else: fade-in complete, _fade is already 0 and we are back to steady.
    }
}

} // namespace JT
