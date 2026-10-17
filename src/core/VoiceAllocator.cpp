// =============================================================================
// VoiceAllocator.cpp — implementation (policy in VoiceAllocator.h)
// © 2026 Kris Bishop — MIT licensed.
// =============================================================================

#include "core/VoiceAllocator.h"

#include <cmath>   // exp2f, log2f

namespace JT {

VoiceAllocator::VoiceAllocator(Voice* voices, size_t count)
    : _voices(voices),
      _count(count <= kMaxVoices ? count : kMaxVoices)
{
}

void VoiceAllocator::setPool(Voice* voices, size_t count)
{
    _voices = voices;
    _count  = (count <= kMaxVoices) ? count : kMaxVoices;

    // Everything below indexes the slice we just replaced.  Sustain flags are
    // per-voice-position, the mono stack and unison note name voices that may
    // now belong to the other layer — all of it has to go, or the first
    // note-off after a split change releases someone else's voice.
    for (size_t i = 0; i < kMaxVoices; ++i) _sustained[i] = false;
    _monoStack.clear();
    _unisonNote = -1;
    _pedal      = false;

    // Unison spread is a property of the slice width, so re-derive it for the
    // new one.  Cheap, and only ever on a split change.
    applyUnisonDetune();
}

float VoiceAllocator::noteToHz(uint8_t note)
{
    // Equal temperament, A4 = MIDI 69 = 440 Hz (same as Voice::noteOn).
    return 440.0f * exp2f(((float)note - 69.0f) / 12.0f);
}

float VoiceAllocator::nextPhase01()
{
    _rng = _rng * 1664525u + 1013904223u;      // Numerical Recipes LCG
    return (float)(_rng >> 8) * (1.0f / 16777216.0f);
}

Voice* VoiceAllocator::pickVoiceFor(uint8_t note)
{
    Voice* sameNote  = nullptr;
    Voice* idle      = nullptr;
    Voice* oldestRel = nullptr;
    Voice* oldestAny = nullptr;

    // One pass gathers every candidate class; preference order applied after.
    for (size_t i = 0; i < _count; ++i) {
        Voice& v = _voices[i];
        if (v.isActive() && v.note() == note) sameNote = &v;
        if (!v.isActive() && idle == nullptr) idle = &v;
        if (v.isReleasing() &&
            (oldestRel == nullptr || v.age() < oldestRel->age()))
            oldestRel = &v;
        if (oldestAny == nullptr || v.age() < oldestAny->age())
            oldestAny = &v;
    }

    if (sameNote)  return sameNote;    // retrigger in place
    if (idle)      return idle;        // free lunch
    if (oldestRel) return oldestRel;   // steal the least-audible tail first
    return oldestAny;                  // full board: oldest-steal
}

void VoiceAllocator::noteOn(uint8_t note, uint8_t velocity,
                            float accentAmp, float accentEnv)
{
    // Note-on with velocity 0 is note-off in disguise (MIDI running status
    // convention) — honour it here so every transport gets it right for free.
    if (velocity == 0) { noteOff(note); return; }

    // Latch the accent for whichever voice(s) this strike lands on.
    _strikeAccentAmp = accentAmp;
    _strikeAccentEnv = accentEnv;

    // Capture the PREVIOUS note the synth played before overwriting it — this
    // is the pitch poly-mode portamento slides FROM (v1 §1.1).
    const float prevHz = _lastNoteHz;
    const float hz     = noteToHz(note);
    _lastNoteHz = hz;

    // Mono / Unison take dedicated paths (spec §4.2); Poly is the default.
    if (_polyMode == PolyMode::Mono)   { noteOnMono(note, velocity, hz);   return; }
    if (_polyMode == PolyMode::Unison) { noteOnUnison(note, velocity, hz); return; }

    // ---- POLY ----
    Voice* v = pickVoiceFor(note);
    if (v == nullptr) return;                     // zero-voice build guard

    // Pre-seed the glide start so a stolen/idle voice slides from the synth's
    // last note, not the stale pitch it happens to hold (v1 setGlideFromFreq).
    // Harmless when glide is off (setGlideFromHz only moves the start ref).
    if (prevHz > 20.0f) v->setGlideFromHz(prevHz);

    v->setAge(++_clock);
    _sustained[(size_t)(v - _voices)] = false;    // fresh strike: not held
    v->setAccent(_strikeAccentAmp, _strikeAccentEnv);
    v->noteOn(note, velocity, nextPhase01());
}

// --- MONO: single voice (index 0), last-note priority (v1 §1.2) -----------
void VoiceAllocator::noteOnMono(uint8_t note, uint8_t velocity, float hz)
{
    (void)hz;   // Voice recomputes Hz from the note; kept for signature parity
    if (_count == 0) return;                      // zero-voice layer guard
    // Was another key already held BEFORE this one?  That is what makes this
    // strike a legato candidate (a fresh press after full release restarts).
    const bool otherHeld = !_monoStack.empty();
    _monoStack.push(note);
    Voice& v = _voices[0];
    // The one mono voice's _baseHz already holds the previous note, so the
    // standard glide gate in Voice::noteOn slides legato runs automatically.
    v.setAge(++_clock);
    _sustained[0] = false;

    // LEGATO: pitch move only.  legatoTo refuses (returns false, no change)
    // if the voice is releasing / idle, and we fall through to a strike.
    if (_legato && otherHeld && v.legatoTo(note, 0.0f)) return;

    v.setAccent(_strikeAccentAmp, _strikeAccentEnv);
    v.noteOn(note, velocity, nextPhase01());
}

// --- UNISON: all voices play the held note, detuned (v1 §1.2) --------------
void VoiceAllocator::noteOnUnison(uint8_t note, uint8_t velocity, float hz)
{
    (void)hz;
    // LEGATO: a new key while the unison note is still held moves every
    // unison voice's pitch; envelopes and the detune spread carry on.  All
    // voices share one state, so if voice 0 can legato they all can.
    if (_legato && _count > 0 && _unisonNote >= 0 && _unisonNote != (int)note
        && _voices[0].legatoTo(note, 0.0f)) {
        for (size_t i = 1; i < _count; ++i) _voices[i].legatoTo(note, 0.0f);
        _unisonNote = (int)note;
        return;
    }
    // Release the previous unison note's voices if a different note arrives.
    if (_unisonNote >= 0 && _unisonNote != (int)note) {
        for (size_t i = 0; i < _count; ++i) _voices[i].noteOff();
    }
    _unisonNote = (int)note;
    for (size_t i = 0; i < _count; ++i) {
        _voices[i].setAge(++_clock);
        _sustained[i] = false;
        _voices[i].setAccent(_strikeAccentAmp, _strikeAccentEnv);
        _voices[i].noteOn(note, velocity, nextPhase01());
    }
    // Spread is per-voice detune, applied after triggering so every voice has
    // fresh state (v1 re-applied on each unison note / detune change).
    applyUnisonDetune();
}

void VoiceAllocator::noteOff(uint8_t note)
{
    // ---- MONO: legato note-stack return (v1 §1.2) ----
    if (_polyMode == PolyMode::Mono) {
        _monoStack.remove(note);
        Voice& v = _voices[0];
        if (!_monoStack.empty()) {
            // Return to the most-recent key still held.  v1 RETRIGGERS the
            // envelopes here (Decision #2 — kept): a full noteOn at the return
            // note.  Reuse this voice's own note as the "previous" so the
            // return glides if armed; velocity reuses the current strike's
            // (v1 used getLastVelocity — here the store-driven velGain already
            // reflects it, so a nominal 100 keeps parity for the retrigger).
            const uint8_t ret = _monoStack.top();
            _lastNoteHz = noteToHz(ret);
            v.setAge(++_clock);
            // LEGATO (signed off 2a): with the switch on, the return moves the
            // pitch only — no envelope restart.  Off: v1's retrigger, as before.
            if (_legato && v.legatoTo(ret, 0.0f)) return;
            v.setAccent(0.0f, 0.0f);                 // a return is never accented
            v.noteOn(ret, 100, nextPhase01());
        } else if (_pedal) {
            _sustained[0] = true;      // pedal holds the last note
        } else {
            v.noteOff();
        }
        return;
    }

    // ---- UNISON: release all owned voices when the held note lifts ----
    if (_polyMode == PolyMode::Unison) {
        if ((int)note != _unisonNote) return;
        if (_pedal) {
            for (size_t i = 0; i < _count; ++i) _sustained[i] = true;
        } else {
            for (size_t i = 0; i < _count; ++i) _voices[i].noteOff();
            _unisonNote = -1;
        }
        return;
    }

    // ---- POLY: release EVERY voice on this note (retriggered dupes incl.) ----
    for (size_t i = 0; i < _count; ++i) {
        Voice& v = _voices[i];
        if (!v.isActive() || v.note() != note || v.isReleasing()) continue;
        if (_pedal) {
            _sustained[i] = true;      // pedal holds it; release on lift
        } else {
            v.noteOff();
        }
    }
}

// --- SLIDE (bass sequencer) — contract in VoiceAllocator.h ---------------
void VoiceAllocator::slideNote(uint8_t fromNote, uint8_t toNote, uint8_t velocity,
                               float slideRate, float accentAmp, float accentEnv)
{
    _lastNoteHz = noteToHz(toNote);    // later poly strikes glide from here

    if (_count > 0) {
        if (_polyMode == PolyMode::Mono) {
            Voice& v = _voices[0];
            if (v.note() == fromNote && v.legatoTo(toNote, slideRate)) {
                // Keep the note stack truthful: the held note IS toNote now.
                _monoStack.remove(fromNote);
                _monoStack.push(toNote);
                return;
            }
        } else if (_polyMode == PolyMode::Unison) {
            if (_unisonNote == (int)fromNote && _voices[0].legatoTo(toNote, slideRate)) {
                for (size_t i = 1; i < _count; ++i) _voices[i].legatoTo(toNote, slideRate);
                _unisonNote = (int)toNote;
                return;
            }
        } else {
            // Poly: move the (first) held voice on fromNote.
            for (size_t i = 0; i < _count; ++i) {
                Voice& v = _voices[i];
                if (v.isActive() && v.note() == fromNote && v.legatoTo(toNote, slideRate))
                    return;
            }
        }
    }

    // Nothing could slide (the note was stolen or already released): end the
    // old note FIRST — in Mono that empties the stack so no return fires —
    // then strike the new one normally, accent included.
    noteOff(fromNote);
    noteOn(toNote, velocity, accentAmp, accentEnv);
}

void VoiceAllocator::sustain(bool pedalDown)
{
    if (pedalDown == _pedal) return;
    _pedal = pedalDown;
    if (pedalDown) return;

    // Pedal lifted: release everything the pedal was holding.
    for (size_t i = 0; i < _count; ++i) {
        if (_sustained[i]) {
            _sustained[i] = false;
            _voices[i].noteOff();
        }
    }
}

void VoiceAllocator::allNotesOff()
{
    // CC 123 is musical: releases run their course.  A held pedal state is
    // also cleared — the spec treats 123 as "as if all keys were lifted".
    for (size_t i = 0; i < _count; ++i) {
        _sustained[i] = false;
        if (_voices[i].isActive()) _voices[i].noteOff();
    }
}

void VoiceAllocator::allSoundOff()
{
    // CC 120 is the emergency stop: immediate silence, tails included.
    for (size_t i = 0; i < _count; ++i) {
        _sustained[i] = false;
        _voices[i].hardKill();
    }
}

// --- Phase 4 performance-mode control (spec §4.2) -------------------------

void VoiceAllocator::setPolyMode(PolyMode mode)
{
    if (mode == _polyMode) return;             // no change: nothing to reset
    _polyMode = mode;

    // Kill all sounding voices on a mode switch to prevent stuck notes
    // (v1 setPolyMode) and clear the mono/unison transient bookkeeping.
    for (size_t i = 0; i < _count; ++i) {
        _voices[i].hardKill();
        _sustained[i] = false;
    }
    _monoStack.clear();
    _unisonNote = -1;

    // Entering Unison: apply the current spread immediately so the next note
    // is already detuned (v1 applied on mode change).
    if (mode == PolyMode::Unison) applyUnisonDetune();
}

void VoiceAllocator::setUnisonDetune(float amount01)
{
    _unisonDetune = (amount01 < 0.0f) ? 0.0f : (amount01 > 1.0f ? 1.0f : amount01);
    if (_polyMode == PolyMode::Unison) applyUnisonDetune();
}

void VoiceAllocator::applyUnisonDetune()
{
    // Even linear spacing from -spread/2 to +spread/2 across the voices
    // (v1 _applyUnisonDetune, §1.2).  With one voice: offset 0 (no divide by
    // zero).  Written to BOTH oscillators' detune — the SAME field OSC_DETUNE
    // uses, matching v1's clobbering behaviour (spec Decision #4).
    const float spread = _unisonDetune * kUnisonMaxSpreadSemis;
    for (size_t i = 0; i < _count; ++i) {
        const float offset = (_count > 1)
            ? (-spread * 0.5f + spread * (float)i / (float)(_count - 1))
            : 0.0f;
        _voices[i].oscSection().setDetuneSemis(0, offset);
        _voices[i].oscSection().setDetuneSemis(1, offset);
    }
}

size_t VoiceAllocator::activeCount() const
{
    size_t n = 0;
    for (size_t i = 0; i < _count; ++i)
        if (_voices[i].isActive()) ++n;
    return n;
}

} // namespace JT
