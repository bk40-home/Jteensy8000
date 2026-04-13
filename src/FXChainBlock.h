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
 * FXChainBlock.h
 * ==============
 * JP-8000-style stereo FX chain: tone → modulation/delay (JPFX) → reverb.
 *
 * SIGNAL FLOW:
 *
 *   Amp out (L/R) ──────────────────────────────────────────► Mixer ch0 (dry)
 *         │
 *         ▼
 *       JPFX  (tone shelf, drive, chorus/flanger/phaser, delay)
 *         │
 *         ├──────────────────────────────────────────────────► Mixer ch1 (JPFX direct)
 *         │
 *         ▼
 *    PlateReverb  (bypassed automatically when mix = 0 → saves ~10% CPU)
 *         │
 *         └──────────────────────────────────────────────────► Mixer ch2 (reverb wet)
 *
 * MIXER CHANNEL MAP (both L and R):
 *   0 = Dry  (wired from SynthEngine amp output, outside this class)
 *   1 = JPFX wet
 *   2 = Reverb wet
 *   3 = Unused / available for expansion
 *
 * MODULATION VARIATIONS (11):
 *   0  Chorus 1      3  Flanger 1     6  Phaser 1     9  Phaser 4
 *   1  Chorus 2      4  Flanger 2     7  Phaser 2    10  Super Chorus
 *   2  Chorus 3      5  Deep Flanger  8  Phaser 3
 *
 * DELAY VARIATIONS (5):
 *   0  Mono Short    2  Pan L→R       4  Pan Stereo
 *   1  Mono Long     3  Pan R→L
 *
 * CPU NOTES:
 *   - Reverb is auto-bypassed (bypass_set(true)) when both mix channels are
 *     below REVERB_MIX_THRESHOLD.  Re-enabled as soon as mix rises again.
 *     Effect tails are preserved because we don't gate on note activity.
 *   - JPFX modulation/delay buffers are only allocated once in the constructor.
 *   - All parameter clamping is done at the setter boundary; the audio update
 *     path sees only valid values.
 */

#pragma once

#include <Arduino.h>
#include "Audio.h"
#include "AudioEffectJPFX.h"
#include "BPMClockManager.h"        // for updateFromBPMClock / TimingMode
#include "AudioEffectPlateReverbJT.h" // JT-8000 plate reverb

// ---------------------------------------------------------------------------
// Minimum mixer gain treated as "active" for the reverb bypass decision.
// Below this the reverb is bypassed to save CPU.
// ---------------------------------------------------------------------------
static constexpr float REVERB_MIX_THRESHOLD = 0.001f;

class FXChainBlock {
public:
    FXChainBlock();
    ~FXChainBlock();

    // =========================================================================
    // TONE CONTROL  (JPFX low/high shelf)
    // =========================================================================

    // Bass low-shelf gain, ±12 dB
    void  setBassGain(float dB);
    float getBassGain()   const;

    // Treble high-shelf gain, ±12 dB
    void  setTrebleGain(float dB);
    float getTrebleGain() const;

    // Pre-tone soft/hard saturation.
    //   0.0       = bypass
    //   0.0..0.5  = soft clip (tanh)
    //   0.5..1.0  = hard clip
    void  setDrive(float norm);     // clamped 0..1
    float getDrive() const;

    // =========================================================================
    // MODULATION EFFECTS  (chorus / flanger / phaser, 11 variations)
    // =========================================================================

    // variation: -1 = off,  0..10 = preset (see header comment for names)
    void   setModEffect(int8_t variation);
    int8_t getModEffect()     const;
    const char* getModEffectName() const;   // returns "Off" or preset name

    // Wet level (0 = dry only, 1 = full wet)
    void  setModMix(float mix);             // clamped 0..1
    float getModMix() const;

    // LFO rate override.  0 = use preset value.
    void  setModRate(float hz);             // clamped 0..20 Hz
    float getModRate() const;

    // Feedback override.  -1 = use preset value.
    void  setModFeedback(float fb);         // clamped -1..0.99
    float getModFeedback() const;

    // =========================================================================
    // DELAY EFFECTS  (mono / panning, 5 variations)
    // =========================================================================

    // variation: -1 = off,  0..4 = preset (see header comment for names)
    void   setDelayEffect(int8_t variation);
    int8_t getDelayEffect()     const;
    const char* getDelayEffectName() const; // returns "Off" or preset name

    // Wet level (0 = dry only, 1 = full wet)
    void  setDelayMix(float mix);           // clamped 0..1
    float getDelayMix() const;

    // Feedback override.  -1 = use preset value.
    void  setDelayFeedback(float fb);       // clamped -1..0.99
    float getDelayFeedback() const;

    // Delay time override in ms.  0 = use preset value.
    void  setDelayTime(float ms);           // clamped 0..1500 ms
    float getDelayTime() const;

    // BPM-sync helper — forwards directly to JPFX
    void updateFromBPMClock(const BPMClockManager& bpmClock);
    void      setDelayTimingMode(TimingMode mode);
    TimingMode getDelayTimingMode() const;

    // =========================================================================
    // REVERB  (AudioEffectPlateReverbJT)
    // =========================================================================

    void  setReverbRoomSize(float size);    // clamped 0..1
    float getReverbRoomSize() const;

    void  setReverbHiDamping(float damp);   // clamped 0..1 (high-freq absorption)
    float getReverbHiDamping() const;

    void  setReverbLoDamping(float damp);   // clamped 0..1 (low-freq absorption)
    float getReverbLoDamping() const;

    // Hard bypass override (in addition to the auto-bypass on mix=0)
    void setReverbBypass(bool bypass);
    bool getReverbBypass() const;

    // Shimmer — pitch-shifted feedback in the tank feedback loop.
    // 0.0 = off (zero CPU), 1.0 = full shimmer.
    // Automatically disabled during freeze to prevent runaway escalation.
    void  setReverbShimmer(float amount);   // clamped 0..1
    float getReverbShimmer() const;

    // Freeze / infinite hold.
    // When true: decay = 1.0, input muted, shimmer disabled.
    // All parameters (size, hidamp, lodamp, shimmer) saved and restored on unfreeze.
    void  setReverbFreeze(bool frozen);
    bool  getReverbFreeze() const;

    // Output lowpass filter — applied AFTER the tank on the wet signal only.
    // 0.0 = bright (no filtering), 1.0 = dark (heavy cut).
    // Different from hidamp() which acts INSIDE the tank and affects tail decay rate.
    void  setReverbLowpass(float amount);   // clamped 0..1
    float getReverbLowpass() const;

    // Output highpass filter — applied AFTER the tank on the wet signal only.
    // 0.0 = full bass, 1.0 = thin (heavy bass cut).
    // Different from lodamp() which acts INSIDE the tank.
    void  setReverbHipass(float amount);    // clamped 0..1
    float getReverbHipass() const;

    // =========================================================================
    // OUTPUT MIX LEVELS
    // =========================================================================

    // Dry signal level per channel (mixer ch0)
    void  setDryMix(float left, float right);
    float getDryMixL() const;
    float getDryMixR() const;

    // JPFX wet level per channel (mixer ch1)
    void  setJPFXMix(float left, float right);
    float getJPFXMixL() const;
    float getJPFXMixR() const;

    // Reverb wet level per channel (mixer ch2).
    // Setting both to 0 triggers auto-bypass of the reverb.
    void  setReverbMix(float left, float right);
    float getReverbMixL() const;
    float getReverbMixR() const;

    // =========================================================================
    // AUDIO GRAPH ACCESS  (for wiring in SynthEngine)
    // =========================================================================

    // Stereo output mixers — wire these to the DAC output stage
    AudioMixer4& getOutputLeft()    { return _mixerOutL; }
    AudioMixer4& getOutputRight()   { return _mixerOutR; }

    // JPFX input — wire the amp output here (SynthEngine sets up ch0 dry too)
    AudioEffectJPFX& getJPFXInput() { return _jpfx; }

private:
    // =========================================================================
    // AUDIO OBJECTS  (order matters: Teensy Audio Library processes in
    //                 declaration order within an AudioStream subclass, but
    //                 here they are separate objects so ordering is flexible)
    // =========================================================================

    AudioEffectJPFX            _jpfx;          // Tone / mod / delay engine
    AudioEffectPlateReverbJT _plateReverb;   // stereo plate reverb

    // 4-channel stereo output mixers
    //   ch0 = dry  ch1 = JPFX wet  ch2 = reverb wet  ch3 = spare
    AudioMixer4 _mixerOutL;
    AudioMixer4 _mixerOutR;

    // =========================================================================
    // AUDIO PATCH CORDS  (heap-allocated so the graph is wired at run time)
    // =========================================================================

    AudioConnection* _patchJPFXtoReverbL;   // JPFX L → reverb in L
    AudioConnection* _patchJPFXtoReverbR;   // JPFX R → reverb in R
    AudioConnection* _patchJPFXtoMixerL;    // JPFX L → mixer ch1 L
    AudioConnection* _patchJPFXtoMixerR;    // JPFX R → mixer ch1 R
    AudioConnection* _patchReverbToMixerL;  // Reverb L → mixer ch2 L
    AudioConnection* _patchReverbToMixerR;  // Reverb R → mixer ch2 R
    // Note: dry path (ch0) is wired from SynthEngine, not here.

    // =========================================================================
    // CACHED PARAMETER STATE
    // =========================================================================

    // -- Tone --
    float  _drive      = 0.0f;   // 0..1
    float  _bassGain   = 0.0f;   // dB, ±12
    float  _trebleGain = 0.0f;   // dB, ±12

    // -- Modulation --
    int8_t _modEffect   = -1;    // -1=off, 0..10
    float  _modMix      = 0.5f;  // 0..1
    float  _modRate     = 0.0f;  // Hz (0=use preset)
    float  _modFeedback = -1.0f; // -1=use preset, 0..0.99

    // -- Delay --
    int8_t _delayEffect   = -1;    // -1=off, 0..4
    float  _delayMix      = 0.5f;  // 0..1
    float  _delayFeedback = -1.0f; // -1=use preset, 0..0.99
    float  _delayTime     = 0.0f;  // ms (0=use preset)

    // -- Reverb --
    float _reverbRoomSize     = 0.5f;   // 0..1
    float _reverbHiDamp       = 0.5f;   // 0..1
    float _reverbLoDamp       = 0.5f;   // 0..1
    bool  _reverbManualBypass = false;   // hard bypass override
    float _reverbShimmer      = 0.0f;   // 0..1 shimmer wet amount
    bool  _reverbFrozen       = false;  // freeze state
    float _reverbLowpass      = 0.0f;   // 0..1 output LP (post-tank)
    float _reverbHipass       = 0.0f;   // 0..1 output HP (post-tank)

    // -- Output mix levels --
    float _dryMixL    = 1.0f;   // ch0 left
    float _dryMixR    = 1.0f;   // ch0 right
    float _jpfxMixL   = 0.0f;   // ch1 left
    float _jpfxMixR   = 0.0f;   // ch1 right
    float _reverbMixL = 0.0f;   // ch2 left
    float _reverbMixR = 0.0f;   // ch2 right

    // =========================================================================
    // PRIVATE HELPERS
    // =========================================================================

    // Re-evaluate reverb bypass state after any mix or manual-bypass change.
    // Bypasses when both channels are silent (saves ~10% CPU); restores as
    // soon as either channel rises above REVERB_MIX_THRESHOLD.
    void updateReverbBypass();
};
