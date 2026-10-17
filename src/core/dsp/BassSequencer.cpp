// =============================================================================
// BassSequencer.cpp — JT-8000 v2 303-style bass-line sequencer
// =============================================================================
// See BassSequencer.h for the design contract (event model, CPU discipline).
// The pattern GENERATOR lives in BassPattern.{h,cpp} (shared verbatim with the
// JUCE plugin and the ESP32 controller); this file is the PLAYBACK side.  House idioms mirrored from
// Arpeggiator.cpp: anon-namespace clamps, xorshift RNG, block-rate phase
// accumulator, early exits.  Control-plane functions are JT_COLD (flash, not
// ITCM — CLAUDE.md memory rules); tick()/fireStep() are not, as they run
// every block.
// (c) 2026 Kris Bishop — MIT licensed.
// =============================================================================
#include "core/dsp/BassSequencer.h"

namespace JT {

namespace {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int   clampi(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }

// Guard on the resolved step rate: protects the 1000/Hz reciprocal.  Same
// ceiling as the arp (1/32 @ 300 BPM = 40 Hz sits well inside it).
constexpr float kRateCeilingHz = 100.0f;

// ---- Scale interval tables -------------------------------------------------
// Semitone offsets from the root for one octave.  Order matches BassScale.
// Degrees past the end of a scale wrap into the next octave (see noteFor).
struct ScaleDef { uint8_t count; uint8_t iv[12]; };

constexpr ScaleDef kScales[(int)BassScale::Count] = {
    { 12, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },   // Chromatic
    {  7, { 0, 2, 4, 5, 7, 9, 11 } },                   // Major (Ionian)
    {  7, { 0, 2, 3, 5, 7, 8, 10 } },                   // Natural minor (Aeolian)
    {  7, { 0, 2, 3, 5, 7, 9, 10 } },                   // Dorian
    {  7, { 0, 1, 3, 5, 7, 8, 10 } },                   // Phrygian
    {  7, { 0, 2, 4, 5, 7, 9, 10 } },                   // Mixolydian
    {  7, { 0, 2, 3, 5, 7, 8, 11 } },                   // Harmonic minor
    {  5, { 0, 3, 5, 7, 10 } },                         // Minor pentatonic
    {  5, { 0, 2, 4, 7, 9 } },                          // Major pentatonic
    {  6, { 0, 3, 5, 6, 7, 10 } },                      // Blues (minor + b5)
};

} // namespace

// ---------------------------------------------------------------------------
// Construction.  The generator (BassPattern) seeds its own amount defaults;
// the playback side only needs its arrays cleared.  The class is DISABLED by
// default, so the default patch is unchanged.
// ---------------------------------------------------------------------------
BassSequencer::BassSequencer()
{
    for (int i = 0; i < kMaxHeld;  ++i) { _heldNote[i] = 0; _heldVel[i] = 0; }
    for (int i = 0; i < kMaxEvents; ++i) _events[i] = BassEvent{ 0, 0, 0, 0 };
}

// ===========================================================================
// Held keys
// ===========================================================================
JT_COLD void BassSequencer::noteOn(uint8_t note, uint8_t velocity)
{
    // De-dupe, then push on top (last-note priority = the transpose root).
    noteOff(note);
    if (_heldCount >= kMaxHeld) {                        // drop the oldest
        for (int i = 1; i < kMaxHeld; ++i) { _heldNote[i - 1] = _heldNote[i]; _heldVel[i - 1] = _heldVel[i]; }
        _heldCount = kMaxHeld - 1;
    }
    // A key pressed after full release (and no latch) restarts the line from
    // its first step — the way a 303 / the arp behave.
    if (_heldCount == 0 && !(_latch && _latchedRootValid)) _startPending = true;

    _heldNote[_heldCount] = note;
    _heldVel [_heldCount] = velocity;
    ++_heldCount;
    _latchedRoot = note; _latchedVel = velocity; _latchedRootValid = true;
}

JT_COLD void BassSequencer::noteOff(uint8_t note)
{
    for (int i = 0; i < _heldCount; ++i) {
        if (_heldNote[i] != note) continue;
        for (int j = i + 1; j < _heldCount; ++j) { _heldNote[j - 1] = _heldNote[j]; _heldVel[j - 1] = _heldVel[j]; }
        --_heldCount;
        return;
    }
}

JT_COLD void BassSequencer::allNotesOff()
{
    _heldCount = 0;
    _latchedRootValid = false;          // tick() releases the sounding note
}

uint8_t BassSequencer::rootNote() const
{
    return _heldCount > 0 ? _heldNote[_heldCount - 1] : _latchedRoot;
}

uint8_t BassSequencer::rootVelocity() const
{
    return _heldCount > 0 ? _heldVel[_heldCount - 1] : _latchedVel;
}

bool BassSequencer::running() const
{
    return _enabled && rootAvailable();
}

// ===========================================================================
// Per-block tick
// ===========================================================================
void BassSequencer::tick(float deltaMs)
{
    _eventCount = 0;

    // Disabled: one test.  Any note left from before the disable is released
    // here (not in setEnabled) so the event lands in a list SynthCore reads.
    if (!_enabled) {
        if (_soundingNote >= 0) releaseSounding();
        return;
    }
    if (!rootAvailable()) {
        if (_soundingNote >= 0) releaseSounding();
        _startPending = true;
        return;
    }

    if (_startPending) {
        _startPending = false;
        _phaseMs      = 0.0f;
        _bounceDir    = 1;
        _currentStep  = startIndex();
        fireStep();
        return;
    }

    _phaseMs += deltaMs;

    // Step boundaries.  The guard bounds the work if a block were ever longer
    // than a step (not reachable with the 100 Hz rate ceiling, but cheap).
    for (int guard = 0; guard < 4 && _phaseMs >= _stepDurMs; ++guard) {
        _phaseMs -= _stepDurMs;
        _currentStep = (_nextStep < _pat.length()) ? _nextStep : startIndex();
        fireStep();
    }

    // Gate-off at gate_len of the step, unless this note slides onward.
    if (_soundingNote >= 0 && !_holdThrough && _phaseMs >= _gateOffMs)
        releaseSounding();
}

// Act on the current step.  Order of events matters to the voice: for a slide
// the new NoteOn precedes the old NoteOff (legato overlap).
void BassSequencer::fireStep()
{
    // One dirty check, then plain reads: a pattern edit since the last step is
    // resolved here, at step rate, never per block.
    const uint16_t* steps = _pat.resolved();
    const uint16_t  s     = steps[_currentStep];
    _nextStep  = nextIndexFrom(_currentStep);
    _gateOffMs = _gateLength * _stepDurMs;

    if (!(s & kGateBit)) {                               // rest
        if (_soundingNote >= 0) releaseSounding();
        _holdThrough = false;
        return;
    }

    const uint8_t note  = noteFor(s);
    const uint8_t vel   = rootVelocity();
    const uint8_t flags = (s & kAccentBit) ? BassEvent::kAccent : 0u;

    if (_soundingNote >= 0 && _holdThrough) {
        // Arriving from a slide.  Same pitch = a TIE: the note simply carries
        // on (no event).  New pitch = legato glide to it.
        if ((int)note != _soundingNote) {
            const uint8_t old = (uint8_t)_soundingNote;
            emit(BassEvent::NoteOn, note, vel, (uint8_t)(flags | BassEvent::kSlide));
            emit(BassEvent::NoteOff, old, 0u, 0u);
            _soundingNote = note;
        }
    } else {
        if (_soundingNote >= 0) releaseSounding();       // safety: gate < 100 %
        emit(BassEvent::NoteOn, note, vel, flags);
        _soundingNote = note;
    }

    // Hold through to the next step only if THIS step slides AND the step that
    // will actually play next is gated (correct for every direction).
    _holdThrough = (s & kSlideBit) && (steps[_nextStep] & kGateBit);
}

int BassSequencer::nextIndexFrom(int cur)
{
    const int len = _pat.length();
    if (len <= 1) return 0;
    switch (_direction) {
    case BassDir::Reverse:
        return (cur > 0) ? cur - 1 : len - 1;
    case BassDir::PingPong: {
        // Ends are NOT repeated: 0 1 2 3 2 1 0 1 ...
        int n = cur + _bounceDir;
        if (n >= len) { _bounceDir = -1; n = len - 2; }
        else if (n < 0) { _bounceDir = 1; n = 1; }
        return n;
    }
    case BassDir::Random:
        return (int)(xorshift(_playRng) % (uint32_t)len);
    case BassDir::Forward:
    default:
        return (cur + 1 < len) ? cur + 1 : 0;
    }
}

int BassSequencer::startIndex() const
{
    return (_direction == BassDir::Reverse) ? _pat.length() - 1 : 0;
}

// Degree -> semitones in the scale, wrapping whole octaves; plus the step's
// octave jump; plus the held root.  Clamped to the MIDI range.
uint8_t BassSequencer::noteFor(uint16_t packed) const
{
    const ScaleDef& sc  = kScales[(int)_scale];
    const int deg       = (int)(packed & kDegreeMask);
    int oct             = (int)((packed & kOctMask) >> kOctShift);
    oct                 = (oct == 3) ? 0 : oct - 1;     // 0,1,2 -> -1,0,+1 ; 3 -> 0
    const int semis     = 12 * (deg / sc.count + oct) + sc.iv[deg % sc.count];
    return (uint8_t)clampi((int)rootNote() + semis, 0, 127);
}

void BassSequencer::releaseSounding()
{
    emit(BassEvent::NoteOff, (uint8_t)_soundingNote, 0u, 0u);
    _soundingNote = -1;
    _holdThrough  = false;
}

void BassSequencer::emit(uint8_t type, uint8_t note, uint8_t vel, uint8_t flags)
{
    if (_eventCount >= kMaxEvents) return;   // cannot happen (max 3/step); never overrun
    _events[_eventCount++] = BassEvent{ type, note, vel, flags };
}

// ===========================================================================
// Transport
// ===========================================================================
JT_COLD void BassSequencer::transportStart() { _startPending = true; }

JT_COLD void BassSequencer::transportStop()
{
    // Parity with the arp: silence what is sounding.  Events are only written
    // inside tick(), so this drops any slide hold-through and pulls the gate-off
    // point to "now"; the very next tick then emits the NoteOff.
    _holdThrough = false;
    _gateOffMs   = 0.0f;
}

// ===========================================================================
// Setters (control plane).  Each marks only the work it actually invalidates.
// ===========================================================================
JT_COLD void BassSequencer::setEnabled(bool on)
{
    if (on && !_enabled) _startPending = true;
    _enabled = on;                     // the release on disable happens in tick()
}

JT_COLD void BassSequencer::setLatch(bool on) { _latch = on; }

JT_COLD void BassSequencer::setDirection(BassDir d)
{
    if ((int)d >= (int)BassDir::Count) return;
    _direction = d;
    _bounceDir = 1;
}

JT_COLD void BassSequencer::setScale(BassScale s)
{
    if ((int)s >= (int)BassScale::Count) return;
    _scale = s;                        // applied in noteFor; no table work needed
}

JT_COLD void BassSequencer::setGateLength(float frac)
{
    _gateLength = clampf(frac, kGateMin, kGateMax);   // takes effect next step
}

JT_COLD void BassSequencer::setTimingMode(int mode, const TempoClock& clock)
{
    // Not clamped above: a mode TempoClock does not know (e.g. an option the
    // table has but the clock lacks) makes freqForMode return <= 0, which
    // falls back to the free rate — the same contract as the arp and seq.
    _timingMode = (mode < 0) ? 0 : mode;
    updateFromClock(clock);
}

JT_COLD void BassSequencer::updateFromClock(const TempoClock& clock)
{
    _syncHz = clock.freqForMode(_timingMode);   // <= 0 for kFree
    recalcDuration();
}

JT_COLD void BassSequencer::setFreeHz(float hz)
{
    _freeHz = clampf(hz, kFreeHzMin, kFreeHzMax);
    recalcDuration();
}

JT_COLD void BassSequencer::recalcDuration()
{
    const float hz = clampf((_syncHz > 0.0f) ? _syncHz : _freeHz, kFreeHzMin, kRateCeilingHz);
    _stepDurMs = 1000.0f / hz;
}

} // namespace JT
