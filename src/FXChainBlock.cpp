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
 * FXChainBlock.cpp — see FXChainBlock.h for signal-flow diagram.
 *
 * MIXER CHANNELS:
 *   Channel 0: Dry (from SynthEngine amp, pre-JPFX)
 *   Channel 1: JPFX wet output
 *   Channel 2: unused (was reverb wet pre-Phase-3; reverb is now in GlobalFX)
 *   Channel 3: unused
 *
 * Parameter clamping is done once at the setter boundary so the audio
 * update path never sees out-of-range values. All constexpr name tables
 * live here in the translation unit; the header only declares the interface.
 */

#include "FXChainBlock.h"

// ---------------------------------------------------------------------------
// Effect name tables — must stay in sync with AudioEffectJPFX enum order.
// Stored in flash via const char* in .rodata (PROGMEM would need F()
// wrappers everywhere; this is the pragmatic choice for Teensy 4.1).
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
    : _jpfx()
{
    // --- Wire audio graph -------------------------------------------------
    // JPFX stereo out → output mixer ch1 (JPFX wet).
    // Ch0 (dry) wired from SynthEngine, not here.
    // Ch2 (formerly reverb wet) now unused — GlobalFX wires the shared
    //   reverb wet into the perf mixer directly.
    _patchJPFXtoMixerL = new AudioConnection(_jpfx, 0, _mixerOutL, 1);
    _patchJPFXtoMixerR = new AudioConnection(_jpfx, 1, _mixerOutR, 1);

    // --- Output mixer defaults --------------------------------------------
    // Ch0 (dry) gain at 1.0; the dry signal is live as soon as SynthEngine
    // wires amp output into ch0. Ch1 (JPFX wet) starts at 0 — user raises
    // it when enabling FX.
    _mixerOutL.gain(0, 1.0f);   // dry
    _mixerOutL.gain(1, 0.0f);   // JPFX wet
    _mixerOutL.gain(2, 0.0f);   // unused
    _mixerOutL.gain(3, 0.0f);   // unused

    _mixerOutR.gain(0, 1.0f);
    _mixerOutR.gain(1, 0.0f);
    _mixerOutR.gain(2, 0.0f);
    _mixerOutR.gain(3, 0.0f);

    // --- JPFX defaults (all effects off) ----------------------------------
    _jpfx.setSaturation(0.0f);
    _jpfx.setBassGain(0.0f);
    _jpfx.setTrebleGain(0.0f);
    _jpfx.setModEffect(AudioEffectJPFX::JPFX_MOD_OFF);
    _jpfx.setModMix(0.5f);
    _jpfx.setDelayEffect(AudioEffectJPFX::JPFX_DELAY_OFF);
    _jpfx.setDelayMix(0.5f);
}

// ---------------------------------------------------------------------------
// Destructor — release audio patch cords. Reverb cords removed (now in GlobalFX).
// ---------------------------------------------------------------------------

FXChainBlock::~FXChainBlock()
{
    delete _patchJPFXtoMixerL;
    delete _patchJPFXtoMixerR;
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
    if (hz > 20.0f) hz = 20.0f;   // cap prevents metallic/aliasing artefacts
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
float       FXChainBlock::getModRate()     const { return _modRate;     }
float       FXChainBlock::getModFeedback() const { return _modFeedback; }

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
float       FXChainBlock::getDelayFeedback() const { return _delayFeedback; }
float       FXChainBlock::getDelayTime()     const { return _delayTime;     }

const char* FXChainBlock::getDelayEffectName() const {
    if (_delayEffect < 0 || _delayEffect > 4) return "Off";
    return DELAY_EFFECT_NAMES[_delayEffect];
}

// BPM-sync helpers — forward directly to JPFX.
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
// REVERB — MOVED to GlobalFX (Phase 3). See GlobalFX.h / LayerManager.h.
// No reverb code lives in this class anymore.
// ===========================================================================

// ===========================================================================
// OUTPUT MIX LEVELS
// ===========================================================================
//
// Each mix setter updates the cached value unconditionally, but only writes
// through to the live mixer hardware when the chain is not fully bypassed.
// This lets callers (presets, UI, MIDI CCs) freely change mix values while
// Layer B is idle in SINGLE mode; when LayerManager un-bypasses the chain,
// applyCachedMixGains() pushes every cached value out to the mixer in one
// shot so the program state is exactly what was requested.
//
// Reverb mix is no longer handled here — FX_REVERB_MIX is intercepted by
// LayerManager and writes to GlobalFX as the global master wet level.
// ===========================================================================

void FXChainBlock::setDryMix(float left, float right) {
    _dryMixL = left;
    _dryMixR = right;
    if (!_fullBypass) {
        _mixerOutL.gain(0, left);
        _mixerOutR.gain(0, right);
    }
}

void FXChainBlock::setJPFXMix(float left, float right) {
    _jpfxMixL = left;
    _jpfxMixR = right;
    if (!_fullBypass) {
        _mixerOutL.gain(1, left);
        _mixerOutR.gain(1, right);
    }
}

float FXChainBlock::getDryMixL()  const { return _dryMixL;  }
float FXChainBlock::getDryMixR()  const { return _dryMixR;  }
float FXChainBlock::getJPFXMixL() const { return _jpfxMixL; }
float FXChainBlock::getJPFXMixR() const { return _jpfxMixR; }

// ===========================================================================
// FULL-CHAIN BYPASS — used by LayerManager to idle Layer B in SINGLE mode.
// ===========================================================================
//
// When engaged: zero the live mixer gains for dry (ch0) and JPFX (ch1) so
// this layer's entire audio output goes silent. Cached values (_dryMixL/R,
// _jpfxMixL/R) are preserved, so setBypass(false) → applyCachedMixGains()
// restores the exact program state.
//
// Reverb is NOT touched here — it's shared between layers in GlobalFX and
// has its own bypass logic driven by master wet level + manual override.
// ===========================================================================
void FXChainBlock::setBypass(bool bypass) {
    if (bypass == _fullBypass) return;   // no-op if already in the requested state
    _fullBypass = bypass;

    if (bypass) {
        // Zero the live mixer gains. Cached values untouched so unbypass
        // restores them exactly.
        _mixerOutL.gain(0, 0.0f);  _mixerOutR.gain(0, 0.0f);   // dry
        _mixerOutL.gain(1, 0.0f);  _mixerOutR.gain(1, 0.0f);   // JPFX wet
        // Ch2/3 already 0 (reverb removed, nothing to touch).
    } else {
        // Restore mixer gains from cached values.
        applyCachedMixGains();
    }
}

// ===========================================================================
// PRIVATE HELPERS
// ===========================================================================

/*
 * applyCachedMixGains()
 *
 * Writes the cached mix values through to the live mixer gains.
 * Called from setBypass(false) to restore state after a bypass.
 * Ch2 and ch3 (formerly reverb / spare) stay at 0 — they're unused now.
 */
void FXChainBlock::applyCachedMixGains() {
    _mixerOutL.gain(0, _dryMixL);   _mixerOutR.gain(0, _dryMixR);
    _mixerOutL.gain(1, _jpfxMixL);  _mixerOutR.gain(1, _jpfxMixR);
}
