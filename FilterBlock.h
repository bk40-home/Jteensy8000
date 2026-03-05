#pragma once
#include <Audio.h>
#include "AudioFilterOBXa_OBXf.h"


class FilterBlock {
public:
    FilterBlock();

    void setCutoff(float freqHz);
    void setResonance(float amount);
    //void setDrive(float amount);
    void setOctaveControl(float octaves);
   // void setPassbandGain(float gain);

    void setEnvModAmount(float amount);
    void setKeyTrackAmount(float amount);

    float getCutoff() const;
    float getResonance() const;
    //float getDrive() const;
    float getOctaveControl() const;
    //float getPassbandGain() const;
    float getEnvModAmount() const;
    float getKeyTrackAmount() const;

    void setMultimode(float _multimode);        // 0..1 (when xpander4Pole=false)
    float getMultimode() const { return _multimode; }
    // --- Runtime mode toggles ---
    void setTwoPole(bool enabled);
    bool getTwoPole() const { return _useTwoPole; }

    void setXpander4Pole(bool enabled);
    bool getXpander4Pole() const { return _xpander4Pole; }

    void setXpanderMode(uint8_t mode);   // 0..14
    uint8_t getXpanderMode() const { return _xpanderMode; }

    // 2-pole options (only used in 2-pole mode)
    void setBPBlend2Pole(bool enabled);
    bool getBPBlend2Pole() const { return _bpBlend2Pole; }

    void setPush2Pole(bool enabled);
    bool getPush2Pole() const { return _push2Pole; }

    // --- Modulation scaling (Audio input busses) ---

    // Resonance modulation depth in resonance-01 units per +1.0 on input2
    void setResonanceModDepth(float depth01);
    float getResonanceModDepth() const { return _resonanceModDepth; }



    // Envelope modulation: octaves per +1 envelope value (0..1 env typical).
    void setEnvModOctaves(float oct);
    float getEnvModOctaves() const { return _envModAmount; }

    void setMidiNote(float note);    // 0..127
    float getMidiNote() const { return _midiNote; }

    void setEnvValue(float env01);   // 0..1 (latest envelope sample)
    float getEnvValue() const { return _envValue; }

    AudioStream& input();
    AudioStream& output();
    AudioStream& envmod();
    AudioMixer4& modMixer();

private:
    AudioFilterOBXa _filter;
    AudioMixer4 _modMixer;
    AudioSynthWaveformDc _envModDc; // going to patch this to the input of the Filter envelope
    AudioSynthWaveformDc _keyTrackDc;

    float _cutoff = 0.0f;
    float _resonance = 0.0f;
    //float _drive = 1.0f;
    float _octaveControl = 4.0f;
    //float _passbandGain = 1.0f;
    float _envModAmount = 0.0f;
    float _keyTrackAmount = 0.0f;
    float _multimode    = 0.0f;
    float _resonanceModDepth = 0.0f;

    float _midiNote = 0.0f;
    float _envValue = 0.0f;

    bool    _useTwoPole   = false;
    bool    _xpander4Pole = false;
    uint8_t _xpanderMode  = 0;
    bool    _bpBlend2Pole = false;
    bool    _push2Pole    = false;

    AudioConnection* _patchCables[2];
};
