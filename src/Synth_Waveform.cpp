/* Audio Library for Teensy
 * Copyright (c) 2018, Paul Stoffregen, paul@pjrc.com
 * JT-8000 fork: class renamed AudioSynthWaveformJT (see Synth_Waveform.h).
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
 */

// =============================================================================
// Synth_Waveform.cpp — JT-8000 fork
// =============================================================================
// ONLY defines AudioSynthWaveformJT::update().
//
// BandLimitedWaveform and AudioSynthWaveform are intentionally NOT defined here.
// Their implementations live in the Teensy Audio library's synth_waveform.cpp,
// which the Arduino build system compiles unconditionally. Defining them here
// too would produce duplicate-symbol linker errors.
//
// The update() body below is derived from the library's
// AudioSynthWaveformModulated::update() with only the class name changed.
// Future delivery will refactor the modulation_type flag into a bitmask to
// allow simultaneous FM pitch (slot 0) and PWM shape (slot 1) modulation.
// =============================================================================

#include <Arduino.h>
#include "AudioSynthWaveformJT.h"

// dspinst.h lives in the Audio library's utility/ subfolder.
// Angle brackets search all library include paths, not just the sketch folder.
#include <utility/dspinst.h>

// Uncomment for higher-accuracy (but more expensive) FM exponential
//#define IMPROVE_EXPONENTIAL_ACCURACY

// Select waveform type and reinitialise band-limit state if required.
void AudioSynthWaveformJT::begin(short t_type) {
    tone_type = t_type;
    if      (t_type == WAVEFORM_BANDLIMIT_SQUARE)    band_limit_waveform.init_square(phase_increment);
    else if (t_type == WAVEFORM_BANDLIMIT_PULSE)     band_limit_waveform.init_pulse(phase_increment, 0x80000000u);
    else if (t_type == WAVEFORM_BANDLIMIT_SAWTOOTH ||
             t_type == WAVEFORM_BANDLIMIT_SAWTOOTH_REVERSE)
                                                     band_limit_waveform.init_sawtooth(phase_increment);
}

void AudioSynthWaveformJT::update(void)
{
    audio_block_t *block, *moddata, *shapedata;
    int16_t *bp, *end;
    int32_t val1, val2;
    int16_t magnitude15;
    uint32_t i, ph, index, index2, scale, priorphase;
    const uint32_t inc = phase_increment;

    // Receive optional modulation inputs.
    // receiveReadOnly() returns nullptr when nothing is connected — zero cost.
    moddata   = receiveReadOnly(0);  // FM or phase mod signal
    shapedata = receiveReadOnly(1);  // PWM / shape signal

    // -------------------------------------------------------------------------
    // Phase computation — one pass over the block populates phasedata[].
    // This separates modulation from waveform generation, keeping each
    // case in the switch below simple and branch-free.
    // -------------------------------------------------------------------------
    ph = phase_accumulator;
    priorphase = phasedata[AUDIO_BLOCK_SAMPLES - 1];

    if (moddata) {
        if (modulation_type == 0) {
            // -----------------------------------------------------------------
            // Frequency modulation: scale phase increment by 2^(mod * octaves).
            // Uses a fast integer exp2 approximation — no powf() per sample.
            // At modulation_factor = 10*4096: ±32767 input = ±10 octaves.
            // -----------------------------------------------------------------
            bp = moddata->data;
            for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                int32_t n = (*bp++) * modulation_factor; // Scaled octave count
                int32_t ipart = n >> 27;                 // Integer part (4 bits)
                n &= 0x7FFFFFF;                          // Fractional part (27 bits)
#ifdef IMPROVE_EXPONENTIAL_ACCURACY
                // Polynomial by Stefan Stenzel (music-dsp, 2014-09-03)
                int32_t x = n << 3;
                n = multiply_accumulate_32x32_rshift32_rounded(536870912, x, 1494202713);
                int32_t sq = multiply_32x32_rshift32_rounded(x, x);
                n = multiply_accumulate_32x32_rshift32_rounded(n, sq, 1934101615);
                n = n + (multiply_32x32_rshift32_rounded(sq,
                    multiply_32x32_rshift32_rounded(x, 1358044250)) << 1);
                n = n << 1;
#else
                // Fast exp2 by Laurent de Soras (musicdsp.org #106)
                n = (n + 134217728) << 3;
                n = multiply_32x32_rshift32_rounded(n, n);
                n = multiply_32x32_rshift32_rounded(n, 715827883) << 3;
                n = n + 715827882;
#endif
                uint32_t sc = n >> (14 - ipart);
                uint64_t phstep = (uint64_t)inc * sc;
                uint32_t phstep_msw = phstep >> 32;
                if (phstep_msw < 0x7FFE) {
                    ph += phstep >> 16;
                } else {
                    ph += 0x7FFE0000;
                }
                phasedata[i] = ph;
            }
        } else {
            // -----------------------------------------------------------------
            // Phase modulation: add a per-sample phase offset.
            // modulation_factor encodes degrees * (65536/180).
            // -----------------------------------------------------------------
            bp = moddata->data;
            for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                int32_t n = (int32_t)(*bp++) * (int32_t)modulation_factor;
                ph += inc;
                phasedata[i] = ph + (uint32_t)(n >> 0);
            }
        }
        release(moddata);
    } else {
        // No modulation — plain phase accumulation
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            phasedata[i] = ph;
            ph += inc;
        }
    }
    phase_accumulator = ph;

    // Silence: phase still advanced above so oscillator stays in sync.
    if (magnitude == 0) {
        if (shapedata) release(shapedata);
        return;
    }
    block = allocate();
    if (!block) {
        if (shapedata) release(shapedata);
        return;
    }
    bp = block->data;

    // -------------------------------------------------------------------------
    // Waveform generation — one sample per phasedata[] entry.
    // -------------------------------------------------------------------------
    switch (tone_type) {

    case WAVEFORM_SINE:
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            ph     = phasedata[i];
            index  = ph >> 24;
            val1   = AudioWaveformSine[index];
            val2   = AudioWaveformSine[index + 1];
            scale  = (ph >> 8) & 0xFFFF;
            val2  *= scale;
            val1  *= 0x10000 - scale;
            *bp++  = multiply_32x32_rshift32(val1 + val2, magnitude);
        }
        break;

    case WAVEFORM_ARBITRARY:
        if (!arbdata) {
            release(block);
            if (shapedata) release(shapedata);
            return;
        }
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            ph     = phasedata[i];
            index  = ph >> 24;
            index2 = index + 1;
            if (index2 >= 256) index2 = 0;
            val1   = arbdata[index];
            val2   = arbdata[index2];
            scale  = (ph >> 8) & 0xFFFF;
            val2  *= scale;
            val1  *= 0x10000 - scale;
            *bp++  = multiply_32x32_rshift32(val1 + val2, magnitude);
        }
        break;

    case WAVEFORM_PULSE:
        // With shape modulation: per-sample pulse width from shapedata.
        // Falls through to WAVEFORM_SQUARE when shapedata is nullptr.
        if (shapedata) {
            magnitude15 = signed_saturate_rshift(magnitude, 16, 1);
            for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                uint32_t width = ((shapedata->data[i] + 0x8000) & 0xFFFF) << 16;
                *bp++ = (phasedata[i] < width) ? magnitude15 : (int16_t)-magnitude15;
            }
            break;
        }
        // fall through

    case WAVEFORM_SQUARE:
        magnitude15 = signed_saturate_rshift(magnitude, 16, 1);
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            *bp++ = (phasedata[i] & 0x80000000u) ? (int16_t)-magnitude15 : magnitude15;
        }
        break;

    case WAVEFORM_BANDLIMIT_PULSE:
        if (shapedata) {
            for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                uint32_t width = ((shapedata->data[i] + 0x8000) & 0xFFFF) << 16;
                int32_t val = band_limit_waveform.generate_pulse(phasedata[i], width, i);
                *bp++ = (int16_t)((val * magnitude) >> 16);
            }
            break;
        }
        // fall through

    case WAVEFORM_BANDLIMIT_SQUARE:
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            int32_t val = band_limit_waveform.generate_square(phasedata[i], i);
            *bp++ = (int16_t)((val * magnitude) >> 16);
        }
        break;

    case WAVEFORM_SAWTOOTH:
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            *bp++ = signed_multiply_32x16t(magnitude, phasedata[i]);
        }
        break;

    case WAVEFORM_SAWTOOTH_REVERSE:
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            *bp++ = signed_multiply_32x16t(0xFFFFFFFFu - magnitude, phasedata[i]);
        }
        break;

    case WAVEFORM_BANDLIMIT_SAWTOOTH:
    case WAVEFORM_BANDLIMIT_SAWTOOTH_REVERSE:
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            int16_t val = band_limit_waveform.generate_sawtooth(phasedata[i], i);
            val = (int16_t)((val * magnitude) >> 16);
            *bp++ = (tone_type == WAVEFORM_BANDLIMIT_SAWTOOTH_REVERSE) ? (int16_t)-val : val;
        }
        break;

    case WAVEFORM_TRIANGLE_VARIABLE:
        if (shapedata) {
            for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                uint32_t width     = (shapedata->data[i] + 0x8000) & 0xFFFF;
                uint32_t rise      = 0xFFFFFFFFu / width;
                uint32_t fall      = 0xFFFFFFFFu / (0xFFFF - width);
                uint32_t halfwidth = width << 15;
                uint32_t n;
                ph = phasedata[i];
                if (ph < halfwidth) {
                    n    = (ph >> 16) * rise;
                    *bp++ = ((n >> 16) * magnitude) >> 16;
                } else if (ph < 0xFFFFFFFFu - halfwidth) {
                    n    = 0x7FFFFFFFu - (((ph - halfwidth) >> 16) * fall);
                    *bp++ = (((int32_t)n >> 16) * magnitude) >> 16;
                } else {
                    n    = ((ph + halfwidth) >> 16) * rise + 0x80000000u;
                    *bp++ = (((int32_t)n >> 16) * magnitude) >> 16;
                }
            }
            break;
        }
        // fall through

    case WAVEFORM_TRIANGLE:
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            ph = phasedata[i];
            uint32_t phtop = ph >> 30;
            if (phtop == 1 || phtop == 2) {
                *bp++ = ((0xFFFF - (ph >> 15)) * magnitude) >> 16;
            } else {
                *bp++ = (((int32_t)ph >> 15) * magnitude) >> 16;
            }
        }
        break;

    case WAVEFORM_SAMPLE_HOLD:
        for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            ph = phasedata[i];
            if (ph < priorphase) {  // Wrap detected: latch new random value
                sample = random(magnitude) - (magnitude >> 1);
            }
            priorphase = ph;
            *bp++ = sample;
        }
        break;
    }

    // Optional DC offset added to every sample (integer add, no branch per sample)
    if (tone_offset) {
        bp  = block->data;
        end = bp + AUDIO_BLOCK_SAMPLES;
        do {
            val1  = *bp;
            *bp++ = signed_saturate_rshift(val1 + tone_offset, 16, 0);
        } while (bp < end);
    }

    if (shapedata) release(shapedata);
    transmit(block, 0);
    release(block);
}
