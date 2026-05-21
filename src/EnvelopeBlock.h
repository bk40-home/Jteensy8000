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
// EnvelopeBlock.h — ADSR envelope wrapper for AudioEffectEnvelopeJT
//
// Provides a clean interface over AudioEffectEnvelopeJT with cached parameter
// readback and an isActive() query that reflects the TRUE hardware state:
//   - Returns true during Attack, Decay, Sustain, AND Release phases.
//   - Returns false only when the envelope output has reached zero (Idle).
//
// CURVE SHAPING:
//   Each of the three timed stages (Attack, Decay, Release) has an independent
//   power-law exponent set via setAttackCurve / setDecayCurve / setReleaseCurve.
//
//     exponent = 1.0  → linear  (default — identical to stock AudioEffectEnvelope)
//     exponent > 1.0  → slow start, fast finish  (exponential feel)
//     exponent < 1.0  → fast start, slow finish  (logarithmic feel)
//     Useful range: 0.2 – 5.0  (engine clamps to 0.05 – 10.0 internally)
//
//   Curves default to 1.0 so existing patches are unaffected until explicitly
//   set. Each is cached here so the UI can read them back without querying the
//   audio engine.
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
    // Delegates directly to AudioEffectEnvelopeJT::isActive().
    //
    // USE THIS — not a separate bool — to determine if a voice is producing audio.
    bool isActive() const { return const_cast<AudioEffectEnvelopeJT&>(_envelope).isActive(); }

    // ---- ADSR time/level setters --------------------------------------------
    // Each setter caches the value for UI readback AND writes to the engine.
    // Calling these while the stage is active takes effect immediately (live
    // update — no click, no restart). See AudioEffectEnvelopeJT for details.

    void setAttackTime(float milliseconds);
    void setDecayTime(float milliseconds);
    void setSustainLevel(float level);
    void setReleaseTime(float milliseconds);
    void setADSR(float attack, float decay, float sustain, float release);

    // ---- Curve setters ------------------------------------------------------
    // Power-law exponent for each timed stage.
    // Applied once per stage transition (not per sample) — cost: one powf().

    void setAttackCurve(float exponent);
    void setDecayCurve(float exponent);
    void setReleaseCurve(float exponent);

    // ---- ADSR getters (cached — no hardware read) ---------------------------

    float getAttackTime()    const { return _attackTime; }
    float getDecayTime()     const { return _decayTime; }
    float getSustainLevel()  const { return _sustainLevel; }
    float getReleaseTime()   const { return _releaseTime; }

    // ---- Curve getters (cached — no hardware read) --------------------------

    float getAttackCurve()   const { return _attackCurve; }
    float getDecayCurve()    const { return _decayCurve; }
    float getReleaseCurve()  const { return _releaseCurve; }

private:
    AudioEffectEnvelopeJT _envelope;

    // ---- Cached ADSR values for UI readback ---------------------------------
    float _attackTime   = 0.01f;
    float _decayTime    = 0.1f;
    float _sustainLevel = 0.8f;
    float _releaseTime  = 0.2f;

    // ---- Cached curve exponents (1.0 = linear = stock behaviour) ------------
    float _attackCurve  = 1.0f;
    float _decayCurve   = 1.0f;
    float _releaseCurve = 1.0f;
};
