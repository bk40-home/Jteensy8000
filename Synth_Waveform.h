/* Audio Library for Teensy
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction.
 */

// =============================================================================
// Synth_Waveform.h — JT-8000 local fork
// =============================================================================
// The include guard synth_waveform_h_ matches the library's guard exactly.
// Audio.h (sketch-local) includes this file FIRST so the guard is set before
// the library's Audio.h tries to include synth_waveform.h — suppressing the
// library header declarations in favour of ours.
//
// WHAT THIS FILE CONTAINS vs the library version:
//   - BandLimitedWaveform:    DECLARATION ONLY (implementation in library .cpp)
//   - AudioSynthWaveform:     DECLARATION ONLY (implementation in library .cpp)
//   - AudioSynthWaveformModulated: OMITTED — nobody in JT-8000 uses it.
//     The library .cpp still defines it; the linker discards it as dead code.
//
// AudioSynthWaveformJT (our modulated oscillator) lives in AudioSynthWaveformJT.h
// with its own #pragma once guard, independent of this file.
// =============================================================================

#ifndef synth_waveform_h_
#define synth_waveform_h_

#include <Arduino.h>
#include <AudioStream.h>
#include <arm_math.h>

// Sine lookup table — defined in the Audio library's waveforms.c
extern "C" {
extern const int16_t AudioWaveformSine[257];
}

// ---------------------------------------------------------------------------
// Waveform type constants — identical to library, do not change.
// ---------------------------------------------------------------------------
#define WAVEFORM_SINE                       0
#define WAVEFORM_SAWTOOTH                   1
#define WAVEFORM_SQUARE                     2
#define WAVEFORM_TRIANGLE                   3
#define WAVEFORM_ARBITRARY                  4
#define WAVEFORM_PULSE                      5
#define WAVEFORM_SAWTOOTH_REVERSE           6
#define WAVEFORM_SAMPLE_HOLD                7
#define WAVEFORM_TRIANGLE_VARIABLE          8
#define WAVEFORM_BANDLIMIT_SAWTOOTH         9
#define WAVEFORM_BANDLIMIT_SAWTOOTH_REVERSE 10
#define WAVEFORM_BANDLIMIT_SQUARE           11
#define WAVEFORM_BANDLIMIT_PULSE            12

// ---------------------------------------------------------------------------
// BandLimitedWaveform — DECLARATION ONLY.
// Implementation lives in the Audio library's synth_waveform.cpp.
// Declared here because suppressing the library header (via guard) means
// classes that embed BandLimitedWaveform as a member must still see it.
// ---------------------------------------------------------------------------
typedef struct step_state {
    int  offset;
    bool positive;
} step_state;

class BandLimitedWaveform {
public:
    BandLimitedWaveform(void);
    int16_t generate_sawtooth(uint32_t new_phase, int i);
    int16_t generate_square(uint32_t new_phase, int i);
    int16_t generate_pulse(uint32_t new_phase, uint32_t pulse_width, int i);
    void    init_sawtooth(uint32_t freq_word);
    void    init_square(uint32_t freq_word);
    void    init_pulse(uint32_t freq_word, uint32_t pulse_width);
private:
    int32_t    lookup(int offset);
    void       insert_step(int offset, bool rising, int i);
    int32_t    process_step(int i);
    int32_t    process_active_steps(uint32_t new_phase);
    int32_t    process_active_steps_saw(uint32_t new_phase);
    int32_t    process_active_steps_pulse(uint32_t new_phase, uint32_t pulse_width);
    void       new_step_check_square(uint32_t new_phase, int i);
    void       new_step_check_pulse(uint32_t new_phase, uint32_t pulse_width, int i);
    void       new_step_check_saw(uint32_t new_phase, int i);
    uint32_t   phase_word;
    int32_t    dc_offset;
    step_state states[32];   // Circular buffer of active band-limit steps
    int        newptr;
    int        delptr;
    int32_t    cyclic[16];   // Circular buffer of output samples
    bool       pulse_state;
    uint32_t   sampled_width;
};

// ---------------------------------------------------------------------------
// AudioSynthWaveform (non-modulated) — DECLARATION ONLY.
// Implementation lives in the Audio library's synth_waveform.cpp.
// Used by LFOBlock and SubOscillatorBlock unchanged from library version.
// ---------------------------------------------------------------------------
class AudioSynthWaveform : public AudioStream {
public:
    AudioSynthWaveform(void) : AudioStream(0, NULL),
        phase_accumulator(0), phase_increment(0), phase_offset(0),
        magnitude(0), pulse_width(0x40000000),
        arbdata(NULL), sample(0), tone_type(WAVEFORM_SINE), tone_offset(0) {}

    void frequency(float freq) {
        if      (freq < 0.0f)                         freq = 0.0f;
        else if (freq > AUDIO_SAMPLE_RATE_EXACT/2.0f) freq = AUDIO_SAMPLE_RATE_EXACT/2.0f;
        phase_increment = freq * (4294967296.0f / AUDIO_SAMPLE_RATE_EXACT);
        if (phase_increment > 0x7FFE0000u) phase_increment = 0x7FFE0000u;
    }
    void phase(float angle) {
        if      (angle < 0.0f)    angle = 0.0f;
        else if (angle > 360.0f) { angle -= 360.0f; if (angle >= 360.0f) return; }
        phase_offset = angle * (float)(4294967296.0 / 360.0);
    }
    void amplitude(float n) {
        if      (n < 0.0f) n = 0.0f;
        else if (n > 1.0f) n = 1.0f;
        magnitude = n * 65536.0f;
    }
    void offset(float n) {
        if      (n < -1.0f) n = -1.0f;
        else if (n > 1.0f)  n = 1.0f;
        tone_offset = n * 32767.0f;
    }
    void pulseWidth(float n) {
        if      (n < 0.0f) n = 0.0f;
        else if (n > 1.0f) n = 1.0f;
        pulse_width = n * 4294967296.0f;
    }
    void begin(short t_type) {
        phase_offset = 0;
        tone_type = t_type;
        if      (t_type == WAVEFORM_BANDLIMIT_SQUARE)    band_limit_waveform.init_square(phase_increment);
        else if (t_type == WAVEFORM_BANDLIMIT_PULSE)     band_limit_waveform.init_pulse(phase_increment, pulse_width);
        else if (t_type == WAVEFORM_BANDLIMIT_SAWTOOTH ||
                 t_type == WAVEFORM_BANDLIMIT_SAWTOOTH_REVERSE)
                                                         band_limit_waveform.init_sawtooth(phase_increment);
    }
    void begin(float t_amp, float t_freq, short t_type) {
        amplitude(t_amp); frequency(t_freq); phase_offset = 0; begin(t_type);
    }
    void arbitraryWaveform(const int16_t *data, float /*maxFreq*/) { arbdata = data; }
    virtual void update(void);  // Implemented in library synth_waveform.cpp

private:
    uint32_t phase_accumulator;
    uint32_t phase_increment;
    uint32_t phase_offset;
    int32_t  magnitude;
    uint32_t pulse_width;
    const int16_t *arbdata;
    int16_t  sample;
    short    tone_type;
    int16_t  tone_offset;
    BandLimitedWaveform band_limit_waveform;
};

#endif // synth_waveform_h_
