/* AudioEffectEnvelopeJT — ADSR envelope with live parameter updates & curve shaping
 *
 * Copyright (c) 2017, Paul Stoffregen, paul@pjrc.com  (original AudioEffectEnvelope)
 * Copyright (c) 2025, Kris Bishop                      (JT-8000 extensions)
 *
 * Derived from the Teensy Audio Library effect_envelope.
 * Original development funded by PJRC.COM, LLC.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef effect_envelope_jt_h_
#define effect_envelope_jt_h_

#include <Arduino.h>
#include <AudioStream.h>
#include "utility/dspinst.h"

// ---------------------------------------------------------------------------
//  Compile-time constant: samples per millisecond at the audio rate.
//  Used only for time -> sample-count conversion.
// ---------------------------------------------------------------------------
#define SAMPLES_PER_MSEC_JT (AUDIO_SAMPLE_RATE_EXACT / 1000.0f)

// ---------------------------------------------------------------------------
//  State-machine identifiers — one active at a time per envelope instance.
// ---------------------------------------------------------------------------
enum EnvelopeStateJT : uint8_t {
    ENV_IDLE    = 0,   // silent — no audio processing needed
    ENV_DELAY   = 1,   // pre-attack silent hold
    ENV_ATTACK  = 2,   // rising from 0 -> unity
    ENV_HOLD    = 3,   // held at unity before decay
    ENV_DECAY   = 4,   // falling from unity -> sustain level
    ENV_SUSTAIN = 5,   // held at sustain level indefinitely
    ENV_RELEASE = 6,   // falling from current level -> 0
    ENV_FORCED  = 7    // fast forced release before re-trigger
};

// ---------------------------------------------------------------------------
//  AudioEffectEnvelopeJT
//
//  Drop-in replacement for AudioEffectEnvelope with two additions:
//
//    1. Live parameter updates — calling attack() / decay() / release() /
//       sustain() while that stage is active recalculates the slope
//       immediately.  No click, no restart.
//
//    2. Per-stage curve shaping via a geometric-series increment.
//       At curve == 1.0 (CC 64) the increment is constant (linear = stock).
//       At curve < 1.0 the increment decays each chunk (logarithmic feel).
//       At curve > 1.0 the increment grows each chunk (exponential feel).
//       Cost: one float multiply per 8-sample chunk when curved; zero when
//       linear.  Total stage time is always exact regardless of curve.
//
//  All stock behaviour is preserved when the new features are unused.
// ---------------------------------------------------------------------------
class AudioEffectEnvelopeJT : public AudioStream
{
public:
    AudioEffectEnvelopeJT() : AudioStream(1, inputQueueArray)
    {
        state = ENV_IDLE;
        inc_hires   = 0;
        inc_hires_f = 0.0f;

        // --- sensible defaults matching the stock envelope ---
        delay(0.0f);
        attack(10.5f);
        hold(2.5f);
        decay(35.0f);
        sustain(0.5f);
        release(300.0f);
        releaseNoteOn(5.0f);

        // --- curves default to linear (stock behaviour) ---
        attack_curve  = 1.0f;
        decay_curve   = 1.0f;
        release_curve = 1.0f;
    }

    // ----- trigger control -----
    void noteOn(void);
    void noteOff(void);

    // ----- time setters (milliseconds) — live-update if stage is active -----
    void delay(float milliseconds);
    void attack(float milliseconds);
    void hold(float milliseconds);
    void decay(float milliseconds);
    void sustain(float level);
    void release(float milliseconds);
    void releaseNoteOn(float milliseconds);

    // ----- curve shaping -----
    //  exponent = 1.0  -> linear  (stock behaviour, CC 64)
    //  exponent < 1.0  -> logarithmic (fast start, slow finish)
    //  exponent > 1.0  -> exponential (slow start, fast finish)
    //  Practical range: 0.15 – 5.0 (mapped from CC 0–127 via Mapping.h)
    void setAttackCurve(float exponent)  { attack_curve  = constrainCurve(exponent); }
    void setDecayCurve(float exponent)   { decay_curve   = constrainCurve(exponent); }
    void setReleaseCurve(float exponent) { release_curve = constrainCurve(exponent); }

    // convenience: set all three curves at once
    void setCurve(float exponent) {
        float c = constrainCurve(exponent);
        attack_curve  = c;
        decay_curve   = c;
        release_curve = c;
    }

    // ----- state queries (safe to call from any context) -----
    bool isActive(void);
    bool isSustain(void);
    EnvelopeStateJT getState(void);

    // ----- audio engine callback -----
    virtual void update(void);

private:
    // --- time conversion: milliseconds -> count of 8-sample chunks ---
    static uint16_t milliseconds2count(float milliseconds) {
        if (milliseconds < 0.0f) milliseconds = 0.0f;
        uint32_t c = ((uint32_t)(milliseconds * SAMPLES_PER_MSEC_JT) + 7) >> 3;
        if (c > 65535) c = 65535;   // max ~11.88 seconds
        return (uint16_t)c;
    }

    // --- clamp curve exponent to safe range ---
    static float constrainCurve(float v) {
        if (v < 0.15f) return 0.15f;   // floor raised to match CC 0 mapping
        if (v > 5.0f)  return 5.0f;    // ceiling matches CC 127 mapping
        return v;
    }

    // --- convert curve exponent to geometric alpha ---
    //  Maps curve 0.15..5.0 -> alpha -6..+6 (0 at curve=1.0).
    //  Alpha controls the per-chunk increment growth/decay rate.
    static float curveToAlpha(float curve) {
        if (curve <= 1.0f) return -6.0f * (1.0f - curve) / 0.85f;
        return 6.0f * (curve - 1.0f) / 4.0f;
    }

    // --- set up geometric-series slope for a stage ---
    //  Computes inc_hires (initial increment) and inc_factor (per-chunk
    //  multiplier) such that the sum of the geometric series over 'count'
    //  chunks exactly equals (target - start).
    //
    //  When curve==1.0, factor=1.0 and inc is constant (stock behaviour).
    //  Cost: one expf() + one powf() at transition time only.
    void initSlope(int32_t start, int32_t target, uint16_t count, float curve);

    // ----- audio input queue (single mono input) -----
    audio_block_t *inputQueueArray[1];

    // ----- runtime state (modified in update() and noteOn/Off ISR context) -----
    volatile uint8_t  state;        // current EnvelopeStateJT
    uint16_t          count;        // 8-sample chunks remaining in current state
    int32_t           mult_hires;   // current gain  — 0 = silent, 0x40000000 = unity
    int32_t           inc_hires;    // integer gain change per chunk, rounded from inc_hires_f each chunk
    float             inc_hires_f;  // AUTHORITATIVE per-chunk delta in 2.30 units, full float precision.
                                    // Curved stages scale THIS (not the int) so accumulated rounding
                                    // error cannot starve a long/curved stage and leave it short of
                                    // target — the bug that made curved releases end abruptly.
    float             inc_factor;   // per-chunk multiplier for inc_hires_f (1.0 = linear)

    // ----- stored durations (in 8-sample chunk counts) -----
    uint16_t delay_count;
    uint16_t attack_count;
    uint16_t hold_count;
    uint16_t decay_count;
    int32_t  sustain_mult;          // sustain level in 2.30 fixed-point
    uint16_t release_count;
    uint16_t release_forced_count;

    // ----- curve exponents (1.0 = linear = stock behaviour) -----
    float attack_curve;
    float decay_curve;
    float release_curve;
};

#undef SAMPLES_PER_MSEC_JT

#endif // effect_envelope_jt_h_
