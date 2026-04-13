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
 * FXChainBlock.cpp
 * ================
 * See FXChainBlock.h for signal-flow diagram and design notes.
 *
 *                JPFX Direct ───────↗
 *
 * MIXER CHANNELS:
 *   Channel 0: Dry (from amp, pre-JPFX)
 *   Channel 1: JPFX wet output (can bypass reverb)
 * Parameter clamping is done once at the setter boundary so the audio
 * update path never sees out-of-range values.  All constexpr name tables
 * live here in the translation unit; the header only declares the interface.
 */

#include "FXChainBlock.h"

// ---------------------------------------------------------------------------
// Effect name tables — must stay in sync with AudioEffectJPFX enum order.
// Stored in flash (PROGMEM would need F() wrappers everywhere, so plain
// const char* in .rodata is the pragmatic choice for Teensy 4.1).
// ---------------------------------------------------------------------------

// 11 modulation presets (index 0..10)
static const char* const MOD_EFFECT_NAMES[11] = {
    "Chorus 1",     // 0
    "Chorus 2",     // 1
    "Chorus 3",     // 2
    "Flanger 1",    // 3
    "Flanger 2",    // 4
    "Deep Flanger", // 5
    "Phaser 1",     // 6
    "Phaser 2",     // 7
    "Phaser 3",     // 8
    "Phaser 4",     // 9
    "Super Chorus", // 10
};

// 5 delay presets (index 0..4)
static const char* const DELAY_EFFECT_NAMES[5] = {
    "Mono Short",   // 0
    "Mono Long",    // 1
    "Pan L>R",      // 2
    "Pan R>L",      // 3
    "Pan Stereo",   // 4
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

FXChainBlock::FXChainBlock()
    : _jpfx(), _plateReverb()
{
    // --- Reverb defaults ---
    // Start bypassed so the expensive reverb DSP is off until the user
    // raises the reverb mix.  updateReverbBypass() manages this thereafter.
    _plateReverb.bypass_set(true);
    _plateReverb.mix(1.0f);             // Fully wet; dry is handled by _mixerOutL/R
    _plateReverb.size(_reverbRoomSize);
    _plateReverb.hidamp(_reverbHiDamp);
    _plateReverb.lodamp(_reverbLoDamp);

    // Compile-time reverb parameter defaults — no live CC assigned for these.
    // Set once here; adjust per patch via kDefaults[] if needed.
    _plateReverb.diffusion(0.65f);           // dense wash (high diffusion)
    _plateReverb.shimmerPitchNormalized(1.0f); // +12 semitones (octave up)
    _plateReverb.pitchSemitones(0);          // no reverb pitch shift
    _plateReverb.pitchMix(0.0f);             // reverb pitch off by default
    _plateReverb.freezeBleedIn(0.0f);        // total mute during freeze

    // --- Wire audio graph ---
    // JPFX stereo out → reverb stereo in
    _patchJPFXtoReverbL = new AudioConnection(_jpfx, 0, _plateReverb, 0);
    _patchJPFXtoReverbR = new AudioConnection(_jpfx, 1, _plateReverb, 1);

    // JPFX stereo out → output mixer ch1 (JPFX direct / pre-reverb wet)
    _patchJPFXtoMixerL  = new AudioConnection(_jpfx, 0, _mixerOutL, 1);
    _patchJPFXtoMixerR  = new AudioConnection(_jpfx, 1, _mixerOutR, 1);

    // Reverb stereo out → output mixer ch2 (reverb wet)
    _patchReverbToMixerL = new AudioConnection(_plateReverb, 0, _mixerOutL, 2);
    _patchReverbToMixerR = new AudioConnection(_plateReverb, 1, _mixerOutR, 2);

    // --- Output mixer defaults ---
    // Ch0 (dry) gain is 1.0; it is wired from SynthEngine, not here.
    _mixerOutL.gain(0, 1.0f);   // dry
    _mixerOutL.gain(1, 0.0f);   // JPFX wet (off until user enables FX)
    _mixerOutL.gain(2, 0.0f);   // reverb wet
    _mixerOutL.gain(3, 0.0f);   // spare

    _mixerOutR.gain(0, 1.0f);
    _mixerOutR.gain(1, 0.0f);
    _mixerOutR.gain(2, 0.0f);
    _mixerOutR.gain(3, 0.0f);

    // --- JPFX defaults (all effects off) ---
    _jpfx.setSaturation(0.0f);
    _jpfx.setBassGain(0.0f);
    _jpfx.setTrebleGain(0.0f);
    _jpfx.setModEffect(AudioEffectJPFX::JPFX_MOD_OFF);
    _jpfx.setModMix(0.5f);
    _jpfx.setDelayEffect(AudioEffectJPFX::JPFX_DELAY_OFF);
    _jpfx.setDelayMix(0.5f);
}

// ---------------------------------------------------------------------------
// Destructor — release audio patch cords
// ---------------------------------------------------------------------------

FXChainBlock::~FXChainBlock()
{
    delete _patchJPFXtoReverbL;
    delete _patchJPFXtoReverbR;
    delete _patchJPFXtoMixerL;
    delete _patchJPFXtoMixerR;
    delete _patchReverbToMixerL;
    delete _patchReverbToMixerR;
}

// ===========================================================================
// TONE CONTROL
// ===========================================================================

void FXChainBlock::setBassGain(float dB) {
    _bassGain = dB;
    _jpfx.setBassGain(dB);
}

void FXChainBlock::setTrebleGain(float dB) {
    _trebleGain = dB;
    _jpfx.setTrebleGain(dB);
}

float FXChainBlock::getBassGain()   const { return _bassGain;   }
float FXChainBlock::getTrebleGain() const { return _trebleGain; }

void FXChainBlock::setDrive(float norm) {
    // Clamp once here; AudioEffectJPFX trusts its inputs.
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    _drive = norm;
    _jpfx.setSaturation(norm);
}

float FXChainBlock::getDrive() const { return _drive; }

// ===========================================================================
// MODULATION EFFECTS
// ===========================================================================

void FXChainBlock::setModEffect(int8_t variation) {
    // Clamp to valid range: -1=off, 0..10=preset
    if (variation < -1) variation = -1;
    if (variation > 10) variation = 10;
    _modEffect = variation;

    if (variation < 0) {
        _jpfx.setModEffect(AudioEffectJPFX::JPFX_MOD_OFF);
    } else {
        _jpfx.setModEffect(static_cast<AudioEffectJPFX::ModEffectType>(variation));
    }
}

void FXChainBlock::setModMix(float mix) {
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;
    _modMix = mix;
    _jpfx.setModMix(mix);
}

void FXChainBlock::setModRate(float hz) {
    if (hz < 0.0f)  hz = 0.0f;
    if (hz > 20.0f) hz = 20.0f;   // Cap prevents metallic/aliasing artefacts
    _modRate = hz;
    _jpfx.setModRate(hz);
}

void FXChainBlock::setModFeedback(float fb) {
    if (fb < -1.0f) fb = -1.0f;
    if (fb > 0.99f) fb = 0.99f;
    _modFeedback = fb;
    _jpfx.setModFeedback(fb);
}

int8_t      FXChainBlock::getModEffect()   const { return _modEffect;   }
float       FXChainBlock::getModMix()      const { return _modMix;      }
float       FXChainBlock::getModRate()     const { return _modRate;      }
float       FXChainBlock::getModFeedback() const { return _modFeedback;  }

const char* FXChainBlock::getModEffectName() const {
    if (_modEffect < 0 || _modEffect > 10) return "Off";
    return MOD_EFFECT_NAMES[_modEffect];
}

// ===========================================================================
// DELAY EFFECTS
// ===========================================================================

void FXChainBlock::setDelayEffect(int8_t variation) {
    // Clamp to valid range: -1=off, 0..4=preset
    if (variation < -1) variation = -1;
    if (variation > 4)  variation = 4;
    _delayEffect = variation;

    if (variation < 0) {
        _jpfx.setDelayEffect(AudioEffectJPFX::JPFX_DELAY_OFF);
    } else {
        _jpfx.setDelayEffect(static_cast<AudioEffectJPFX::DelayEffectType>(variation));
    }
}

void FXChainBlock::setDelayMix(float mix) {
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;
    _delayMix = mix;
    _jpfx.setDelayMix(mix);
}

void FXChainBlock::setDelayFeedback(float fb) {
    if (fb < -1.0f) fb = -1.0f;
    if (fb > 0.99f) fb = 0.99f;
    _delayFeedback = fb;
    _jpfx.setDelayFeedback(fb);
}

void FXChainBlock::setDelayTime(float ms) {
    if (ms < 0.0f)    ms = 0.0f;
    if (ms > 1500.0f) ms = 1500.0f;
    _delayTime = ms;
    _jpfx.setDelayTime(ms);
}

int8_t      FXChainBlock::getDelayEffect()   const { return _delayEffect;   }
float       FXChainBlock::getDelayMix()      const { return _delayMix;      }
float       FXChainBlock::getDelayFeedback() const { return _delayFeedback;  }
float       FXChainBlock::getDelayTime()     const { return _delayTime;      }

const char* FXChainBlock::getDelayEffectName() const {
    if (_delayEffect < 0 || _delayEffect > 4) return "Off";
    return DELAY_EFFECT_NAMES[_delayEffect];
}

// BPM-sync helpers — forward directly to JPFX
void FXChainBlock::updateFromBPMClock(const BPMClockManager& bpmClock) {
    _jpfx.updateFromBPMClock(bpmClock);
}

void FXChainBlock::setDelayTimingMode(TimingMode mode) {
    _jpfx.setDelayTimingMode(mode);
}

TimingMode FXChainBlock::getDelayTimingMode() const {
    return _jpfx.getDelayTimingMode();
}

// ===========================================================================
// REVERB
// ===========================================================================

void FXChainBlock::setReverbRoomSize(float size) {
    if (size < 0.0f) size = 0.0f;
    if (size > 1.0f) size = 1.0f;
    _reverbRoomSize = size;
    _plateReverb.size(size);
}

void FXChainBlock::setReverbHiDamping(float damp) {
    if (damp < 0.0f) damp = 0.0f;
    if (damp > 1.0f) damp = 1.0f;
    _reverbHiDamp = damp;
    _plateReverb.hidamp(damp);
}

void FXChainBlock::setReverbLoDamping(float damp) {
    if (damp < 0.0f) damp = 0.0f;
    if (damp > 1.0f) damp = 1.0f;
    _reverbLoDamp = damp;
    _plateReverb.lodamp(damp);
}

float FXChainBlock::getReverbRoomSize()  const { return _reverbRoomSize; }
float FXChainBlock::getReverbHiDamping() const { return _reverbHiDamp;   }
float FXChainBlock::getReverbLoDamping() const { return _reverbLoDamp;   }

void FXChainBlock::setReverbBypass(bool bypass) {
    _reverbManualBypass = bypass;
    updateReverbBypass();
}

bool FXChainBlock::getReverbBypass() const { return _reverbManualBypass; }

// ===========================================================================
// REVERB EXTENDED CONTROLS (CC 95-98)
// ===========================================================================

// Shimmer: pitch-shifted feedback inside the tank loop.
// Zero CPU when amount == 0 (PitchShifter early-return guard).
void FXChainBlock::setReverbShimmer(float amount) {
    amount = constrain(amount, 0.0f, 1.0f);
    _reverbShimmer = amount;
    _plateReverb.shimmer(amount);
}
float FXChainBlock::getReverbShimmer() const { return _reverbShimmer; }

// Freeze: saves size/hidamp/lodamp/shimmer, sets decay=1.0, mutes input.
// All parameters restored automatically on unfreeze.
void FXChainBlock::setReverbFreeze(bool frozen) {
    _reverbFrozen = frozen;
    _plateReverb.freeze(frozen);
}
bool FXChainBlock::getReverbFreeze() const { return _reverbFrozen; }

// Output lowpass: post-tank EQ on wet signal only.
// Does NOT affect tail decay rate (unlike hidamp which is inside the tank).
void FXChainBlock::setReverbLowpass(float amount) {
    amount = constrain(amount, 0.0f, 1.0f);
    _reverbLowpass = amount;
    _plateReverb.lowpass(amount);
}
float FXChainBlock::getReverbLowpass() const { return _reverbLowpass; }

// Output highpass: post-tank EQ on wet signal only.
// Does NOT affect tail decay rate (unlike lodamp which is inside the tank).
void FXChainBlock::setReverbHipass(float amount) {
    amount = constrain(amount, 0.0f, 1.0f);
    _reverbHipass = amount;
    _plateReverb.hipass(amount);
}
float FXChainBlock::getReverbHipass() const { return _reverbHipass; }

// ===========================================================================
// OUTPUT MIX LEVELS
// ===========================================================================

void FXChainBlock::setDryMix(float left, float right) {
    _dryMixL = left;
    _dryMixR = right;
    _mixerOutL.gain(0, left);
    _mixerOutR.gain(0, right);
}

void FXChainBlock::setJPFXMix(float left, float right) {
    _jpfxMixL = left;
    _jpfxMixR = right;
    _mixerOutL.gain(1, left);
    _mixerOutR.gain(1, right);
}

void FXChainBlock::setReverbMix(float left, float right) {
    _reverbMixL = left;
    _reverbMixR = right;
    _mixerOutL.gain(2, left);
    _mixerOutR.gain(2, right);
    // Re-evaluate bypass: saving ~10% CPU when both channels are 0
    updateReverbBypass();
}

float FXChainBlock::getDryMixL()    const { return _dryMixL;    }
float FXChainBlock::getDryMixR()    const { return _dryMixR;    }
float FXChainBlock::getJPFXMixL()   const { return _jpfxMixL;   }
float FXChainBlock::getJPFXMixR()   const { return _jpfxMixR;   }
float FXChainBlock::getReverbMixL() const { return _reverbMixL; }
float FXChainBlock::getReverbMixR() const { return _reverbMixR; }

// ===========================================================================
// PRIVATE — reverb bypass decision
// ===========================================================================

/*
 * updateReverbBypass()
 *
 * Bypasses the PlateReverb when it would produce no output, saving ~10% CPU.
 *
 * Rules:
 *   bypass = _reverbManualBypass  OR  (L mix < threshold AND R mix < threshold)
 *
 * Why we don't gate on note activity:
 *   Reverb must keep running through the tail after note-off; gating on audio
 *   level would cut the tail abruptly.  Gating on mix=0 is safe because setting
 *   mix to 0 means the user explicitly wants no reverb — there is no tail to
 *   preserve.
 */
void FXChainBlock::updateReverbBypass() {
    const bool reverbWanted = !_reverbManualBypass &&
                              (_reverbMixL > REVERB_MIX_THRESHOLD ||
                               _reverbMixR > REVERB_MIX_THRESHOLD);
    _plateReverb.bypass_set(!reverbWanted);
}
