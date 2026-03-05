#include "EnvelopeBlock.h"

// --- Lifecycle
void EnvelopeBlock::noteOn() {
    // _stage = ATTACK;
    // calculateRates();
    // _lastUpdateMicros = micros();
    _envelope.noteOn();
}

void EnvelopeBlock::noteOff() {
    // _stage = RELEASE;
    // calculateRates();
    // _lastUpdateMicros = micros();
    _envelope.noteOff();
}

// void EnvelopeBlock::update() {
//     unsigned long now = micros();
//     float deltaTime = (now - _lastUpdateMicros) / 1e6f;
//     _lastUpdateMicros = now;

//     switch (_stage) {
//         case ATTACK:
//             if (_attackTime > 0){
//             _value += deltaTime / _attackTime;
//                 if (_value >= 1.0f) {
//                     _value = 1.0f;
//                     _stage = DECAY;
//                 }
//             }
//             else{
//                 _value = 1.0f;
//                 _stage = DECAY;
//             }
//             break;

//         case DECAY:
//             _value -= deltaTime * (1.0f - _sustainLevel) / _decayTime;
//             if (_value <= _sustainLevel) {
//                 _value = _sustainLevel;
//                 _stage = SUSTAIN;
//             }
//             break;

//         case SUSTAIN:
//             _value = _sustainLevel;
//             if (_value <= 0.0f) {
//                 _value = 0.0f;
//                 _stage = IDLE;
//             }
//             break;

//         case RELEASE:
//             _value -= deltaTime * _sustainLevel / _releaseTime;
//             if (_value <= 0.0f) {
//                 _value = 0.0f;
//                 _stage = IDLE;
//             }
//             break;

//         case IDLE:
//         default:
//             _value = 0.0f;
//             break;
//     }
// }

// // --- Outputs
// float EnvelopeBlock::getValue() {
//     return _value;
// }

AudioStream& EnvelopeBlock::input() {
    return _envelope;
}
AudioStream& EnvelopeBlock::output() {
    return _envelope;
}
// --- Parameter Setters
void EnvelopeBlock::setAttackTime(float time) {
     _attackTime = time;
    // calculateRates();
    _envelope.attack(time);
}

void EnvelopeBlock::setDecayTime(float time) {
    _decayTime = time;
    _envelope.decay(_decayTime);
    //calculateRates();
}

void EnvelopeBlock::setSustainLevel(float level) {
    _sustainLevel = level;
    _envelope.sustain(_sustainLevel);
}

void EnvelopeBlock::setReleaseTime(float time) {
    _releaseTime = time;
    _envelope.release(_releaseTime);
    //calculateRates();
}

void EnvelopeBlock::setADSR(float attack, float decay, float sustain, float release) {
    setAttackTime(attack);
    setDecayTime(decay);
    setSustainLevel(sustain);
    setReleaseTime(release);
}

// void EnvelopeBlock::calculateRates() {
//     // No rate calculations needed with real-time delta applied
// }