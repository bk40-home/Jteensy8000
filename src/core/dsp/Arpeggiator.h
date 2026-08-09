// =============================================================================
// Arpeggiator.h — JT-8000 v2 arpeggiator (7 modes, 16-step pattern, block-rate)
// =============================================================================
//
// WHAT THIS IS
//   A CONTROL-PLANE note generator.  Given the set of currently-held MIDI notes
//   it produces note-on / note-off events on a musical clock, driving the synth
//   voices DIRECTLY through a VoiceAllocator* (the same note API the keyboard
//   uses — "B-note", user-signed-off).  It emits no audio and no modulation
//   value; it is a sibling of StepSequencer, not a variant of it.
//
//   Ticked once per SynthCore::renderBlock at kBlockMs (≈ 2.9025 ms), exactly
//   like StepSequencer — same block-rate phase accumulator idiom.
//
// PLAYBACK MODEL (docs/PHASE9_ARP_SPEC.md — user-signed-off)
//   * CLASSIC note-consumption: while enabled, played notes are CONSUMED into
//     the held-note list and only the arp sounds.  SynthCore stops feeding the
//     keyboard notes straight to the allocator (see SynthCore drainNoteEvents).
//   * 7 note-ordering modes × 1..4 octaves (see ArpMode).
//   * A 16-step PATTERN LAYER: each step carries {on/off, accent, ratchet}.
//       - off  -> a rest (no note this step), but the note pointer still moves
//                 so the melodic sequence keeps walking.
//       - accent -> 0..1 scale on output velocity (patch to cutoff for the
//                 classic accented trance line).
//       - ratchet 1..4 -> N rapid re-hits, subdividing the GATE portion of the
//                 step, so gate-length still shapes staccato within a ratchet.
//   * Independent clock: its own division / phase, but it reads the SHARED
//     TempoClock BPM so internal-tempo and external-MIDI-clock changes move the
//     arp, LFOs and seq together.  Unlike StepSequencer (D-1), the arp IS
//     tempo-synced: TempoClock::freqForMode(Hz) is all the sync path needs, so
//     no deferred ms accessor is required.
//   * rate == kFree falls back to arp.free_hz (exp-mapped 0.1..20 Hz), the LFO
//     freeHz idiom.
//   * Swing delays ODD steps (0.5 = straight).  Latch holds the pattern running
//     after keys release.  Transport Start (MIDI 0xFA) resets phase to step 0.
//
// CPU DISCIPLINE ("do not calculate if not required")
//   tick() early-returns immediately when disabled or when no notes are held —
//   before any phase maths.  The ordered play-list is rebuilt ONLY when the
//   held set or mode/octaves change (a dirty flag), never per tick.  Per block
//   when idle: one bool test.  Per block when running: one phase compare plus,
//   on a step boundary only, the trigger logic.
//
// THREADING
//   Control plane only, exactly like VoiceAllocator and StepSequencer.  tick()
//   is called from renderBlock (audio plane) in this synth's single-threaded
//   render model — the same place StepSequencer::tick() and drainNoteEvents()
//   already run — so calling _alloc->noteOn/off from here is the identical
//   context the note-ring drain already uses.  No new concurrency.
//
// © 2026 Kris Bishop — MIT licensed.
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
    static constexpr int kMaxHeld     = 16;   // held-note capacity (matches MonoNoteStack)
    static constexpr int kMaxOctaves  = 4;
    // Expanded play-list ceiling: held × octaves, both capped above.
    static constexpr int kMaxPlayList = kMaxHeld * kMaxOctaves;

    Arpeggiator() = default;

    // ---- Held-note feed (from SynthCore::drainNoteEvents while enabled) ----
    // noteOn de-dupes; a repeated note re-anchors nothing (arp keeps walking).
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
    void transportStop();               // MIDI 0xFC — silence any sounding arp note

    // ---- Parameter setters (clamp / validate at the boundary) --------------
    void setEnabled(bool on);           // OFF releases any arp-sounded note
    void setMode(ArpMode m);
    void setOctaves(int oct);           // 1..4
    void setLatch(bool on);             // OFF with no keys held clears the list
    void setRateMode(int mode);         // TempoClock::Mode index; kFree => free_hz
    void setFreeHz(float norm01);       // 0..1 -> exp 0.1..20 Hz (rate==kFree)
    void setGateLength(float frac);     // 0..1 of the step
    void setSwing(float norm01);        // 0..1; 0.5 straight, >0.5 shuffle
    void setStepCount(int count);       // 1..16
    void setStepSelect(int step);       // edit cursor 0..15 (SynthCore maps 1..16)
    void setStepOnOff(bool on);         // writes step at cursor
    void setStepAccent(float frac);     // 0..1 writes step at cursor
    void setStepRatchet(int n);         // 1..4 writes step at cursor

    // ---- Queries -----------------------------------------------------------
    bool    enabled()   const { return _enabled; }
    ArpMode mode()      const { return _mode; }
    // Rate-mode index (TempoClock::Mode).  Hardware-visible (not test-gated) so
    // bring-up can see whether the arp is on a synced division or on kFree(0).
    int     debugRateModeOrMinus1() const { return _enabled ? _rateMode : -1; }
    int     heldCount() const { return _heldCount; }

#ifdef JT_TESTING
    int   debugPlayCount()   const { return _playCount; }
    int   debugStepDurMs()   const { return (int)_stepDurationMs; }
    int   debugCurrentStep() const { return _currentStep; }
    uint8_t debugPlayNote(int i) const
    { return (i >= 0 && i < _playCount) ? _playList[i] : 0; }
    bool  debugStepOn(int s) const
    { return (s >= 0 && s < kMaxSteps) ? _stepOn[s] : false; }
#endif

private:
    // --- One held note: pitch + velocity + press order --------------------
    struct Held { uint8_t note; uint8_t vel; uint32_t order; };

    void  rebuildPlayList();                     // held × mode × octaves -> _playList
    void  recalcDuration(const TempoClock& clock);
    void  fireStep(VoiceAllocator& alloc);       // trigger current step's note(s)
    void  releaseSounding(VoiceAllocator& alloc);// note-off whatever the arp holds down
    void  advanceStep();
    static float clampf(float v, float lo, float hi)
    { return v < lo ? lo : (v > hi ? hi : v); }
    static int   clampi(int v, int lo, int hi)
    { return v < lo ? lo : (v > hi ? hi : v); }

    // D-2 idiom: house xorshift (matches StepSequencer / VoiceAllocator).
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

    // ---- Pattern layer (per step) ----------------------------------------
    bool     _stepOn    [kMaxSteps];              // ctor fills true
    float    _stepAccent[kMaxSteps];              // ctor fills 1.0
    uint8_t  _stepRatchet[kMaxSteps];             // ctor fills 1
    int      _stepCount  = 16;
    int      _editStep   = 0;                     // cursor for setStep* writes

    // ---- Clock / phase ----------------------------------------------------
    int      _rateMode      = TempoClock::k1_16;  // default 1/16 (trance staple)
    float    _freeHz        = 2.0f;               // used when _rateMode==kFree
    float    _stepDurationMs = 125.0f;            // recomputed each tick from clock
    float    _phaseMs       = 0.0f;
    int      _currentStep   = 0;                  // 0.._stepCount-1

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
    float    _ratchetSubMs    = 0.0f;             // sub-slot length (gate/N portion)
    bool     _noteHeldThisStep = false;           // a sub-hit note is currently down
    float    _noteOffAtMs      = 0.0f;            // phase within step to release at

    uint32_t _rng = 0x2BD5F13u;
};

} // namespace JT
