/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
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
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
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
