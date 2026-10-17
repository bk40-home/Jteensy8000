// =============================================================================
// BassPattern.cpp — standalone bass pattern generator (see BassPattern.h)
// =============================================================================
// Shared VERBATIM by the firmware, the JUCE plugin and the ESP32 controller,
// so it includes nothing project-specific.  The one project hook is JT_COLD:
// on the Teensy, control-plane code goes to flash rather than ITCM (CLAUDE.md
// memory rules).  When AudioConfig.h is reachable (firmware, JUCE engine
// build) it supplies JT_COLD; anywhere else (the ESP32 copy) it compiles to
// nothing.
// (c) 2026 Kris Bishop — MIT licensed.
// =============================================================================
#include "BassPattern.h"

#if defined(__has_include)
#  if __has_include("core/AudioConfig.h")
#    include "core/AudioConfig.h"
#  endif
#endif
#ifndef JT_COLD
#  define JT_COLD
#endif

namespace JT {

namespace {
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int   clampi(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

// ---------------------------------------------------------------------------
// Construction.  Amount defaults = the params.yaml defaults, so a fresh object
// matches a default patch before any parameter has been applied.
// ---------------------------------------------------------------------------
BassPattern::BassPattern()
{
    for (int i = 0; i < kMaxSteps; ++i) { _steps[i] = 0; _edits[i] = 0; _rolls[i] = Rolls{ 0, 0, 0, 0, 0 }; }
    _densityThr = amtToThr(0.75f);
    _accentThr  = amtToThr(0.25f);
    _slideThr   = amtToThr(0.25f);
    _octaveThr  = amtToThr(0.15f);
}

// Seed + salt -> a non-zero xorshift state.  Two odd multipliers (golden-ratio
// and MurmurHash3 constants) spread neighbouring seeds / salts apart so seed
// 0x0001 and 0x0002 do not give near-identical opening steps.  A few warm-up
// draws discard xorshift's weak first outputs for small states.
JT_COLD uint32_t BassPattern::streamState(uint16_t seed, uint32_t salt)
{
    uint32_t s = ((uint32_t)seed + 1u) * 0x9E3779B1u;
    s ^= (salt + 1u) * 0x85EBCA6Bu;
    s ^= 0x6A09E667u;
    if (s == 0u) s = 0x1u;                 // xorshift's one forbidden state
    for (int i = 0; i < 4; ++i) (void)xorshift(s);
    return s;
}

// Five draws per step, upper 16 bits (xorshift's better-mixed half).
JT_COLD void BassPattern::rollStep(uint32_t& st, Rolls& r)
{
    r.gate   = (uint16_t)(xorshift(st) >> 16);
    r.accent = (uint16_t)(xorshift(st) >> 16);
    r.slide  = (uint16_t)(xorshift(st) >> 16);
    r.octave = (uint16_t)(xorshift(st) >> 16);
    r.pitch  = (uint16_t)(xorshift(st) >> 16);
}

// 0..1 -> 0..65536.  A roll r (0..65535) passes when r < thr, so 0 never
// passes and 1.0 (65536) always passes — both ends are exact.
JT_COLD uint32_t BassPattern::amtToThr(float amt01)
{
    return (uint32_t)(clampf(amt01, 0.0f, 1.0f) * 65536.0f);
}

// Rolls are ALWAYS generated for all 32 steps, whatever the length, so changing
// the length never reshuffles the steps that remain.
JT_COLD void BassPattern::rebuildRolls()
{
    uint32_t st = streamState(_seed, 0u);
    for (int i = 0; i < kMaxSteps; ++i) rollStep(st, _rolls[i]);

    // Replay each mutate pass k = 1.._mutation.  Pass k has its own salted
    // stream, so the result depends only on (seed, mutation) — saveable.
    for (uint32_t k = 1; k <= (uint32_t)_mutation; ++k) {
        uint32_t ms = streamState(_seed, k);
        for (int i = 0; i < kMaxSteps; ++i) {
            if ((xorshift(ms) >> 16) < kMutateThr) rollStep(ms, _rolls[i]);
        }
    }
}

// Rolls + amounts -> packed steps.  Pure integer compares; ~32 x 6 ops.
JT_COLD void BassPattern::resolveTable()
{
    // Pass 1: gate, accent, octave, degree for every non-edited step.
    for (int i = 0; i < kMaxSteps; ++i) {
        if (_editedMask & (1u << i)) { _steps[i] = _edits[i]; continue; }
        const Rolls& r = _rolls[i];

        // Degree is stored even on rests, so tapping a rest back to a note in
        // the UI gives a sensible pitch rather than always the root.
        uint16_t p = (uint16_t)(((uint32_t)r.pitch * (uint32_t)_range) >> 16);  // 0.._range-1

        // Octave: jump when the roll passes; low bit of the same roll picks
        // up (+1) or down (-1).  Among passing rolls the low bit is ~50/50.
        uint16_t oct = 1u;                                   // 0 = no jump
        if (r.octave < _octaveThr) oct = (r.octave & 1u) ? 2u : 0u;
        p |= (uint16_t)(oct << kOctShift);

        if (r.gate < _densityThr) {
            p |= kGateBit;
            if (r.accent < _accentThr) p |= kAccentBit;
        }
        _steps[i] = p;
    }

    // Pass 2: slide.  Only into a GATED next step in forward order — a slide
    // into a rest is meaningless.  Other directions are guarded at playback
    // time (BassSequencer::fireStep), where the real next step is known.
    for (int i = 0; i < _length; ++i) {
        if (_editedMask & (1u << i)) continue;
        if (!(_steps[i] & kGateBit)) continue;
        const int nxt = (i + 1 < _length) ? i + 1 : 0;
        if ((_steps[nxt] & kGateBit) && _rolls[i].slide < _slideThr)
            _steps[i] |= kSlideBit;
    }
}

// ---------------------------------------------------------------------------
// Inputs.  Each marks only the work it actually invalidates.
// ---------------------------------------------------------------------------
JT_COLD void BassPattern::setDensity(float a)
{ const uint32_t t = amtToThr(a); if (t != _densityThr) { _densityThr = t; _tableDirty = true; } }
JT_COLD void BassPattern::setAccentAmount(float a)
{ const uint32_t t = amtToThr(a); if (t != _accentThr)  { _accentThr  = t; _tableDirty = true; } }
JT_COLD void BassPattern::setSlideAmount(float a)
{ const uint32_t t = amtToThr(a); if (t != _slideThr)   { _slideThr   = t; _tableDirty = true; } }
JT_COLD void BassPattern::setOctaveAmount(float a)
{ const uint32_t t = amtToThr(a); if (t != _octaveThr)  { _octaveThr  = t; _tableDirty = true; } }

JT_COLD void BassPattern::setRange(int degrees)
{
    const int v = clampi(degrees, 1, kMaxRange);
    if (v == _range) return;
    _range = v;
    _tableDirty = true;
}

JT_COLD void BassPattern::setLength(int steps)
{
    const int v = clampi(steps, 1, kMaxSteps);
    if (v == _length) return;
    _length = v;
    _tableDirty = true;                // the slide rule wraps at the length
}

JT_COLD void BassPattern::setSeed(uint16_t seed)
{
    const uint16_t v = (seed > kSeedMax) ? kSeedMax : seed;
    if (v == _seed) return;
    _seed = v;
    _rollsDirty = true;
}

JT_COLD void BassPattern::setMutation(uint16_t count)
{
    const uint16_t v = (count > kMaxMutation) ? kMaxMutation : count;
    if (v == _mutation) return;
    _mutation = v;
    _rollsDirty = true;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
JT_COLD uint16_t BassPattern::newRandomSeed()
{
    _undoSeed = _seed; _undoMutation = _mutation; _undoValid = true;
    uint16_t s;
    do { s = (uint16_t)((xorshift(_seedRng) >> 16) & kSeedMax); } while (s == _seed);
    _seed = s;
    _mutation = 0;
    _rollsDirty = true;
    return _seed;
}

JT_COLD uint16_t BassPattern::mutate()
{
    if (_mutation >= kMaxMutation) return _mutation;   // ceiling: no-op, no undo churn
    _undoSeed = _seed; _undoMutation = _mutation; _undoValid = true;
    ++_mutation;
    _rollsDirty = true;
    return _mutation;
}

JT_COLD bool BassPattern::undo()
{
    if (!_undoValid) return false;
    _seed = _undoSeed;
    _mutation = _undoMutation;
    _undoValid = false;                // one level, as proposed
    _rollsDirty = true;
    return true;
}

// ---------------------------------------------------------------------------
// Hand edits
// ---------------------------------------------------------------------------
JT_COLD void BassPattern::setStepPacked(int step, uint16_t packed)
{
    if (step < 0 || step >= kMaxSteps) return;        // ignore, never fold (arp rule)
    const uint32_t bit = 1u << step;
    if (packed & kUserBit) {
        // Guard the hand-edit invariants: no accent / slide on a rest.
        uint16_t v = (uint16_t)(packed & kPackedMask);
        if (!(v & kGateBit)) v = (uint16_t)(v & ~(kAccentBit | kSlideBit));
        _edits[step] = v;
        _editedMask |= bit;
    } else {
        _editedMask &= ~bit;
    }
    _tableDirty = true;
}

JT_COLD void BassPattern::clearAllEdits()
{
    if (_editedMask == 0u) return;
    _editedMask = 0u;
    _tableDirty = true;
}

JT_COLD uint16_t BassPattern::stepPacked(int step)
{
    if (step < 0 || step >= kMaxSteps) return 0u;
    ensureResolved();
    return _steps[step];
}

} // namespace JT
