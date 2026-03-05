#pragma once

#include <Audio.h>

// SubOscillatorBlock provides a square wave an octave below the main oscillator
class SubOscillatorBlock {
public:
    // --- Lifecycle
    SubOscillatorBlock();
    void update();

    // --- Modulation
    void setModInputs(audio_block_t** modSources);

    // --- Parameter Setters
    void setFrequency(float freq);
    void setAmplitude(float amp);
    void setWaveform(int type);


    // --- Outputs
    AudioStream& output();

private:
    AudioSynthWaveform _subOsc;
};
