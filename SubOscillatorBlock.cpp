#include "synth_waveform.h"
#include "SubOscillatorBlock.h"

// --- Lifecycle
SubOscillatorBlock::SubOscillatorBlock() {
    _subOsc.begin(WAVEFORM_SINE);
    _subOsc.frequency(110.0f);  // default 1 octave below A440
    _subOsc.amplitude(0.0f);
}

void SubOscillatorBlock::update() {
    // Placeholder for future use
}

// --- Modulation
void SubOscillatorBlock::setModInputs(audio_block_t** modSources) {
    // Placeholder for modulation support
}

// --- Parameter Setters
void SubOscillatorBlock::setFrequency(float freq) {
    _subOsc.frequency(freq * 0.5f);
}

void SubOscillatorBlock::setAmplitude(float amp) {
    _subOsc.amplitude(amp * 0.9f); //allow had room
}
void SubOscillatorBlock::setWaveform(int type)  {
    _subOsc.begin(type);
}

// --- Outputs
AudioStream& SubOscillatorBlock::output() {
    return _subOsc;
}
