// =============================================================================
// Arpeggiator.cpp — JT-8000 v2 arpeggiator implementation
// =============================================================================
// See Arpeggiator.h for the design contract and docs/PHASE9_ARP_SPEC.md for the
// file:line diagnosis this build follows.  House idioms mirrored from
// StepSequencer.cpp: anon-namespace clamps, xorshift RNG, block-rate phase
// accumulator, MIT header, "do not calculate if not required" early-exits.
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#include "core/dsp/Arpeggiator.h"

#include "core/VoiceAllocator.h"

#include <math.h>   // exp2f for the free-rate exponential map

namespace JT {

// ---------------------------------------------------------------------------
// Construction: pattern layer defaults to "all steps play, full accent, single
// hit" so a freshly-enabled arp plays every step — the expected behaviour.
// (Non-trivial member-array init, so done here rather than in the header.)
// ---------------------------------------------------------------------------
// NOTE: kept as an out-of-line initialiser via a small helper the ctor calls,
// so the header stays declaration-only for the arrays.
namespace {
inline float exp01ToHz(float n)                 // 0..1 -> 0.1..20 Hz (log sweep)
{
    // 0.1 * 2^(n * log2(20/0.1)) = 0.1 .. 20.  Matches the LFO free-rate law.
    const float span = 7.6438562f;              // log2(200)
    return 0.1f * exp2f(n * span);
}
} // namespace

// The arrays can't get member initialisers of non-equal length cleanly in the
// header without C++ verbosity, so init them the first time we need them via a
// tiny guard.  Simpler: initialise in every setter path is wasteful — instead
// we lazily prime once.  Cheapest correct approach: a private prime on first
// tick / first note.  We use a static-like bool member? No — keep it explicit:
static inline void primeSteps(bool* on, float* acc, uint8_t* rat, int n)
{
    for (int i = 0; i < n; ++i) { on[i] = true; acc[i] = 1.0f; rat[i] = 1; }
}

// ---------------------------------------------------------------------------
// Held-note feed
// ---------------------------------------------------------------------------
void Arpeggiator::noteOn(uint8_t note, uint8_t velocity)
{
    // Lazily prime the pattern layer the first time the arp sees any input.
    // (_pressClock == 0 only before the very first note.)  Zero cost after.
    if (_pressClock == 0) primeSteps(_stepOn, _stepAccent, _stepRatchet, kMaxSteps);

    // De-dupe: a re-struck note refreshes its velocity, keeps its slot.
    for (int i = 0; i < _heldCount; ++i) {
        if (_held[i].note == note) {
            _held[i].vel   = velocity;
            _listDirty = true;
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
    // from the pattern.  The list is cleared only when latch is turned off with
    // no keys down, or by allNotesOff.
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
// Play-list construction — held × mode × octaves.  Rebuilt ONLY when the held
// set / mode / octaves change (dirty flag), never per tick.
// ---------------------------------------------------------------------------
void Arpeggiator::rebuildPlayList()
{
    _listDirty = false;
    _playCount = 0;
    if (_heldCount == 0) { _playIndex = 0; return; }

    // 1) Build the base note ordering for a single octave into a small scratch.
    uint8_t  baseNote[kMaxHeld];
    uint8_t  baseVel [kMaxHeld];
    int      baseN = 0;

    if (_mode == ArpMode::AsPlayed || _mode == ArpMode::Chord) {
        // Press order (Chord uses the same ordering; it just fires them all at
        // once — see fireStep).  Sort held[] by .order ascending.
        int idx[kMaxHeld];
        for (int i = 0; i < _heldCount; ++i) idx[i] = i;
        for (int i = 0; i < _heldCount - 1; ++i)          // insertion sort (n<=16)
            for (int j = i + 1; j < _heldCount; ++j)
                if (_held[idx[j]].order < _held[idx[i]].order) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
        for (int i = 0; i < _heldCount; ++i) {
            baseNote[baseN] = _held[idx[i]].note;
            baseVel [baseN] = _held[idx[i]].vel;
            ++baseN;
        }
    } else {
        // Pitch-sorted ascending for Up / Down / UpDn / Random.
        int idx[kMaxHeld];
        for (int i = 0; i < _heldCount; ++i) idx[i] = i;
        for (int i = 0; i < _heldCount - 1; ++i)
            for (int j = i + 1; j < _heldCount; ++j)
                if (_held[idx[j]].note < _held[idx[i]].note) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
        for (int i = 0; i < _heldCount; ++i) {
            baseNote[baseN] = _held[idx[i]].note;
            baseVel [baseN] = _held[idx[i]].vel;
            ++baseN;
        }
    }

    // 2) Expand across octaves (each octave shifts the whole base up 12 semis),
    //    with pitch clamping to the MIDI range.  Chord/AsPlayed/Random still
    //    honour octaves — the ordering just differs.
    auto pushNote = [&](uint8_t n, uint8_t v) {
        if (_playCount < kMaxPlayList) { _playList[_playCount] = n; _playVel[_playCount] = v; ++_playCount; }
    };

    const int oct = clampi(_octaves, 1, kMaxOctaves);

    switch (_mode) {
    case ArpMode::Down: {
        // Highest octave first, each octave top→bottom.
        for (int o = oct - 1; o >= 0; --o)
            for (int i = baseN - 1; i >= 0; --i) {
                int n = (int)baseNote[i] + 12 * o;
                if (n <= 127) pushNote((uint8_t)n, baseVel[i]);
            }
        break;
    }
    case ArpMode::UpDnInc: {
        // Up then down, INCLUSIVE (top & bottom repeat).  Build the up leg
        // across all octaves, then mirror the whole thing.
        int upLen = 0; uint8_t upN[kMaxPlayList]; uint8_t upV[kMaxPlayList];
        for (int o = 0; o < oct; ++o)
            for (int i = 0; i < baseN; ++i) {
                int n = (int)baseNote[i] + 12 * o;
                if (n <= 127 && upLen < kMaxPlayList) { upN[upLen] = (uint8_t)n; upV[upLen] = baseVel[i]; ++upLen; }
            }
        for (int i = 0; i < upLen; ++i) pushNote(upN[i], upV[i]);          // up
        for (int i = upLen - 1; i >= 0; --i) pushNote(upN[i], upV[i]);     // down (incl. ends)
        break;
    }
    case ArpMode::UpDnExc: {
        // Up then down, EXCLUSIVE (don't repeat top & bottom).
        int upLen = 0; uint8_t upN[kMaxPlayList]; uint8_t upV[kMaxPlayList];
        for (int o = 0; o < oct; ++o)
            for (int i = 0; i < baseN; ++i) {
                int n = (int)baseNote[i] + 12 * o;
                if (n <= 127 && upLen < kMaxPlayList) { upN[upLen] = (uint8_t)n; upV[upLen] = baseVel[i]; ++upLen; }
            }
        for (int i = 0; i < upLen; ++i) pushNote(upN[i], upV[i]);          // up incl. ends
        for (int i = upLen - 2; i >= 1; --i) pushNote(upN[i], upV[i]);     // down, skip both ends
        break;
    }
    case ArpMode::Up:
    case ArpMode::AsPlayed:
    case ArpMode::Random:
    case ArpMode::Chord:
    default: {
        // Ascending octave stack in the base ordering.  (Random picks from this
        // list at fire time; Chord fires the whole current octave-slice — but
        // for simplicity Chord advances one octave-slice per step, see fireStep.)
        for (int o = 0; o < oct; ++o)
            for (int i = 0; i < baseN; ++i) {
                int n = (int)baseNote[i] + 12 * o;
                if (n <= 127) pushNote((uint8_t)n, baseVel[i]);
            }
        break;
    }
    }

    if (_playCount < 1) _playCount = 0;
    if (_playIndex >= _playCount) _playIndex = 0;
}

// ---------------------------------------------------------------------------
// recalcDuration — synced rate from the shared clock, else the free-rate knob.
// Unlike StepSequencer (D-1), the arp DOES sync: freqForMode(Hz) is sufficient.
// ---------------------------------------------------------------------------
void Arpeggiator::recalcDuration(const TempoClock& clock)
{
    float hz;
    if (_rateMode == TempoClock::kFree) {
        hz = _freeHz;
    } else {
        const float f = clock.freqForMode(_rateMode);
        hz = (f > 0.0f) ? f : _freeHz;          // guard; kFree handled above
    }
    if (hz < 0.05f) hz = 0.05f;                 // clamp — avoids div blow-up
    if (hz > 100.0f) hz = 100.0f;
    _stepDurationMs = 1000.0f / hz;
}

// ---------------------------------------------------------------------------
// tick — the per-block state machine.
// ---------------------------------------------------------------------------
void Arpeggiator::tick(float deltaMs, VoiceAllocator& alloc, const TempoClock& clock)
{
    // --- Early-exit: nothing to do.  Release any lingering arp note first so
    //     disabling / releasing all keys never leaves a stuck note. -----------
    if (!_enabled) { releaseSounding(alloc); return; }

    if (_listDirty) rebuildPlayList();

    if (_heldCount == 0 || _playCount == 0) {
        // No notes: silence anything still sounding, hold phase at 0 so the next
        // press starts cleanly on step 0.
        releaseSounding(alloc);
        _phaseMs = 0.0f;
        _currentStep = 0;
        return;
    }

    recalcDuration(clock);

    // --- Swing: even steps keep nominal duration; odd steps are delayed.
    //     swing 0.5 = straight; up to 0.75 phase of the pair shifts to the odd
    //     step.  We implement it as a per-step duration offset. --------------
    const float swingAmt = clampf((_swing - 0.5f) * 2.0f, 0.0f, 1.0f); // 0..1
    const bool  oddStep  = (_currentStep & 1) != 0;
    float thisStepDur = _stepDurationMs;
    if (swingAmt > 0.0f) {
        const float shift = _stepDurationMs * 0.5f * swingAmt; // up to half a step
        thisStepDur += oddStep ? -shift : shift;               // even longer, odd shorter
        if (thisStepDur < 1.0f) thisStepDur = 1.0f;
    }

    // --- Note-off scheduling within the current step (gate + ratchet) --------
    // If a sub-hit note is down and we've passed its off point, release it.
    if (_noteHeldThisStep && _phaseMs >= _noteOffAtMs) {
        releaseSounding(alloc);
        _noteHeldThisStep = false;
    }

    // --- Ratchet sub-hits: fire the remaining sub-hits at their slot times ---
    if (_ratchetFired < _ratchetTotal) {
        const float slotStart = _ratchetSubMs * (float)_ratchetFired;
        if (_phaseMs >= slotStart && !_noteHeldThisStep) {
            fireStep(alloc);                    // triggers with accent/velocity
            ++_ratchetFired;
            // gate portion of THIS sub-slot: gateLength × sub-slot length.
            _noteHeldThisStep = true;
            _noteOffAtMs = slotStart + _ratchetSubMs * clampf(_gateLength, 0.02f, 1.0f);
        }
    }

    // --- Advance phase; cross a step boundary => set up the next step ---------
    _phaseMs += deltaMs;
    while (_phaseMs >= thisStepDur) {
        _phaseMs -= thisStepDur;
        // Make sure the previous step's note is released before stepping.
        if (_noteHeldThisStep) { releaseSounding(alloc); _noteHeldThisStep = false; }
        advanceStep();

        // Prime ratchet state for the NEW step.
        const int rat = clampi((int)_stepRatchet[_currentStep], 1, 4);
        _ratchetTotal  = rat;
        _ratchetFired  = 0;
        _ratchetSubMs  = thisStepDur / (float)rat;

        // A rest step: skip firing but keep the melodic pointer moving so the
        // pattern's rhythm and the note sequence stay independent.
        if (!_stepOn[_currentStep]) {
            _ratchetFired = _ratchetTotal;      // nothing fires this step
        }
    }
}

// ---------------------------------------------------------------------------
// advanceStep — pattern position + play-list pointer.
// ---------------------------------------------------------------------------
void Arpeggiator::advanceStep()
{
    _currentStep = (_currentStep + 1) % _stepCount;

    // Move the melodic pointer.  Random picks a fresh index; everyone else
    // walks forward through the pre-ordered play-list (which already encodes
    // Down / UpDn as a linear sequence).
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
// ---------------------------------------------------------------------------
void Arpeggiator::fireStep(VoiceAllocator& alloc)
{
    const float accent = clampf(_stepAccent[_currentStep], 0.0f, 1.0f);

    if (_mode == ArpMode::Chord) {
        // Fire every held note of the CURRENT octave slice at once.  The
        // play-list is [oct0 notes..][oct1 notes..]; _playIndex walks it, so we
        // fire the base-chord shifted by the octave the pointer is currently in.
        const int baseN = _heldCount;
        if (baseN < 1) return;
        const int octSlice = (_playCount > 0) ? (_playIndex / baseN) : 0;
        const int shift = 12 * octSlice;
        for (int i = 0; i < baseN; ++i) {
            int n = (int)_playList[i] + shift; // _playList[0..baseN-1] is octave 0 already ordered
            if (n > 127) continue;
            uint8_t vel = (uint8_t)clampi((int)(_playVel[i] * accent + 0.5f), 1, 127);
            alloc.noteOn((uint8_t)n, vel);
            if (_soundingCount < kMaxPlayList) _sounding[_soundingCount++] = (uint8_t)n;
        }
        return;
    }

    // Single-note modes.
    const uint8_t note = _playList[_playIndex];
    const uint8_t rawV = _playVel[_playIndex];
    const uint8_t vel  = (uint8_t)clampi((int)(rawV * accent + 0.5f), 1, 127);
    alloc.noteOn(note, vel);
    if (_soundingCount < kMaxPlayList) _sounding[_soundingCount++] = note;
}

// ---------------------------------------------------------------------------
// releaseSounding — note-off exactly the notes the arp triggered.  Never
// touches a key the player is physically holding (we only release from our own
// _sounding set).
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
    _phaseMs = 0.0f;
    _currentStep = 0;
    _playIndex = 0;
    _ratchetFired = 0;
    _ratchetTotal = 1;
    _noteHeldThisStep = false;
}

void Arpeggiator::transportStop()
{
    // We can't release notes here (no allocator handle); mark so the next tick
    // clears them.  In practice SynthCore calls allNotesOff on stop as well.
    _noteHeldThisStep = false;
}

// ---------------------------------------------------------------------------
// Parameter setters
// ---------------------------------------------------------------------------
void Arpeggiator::setEnabled(bool on)
{
    if (_enabled && !on) {
        // Turning OFF: mark sounding notes for release on the next tick (the
        // tick has the allocator handle).  Clear phase so a re-enable is clean.
        _phaseMs = 0.0f;
        _currentStep = 0;
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
    // Turning latch OFF while no keys are physically down should clear the
    // pattern.  We can't see physical key state here, but SynthCore only feeds
    // us held notes it still considers down; if latch was holding phantom notes
    // the caller clears via allNotesOff.  Here we just store the flag.
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
    if (_currentStep >= _stepCount) _currentStep = 0;
}

void Arpeggiator::setStepSelect(int step) { _editStep = clampi(step, 0, kMaxSteps - 1); }
void Arpeggiator::setStepOnOff(bool on)   { _stepOn[_editStep] = on; }
void Arpeggiator::setStepAccent(float f)  { _stepAccent[_editStep] = clampf(f, 0.0f, 1.0f); }
void Arpeggiator::setStepRatchet(int n)   { _stepRatchet[_editStep] = (uint8_t)clampi(n, 1, 4); }

} // namespace JT
