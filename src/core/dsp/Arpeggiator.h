// =============================================================================
// Arpeggiator.h — JT-8000 v2 arpeggiator (7 modes, 16-step pattern, block-rate)
// =============================================================================
//
// WHAT THIS IS
//   A CONTROL-PLANE note generator.  Given the set of currently-held MIDI notes
//   it produces note-on / note-off events on a musical clock, driving the synth
//   voices DIRECTLY through a VoiceAllocator (the same note API the keyboard
//   uses).  It emits no audio and no modulation value; it is a sibling of
//   StepSequencer, not a variant of it.
//
//   Ticked once per SynthCore::renderBlock at kBlockMs (~2.9025 ms), exactly
//   like StepSequencer — same block-rate phase accumulator idiom.
//
// PLAYBACK MODEL
//   * CLASSIC note-consumption: while enabled, played notes are CONSUMED into
//     the held-note list and only the arp sounds.  SynthCore stops feeding the
//     keyboard notes straight to the allocator (see SynthCore drainNoteEvents).
//   * 7 note-ordering modes x 1..4 octaves (see ArpMode).
//   * A 16-step PATTERN of three parallel lanes.  These are the lane meanings,
//     stated here because they were previously only discoverable from code:
//       ON      rest vs trigger.  A rest sounds nothing, but the melodic
//               pointer STILL ADVANCES, so rhythm and note order stay
//               independent.
//       ACCENT  velocity MULTIPLIER, 0..1, applied to the held key's own
//               velocity.  It attenuates; it never boosts above the key.
//       RATCHET 1..4 re-triggers packed into the step's GATE window, so gate
//               length still shapes staccato inside a ratchet.
//   * Independent clock: its own division and phase, but it reads the SHARED
//     TempoClock BPM so internal-tempo and external-MIDI-clock changes move the
//     arp, LFOs and sequencer together.  Unlike StepSequencer (D-1), the arp IS
//     tempo-synced: TempoClock::freqForMode(Hz) is all the sync path needs.
//   * rate == kFree falls back to arp.free_hz, exp-mapped over kFreeHzMin..
//     kFreeHzMax — a range chosen to COVER the synced extremes (see below).
//   * SWING lengthens EVEN steps and shortens ODD ones by the same amount, so a
//     step pair keeps its total duration.  0.5 = straight (50/50); 1.0 = maximum
//     shuffle (75/25).  NOTE: the even/odd test is on the absolute step index,
//     so an ODD step_count flips the swing polarity at the pattern wrap.  That
//     is inherent to index-parity swing and is left as-is deliberately.
//   * Latch holds the pattern running after keys release.  Transport Start
//     (MIDI 0xFA) resets phase to step 0.
//
// STEP ADDRESSING (changed — see DEFERRALS_LEDGER.md)
//   Steps are addressed EXPLICITLY: setStepOn(step, on) and friends take the
//   step index.  The former cursor idiom (a step_select parameter followed by a
//   value write) has been removed from this class.  It could not represent a
//   sixteen-value pattern in a single store slot, so patterns were neither
//   saved in patches nor visible to editors.  params.yaml now carries 48
//   explicit arp step parameters and SynthCore drives these setters from them.
//
// CPU DISCIPLINE ("do not calculate if not required")
//   tick() early-returns immediately when disabled or when no notes are held —
//   before any phase maths.  The ordered play-list is rebuilt ONLY when the
//   held set or mode/octaves change (a dirty flag), never per tick.  The step
//   duration is CACHED and recomputed only when BPM, rate mode or free rate
//   actually move, so the steady state costs three compares rather than two
//   divides.  Per block when idle: one bool test.
//
// THREADING
//   Control plane only, exactly like VoiceAllocator and StepSequencer.  tick()
//   is called from renderBlock in this synth's single-threaded render model —
//   the same place StepSequencer::tick() and drainNoteEvents() already run — so
//   calling alloc.noteOn/off from here is the identical context the note-ring
//   drain already uses.  No new concurrency.
//
// (c) 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/AudioConfig.h"
#include "core/dsp/TempoClock.h"

namespace JT {

class VoiceAllocator;   // fwd — engine drives it, never owns it

// Note-ordering modes.  Order is FROZEN with kOpt_arp_mode in ParamTable.h
// (params.yaml option set `arp_mode`) — stored in patches, never reorder.
//   UpDnInc repeats the top & bottom notes each turn; UpDnExc does not.
//   AsPlayed follows key-press order; Chord fires ALL held notes per step.
enum class ArpMode : uint8_t {
    Up = 0, Down, UpDnInc, UpDnExc, AsPlayed, Random, Chord, Count
};

class Arpeggiator {
public:
    static constexpr int kMaxSteps    = 16;   // pattern length ceiling
    static constexpr int kMaxHeld     = 16;   // held-note capacity
    static constexpr int kMaxOctaves  = 4;
    static constexpr int kMaxRatchet  = 4;
    // Expanded play-list ceiling: held x octaves, both capped above.
    static constexpr int kMaxPlayList = kMaxHeld * kMaxOctaves;

    // FREE-RATE RANGE.  Deliberately chosen to COVER the synced extremes so
    // that switching sync off never loses reach:
    //   fastest synced = 1/32 @ 300 BPM = 40 Hz
    //   slowest synced = 4 bars @ 40 BPM = 0.0417 Hz
    // The previous 0.1..20 Hz map fell short at BOTH ends.
    static constexpr float kFreeHzMin = 0.02f;
    static constexpr float kFreeHzMax = 50.0f;

    Arpeggiator();

    // ---- Held-note feed (from SynthCore::drainNoteEvents while enabled) ----
    // noteOn de-dupes; a repeated note refreshes velocity and keeps its slot.
    // noteOff removes; with latch ON the note is KEPT (see setLatch).
    void noteOn(uint8_t note, uint8_t velocity);
    void noteOff(uint8_t note);
    void allNotesOff();                 // panic / mode-change clear

    // ---- Per-block tick ----------------------------------------------------
    // deltaMs = block period (kBlockMs).  Drives `alloc` via its note API and
    // reads `clock` for synced rate.  Both are borrowed, never owned.
    void tick(float deltaMs, VoiceAllocator& alloc, const TempoClock& clock);

    // ---- Transport (external clock; wired by SynthCore) --------------------
    void transportStart();              // MIDI 0xFA — reset phase to step 0, run
    void transportStop();               // MIDI 0xFC — silence any sounding note

    // ---- Parameter setters (clamp / validate at the boundary) --------------
    void setEnabled(bool on);           // OFF releases any arp-sounded note
    void setMode(ArpMode m);
    void setOctaves(int oct);           // 1..4
    void setLatch(bool on);
    void setRateMode(int mode);         // TempoClock::Mode index; kFree => free_hz
    void setFreeHz(float norm01);       // 0..1 -> exp kFreeHzMin..kFreeHzMax
    void setGateLength(float frac);     // 0..1 of the step
    void setSwing(float norm01);        // 0..1; 0.5 straight, 1.0 = 75/25
    void setStepCount(int count);       // 1..16

    // ---- Explicit per-step pattern writes (step is 0-based) ----------------
    // Out-of-range steps are IGNORED rather than clamped: a clamp would fold a
    // bad index onto a real step and silently corrupt the pattern.
    void setStepOn(int step, bool on);
    void setStepAccent(int step, float frac);   // 0..1 velocity multiplier
    void setStepRatchet(int step, int n);       // 1..4

    // ---- Queries -----------------------------------------------------------
    bool    enabled()   const { return _enabled; }
    ArpMode mode()      const { return _mode; }
    int     heldCount() const { return _heldCount; }

    // Telemetry for the controller's arp playhead (NRPN 0x3FFE).  "Running"
    // means the pattern is actually walking — enabled AND with something to
    // play — so a stopped arp shows no playhead rather than a stuck one.
    int  currentStep() const { return _currentStep; }
    bool running()     const { return _enabled && _playCount > 0; }

    // Rate-mode index (TempoClock::Mode).  Hardware-visible (not test-gated) so
    // bring-up can see whether the arp is on a synced division or on kFree(0).
    int  debugRateModeOrMinus1() const { return _enabled ? _rateMode : -1; }

#ifdef JT_TESTING
    int   debugPlayCount()   const { return _playCount; }
    int   debugStepDurMs()   const { return (int)_stepDurationMs; }
    int   debugCurrentStep() const { return _currentStep; }
    float debugFreeHz()      const { return _freeHz; }
    uint8_t debugPlayNote(int i) const
    { return (i >= 0 && i < _playCount) ? _playList[i] : 0; }
    bool  debugStepOn(int s) const
    { return (s >= 0 && s < kMaxSteps) ? _stepOn[s] : false; }
    float debugStepAccent(int s) const
    { return (s >= 0 && s < kMaxSteps) ? _stepAccent[s] : 0.0f; }
    int   debugStepRatchet(int s) const
    { return (s >= 0 && s < kMaxSteps) ? (int)_stepRatchet[s] : 0; }
#endif

private:
    // --- One held note: pitch + velocity + press order --------------------
    struct Held { uint8_t note; uint8_t vel; uint32_t order; };

    void  rebuildPlayList();                     // held x mode x octaves -> _playList
    // Recomputes _stepDurationMs ONLY when an input actually moved.  The cache
    // guard is the whole point — see CPU DISCIPLINE.
    void  refreshDurationIfStale(const TempoClock& clock);
    float stepDurationFor(int step) const;       // nominal duration + swing
    void  fireStep(VoiceAllocator& alloc);       // trigger current step's note(s)
    void  releaseSounding(VoiceAllocator& alloc);// note-off whatever the arp holds
    void  advanceStep();
    void  primeStepState(float stepDurMs);       // ratchet/gate state for _currentStep

    static float clampf(float v, float lo, float hi)
    { return v < lo ? lo : (v > hi ? hi : v); }
    static int   clampi(int v, int lo, int hi)
    { return v < lo ? lo : (v > hi ? hi : v); }

    // House xorshift (matches StepSequencer / VoiceAllocator).
    inline uint32_t nextRand()
    { uint32_t x = _rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; _rng = x; return x; }

    // ---- Held-note set (unsorted; press order in .order) -----------------
    Held     _held[kMaxHeld];
    int      _heldCount = 0;
    uint32_t _pressClock = 0;                    // monotonic press-order stamp

    // ---- Expanded ordered play-list (rebuilt on change only) -------------
    uint8_t  _playList[kMaxPlayList] = { 0 };    // MIDI notes in play order
    uint8_t  _playVel [kMaxPlayList] = { 0 };    // matching velocities
    int      _playCount = 0;
    int      _playIndex = 0;                      // which play-list note is "next"
    bool     _listDirty = false;                  // held/mode/octaves changed

    // ---- Pattern lanes ----------------------------------------------------
    // Initialised by the CONSTRUCTOR to "every step plays, full accent, single
    // hit".  They are NOT lazily primed on first note: the lazy prime this
    // replaces overwrote pattern writes that arrived from a patch load before
    // the first key press, silently discarding the whole pattern.
    bool     _stepOn    [kMaxSteps];
    float    _stepAccent[kMaxSteps];
    uint8_t  _stepRatchet[kMaxSteps];
    int      _stepCount  = 16;

    // ---- Clock / phase ----------------------------------------------------
    int      _rateMode      = TempoClock::k1_16;  // default 1/16
    float    _freeHz        = 2.0f;               // used when _rateMode==kFree
    float    _stepDurationMs = 125.0f;            // CACHED; see refreshDurationIfStale
    float    _phaseMs       = 0.0f;
    int      _currentStep   = 0;                  // 0.._stepCount-1

    // Duration-cache witnesses.  Impossible sentinels, so the first tick always
    // recomputes whatever the constructor left in _stepDurationMs.
    float    _cachedBpm      = -1.0f;
    int      _cachedRateMode = -1;
    float    _cachedFreeHz   = -1.0f;

    // ---- Musical params ---------------------------------------------------
    ArpMode  _mode        = ArpMode::Up;
    int      _octaves     = 1;                     // 1..4
    float    _gateLength  = 0.5f;                  // frac of step
    float    _swing       = 0.5f;                  // 0.5 straight
    bool     _latch       = false;
    bool     _enabled     = false;

    // ---- Sounding-note tracking (for precise note-off) --------------------
    // The arp may hold several notes down at once (Chord mode, or a long gate
    // that overlaps the next step).  We track every note WE triggered so we can
    // release exactly those — never a key the player is physically holding.
    uint8_t  _sounding[kMaxPlayList];
    int      _soundingCount = 0;

    // ---- Within-step ratchet / gate state --------------------------------
    int      _ratchetTotal   = 1;                 // ratchets for the current step
    int      _ratchetFired    = 0;                // how many sub-hits done
    float    _ratchetSubMs    = 0.0f;             // sub-slot length
    bool     _noteHeldThisStep = false;           // a sub-hit note is currently down
    float    _noteOffAtMs      = 0.0f;            // phase within step to release at

    // Set whenever the arp (re)starts from idle — enable, transport start, or
    // the first held note after silence.  The first tick after that must build
    // the current step's ratchet/gate state BEFORE firing, or the opening note
    // inherits a zero-length sub-slot and lasts a single block.
    bool     _primePending = true;

    uint32_t _rng = 0x2BD5F13u;
};

} // namespace JT
