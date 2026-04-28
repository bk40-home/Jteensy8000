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
/*
 * GlobalFX.cpp — see GlobalFX.h for design notes.
 */

#include "GlobalFX.h"

// ---------------------------------------------------------------------------
// Constructor — no AudioConnections here (begin() handles that).
// Sets the reverb's one-shot defaults that FXChainBlock used to set.
// ---------------------------------------------------------------------------
GlobalFX::GlobalFX()
    : _plateReverb()
{
    // Start bypassed — tank is off until master wet rises above threshold.
    _plateReverb.bypass_set(true);

    // 100% wet internal mix: the reverb's dry output path is unused (layer
    // dry is already on the perf mixer separately). The tank's only job is
    // to produce wet; _wetAmpL/R scales it on the way out.
    _plateReverb.mix(1.0f);

    // Initial sizing/damping — get overwritten by preset load, but give
    // sensible state on first boot.
    _plateReverb.size   (_roomSize);
    _plateReverb.hidamp (_hiDamp);
    _plateReverb.lodamp (_loDamp);

    // Fine-tuning defaults previously set once in FXChainBlock's ctor.
    // Kept here unchanged so tails sound identical to pre-Phase-3.
    _plateReverb.diffusion             (0.65f);  // dense wash
    _plateReverb.shimmerPitchNormalized(1.0f);   // +12 semitones
    _plateReverb.pitchSemitones        (0);      // no pitch shift
    _plateReverb.pitchMix              (0.0f);   // pitch-shifted reverb off
    _plateReverb.freezeBleedIn         (0.0f);   // total mute during freeze

    // Send-sum mixer defaults — unity on both layer slots so taps pass
    // through at full amplitude into the reverb tank. Layers do not scale
    // per-layer; if you want one layer drier, lower its volume.
    _sendMixL.gain(0, 1.0f);  // Layer A L
    _sendMixL.gain(1, 1.0f);  // Layer B L
    _sendMixL.gain(2, 0.0f);  // spare
    _sendMixL.gain(3, 0.0f);  // spare
    _sendMixR.gain(0, 1.0f);
    _sendMixR.gain(1, 1.0f);
    _sendMixR.gain(2, 0.0f);
    _sendMixR.gain(3, 0.0f);

    // Wet-out amps start at 0 — setReverbMix() from preset/CC raises them.
    // Starting silent means no audible reverb on boot until asked for.
    _wetAmpL.gain(0.0f);
    _wetAmpR.gain(0.0f);
}

GlobalFX::~GlobalFX()
{
    delete _patchSendLToReverb;
    delete _patchSendRToReverb;
    delete _patchReverbToWetL;
    delete _patchReverbToWetR;
}

// ---------------------------------------------------------------------------
// begin() — wire internal connections. Call after AudioMemory().
// External inputs (from FXChainBlock A/B) and outputs (to perf mixer) are
// wired by LayerManager once it knows where the consumers live.
// ---------------------------------------------------------------------------
void GlobalFX::begin()
{
    // Stereo in: send-sum mixers feed the reverb tank.
    _patchSendLToReverb = new AudioConnection(_sendMixL, 0, _plateReverb, 0);
    _patchSendRToReverb = new AudioConnection(_sendMixR, 0, _plateReverb, 1);

    // Stereo out: reverb tank feeds the master-wet amplifiers. LayerManager
    // takes it from there, wiring the amplifier outputs into the perf mixer.
    _patchReverbToWetL = new AudioConnection(_plateReverb, 0, _wetAmpL, 0);
    _patchReverbToWetR = new AudioConnection(_plateReverb, 1, _wetAmpR, 0);
}

// ===========================================================================
// TANK PARAMETERS
// ===========================================================================

void GlobalFX::setReverbRoomSize(float size)
{
    size = constrain(size, 0.0f, 1.0f);
    _roomSize = size;
    _plateReverb.size(size);
}

void GlobalFX::setReverbHiDamping(float damp)
{
    damp = constrain(damp, 0.0f, 1.0f);
    _hiDamp = damp;
    _plateReverb.hidamp(damp);
}

void GlobalFX::setReverbLoDamping(float damp)
{
    damp = constrain(damp, 0.0f, 1.0f);
    _loDamp = damp;
    _plateReverb.lodamp(damp);
}

// ===========================================================================
// MASTER WET LEVEL — FX_REVERB_MIX writes here.
// ===========================================================================
//
// Scales the reverb tank output on its way into the perf mixer. Both
// channels usually set to the same value; independent L/R stays available
// for future stereo tricks.
//
// Also drives the auto-bypass check: if both channels drop below
// GLOBAL_REVERB_MIX_THRESHOLD the tank bypasses to save CPU.
// ===========================================================================
void GlobalFX::setReverbMix(float left, float right)
{
    _mixL = left;
    _mixR = right;
    _wetAmpL.gain(left);
    _wetAmpR.gain(right);
    updateReverbBypass();
}

// ===========================================================================
// MANUAL BYPASS
// ===========================================================================
void GlobalFX::setReverbBypass(bool bypass)
{
    _manualBypass = bypass;
    updateReverbBypass();
}

// ===========================================================================
// EXTENDED CONTROLS
// ===========================================================================

void GlobalFX::setReverbShimmer(float amount)
{
    amount = constrain(amount, 0.0f, 1.0f);
    _shimmer = amount;
    _plateReverb.shimmer(amount);
}

void GlobalFX::setReverbFreeze(bool frozen)
{
    _frozen = frozen;
    _plateReverb.freeze(frozen);
}

void GlobalFX::setReverbLowpass(float amount)
{
    amount = constrain(amount, 0.0f, 1.0f);
    _lowpass = amount;
    _plateReverb.lowpass(amount);
}

void GlobalFX::setReverbHipass(float amount)
{
    amount = constrain(amount, 0.0f, 1.0f);
    _hipass = amount;
    _plateReverb.hipass(amount);
}

// ===========================================================================
// updateReverbBypass — bypass when the tank would be inaudible.
//
// Rules (priority order):
//   1. Manual bypass override        → bypass.
//   2. Both master mix channels <thr → bypass.
//   3. Otherwise                     → active.
//
// We don't factor per-layer conditions into the bypass because layers
// don't scale here. Master wet mix alone decides audibility.
// ===========================================================================
void GlobalFX::updateReverbBypass()
{
    const bool audible = !_manualBypass &&
                         (_mixL > GLOBAL_REVERB_MIX_THRESHOLD ||
                          _mixR > GLOBAL_REVERB_MIX_THRESHOLD);
    _plateReverb.bypass_set(!audible);
}
