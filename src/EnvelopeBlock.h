/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
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

#include "effect_envelope_jt.h"
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
    bool isActive() const { return const_cast<AudioEffectEnvelopeJT&>(_envelope).isActive(); }

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
    AudioEffectEnvelopeJT _envelope;

    // Cached parameter values for UI readback — avoids reading hardware registers.
    float _attackTime   = 0.01f;
    float _decayTime    = 0.1f;
    float _sustainLevel = 0.8f;
    float _releaseTime  = 0.2f;
};
