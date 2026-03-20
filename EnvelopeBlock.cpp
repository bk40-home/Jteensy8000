// =============================================================================
// EnvelopeBlock.cpp — ADSR envelope wrapper implementation
// =============================================================================

#include "EnvelopeBlock.h"

// ---- Lifecycle --------------------------------------------------------------

void EnvelopeBlock::noteOn() {
    _envelope.noteOn();
}

void EnvelopeBlock::noteOff() {
    _envelope.noteOff();
}

// ---- Audio routing ----------------------------------------------------------

AudioStream& EnvelopeBlock::input() {
    return _envelope;
}

AudioStream& EnvelopeBlock::output() {
    return _envelope;
}

// ---- Parameter setters ------------------------------------------------------
// Each setter caches the value for UI readback AND writes to the hardware.

void EnvelopeBlock::setAttackTime(float milliseconds) {
    _attackTime = milliseconds;
    _envelope.attack(milliseconds);
}

void EnvelopeBlock::setDecayTime(float milliseconds) {
    _decayTime = milliseconds;
    _envelope.decay(milliseconds);
}

void EnvelopeBlock::setSustainLevel(float level) {
    _sustainLevel = level;
    _envelope.sustain(level);
}

void EnvelopeBlock::setReleaseTime(float milliseconds) {
    _releaseTime = milliseconds;
    _envelope.release(milliseconds);
}

void EnvelopeBlock::setADSR(float attack, float decay, float sustain, float release) {
    setAttackTime(attack);
    setDecayTime(decay);
    setSustainLevel(sustain);
    setReleaseTime(release);
}
