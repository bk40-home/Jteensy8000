#pragma once
// =============================================================================
// EnvelopeBlock.h — ADSR envelope wrapper for Teensy AudioEffectEnvelope
//
// Provides a clean interface over AudioEffectEnvelope with cached parameter
// readback and an isActive() query that reflects the TRUE hardware state:
//   - Returns true during Attack, Decay, Sustain, AND Release phases.
//   - Returns false only when the envelope output has reached zero (Idle).
//
// This is the single source of truth for "is this voice producing audio?"
// Do NOT maintain separate boolean flags — they will go stale.
// =============================================================================

#include "effect_envelope.h"
#include "Audio.h"

class EnvelopeBlock {
public:
    // ---- Lifecycle ----------------------------------------------------------

    AudioStream& input();
    AudioStream& output();
    void noteOn();
    void noteOff();

    // ---- State query --------------------------------------------------------

    // True when the envelope is in ANY active phase (Attack/Decay/Sustain/Release).
    // False only when output has decayed to zero (Idle).
    // Delegates directly to AudioEffectEnvelope::isActive().
    //
    // USE THIS — not a separate bool — to determine if a voice is producing audio.
    bool isActive() const { return _envelope.isActive(); }

    // ---- Parameter setters --------------------------------------------------

    void setAttackTime(float milliseconds);
    void setDecayTime(float milliseconds);
    void setSustainLevel(float level);
    void setReleaseTime(float milliseconds);
    void setADSR(float attack, float decay, float sustain, float release);

    // ---- Parameter getters (cached values, no hardware read) ----------------

    float getAttackTime()    const { return _attackTime; }
    float getDecayTime()     const { return _decayTime; }
    float getSustainLevel()  const { return _sustainLevel; }
    float getReleaseTime()   const { return _releaseTime; }

private:
    AudioEffectEnvelope _envelope;

    // Cached parameter values for UI readback — avoids reading hardware registers.
    float _attackTime   = 0.01f;
    float _decayTime    = 0.1f;
    float _sustainLevel = 0.8f;
    float _releaseTime  = 0.2f;
};
