// =============================================================================
// BassSequencer.h — JT-8000 v2 303-style bass-line sequencer (32-step, block-rate)
// =============================================================================
//
// PROVENANCE / LICENCE (decision L1, signed off 2026-10-17)
//   Clean-room design.  Inspired by the BEHAVIOUR of TB-3PO (Logarhythm1 /
//   Phazerville Hemisphere Suite) as described in its public documentation.
//   No GPL source (TB_3PO.h, schwung-tb3po) was read, translated or copied.
//   Patterns therefore do NOT match TB-3PO seeds — accepted at sign-off.
//   See bass-seq/phase0-proposal.md for the design record.
//
// WHAT THIS IS
//   A CONTROL-PLANE note generator, a sibling of Arpeggiator and StepSequencer
//   (decision 2: StepSequencer stays a modulation sequencer, untouched).
//   It plays a 1..32-step bass line transposed by the held key, with per-step
//   GATE / ACCENT / SLIDE / OCTAVE / SCALE DEGREE.  It emits no audio.
//
// OUTPUT MODEL — EVENT LIST (differs from the arp, deliberately)
//   The arp drives a VoiceAllocator directly.  This class instead writes up to
//   kMaxEvents BassEvents per tick into a small fixed list that SynthCore reads
//   straight after tick().  Why:
//     * SLIDE needs a legato note change WITHOUT an envelope retrigger, and the
//       current VoiceAllocator always retriggers (Phase 0a finding, O-4).  How
//       a slide reaches the voice is a Phase 2 decision; this class must not
//       pre-empt it.
//     * ACCENT amounts (bass.accent_amp / bass.accent_filter) act on the voice,
//       not the sequencer.  The event just carries an accent flag.
//     * The class is fully testable with no voices, no store and no audio.
//   Event order within one tick is the order the voice must apply them: for a
//   slide, NoteOn(new, kSlide) comes BEFORE NoteOff(old) — legato overlap.
//
// GENERATOR — lives in BassPattern.{h,cpp} (owned here as _pat), so the
//   JUCE plugin and the ESP32 controller can run the IDENTICAL code to draw
//   the pattern.  This class is the PLAYBACK side: held keys, clock, events.
//   "Rolls, not results" (proposal §Generator design):
//   One seeded xorshift32 stream fills a ROLL table: five uint16 random values
//   per step.  The step flags are then plain threshold compares against the
//   amounts.  Consequences:
//     * Monotonic: raising Density only ever ADDS gates; it never reshuffles.
//       Same for accent / slide / octave amounts.
//     * Amount changes are cheap: one compare pass over 32 steps, run lazily
//       on the NEXT step fire after a change (dirty flag), never per block.
//
// SEED + MUTATION = THE WHOLE PATTERN STATE (refinement vs proposal — flagged)
//   The proposal kept a 320-byte undo copy of the roll table.  That would make
//   a mutated pattern impossible to SAVE, because rolls are not patch data.
//   Instead each mutate pass k is itself deterministic (salted by k), so the
//   pattern is fully defined by the pair (seed, mutation count) and both are
//   ordinary 14-bit params.  Undo restores the previous (seed, mutation) pair.
//   Replaying mutation passes costs kMaxMutation x 32 xorshifts at most, on a
//   seed/mutation CHANGE only — never in the audio path.
//
// HAND EDITS WIN
//   setStepPacked(step, v) with kUserBit set overrides that step; generation
//   skips it.  kUserBit clear (e.g. 0) hands the step back to the generator.
//   This makes "0 = generated" the natural stored default for bass.step[i].
//
// TIMING
//   Block-rate phase accumulator, the same idiom as the arp: events land on
//   block boundaries, so jitter is at most one block (128 @ 48 kHz = 2.67 ms).
//   Step duration is CACHED and recomputed only on a timing/BPM change
//   (setTimingMode / updateFromClock, both event-driven like StepSequencer D-1).
//
// CPU DISCIPLINE ("do not calculate if not required")
//   Disabled: one bool test per block.  Running, between steps: one add and two
//   compares per block.  Per step: one table read + a note calculation.
//   Generation work only on parameter change.  No per-sample work at all.
//
// THREADING
//   Control plane only, same context as Arpeggiator::tick (renderBlock, before
//   voices render).  Setters are called from SynthCore::applyParam.
//
// (c) 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/AudioConfig.h"
#include "core/dsp/TempoClock.h"
#include "core/dsp/BassPattern.h"

namespace JT {

// Playback directions.  Order will be FROZEN with the params.yaml option set
// in Phase 2 (patches store the index) — append only, never reorder.
enum class BassDir : uint8_t { Forward = 0, Reverse, PingPong, Random, Count };

// Scales.  Same freeze rule as BassDir.  Interval tables live in the .cpp.
enum class BassScale : uint8_t {
    Chromatic = 0, Major, Minor, Dorian, Phrygian, Mixolydian,
    HarmonicMinor, MinorPent, MajorPent, Blues, Count
};

// One note event for SynthCore to apply to the layer's voice(s).
struct BassEvent {
    enum Type : uint8_t { NoteOff = 0, NoteOn = 1 };
    static constexpr uint8_t kAccent = 0x01;   // NoteOn: accented step
    static constexpr uint8_t kSlide  = 0x02;   // NoteOn: legato from the previous note

    uint8_t type;
    uint8_t note;       // MIDI note 0..127
    uint8_t velocity;   // held key's velocity (NoteOn); 0 for NoteOff
    uint8_t flags;      // kAccent | kSlide
};

class BassSequencer {
public:
    // ---- Capacities -------------------------------------------------------
    // Generator limits are BassPattern's (one definition); aliased here so
    // callers can keep naming them through the sequencer.
    static constexpr int      kMaxSteps     = BassPattern::kMaxSteps;
    static constexpr int      kMaxHeld      = 16;     // held-key stack depth
    static constexpr int      kMaxEvents    = 8;      // per tick; worst case is 2-3
    static constexpr uint16_t kSeedMax      = BassPattern::kSeedMax;
    static constexpr uint16_t kMaxMutation  = BassPattern::kMaxMutation;
    static constexpr int      kMaxRange     = BassPattern::kMaxRange;

    // Free-rate range: the arp's, chosen to cover the synced extremes.
    static constexpr float kFreeHzMin = 0.02f;
    static constexpr float kFreeHzMax = 50.0f;

    // Gate length limits.  Below 10 % a step is a click; above 90 % there is no
    // audible gap, which is what SLIDE is for.
    static constexpr float kGateMin = 0.10f;
    static constexpr float kGateMax = 0.90f;

    // Fraction of steps re-rolled by one mutate pass (x/65536 compare).
    static constexpr uint32_t kMutateThr = BassPattern::kMutateThr;   // 25 %

    // ---- Packed step format — defined (and documented) in BassPattern.h ----
    static constexpr uint16_t kDegreeMask = BassPattern::kDegreeMask;
    static constexpr uint16_t kGateBit    = BassPattern::kGateBit;
    static constexpr uint16_t kAccentBit  = BassPattern::kAccentBit;
    static constexpr uint16_t kSlideBit   = BassPattern::kSlideBit;
    static constexpr int      kOctShift   = BassPattern::kOctShift;
    static constexpr uint16_t kOctMask    = BassPattern::kOctMask;
    static constexpr uint16_t kUserBit    = BassPattern::kUserBit;
    static constexpr uint16_t kPackedMask = BassPattern::kPackedMask;

    BassSequencer();

    // ---- Held-key feed (SynthCore consumes keys while enabled, like the arp)
    // The most recently pressed held key is the transpose ROOT (degree 0).
    void noteOn(uint8_t note, uint8_t velocity);
    void noteOff(uint8_t note);
    void allNotesOff();                 // panic: clears held keys + latched root

    // ---- Per-block tick -----------------------------------------------------
    // Clears the event list, then fills it.  deltaMs = kBlockMs.
    void tick(float deltaMs);
    int              eventCount() const { return _eventCount; }
    const BassEvent& event(int i) const { return _events[i]; }

    // ---- Transport (MIDI 0xFA / 0xFC) --------------------------------------
    void transportStart();              // restart from the first step
    void transportStop();               // release the sounding note (arp parity)

    // ---- Parameter setters (clamp at the boundary) -------------------------
    void setEnabled(bool on);
    void setLatch(bool on);
    void setDirection(BassDir d);
    void setScale(BassScale s);
    void setGateLength(float frac);     // 0.10..0.90

    // Generator inputs: forwarded to the pattern (one definition, BassPattern).
    void setLength(int steps)          { _pat.setLength(steps); }     // 1..32
    void setRange(int degrees)         { _pat.setRange(degrees); }    // 1..15
    void setDensity(float amt01)       { _pat.setDensity(amt01); }
    void setAccentAmount(float amt01)  { _pat.setAccentAmount(amt01); }
    void setSlideAmount(float amt01)   { _pat.setSlideAmount(amt01); }
    void setOctaveAmount(float amt01)  { _pat.setOctaveAmount(amt01); }
    void setSeed(uint16_t seed)        { _pat.setSeed(seed); }
    void setMutation(uint16_t count)   { _pat.setMutation(count); }

    // Tempo: identical contract to StepSequencer — Free (0) uses setFreeHz.
    void setTimingMode(int mode, const TempoClock& clock);
    void updateFromClock(const TempoClock& clock);
    void setFreeHz(float hz);

    // ---- Actions (momentary). Each returns the new state for write-back to the
    // ParameterStore, so the patch always reflects what is playing.
    uint16_t newRandomSeed() { return _pat.newRandomSeed(); }  // mutation -> 0
    uint16_t mutate()        { return _pat.mutate(); }
    bool     undo()          { return _pat.undo(); }   // one level

    // ---- Hand edits --------------------------------------------------------
    void setStepPacked(int step, uint16_t packed) { _pat.setStepPacked(step, packed); }
    void clearAllEdits()                          { _pat.clearAllEdits(); }

    // ---- Queries -----------------------------------------------------------
    // Resolved packed value of a step (generated or edited) for UI / telemetry.
    // Non-const: resolves lazily if an amount changed since the last resolve.
    uint16_t stepPacked(int step) { return _pat.stepPacked(step); }
    bool     enabled()     const { return _enabled; }
    bool     running()     const;
    int      currentStep() const { return _currentStep; }
    uint16_t seed()        const { return _pat.seed(); }
    uint16_t mutation()    const { return _pat.mutation(); }
    uint32_t editedMask()  const { return _pat.editedMask(); }

#ifdef JT_TESTING
    float    debugStepDurMs() const { return _stepDurMs; }
    uint16_t debugRollPitch(int s) const { return _pat.debugRollPitch(s); }
    uint16_t debugRollGate(int s) const  { return _pat.debugRollGate(s); }
#endif

private:
    // ---- Playback ----------------------------------------------------------
    void    fireStep();                 // act on the resolved step _currentStep
    int     nextIndexFrom(int cur);     // direction rule (advances bounce/random state)
    int     startIndex() const;
    uint8_t noteFor(uint16_t packed) const;
    void    releaseSounding();
    void    emit(uint8_t type, uint8_t note, uint8_t vel, uint8_t flags);
    void    recalcDuration();
    bool    rootAvailable() const { return _heldCount > 0 || (_latch && _latchedRootValid); }
    uint8_t rootNote() const;
    uint8_t rootVelocity() const;

    // ---- Helpers -----------------------------------------------------------
    // House xorshift32 (same as StepSequencer / Arpeggiator).  State never 0.
    static uint32_t xorshift(uint32_t& s)
    { uint32_t x = s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s = x; return x; }

    // ---- Pattern (generator + hand edits + length) -------------------------
    BassPattern _pat;

    // ---- Playback parameters -----------------------------------------------
    BassScale _scale      = BassScale::Minor;
    BassDir  _direction   = BassDir::Forward;
    float    _gateLength  = 0.5f;        // 303-style half-step gate

    // ---- Clock -------------------------------------------------------------
    int      _timingMode  = TempoClock::k1_16;
    float    _syncHz      = -1.0f;       // <= 0: not synced, use _freeHz
    float    _freeHz      = 8.0f;        // = 1/16 @ 120 BPM
    float    _stepDurMs   = 125.0f;

    // ---- Playback state ----------------------------------------------------
    bool     _enabled       = false;
    bool     _latch         = false;
    bool     _startPending  = true;      // next running tick fires the first step
    float    _phaseMs       = 0.0f;
    float    _gateOffMs     = 62.5f;
    int      _currentStep   = 0;
    int      _nextStep      = 1;
    int      _bounceDir     = 1;
    int      _soundingNote  = -1;        // note we hold down, -1 = none
    bool     _holdThrough   = false;     // current note slides into the next step

    // ---- Held keys (last-note priority stack) ------------------------------
    uint8_t  _heldNote[kMaxHeld];
    uint8_t  _heldVel [kMaxHeld];
    int      _heldCount        = 0;
    uint8_t  _latchedRoot      = 36;
    uint8_t  _latchedVel       = 100;
    bool     _latchedRootValid = false;

    // ---- Output ------------------------------------------------------------
    BassEvent _events[kMaxEvents];
    int       _eventCount = 0;

    uint32_t  _playRng = 0x3C6EF372u;    // Random direction (non-reproducible by design)
};

} // namespace JT
