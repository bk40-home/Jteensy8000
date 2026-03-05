#include "DebugTrace.h"
#include "FilterBlock.h"

FilterBlock::FilterBlock() {
    _patchCables[0] = new AudioConnection(_modMixer, 0, _filter, 1);
    _patchCables[1] = new AudioConnection(_keyTrackDc, 0, _modMixer,0);

    _envModDc.amplitude(0.0f);
    _keyTrackDc.amplitude(0.0f);


static constexpr float KEY_TRACK_GAIN = 1.0f;
static constexpr float ENV_MOD_GAIN = 1.0f;
static constexpr float LFO1_GAIN_INIT = 0.0f;
static constexpr float LFO2_GAIN_INIT = 0.0f;

_modMixer.gain(0, KEY_TRACK_GAIN);
_modMixer.gain(1, ENV_MOD_GAIN);
_modMixer.gain(2, LFO1_GAIN_INIT);
_modMixer.gain(3, LFO2_GAIN_INIT);

    _filter.setCutoffModOctaves(_octaveControl);

}

void FilterBlock::setCutoff(float freqHz) {
    if (freqHz != _cutoff) {
        _cutoff = freqHz;
        _filter.frequency(freqHz);
        JT_LOGF_RATE(200, "[FLT] Cutoff: %.2f Hz\n", freqHz);
    }
}

void FilterBlock::setResonance(float amount) {
    _resonance = amount;  
    _filter.resonance(amount);
    JT_LOGF("[FLT] Resonance: %.4f\n", amount);
}



void FilterBlock::setOctaveControl(float octaves) {
    _octaveControl = octaves;
    _filter.setCutoffModOctaves(octaves);
     JT_LOGF("[FLT] Octave Ctrl: %.2f\n", octaves);
}


void FilterBlock::setEnvModAmount(float amount) {
    _envModAmount = amount;
    _envModDc.amplitude(amount);
     JT_LOGF("[FLT] Env Mod: %.2f\n", amount);
}


void FilterBlock::setKeyTrackAmount(float amount) {
    _keyTrackAmount = amount;
    _keyTrackDc.amplitude(amount);
     JT_LOGF("[FLT] Key Track: %.2f\n", amount);
}

void FilterBlock::setMultimode(float amount) {
    _multimode = amount;
    _filter.multimode(amount);
     JT_LOGF("[FLT] Multimode: %.2f\n", amount);
}

void FilterBlock::setTwoPole(bool enabled) {
    _useTwoPole = enabled;
    _filter.setTwoPole(enabled);
     JT_LOGF("[FLT] TwoPole: %d\n", (int)enabled);
}

void FilterBlock::setXpander4Pole(bool enabled) {
    _xpander4Pole = enabled;
    _filter.setXpander4Pole(enabled);
     JT_LOGF("[FLT] Xpander4P: %d\n", (int)enabled);
}

void FilterBlock::setXpanderMode(uint8_t amount) {
    _xpanderMode = amount;
    _filter.setXpanderMode(amount);
     JT_LOGF("[FLT] XpanderMode: %u\n", (unsigned)amount);
}

void FilterBlock::setBPBlend2Pole(bool enabled) {
    _bpBlend2Pole = enabled;
    _filter.setBPBlend2Pole(enabled);
     JT_LOGF("[FLT] BPBlend2P: %d\n", (int)enabled);
}

void FilterBlock::setPush2Pole(bool enabled) {
    _push2Pole = enabled;
    _filter.setPush2Pole(enabled);
     JT_LOGF("[FLT] Push2P: %d\n", (int)enabled);
}

void FilterBlock::setResonanceModDepth(float amount) {
    _resonanceModDepth = amount;
    _filter.setResonanceModDepth(amount);
     JT_LOGF("[FLT] ResModDepth: %.2f\n", amount);
}

float FilterBlock::getCutoff() const { return _cutoff; }
float FilterBlock::getResonance() const { return _resonance; }
float FilterBlock::getOctaveControl() const { return _octaveControl; }
float FilterBlock::getEnvModAmount() const { return _envModAmount; }
float FilterBlock::getKeyTrackAmount() const { return _keyTrackAmount; }

AudioStream& FilterBlock::input() { return _filter; }
AudioStream& FilterBlock::output() { return _filter; }
AudioStream& FilterBlock::envmod() { return _envModDc; };
AudioMixer4& FilterBlock::modMixer() { return _modMixer; }
