// =============================================================================
// VoiceAllocator.h — note -> voice assignment for JT-8000 v2
// =============================================================================
//
// POLICY (Phase 1: 8-voice poly; mono/unison modes join in Phase 2)
//   noteOn picks, in order of preference:
//     1. the voice already playing this note (retrigger — no duplicate)
//     2. any idle voice
//     3. the OLDEST releasing voice          (its tail is least missed)
//     4. the OLDEST held voice               (classic oldest-steal)
//   Stolen voices restart via Voice::noteOn(): an audible voice fades for
//   one block before its phase/filter resets run (Voice.h §STEAL FADE), so
//   steals are click-free — no hardKill needed in normal play.
//
// SUSTAIN PEDAL (CC 64 — routed here by main.cpp, NOT a parameter)
//   While down, noteOff is deferred: the voice is marked sustained and
//   released only when the pedal lifts.  Re-striking a sustained note
//   retriggers the same voice, as on a real instrument.
//
// CONTEXT
//   Control plane only.  The allocator touches voices exclusively through
//   their note API; rendering never runs here.
//
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================
#pragma once

#include <stdint.h>

#include "core/Voice.h"

namespace JT {

// Voice-allocation / stacking mode (v1 SynthEngine PolyMode, spec §1.2).
//   Poly   : standard polyphony, the Phase 1 policy.
//   Mono   : one voice, last-note priority with legato note-stack return.
//   Unison : all voices play the held note, detuned across a spread.
// Option order is frozen with kOpt_poly_mode {Poly,Mono,Unison} == 0,1,2.
enum class PolyMode : uint8_t { Poly = 0, Mono = 1, Unison = 2 };

// Fixed-capacity last-note stack for Mono legato (v1 MonoNoteStack, §1.2).
// Releasing the top key returns to the previous held key.  Zero allocation.
struct MonoNoteStack {
    static constexpr int kCapacity = 16;   // generous; aggressive playing < 10
    uint8_t notes[kCapacity] = {};
    int     count            = 0;

    void push(uint8_t n) {                 // de-dupe, then append on top
        remove(n);
        if (count < kCapacity) notes[count++] = n;
    }
    bool remove(uint8_t n) {
        for (int i = 0; i < count; ++i) {
            if (notes[i] == n) {
                for (int j = i; j < count - 1; ++j) notes[j] = notes[j + 1];
                --count;
                return true;
            }
        }
        return false;
    }
    uint8_t top()   const { return notes[count - 1]; }   // valid when !empty
    bool    empty() const { return count == 0; }
    void    clear()       { count = 0; }
};

class VoiceAllocator {
public:
    static constexpr size_t kMaxVoices = 8;

    // Max unison detune spread in semitones (v1 UNISON_MAX_SPREAD_SEMITONES).
    static constexpr float kUnisonMaxSpreadSemis = 1.0f;

    explicit VoiceAllocator(Voice* voices, size_t count = kMaxVoices);

    // Rebind this allocator to a different SLICE of the shared voice pool.
    // Performance mode splits the 8 voices between two layers (perf.voice_split),
    // and that split is a live parameter.
    //
    // CALLER CONTRACT: the caller MUST silence the old slice first.  This
    // function deliberately does not do it, because when BOTH layers are being
    // repartitioned every voice has to be killed once, before either allocator
    // is rebound — doing it per-allocator would let layer A silence voices that
    // layer B had just been handed.  See SynthCore::repartitionVoices().
    //
    // Internal note bookkeeping (mono stack, unison note, sustain flags) is
    // cleared here: it indexes the OLD slice, so carrying it across would
    // release or re-pitch a voice that now belongs to the other layer.
    void setPool(Voice* voices, size_t count);

    void noteOn(uint8_t note, uint8_t velocity);
    void noteOff(uint8_t note);

    void sustain(bool pedalDown);

    // --- Phase 4 performance modes (spec §4.2) ---------------------------
    // setPolyMode kills all sounding voices on an actual change (v1 prevents
    // stuck notes) and clears mono/unison transient state.  setUnisonDetune
    // stores the 0..1 amount and re-spreads immediately when in Unison.
    void setPolyMode(PolyMode mode);
    void setUnisonDetune(float amount01);

    // Channel-mode handlers (main.cpp routes CC 123 / CC 120 here — in v2
    // a DAW panic actually silences the synth, the v1 bug that started
    // this whole redesign).
    void allNotesOff();     // CC 123: release everything musically
    void allSoundOff();     // CC 120: hard-kill everything immediately

    size_t activeCount() const;

private:
    Voice* pickVoiceFor(uint8_t note);

    // Deterministic per-voice phase randomisation: a tiny LCG beats
    // rand() (no libc state, reproducible in tests).
    float nextPhase01();

    // Equal-temperament note → Hz (A4 = 69 = 440), for the poly glide
    // pre-seed and mono legato return (v1 used the same formula inline).
    static float noteToHz(uint8_t note);

    // Spread even detune across all voices for the current unison amount
    // (v1 _applyUnisonDetune, §1.2).  ±spread/2 linear, both oscillators.
    void applyUnisonDetune();

    // Mode-specific note handlers (poly is the fall-through in noteOn/Off).
    void noteOnMono(uint8_t note, uint8_t velocity, float hz);
    void noteOnUnison(uint8_t note, uint8_t velocity, float hz);

public:
    // Bit i set = voice i currently sounding. For the controller's status
    // feed (voice-activity dots). Called from loop(); the audio ISR mutates
    // voice state concurrently, so a bit can be one block stale — telemetry
    // semantics, self-correcting on the next send, never used for logic.
    // NOTE: bits are relative to THIS allocator's slice.  With Performance
    // active each layer owns a different slice, so a caller that wants a mask
    // over the whole 8-voice pool must build it from the pool itself rather
    // than OR-ing two allocators' masks — see SynthCore::activeVoiceMask().
    uint8_t activeMask() const {
        uint8_t m = 0;
        for (size_t i = 0; i < _count && i < 8; ++i) {
            if (_voices[i].isActive()) m |= static_cast<uint8_t>(1u << i);
        }
        return m;
    }

private:
    Voice*   _voices;
    size_t   _count;
    uint32_t _clock = 0;          // monotonic age stamp for steal ordering
    uint32_t _rng   = 0x1234567u;
    bool     _pedal = false;
    bool     _sustained[kMaxVoices] = { false };

    // --- Phase 4 performance state ---------------------------------------
    PolyMode      _polyMode      = PolyMode::Poly;
    float         _unisonDetune  = 0.0f;   // 0..1 knob
    MonoNoteStack _monoStack;              // Mono legato note tracking
    int           _unisonNote    = -1;     // -1 = no unison note held
    // Last note the SYNTH played (any mode), so a stolen/idle poly voice
    // glides from it rather than its own stale pitch (v1 lastNoteFreq, §1.1).
    float         _lastNoteHz    = 0.0f;
};

} // namespace JT
