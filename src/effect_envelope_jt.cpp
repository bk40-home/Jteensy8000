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
//  recalcSlope — recompute inc_hires from current mult_hires toward target
//
//  Called when:
//    • a state transition fires (same as stock)
//    • a setter is called while that stage is already running (NEW)
//
//  The curve exponent warps the remaining normalised time position so that
//  the slope is steeper or shallower.  When curve == 1.0 this collapses to
//  the stock linear calculation with zero extra cost (powf is skipped).
//
//  The warp is applied once here — the per-sample inner loop stays the same
//  cheap multiply-and-accumulate as the stock code.  This means the curve is
//  a piecewise-linear approximation to the true power curve, which is
//  perceptually indistinguishable at audio rates and costs nothing per sample.
// ===========================================================================
void AudioEffectEnvelopeJT::recalcSlope(int32_t target, uint16_t remaining, float curve)
{
    if (remaining == 0) {
        // no time left — snap to target instantly
        mult_hires = target;
        inc_hires  = 0;
        return;
    }

    if (curve != 1.0f) {
        // -----------------------------------------------------------------
        //  Curved slope approximation:
        //  Figure out what fraction of this stage has already elapsed,
        //  warp both the current and next-chunk positions through the
        //  power curve, then derive the linear increment that connects them.
        //
        //  This is evaluated ONCE per setter call / state transition,
        //  NOT per sample.  Cost: two powf() calls at transition time.
        // -----------------------------------------------------------------
        float total;

        // determine which stage we are in to get the total count
        switch (state) {
            case ENV_ATTACK:  total = (float)attack_count;          break;
            case ENV_DECAY:   total = (float)decay_count;           break;
            case ENV_RELEASE: total = (float)release_count;         break;
            case ENV_FORCED:  total = (float)release_forced_count;  break;
            default:          total = (float)remaining;             break;
        }

        if (total < 1.0f) total = 1.0f;  // guard against zero

        // normalised position: 0.0 = stage start, 1.0 = stage end
        float elapsed   = total - (float)remaining;
        float t_now     = elapsed / total;
        float t_next    = (elapsed + 1.0f) / total;
        if (t_next > 1.0f) t_next = 1.0f;

        // warp through power curve
        float w_now  = applyCurve(t_now,  curve);
        float w_next = applyCurve(t_next, curve);

        // the full amplitude range this stage covers
        int32_t start_val = (state == ENV_ATTACK)
                          ? 0                     // attack always starts from silence
                          : (int32_t)0x40000000;  // decay/release start from unity

        int32_t range = target - start_val;

        // where we should be now vs next chunk, in fixed-point
        int32_t val_now  = start_val + (int32_t)((float)range * w_now);
        int32_t val_next = start_val + (int32_t)((float)range * w_next);

        // snap mult_hires to the curve and set slope for next chunk
        mult_hires = val_now;
        inc_hires  = val_next - val_now;
    } else {
        // -----------------------------------------------------------------
        //  Linear slope — identical to stock behaviour, zero extra cost.
        // -----------------------------------------------------------------
        inc_hires = (target - mult_hires) / (int32_t)remaining;
    }
}


// ===========================================================================
//  noteOn — begin or re-trigger the envelope
//
//  Three paths:
//    1. Idle / delay / instant retrigger → start from silence
//    2. Already in forced release        → do nothing (let it finish)
//    3. Any other active state           → enter forced release first
// ===========================================================================
void AudioEffectEnvelopeJT::noteOn(void)
{
    __disable_irq();

    if (state == ENV_IDLE || state == ENV_DELAY || release_forced_count == 0) {
        // --- path 1: clean start from silence ---
        mult_hires = 0;
        count = delay_count;
        if (count > 0) {
            state     = ENV_DELAY;
            inc_hires = 0;
        } else {
            state = ENV_ATTACK;
            count = attack_count;
            recalcSlope(0x40000000, count, attack_curve);
        }
    } else if (state != ENV_FORCED) {
        // --- path 3: force-release current level before re-attack ---
        state = ENV_FORCED;
        count = release_forced_count;
        inc_hires = (-mult_hires) / (int32_t)count;  // always linear for forced
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
        recalcSlope(0, count, release_curve);
    }

    __enable_irq();
}


// ===========================================================================
//  Live parameter setters
//
//  Each setter stores the new duration / level AND, if that stage is
//  currently running, recalculates the slope so the change is heard
//  immediately without restarting the envelope.
// ===========================================================================

void AudioEffectEnvelopeJT::delay(float milliseconds)
{
    delay_count = milliseconds2count(milliseconds);
    // delay stage has no slope — nothing to recalculate live
}

void AudioEffectEnvelopeJT::attack(float milliseconds)
{
    uint16_t newCount = milliseconds2count(milliseconds);
    if (newCount == 0) newCount = 1;  // prevent divide-by-zero

    __disable_irq();
    attack_count = newCount;

    if (state == ENV_ATTACK) {
        // --- live update: scale remaining time proportionally ---
        //  If old attack was 100 chunks and we were at chunk 60 (40 left),
        //  and new attack is 200 chunks, remaining becomes 80.
        //  This keeps the envelope at the same proportional position.
        float progress = (count > 0 && attack_count > 0)
                       ? 1.0f - ((float)count / (float)attack_count)
                       : 0.0f;

        // clamp — can overshoot if old count was very small
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;

        count = (uint16_t)((1.0f - progress) * (float)newCount);
        if (count == 0) count = 1;
        attack_count = newCount;  // store again after calculation

        recalcSlope(0x40000000, count, attack_curve);
    }

    __enable_irq();
}

void AudioEffectEnvelopeJT::hold(float milliseconds)
{
    hold_count = milliseconds2count(milliseconds);
    // hold stage has no slope — flat at unity, nothing to recalculate
}

void AudioEffectEnvelopeJT::decay(float milliseconds)
{
    uint16_t newCount = milliseconds2count(milliseconds);
    if (newCount == 0) newCount = 1;

    __disable_irq();

    if (state == ENV_DECAY) {
        float progress = (count > 0 && decay_count > 0)
                       ? 1.0f - ((float)count / (float)decay_count)
                       : 0.0f;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;

        count = (uint16_t)((1.0f - progress) * (float)newCount);
        if (count == 0) count = 1;
        decay_count = newCount;

        recalcSlope(sustain_mult, count, decay_curve);
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
        // --- snap to new sustain level immediately ---
        mult_hires = newMult;
        inc_hires  = 0;
    } else if (state == ENV_DECAY) {
        // --- decay target changed — recalculate slope to new sustain ---
        recalcSlope(newMult, count, decay_curve);
    }

    __enable_irq();
}

void AudioEffectEnvelopeJT::release(float milliseconds)
{
    uint16_t newCount = milliseconds2count(milliseconds);
    if (newCount == 0) newCount = 1;

    __disable_irq();

    if (state == ENV_RELEASE) {
        float progress = (count > 0 && release_count > 0)
                       ? 1.0f - ((float)count / (float)release_count)
                       : 0.0f;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;

        count = (uint16_t)((1.0f - progress) * (float)newCount);
        if (count == 0) count = 1;
        release_count = newCount;

        recalcSlope(0, count, release_curve);
    } else {
        release_count = newCount;
    }

    __enable_irq();
}

void AudioEffectEnvelopeJT::releaseNoteOn(float milliseconds)
{
    release_forced_count = milliseconds2count(milliseconds);
    if (release_forced_count == 0) release_forced_count = 1;
    // forced release is transient — no live recalc needed
}


// ===========================================================================
//  update — called by the audio engine every 128 samples (~2.9 ms)
//
//  Processes 8 samples at a time (16 iterations per block).
//  The inner multiply loop is identical to the stock envelope for
//  maximum performance — curve shaping only affects the slope
//  calculated at state transitions.
//
//  Key optimisation: when state is IDLE, we release the block and
//  return immediately — no sample processing at all.
// ===========================================================================
void AudioEffectEnvelopeJT::update(void)
{
    audio_block_t *block;
    uint32_t *p, *end;
    uint32_t sample12, sample34, sample56, sample78, tmp1, tmp2;

    block = receiveWritable();
    if (block) {
        if (state == ENV_IDLE) {
            // --- fast exit: envelope is off, silence the block ---
            AudioStream::release(block);
            return;
        }
        p = (uint32_t *)(block->data);
    } else {
        p = nullptr;
    }

    end = p + AUDIO_BLOCK_SAMPLES / 2;

    // --- main loop: must run even with no block so state machine advances ---
    while (p < end) {

        // --- state transitions happen when a stage's time runs out ---
        if (count == 0) {
            if (state == ENV_ATTACK) {
                // attack complete → hold or decay
                count = hold_count;
                if (count > 0) {
                    state      = ENV_HOLD;
                    mult_hires = 0x40000000;   // snap to unity
                    inc_hires  = 0;
                } else {
                    state = ENV_DECAY;
                    count = decay_count;
                    recalcSlope(sustain_mult, count, decay_curve);
                }
                continue;   // re-evaluate immediately in case count is still 0

            } else if (state == ENV_HOLD) {
                // hold complete → decay
                state = ENV_DECAY;
                count = decay_count;
                recalcSlope(sustain_mult, count, decay_curve);
                continue;

            } else if (state == ENV_DECAY) {
                // decay complete → sustain (indefinite)
                state      = ENV_SUSTAIN;
                count      = 0xFFFF;
                mult_hires = sustain_mult;
                inc_hires  = 0;

            } else if (state == ENV_SUSTAIN) {
                // sustain: just reload the counter (runs forever)
                count = 0xFFFF;

            } else if (state == ENV_RELEASE) {
                // release complete → idle, zero remaining samples
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
                break;  // exit main loop

            } else if (state == ENV_FORCED) {
                // forced release complete → restart from delay/attack
                mult_hires = 0;
                count = delay_count;
                if (count > 0) {
                    state     = ENV_DELAY;
                    inc_hires = 0;
                } else {
                    state = ENV_ATTACK;
                    count = attack_count;
                    recalcSlope(0x40000000, count, attack_curve);
                }

            } else if (state == ENV_DELAY) {
                // delay complete → attack
                state = ENV_ATTACK;
                count = attack_count;
                recalcSlope(0x40000000, count, attack_curve);
                continue;
            }
        }

        // --- apply gain to 8 samples (identical to stock inner loop) ---
        if (block != nullptr) {
            int32_t mult = mult_hires >> 14;   // 16-bit working resolution
            int32_t inc  = inc_hires  >> 17;

            // read 4 × uint32 = 8 × int16 samples
            sample12 = *p++;
            sample34 = *p++;
            sample56 = *p++;
            sample78 = *p++;
            p -= 4;

            // multiply each sample pair by the ramping gain
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

            // write processed samples back
            *p++ = sample12;
            *p++ = sample34;
            *p++ = sample56;
            *p++ = sample78;
        } else {
            // no block — advance pointer to keep state machine in sync
            p += 4;
        }

        // --- advance the high-resolution gain accumulator ---
        mult_hires += inc_hires;
        count--;
    }

    // --- transmit the processed block downstream ---
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
