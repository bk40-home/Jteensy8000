/* AudioEffectEnvelopeJT — ADSR envelope with live parameter updates & curve shaping
 *
 * Copyright (c) 2017, Paul Stoffregen, paul@pjrc.com  (original AudioEffectEnvelope)
 * Copyright (c) 2025, Kris Bishop                      (JT-8000 extensions)
 *
 * Derived from the Teensy Audio Library effect_envelope.
 * See effect_envelope_jt.h for full license text.
 */

#include <Arduino.h>
#include "effect_envelope_jt.h"

// ===========================================================================
//  initSlope — compute geometric-series increment and per-chunk factor
//
//  The geometric series:  inc * (1 + r + r^2 + ... + r^(N-1)) = range
//  Solving:  inc = range * (r - 1) / (r^N - 1)
//
//  When curve==1.0, r=1.0 and the series reduces to N*inc = range,
//  giving the stock linear slope with zero extra cost.
//
//  When curve!=1.0, r = exp(alpha/N) where alpha is derived from the
//  curve exponent.  The factor r is very close to 1.0 (e.g. 0.9997),
//  so one float multiply per chunk is cheap and numerically stable.
//
//  This is called ONCE per state transition or live parameter change.
//  Cost: one expf() + one powf() when curved; nothing when linear.
// ===========================================================================
void AudioEffectEnvelopeJT::initSlope(int32_t start, int32_t target,
                                      uint16_t chunks, float curve)
{
    // set mult_hires to the starting level for this stage
    mult_hires = start;

    if (chunks == 0) {
        // no time — snap to target instantly
        mult_hires = target;
        inc_hires  = 0;
        inc_factor = 1.0f;
        return;
    }

    int32_t range = target - start;

    if (range == 0) {
        // start == target — nothing to ramp
        inc_hires  = 0;
        inc_factor = 1.0f;
        return;
    }

    float alpha = curveToAlpha(curve);

    // --- linear: curve==1.0 -> alpha==0 -> factor==1.0 ---
    // Use a small dead-zone to avoid expf/powf for nearly-linear curves
    if (fabsf(alpha) < 0.01f) {
        inc_hires  = range / (int32_t)chunks;
        inc_factor = 1.0f;
        return;
    }

    // --- curved: geometric series ---
    float N  = (float)chunks;
    float r  = expf(alpha / N);           // per-chunk multiplier (~0.9995..1.0005)
    float rN = powf(r, N);                // r^N — total ratio over entire stage

    // inc_initial = range * (r - 1) / (r^N - 1)
    // Guard against rN==1.0 (shouldn't happen given alpha dead-zone above)
    float denom = rN - 1.0f;
    if (fabsf(denom) < 1e-9f) {
        // degenerate — fall back to linear
        inc_hires  = range / (int32_t)chunks;
        inc_factor = 1.0f;
        return;
    }

    float inc_f = (float)range * (r - 1.0f) / denom;
    inc_hires  = (int32_t)inc_f;
    inc_factor = r;

    // If initial inc rounds to zero but range is non-zero, force a minimum
    // so the envelope still moves (very long stages with strong curve)
    if (inc_hires == 0) {
        inc_hires = (range > 0) ? 1 : -1;
    }
}


// ===========================================================================
//  noteOn — begin or re-trigger the envelope
// ===========================================================================
void AudioEffectEnvelopeJT::noteOn(void)
{
    __disable_irq();

    if (state == ENV_IDLE || state == ENV_DELAY || release_forced_count == 0) {
        // clean start from silence
        count = delay_count;
        if (count > 0) {
            state      = ENV_DELAY;
            mult_hires = 0;
            inc_hires  = 0;
            inc_factor = 1.0f;
        } else {
            state = ENV_ATTACK;
            count = attack_count;
            initSlope(0, 0x40000000, count, attack_curve);
        }
    } else if (state != ENV_FORCED) {
        // force-release current level before re-attack (always linear)
        state = ENV_FORCED;
        count = release_forced_count;
        inc_hires  = (-mult_hires) / (int32_t)count;
        inc_factor = 1.0f;
    }

    __enable_irq();
}


// ===========================================================================
//  noteOff — begin release from whatever level we are at
// ===========================================================================
void AudioEffectEnvelopeJT::noteOff(void)
{
    __disable_irq();

    if (state != ENV_RELEASE && state != ENV_IDLE && state != ENV_FORCED) {
        state = ENV_RELEASE;
        count = release_count;
        initSlope(mult_hires, 0, count, release_curve);
    }

    __enable_irq();
}


// ===========================================================================
//  Live parameter setters
//
//  Each setter stores the new duration/level AND, if that stage is currently
//  active, recalculates the slope so the change is heard immediately.
// ===========================================================================

void AudioEffectEnvelopeJT::delay(float milliseconds)
{
    delay_count = milliseconds2count(milliseconds);
}

void AudioEffectEnvelopeJT::attack(float milliseconds)
{
    uint16_t newCount = milliseconds2count(milliseconds);
    if (newCount == 0) newCount = 1;

    __disable_irq();
    attack_count = newCount;

    if (state == ENV_ATTACK) {
        // Recompute slope from current mult_hires to unity over remaining time.
        // Scale 'count' proportionally to preserve the envelope's position.
        uint16_t remaining = count;
        if (remaining > newCount) remaining = newCount;
        if (remaining == 0) remaining = 1;
        count = remaining;
        initSlope(mult_hires, 0x40000000, count, attack_curve);
    }

    __enable_irq();
}

void AudioEffectEnvelopeJT::hold(float milliseconds)
{
    hold_count = milliseconds2count(milliseconds);
}

void AudioEffectEnvelopeJT::decay(float milliseconds)
{
    uint16_t newCount = milliseconds2count(milliseconds);
    if (newCount == 0) newCount = 1;

    __disable_irq();

    if (state == ENV_DECAY) {
        uint16_t remaining = count;
        if (remaining > newCount) remaining = newCount;
        if (remaining == 0) remaining = 1;
        count = remaining;
        decay_count = newCount;
        initSlope(mult_hires, sustain_mult, count, decay_curve);
    } else {
        decay_count = newCount;
    }

    __enable_irq();
}

void AudioEffectEnvelopeJT::sustain(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    int32_t newMult = (int32_t)(level * 1073741824.0f);  // 2.30 fixed-point

    __disable_irq();
    sustain_mult = newMult;

    if (state == ENV_SUSTAIN) {
        // snap to new level immediately
        mult_hires = newMult;
        inc_hires  = 0;
        inc_factor = 1.0f;
    } else if (state == ENV_DECAY) {
        // decay target changed — reslope from current to new sustain
        initSlope(mult_hires, newMult, count, decay_curve);
    }

    __enable_irq();
}

void AudioEffectEnvelopeJT::release(float milliseconds)
{
    uint16_t newCount = milliseconds2count(milliseconds);
    if (newCount == 0) newCount = 1;

    __disable_irq();

    if (state == ENV_RELEASE) {
        uint16_t remaining = count;
        if (remaining > newCount) remaining = newCount;
        if (remaining == 0) remaining = 1;
        count = remaining;
        release_count = newCount;
        initSlope(mult_hires, 0, count, release_curve);
    } else {
        release_count = newCount;
    }

    __enable_irq();
}

void AudioEffectEnvelopeJT::releaseNoteOn(float milliseconds)
{
    release_forced_count = milliseconds2count(milliseconds);
    if (release_forced_count == 0) release_forced_count = 1;
}


// ===========================================================================
//  update — called by the audio engine every 128 samples (~2.9 ms)
//
//  Processes 8 samples per iteration (16 iterations per block).
//  The inner multiply loop is identical to the stock envelope.
//
//  Curve shaping adds one float multiply per chunk (inc_hires *= inc_factor)
//  at the bottom of the loop.  When inc_factor == 1.0 (linear / stock),
//  the compiler may optimise this away, but even if not, it is a single
//  VMUL.F32 instruction — negligible.
// ===========================================================================
void AudioEffectEnvelopeJT::update(void)
{
    audio_block_t *block;
    uint32_t *p, *end;
    uint32_t sample12, sample34, sample56, sample78, tmp1, tmp2;

    block = receiveWritable();
    if (block) {
        if (state == ENV_IDLE) {
            AudioStream::release(block);
            return;
        }
        p = (uint32_t *)(block->data);
    } else {
        p = nullptr;
    }

    end = p + AUDIO_BLOCK_SAMPLES / 2;

    while (p < end) {

        // --- state transitions when a stage's time runs out ---
        if (count == 0) {
            if (state == ENV_ATTACK) {
                count = hold_count;
                if (count > 0) {
                    state      = ENV_HOLD;
                    mult_hires = 0x40000000;
                    inc_hires  = 0;
                    inc_factor = 1.0f;
                } else {
                    state = ENV_DECAY;
                    count = decay_count;
                    initSlope(0x40000000, sustain_mult, count, decay_curve);
                }
                continue;

            } else if (state == ENV_HOLD) {
                state = ENV_DECAY;
                count = decay_count;
                initSlope(0x40000000, sustain_mult, count, decay_curve);
                continue;

            } else if (state == ENV_DECAY) {
                state      = ENV_SUSTAIN;
                count      = 0xFFFF;
                mult_hires = sustain_mult;
                inc_hires  = 0;
                inc_factor = 1.0f;

            } else if (state == ENV_SUSTAIN) {
                count = 0xFFFF;

            } else if (state == ENV_RELEASE) {
                state = ENV_IDLE;
                while (p < end) {
                    if (block != nullptr) {
                        *p++ = 0;
                        *p++ = 0;
                        *p++ = 0;
                        *p++ = 0;
                    } else {
                        p += 4;
                    }
                }
                break;

            } else if (state == ENV_FORCED) {
                count = delay_count;
                if (count > 0) {
                    state      = ENV_DELAY;
                    mult_hires = 0;
                    inc_hires  = 0;
                    inc_factor = 1.0f;
                } else {
                    state = ENV_ATTACK;
                    count = attack_count;
                    initSlope(0, 0x40000000, count, attack_curve);
                }

            } else if (state == ENV_DELAY) {
                state = ENV_ATTACK;
                count = attack_count;
                initSlope(0, 0x40000000, count, attack_curve);
                continue;
            }
        }

        // --- apply gain to 8 samples (identical to stock inner loop) ---
        if (block != nullptr) {
            int32_t mult = mult_hires >> 14;
            int32_t inc  = inc_hires  >> 17;

            sample12 = *p++;
            sample34 = *p++;
            sample56 = *p++;
            sample78 = *p++;
            p -= 4;

            mult += inc;
            tmp1 = signed_multiply_32x16b(mult, sample12);
            mult += inc;
            tmp2 = signed_multiply_32x16t(mult, sample12);
            sample12 = pack_16b_16b(tmp2, tmp1);

            mult += inc;
            tmp1 = signed_multiply_32x16b(mult, sample34);
            mult += inc;
            tmp2 = signed_multiply_32x16t(mult, sample34);
            sample34 = pack_16b_16b(tmp2, tmp1);

            mult += inc;
            tmp1 = signed_multiply_32x16b(mult, sample56);
            mult += inc;
            tmp2 = signed_multiply_32x16t(mult, sample56);
            sample56 = pack_16b_16b(tmp2, tmp1);

            mult += inc;
            tmp1 = signed_multiply_32x16b(mult, sample78);
            mult += inc;
            tmp2 = signed_multiply_32x16t(mult, sample78);
            sample78 = pack_16b_16b(tmp2, tmp1);

            *p++ = sample12;
            *p++ = sample34;
            *p++ = sample56;
            *p++ = sample78;
        } else {
            p += 4;
        }

        // --- advance gain accumulator and apply curve ---
        mult_hires += inc_hires;
        count--;

        // geometric curve: scale increment for next chunk.
        // When inc_factor==1.0 (linear), this is a no-op multiply.
        // Cost: one VMUL.F32 + one float->int conversion per chunk.
        inc_hires = (int32_t)((float)inc_hires * inc_factor);
    }

    if (block != nullptr) {
        transmit(block);
        AudioStream::release(block);
    }
}


// ===========================================================================
//  State queries — safe to call from any thread (volatile read)
// ===========================================================================

bool AudioEffectEnvelopeJT::isActive(void)
{
    return *(volatile uint8_t *)&state != ENV_IDLE;
}

bool AudioEffectEnvelopeJT::isSustain(void)
{
    return *(volatile uint8_t *)&state == ENV_SUSTAIN;
}

EnvelopeStateJT AudioEffectEnvelopeJT::getState(void)
{
    return (EnvelopeStateJT)(*(volatile uint8_t *)&state);
}
