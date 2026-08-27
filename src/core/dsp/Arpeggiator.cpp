// =============================================================================
// Arpeggiator.cpp — JT-8000 v2 arpeggiator implementation
// =============================================================================
// See Arpeggiator.h for the design contract, the lane meanings and the swing
// definition.  House idioms mirrored from StepSequencer.cpp: anon-namespace
// clamps, xorshift RNG, block-rate phase accumulator, "do not calculate if not
// required" early-exits.
// (c) 2026 Kris Bishop — MIT licensed.
// =============================================================================
#include "core/dsp/Arpeggiator.h"

#include "core/VoiceAllocator.h"

#include <math.h>   // exp2f for the free-rate exponential map

namespace JT {

namespace {

// 0..1 -> kFreeHzMin..kFreeHzMax, logarithmic (constant cents per knob degree).
// log2(50 / 0.02) = log2(2500) = 11.2877123795.
constexpr float kFreeSpanLog2 = 11.2877124f;

inline float exp01ToHz(float n)
{
    return Arpeggiator::kFreeHzMin * exp2f(n * kFreeSpanLog2);
}

// Hard ceiling on the resolved step rate.  Above the free-rate maximum only to
// leave headroom for a synced division at an extreme BPM; the reciprocal below
// is what actually needs protecting from a divide by ~zero.
constexpr float kRateCeilingHz = 100.0f;

} // namespace

// ---------------------------------------------------------------------------
// Construction.  The pattern lanes are filled HERE, once, so a patch load that
// arrives before the first key press is not overwritten later.  (The previous
// build primed them lazily inside noteOn, which silently wiped any pattern
// written beforehand — the reason arp patterns did not stick.)
// ---------------------------------------------------------------------------
Arpeggiator::Arpeggiator()
{
    for (int i = 0; i < kMaxSteps; ++i) {
        _stepOn[i]      = true;
        _stepAccent[i]  = 1.0f;
        _stepRatchet[i] = 1;
    }
}

// ---------------------------------------------------------------------------
// Held-note feed
// ---------------------------------------------------------------------------
void Arpeggiator::noteOn(uint8_t note, uint8_t velocity)
{
    // De-dupe: a re-struck note refreshes its velocity and keeps its slot, so
    // the melodic order does not jump under a trill.
    for (int i = 0; i < _heldCount; ++i) {
        if (_held[i].note == note) {
            _held[i].vel = velocity;
            _listDirty   = true;
            return;
        }
    }
    if (_heldCount >= kMaxHeld) return;         // capacity — ignore extra notes
    _held[_heldCount].note  = note;
    _held[_heldCount].vel   = velocity;
    _held[_heldCount].order = ++_pressClock;
    ++_heldCount;
    _listDirty = true;
}

void Arpeggiator::noteOff(uint8_t note)
{
    // With latch ON, keys are kept — releasing the key does NOT remove the note
    // from the pattern.  The list is cleared by allNotesOff (panic, or the
    // caller noticing latch went off with no keys down).
    if (_latch) return;

    for (int i = 0; i < _heldCount; ++i) {
        if (_held[i].note == note) {
            for (int j = i; j < _heldCount - 1; ++j) _held[j] = _held[j + 1];
            --_heldCount;
            _listDirty = true;
            return;
        }
    }
}

void Arpeggiator::allNotesOff()
{
    _heldCount = 0;
    _listDirty = true;
}

// ---------------------------------------------------------------------------
// Play-list construction — held x mode x octaves.  Rebuilt ONLY when the held
// set / mode / octaves change (dirty flag), never per tick.
// ---------------------------------------------------------------------------
void Arpeggiator::rebuildPlayList()
{
    _listDirty = false;
    _playCount = 0;
    if (_heldCount == 0) { _playIndex = 0; return; }

    // 1) Base ordering for a single octave, into a small scratch.
    uint8_t baseNote[kMaxHeld];
    uint8_t baseVel [kMaxHeld];
    int     baseN = 0;

    // AsPlayed and Chord follow press order; everything else is pitch-sorted.
    // One selection sort either way (n <= 16, and only on a change).
    const bool byPressOrder = (_mode == ArpMode::AsPlayed || _mode == ArpMode::Chord);

    int idx[kMaxHeld];
    for (int i = 0; i < _heldCount; ++i) idx[i] = i;
    for (int i = 0; i < _heldCount - 1; ++i) {
        for (int j = i + 1; j < _heldCount; ++j) {
            const bool swap = byPressOrder
                            ? (_held[idx[j]].order < _held[idx[i]].order)
                            : (_held[idx[j]].note  < _held[idx[i]].note);
            if (swap) { const int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
        }
    }
    for (int i = 0; i < _heldCount; ++i) {
        baseNote[baseN] = _held[idx[i]].note;
        baseVel [baseN] = _held[idx[i]].vel;
        ++baseN;
    }

    // 2) Expand across octaves (each octave shifts the base up 12 semitones),
    //    clamping to the MIDI range.
    auto pushNote = [&](uint8_t n, uint8_t v) {
        if (_playCount < kMaxPlayList) {
            _playList[_playCount] = n;
            _playVel [_playCount] = v;
            ++_playCount;
        }
    };

    const int oct = clampi(_octaves, 1, kMaxOctaves);

    switch (_mode) {
    case ArpMode::Down: {
        // Highest octave first, each octave top -> bottom.
        for (int o = oct - 1; o >= 0; --o)
            for (int i = baseN - 1; i >= 0; --i) {
                const int n = (int)baseNote[i] + 12 * o;
                if (n <= 127) pushNote((uint8_t)n, baseVel[i]);
            }
        break;
    }
    case ArpMode::UpDnInc:
    case ArpMode::UpDnExc: {
        // Build the up leg across all octaves, then mirror it.  INC repeats the
        // top and bottom notes at the turn; EXC skips both ends on the way down.
        int     upLen = 0;
        uint8_t upN[kMaxPlayList];
        uint8_t upV[kMaxPlayList];
        for (int o = 0; o < oct; ++o)
            for (int i = 0; i < baseN; ++i) {
                const int n = (int)baseNote[i] + 12 * o;
                if (n <= 127 && upLen < kMaxPlayList) {
                    upN[upLen] = (uint8_t)n;
                    upV[upLen] = baseVel[i];
                    ++upLen;
                }
            }
        for (int i = 0; i < upLen; ++i) pushNote(upN[i], upV[i]);       // up leg
        const int from = (_mode == ArpMode::UpDnInc) ? upLen - 1 : upLen - 2;
        const int to   = (_mode == ArpMode::UpDnInc) ? 0         : 1;
        for (int i = from; i >= to; --i) pushNote(upN[i], upV[i]);      // down leg
        break;
    }
    case ArpMode::Up:
    case ArpMode::AsPlayed:
    case ArpMode::Random:
    case ArpMode::Chord:
    default: {
        // Ascending octave stack in the base ordering.  Random picks from this
        // list at fire time; Chord fires a whole octave slice — see fireStep.
        for (int o = 0; o < oct; ++o)
            for (int i = 0; i < baseN; ++i) {
                const int n = (int)baseNote[i] + 12 * o;
                if (n <= 127) pushNote((uint8_t)n, baseVel[i]);
            }
        break;
    }
    }

    if (_playIndex >= _playCount) _playIndex = 0;
}

// ---------------------------------------------------------------------------
// refreshDurationIfStale — the CACHE.  Two divides (freqForMode, then the
// reciprocal) used to run every block for a value that only moves when the
// player turns a knob or the tempo changes.  Now the steady state is three
// compares.
// ---------------------------------------------------------------------------
void Arpeggiator::refreshDurationIfStale(const TempoClock& clock)
{
    const float bpm    = clock.bpm();
    const bool  synced = (_rateMode != TempoClock::kFree);

    // BPM only participates when synced, so an internal tempo sweep does not
    // invalidate the cache of a free-running arp.
    if (_rateMode == _cachedRateMode &&
        _freeHz   == _cachedFreeHz &&
        (!synced || bpm == _cachedBpm)) {
        return;
    }

    _cachedRateMode = _rateMode;
    _cachedFreeHz   = _freeHz;
    _cachedBpm      = bpm;

    float hz = _freeHz;
    if (synced) {
        const float f = clock.freqForMode(_rateMode);
        if (f > 0.0f) hz = f;                   // <= 0 means "not synced"
    }
    hz = clampf(hz, kFreeHzMin, kRateCeilingHz);
    _stepDurationMs = 1000.0f / hz;
}

// ---------------------------------------------------------------------------
// stepDurationFor — nominal duration with SWING applied to this step index.
// Even steps lengthen, odd steps shorten by the same amount, so a pair keeps
// its total: swing 0.5 = 50/50, swing 1.0 = 75/25.  See the header note about
// odd step counts.
// ---------------------------------------------------------------------------
float Arpeggiator::stepDurationFor(int step) const
{
    const float amount = clampf((_swing - 0.5f) * 2.0f, 0.0f, 1.0f);
    if (amount <= 0.0f) return _stepDurationMs;     // straight — no maths at all

    const float shift = _stepDurationMs * 0.5f * amount;
    const float dur   = ((step & 1) != 0) ? (_stepDurationMs - shift)
                                          : (_stepDurationMs + shift);
    return (dur < 1.0f) ? 1.0f : dur;
}

// ---------------------------------------------------------------------------
// primeStepState — ratchet / gate state for whatever _currentStep now is.
// Takes the step's OWN duration so a swung step subdivides its own length
// rather than the previous step's.
// ---------------------------------------------------------------------------
void Arpeggiator::primeStepState(float stepDurMs)
{
    const int rat = clampi((int)_stepRatchet[_currentStep], 1, kMaxRatchet);
    _ratchetTotal = rat;
    _ratchetFired = 0;
    _ratchetSubMs = stepDurMs / (float)rat;

    // A rest: nothing fires this step, but advanceStep has already moved the
    // melodic pointer, so rhythm and note order stay independent.
    if (!_stepOn[_currentStep]) _ratchetFired = _ratchetTotal;
}

// ---------------------------------------------------------------------------
// tick — the per-block state machine.
//
// ORDER MATTERS.  The phase advance and any step boundary are handled FIRST,
// and only then are the gate-close and fire decisions taken.  The previous
// build tested for a fire before advancing, so a boundary crossed at the end of
// one tick could not sound until the next: every arp note was a fixed block
// (~2.9 ms) late against the clock.  Doing the boundary first removes that lag.
// ---------------------------------------------------------------------------
void Arpeggiator::tick(float deltaMs, VoiceAllocator& alloc, const TempoClock& clock)
{
    // Disabled: release anything we sounded so toggling off never strands a
    // note, then get out before any maths.
    if (!_enabled) { releaseSounding(alloc); _noteHeldThisStep = false; return; }

    if (_listDirty) rebuildPlayList();

    if (_heldCount == 0 || _playCount == 0) {
        // Nothing to play: silence, park at the top of the pattern, and mark
        // that the next run must prime its step state.
        releaseSounding(alloc);
        _noteHeldThisStep = false;
        _phaseMs      = 0.0f;
        _currentStep  = 0;
        _primePending = true;
        return;
    }

    refreshDurationIfStale(clock);

    float stepDur = stepDurationFor(_currentStep);

    if (_primePending) {
        _primePending = false;
        primeStepState(stepDur);
    }

    // --- Phase advance and step boundaries ---------------------------------
    _phaseMs += deltaMs;
    while (_phaseMs >= stepDur) {
        _phaseMs -= stepDur;

        // Release the outgoing step's note before stepping off it.
        if (_noteHeldThisStep) { releaseSounding(alloc); _noteHeldThisStep = false; }

        advanceStep();
        stepDur = stepDurationFor(_currentStep);   // the NEW step's own swing
        primeStepState(stepDur);
    }

    // --- Gate close inside the step ----------------------------------------
    if (_noteHeldThisStep && _phaseMs >= _noteOffAtMs) {
        releaseSounding(alloc);
        _noteHeldThisStep = false;
    }

    // --- Fire the next ratchet sub-hit whose slot has opened ----------------
    if (_ratchetFired < _ratchetTotal && !_noteHeldThisStep) {
        const float slotStart = _ratchetSubMs * (float)_ratchetFired;
        if (_phaseMs >= slotStart) {
            fireStep(alloc);
            ++_ratchetFired;
            _noteHeldThisStep = true;
            // Gate portion of THIS sub-slot.  The floor keeps a zero gate
            // audible as a click rather than a silent step.
            _noteOffAtMs = slotStart + _ratchetSubMs * clampf(_gateLength, 0.02f, 1.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// advanceStep — pattern position + play-list pointer.
// ---------------------------------------------------------------------------
void Arpeggiator::advanceStep()
{
    _currentStep = (_currentStep + 1) % _stepCount;

    // Random picks a fresh index; everyone else walks forward through the
    // pre-ordered play-list, which already encodes Down / UpDn as a linear
    // sequence.
    if (_mode == ArpMode::Random) {
        if (_playCount > 1) {
            int n;
            do { n = (int)(nextRand() % (uint32_t)_playCount); } while (n == _playIndex);
            _playIndex = n;
        } else {
            _playIndex = 0;
        }
    } else {
        _playIndex = (_playIndex + 1) % _playCount;
    }
}

// ---------------------------------------------------------------------------
// fireStep — trigger the current step's note(s) via the allocator.
// ACCENT is a multiplier on the held key's velocity (never a boost above it).
// ---------------------------------------------------------------------------
void Arpeggiator::fireStep(VoiceAllocator& alloc)
{
    const float accent = clampf(_stepAccent[_currentStep], 0.0f, 1.0f);

    if (_mode == ArpMode::Chord) {
        // Fire every held note of the CURRENT octave slice at once.  The
        // play-list is [oct0 notes..][oct1 notes..]; _playIndex walks it, so we
        // fire the base chord shifted by the octave the pointer is now in.
        const int baseN = _heldCount;
        if (baseN < 1) return;
        const int shift = 12 * (_playIndex / baseN);
        for (int i = 0; i < baseN; ++i) {
            const int n = (int)_playList[i] + shift;
            if (n > 127) continue;
            const uint8_t vel = (uint8_t)clampi((int)((float)_playVel[i] * accent + 0.5f), 1, 127);
            alloc.noteOn((uint8_t)n, vel);
            if (_soundingCount < kMaxPlayList) _sounding[_soundingCount++] = (uint8_t)n;
        }
        return;
    }

    // Single-note modes.
    const uint8_t note = _playList[_playIndex];
    const uint8_t rawV = _playVel [_playIndex];
    const uint8_t vel  = (uint8_t)clampi((int)((float)rawV * accent + 0.5f), 1, 127);
    alloc.noteOn(note, vel);
    if (_soundingCount < kMaxPlayList) _sounding[_soundingCount++] = note;
}

// ---------------------------------------------------------------------------
// releaseSounding — note-off exactly the notes the arp triggered.  Never
// touches a key the player is physically holding: we only release from our own
// _sounding set.
// ---------------------------------------------------------------------------
void Arpeggiator::releaseSounding(VoiceAllocator& alloc)
{
    for (int i = 0; i < _soundingCount; ++i) alloc.noteOff(_sounding[i]);
    _soundingCount = 0;
}

// ---------------------------------------------------------------------------
// Transport (external clock)
// ---------------------------------------------------------------------------
void Arpeggiator::transportStart()
{
    // Reset phase to the top of the pattern so the arp locks to the downbeat.
    _phaseMs          = 0.0f;
    _currentStep      = 0;
    _playIndex        = 0;
    _ratchetFired     = 0;
    _ratchetTotal     = 1;
    _noteHeldThisStep = false;
    _primePending     = true;
}

void Arpeggiator::transportStop()
{
    // No allocator handle here, so the release happens on the next tick.  In
    // practice SynthCore calls allNotesOff on stop as well.
    _noteHeldThisStep = false;
    _primePending     = true;
}

// ---------------------------------------------------------------------------
// Parameter setters
// ---------------------------------------------------------------------------
void Arpeggiator::setEnabled(bool on)
{
    if (on && !_enabled) {
        // Turning ON: start clean at the top of the pattern.
        _phaseMs      = 0.0f;
        _currentStep  = 0;
        _primePending = true;
    } else if (_enabled && !on) {
        // Turning OFF: sounding notes are released by the next tick, which has
        // the allocator handle.  Clear phase so a re-enable is clean.
        _phaseMs      = 0.0f;
        _currentStep  = 0;
        _primePending = true;
    }
    _enabled = on;
}

void Arpeggiator::setMode(ArpMode m)
{
    if (m >= ArpMode::Count) m = ArpMode::Up;
    if (m != _mode) { _mode = m; _listDirty = true; }
}

void Arpeggiator::setOctaves(int oct)
{
    oct = clampi(oct, 1, kMaxOctaves);
    if (oct != _octaves) { _octaves = oct; _listDirty = true; }
}

void Arpeggiator::setLatch(bool on)
{
    // Physical key state is not visible here.  SynthCore only feeds notes it
    // still considers down; if latch was holding phantom notes the caller
    // clears them via allNotesOff.
    _latch = on;
}

void Arpeggiator::setRateMode(int mode)
{
    if (mode < 0 || mode >= TempoClock::kNumModes) mode = TempoClock::k1_16;
    _rateMode = mode;
}

void Arpeggiator::setFreeHz(float norm01)
{
    _freeHz = exp01ToHz(clampf(norm01, 0.0f, 1.0f));
}

void Arpeggiator::setGateLength(float frac) { _gateLength = clampf(frac, 0.0f, 1.0f); }
void Arpeggiator::setSwing(float norm01)    { _swing      = clampf(norm01, 0.0f, 1.0f); }

void Arpeggiator::setStepCount(int count)
{
    _stepCount = clampi(count, 1, kMaxSteps);
    if (_currentStep >= _stepCount) { _currentStep = 0; _primePending = true; }
}

// ---- Explicit per-step writes.  Out-of-range is ignored, never clamped. ----
void Arpeggiator::setStepOn(int step, bool on)
{
    if (step < 0 || step >= kMaxSteps) return;
    _stepOn[step] = on;
}

void Arpeggiator::setStepAccent(int step, float frac)
{
    if (step < 0 || step >= kMaxSteps) return;
    _stepAccent[step] = clampf(frac, 0.0f, 1.0f);
}

void Arpeggiator::setStepRatchet(int step, int n)
{
    if (step < 0 || step >= kMaxSteps) return;
    _stepRatchet[step] = (uint8_t)clampi(n, 1, kMaxRatchet);
}

} // namespace JT
