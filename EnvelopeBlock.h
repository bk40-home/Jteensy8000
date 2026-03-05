#include "effect_envelope.h"
#pragma once
#include <Audio.h>  // Assume for now

// EnvelopeBlock handles ADSR envelope generation
class EnvelopeBlock {
public:
    // --- Lifecycle

    AudioStream& input();
    //void update();
    void noteOn();
    void noteOff();

    // --- Outputs
    //float getValue();
    AudioStream& output();

    // --- Parameter Setters
    void setAttackTime(float time);
    void setDecayTime(float time);
    void setSustainLevel(float level);
    void setReleaseTime(float time);
    void setADSR(float attack, float decay, float sustain, float release);

    float getAttackTime() const { return _attackTime; }
    float getDecayTime() const { return _decayTime; }
    float getSustainLevel() const { return _sustainLevel; }
    float getReleaseTime() const { return _releaseTime; }
    //bool isIdle() const { return _envelope.isActive() == false; }


private:
    // enum Stage { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };
    // Stage _stage = IDLE;
    
    AudioEffectEnvelope _envelope;
    
    float _value = 0.0f;
    float _attackTime = 0.01f;
    float _decayTime = 0.1f;
    float _sustainLevel = 0.8f;
    float _releaseTime = 0.2f;

    float _attackRate = 0.0f;
    float _decayRate = 0.0f;
    float _releaseRate = 0.0f;

    unsigned long _lastUpdateMicros = 0;

    void calculateRates();
};