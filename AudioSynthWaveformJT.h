#pragma once
// =============================================================================
// AudioSynthWaveformJT.h — JT-8000 modulated oscillator
// =============================================================================
// Renamed from AudioSynthWaveformModulated to avoid duplicate-symbol linker
// errors.  The Arduino build system always compiles the Audio library's
// synth_waveform.cpp, which defines AudioSynthWaveformModulated::update().
// By using a different class name, our Synth_Waveform.cpp only defines
// AudioSynthWaveformJT::update() — a symbol the library never defines.
//
// This header has its own #pragma once guard, completely independent of the
// synth_waveform_h_ guard used by Synth_Waveform.h / the library.
// OscillatorBlock.h includes this directly, removing any include-order
// dependency for AudioSynthWaveformJT's declaration.
//
// BandLimitedWaveform is needed as a complete type (embedded member, not ptr).
// We get it by including Synth_Waveform.h — our fork if the guard hasn't fired
// yet, or the library version if it has. Both declare BandLimitedWaveform
// identically, so either satisfies the member type requirement.
// =============================================================================

// Must come before AudioStream.h so WAVEFORM_* constants and BandLimitedWaveform
// are defined. Whichever version satisfies synth_waveform_h_ first is fine —
// BandLimitedWaveform is binary-compatible between fork and library.
#include "Synth_Waveform.h"

#include <AudioStream.h>
#include <arm_math.h>

class AudioSynthWaveformJT : public AudioStream {
public:
    AudioSynthWaveformJT(void) : AudioStream(2, inputQueueArray),
        phase_accumulator(0), phase_increment(0), modulation_factor(32768),
        magnitude(0), arbdata(NULL), sample(0), tone_offset(0),
        tone_type(WAVEFORM_SINE), modulation_type(0) {}

    // -------------------------------------------------------------------------
    // Set base frequency in Hz — called ONCE on noteOn().
    // All subsequent pitch changes go through the FM input (input 0).
    // -------------------------------------------------------------------------
    void frequency(float freq) {
        if      (freq < 0.0f)                         freq = 0.0f;
        else if (freq > AUDIO_SAMPLE_RATE_EXACT/2.0f) freq = AUDIO_SAMPLE_RATE_EXACT/2.0f;
        phase_increment = freq * (4294967296.0f / AUDIO_SAMPLE_RATE_EXACT);
        if (phase_increment > 0x7FFE0000u) phase_increment = 0x7FFE0000u;
    }

    // Output amplitude 0.0–1.0
    void amplitude(float n) {
        if      (n < 0.0f) n = 0.0f;
        else if (n > 1.0f) n = 1.0f;
        magnitude = n * 65536.0f;
    }

    // DC offset added to every output sample
    void offset(float n) {
        if      (n < -1.0f) n = -1.0f;
        else if (n > 1.0f)  n = 1.0f;
        tone_offset = n * 32767.0f;
    }

    // Select waveform type; reinitialises band-limit state if needed
    void begin(short t_type);
    void begin(float t_amp, float t_freq, short t_type) {
        amplitude(t_amp); frequency(t_freq); begin(t_type);
    }

    void arbitraryWaveform(const int16_t *data, float /*maxFreq*/) { arbdata = data; }

    // -------------------------------------------------------------------------
    // Route input 0 as FREQUENCY modulation.
    // octaves: pitch swing at ±full-scale input. Must match FM_OCTAVE_RANGE.
    // Clamped 0.1–12 octaves.
    // -------------------------------------------------------------------------
    void frequencyModulation(float octaves) {
        if      (octaves > 12.0f) octaves = 12.0f;
        else if (octaves < 0.1f)  octaves = 0.1f;
        modulation_factor = octaves * 4096.0f;
        modulation_type = 0;
    }

    // -------------------------------------------------------------------------
    // Route input 0 as PHASE modulation.
    // degrees: phase swing at ±full-scale input. Clamped 30–9000°.
    // -------------------------------------------------------------------------
    void phaseModulation(float degrees) {
        if      (degrees > 9000.0f) degrees = 9000.0f;
        else if (degrees < 30.0f)   degrees = 30.0f;
        modulation_factor = degrees * (float)(65536.0 / 180.0);
        modulation_type = 1;
    }

    virtual void update(void);  // Implemented in Synth_Waveform.cpp

private:
    audio_block_t *inputQueueArray[2];

    uint32_t phase_accumulator;
    uint32_t phase_increment;
    uint32_t modulation_factor;           // Octaves×4096 (FM) or degrees×(65536/180) (PM)
    int32_t  magnitude;
    const int16_t *arbdata;
    uint32_t phasedata[AUDIO_BLOCK_SAMPLES];  // Phase after modulation, per sample
    int16_t  sample;                      // Held value for WAVEFORM_SAMPLE_HOLD
    int16_t  tone_offset;                 // DC offset added post-generation
    uint8_t  tone_type;
    uint8_t  modulation_type;             // 0 = FM, 1 = phase mod (future: bitmask)
    BandLimitedWaveform band_limit_waveform;  // Requires complete BandLimitedWaveform type
};
