/* Audio Library for Teensy
 * Copyright (c) 2025, Paul Stoffregen, paul@pjrc.com
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
 * JP-8000-style per-layer stereo FX chain: tone → modulation/delay (JPFX).
 *
 * Post-Phase-3 scope: this class handles ONLY per-layer effects (EQ, drive,
 * mod, delay). Reverb used to live here but is now shared across layers in
 * GlobalFX — see GlobalFX.h. LayerManager intercepts all reverb CCs and
 * routes them to the single global instance, saving ~10% CPU per disabled
 * layer tank and matching JP-8000 Performance-mode behaviour.
 *
 * SIGNAL FLOW:
 *
 *   Amp out (L/R) ─────────────────────────────► Mixer ch0 (dry)
 *         │
 *         ▼
 *       JPFX  (tone shelf, drive, chorus/flanger/phaser, delay)
 *         │
 *         └────────────────────────────────────► Mixer ch1 (JPFX wet)
 *
 *   The mixer's stereo output goes TWO places, wired by LayerManager:
 *     (a) into the perf mixer (this layer's main output)
 *     (b) into GlobalFX's reverb-send input (one of two layer taps)
 *
 * MIXER CHANNEL MAP (both L and R):
 *   0 = Dry  (wired from SynthEngine amp output, outside this class)
 *   1 = JPFX wet
 *   2 = Unused (was reverb wet pre-Phase-3)
 *   3 = Unused
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
 *   - setBypass(true) silences the output-mixer gains (dry + JPFX).
 *     JPFX itself still ticks but its input is silent (upstream amp muted),
 *     so it processes zeros and costs near-zero CPU. Used by LayerManager
 *     to idle Layer B in SINGLE mode.
 *   - JPFX modulation/delay buffers are only allocated once in the ctor.
 *   - All parameter clamping is done at the setter boundary; the audio
 *     update path sees only valid values.
 */

#pragma once

#include <Arduino.h>
#include "Audio.h"
#include "AudioEffectJPFX.h"
#include "BPMClockManager.h"        // for updateFromBPMClock / TimingMode
// NOTE: AudioEffectPlateReverbJT no longer included — reverb moved to GlobalFX
// (Phase 3). Translation units that need the PlateReverb type should include
// GlobalFX.h.

class FXChainBlock
{
public:
    FXChainBlock();
    ~FXChainBlock();

    // ========================================================================
    // Audio-graph accessors
    // ========================================================================
    // LayerManager wires these into the perf mixer AND into GlobalFX's send
    // inputs — one AudioStream output can feed multiple AudioConnections.
    //
    // Return type is the concrete AudioMixer4& (not AudioStream&) so callers
    // that want the derived type — e.g. SynthEngine::getFXOutL/R declared as
    // AudioMixer4& — can forward the reference without a downcast. Callers
    // that only need AudioStream& (the AudioConnection ctor's destination
    // parameter) still work, since AudioMixer4 → AudioStream is an implicit
    // upcast.
    AudioMixer4& getOutputLeft()  { return _mixerOutL; }
    AudioMixer4& getOutputRight() { return _mixerOutR; }

    // JPFX reference — SynthEngine wires amp output into JPFX input directly;
    // the dry path is branched off inside SynthEngine before reaching JPFX.
    AudioEffectJPFX& getJPFX() { return _jpfx; }

    // Dry input landing points — mixer ch0 on each side. SynthEngine wires
    // the amp output here in addition to feeding JPFX.
    //
    // Intentionally aliases _mixerOutL/R (same mixer as getOutputLeft/Right).
    // The dry signal is injected on slot 0 of the output mixer; the FX-chain's
    // wet stages write to other slots of the same mixer. Kept as a separately
    // named accessor so wiring code reads by role, not by implementation.
    AudioMixer4& getDryInputL() { return _mixerOutL; }
    AudioMixer4& getDryInputR() { return _mixerOutR; }

    // =========================================================================
    // TONE  (EQ + drive — pass straight through to JPFX)
    // =========================================================================
    void  setDrive(float amount);   // 0..1
    void  setBassGain(float dB);    // ±12 dB low-shelf
    void  setTrebleGain(float dB);  // ±12 dB high-shelf
    float getDrive()      const;
    float getBassGain()   const;
    float getTrebleGain() const;

    // =========================================================================
    // MODULATION  (chorus / flanger / phaser)
    // =========================================================================
    void setModEffect(int8_t variation);   // -1=off, 0..10
    void setModMix(float mix);             // 0..1 dry/wet
    void setModRate(float hz);             // 0=use preset, else override
    void setModFeedback(float fb);         // -1=use preset, 0..0.99

    int8_t      getModEffect()     const;
    float       getModMix()        const;
    float       getModRate()       const;
    float       getModFeedback()   const;
    const char* getModEffectName() const;

    // =========================================================================
    // DELAY
    // =========================================================================
    void setDelayEffect(int8_t variation);   // -1=off, 0..4
    void setDelayMix(float mix);             // 0..1
    void setDelayFeedback(float fb);         // -1=use preset, 0..0.99
    void setDelayTime(float ms);             // 0=use preset, else override

    int8_t      getDelayEffect()     const;
    float       getDelayMix()        const;
    float       getDelayFeedback()   const;
    float       getDelayTime()       const;
    const char* getDelayEffectName() const;

    // BPM-sync helper — forwards directly to JPFX.
    void       updateFromBPMClock(const BPMClockManager& bpmClock);
    void       setDelayTimingMode(TimingMode mode);
    TimingMode getDelayTimingMode() const;

    // =========================================================================
    // REVERB — MOVED to GlobalFX (Phase 3). See GlobalFX.h / LayerManager.h.
    //
    // No per-layer reverb state, setters, or getters remain on this class.
    // LayerManager intercepts reverb CCs before they reach any engine and
    // routes them to the shared GlobalFX instance.
    // =========================================================================

    // =========================================================================
    // OUTPUT MIX  (dry + JPFX — reverb mix now in GlobalFX)
    // =========================================================================
    void  setDryMix(float left, float right);
    float getDryMixL() const;
    float getDryMixR() const;

    void  setJPFXMix(float left, float right);
    float getJPFXMixL() const;
    float getJPFXMixR() const;

    // =========================================================================
    // FULL-CHAIN BYPASS  — used by LayerManager to idle Layer B in SINGLE mode
    // =========================================================================
    //
    // When true: output mixer gains for dry (ch0) and JPFX wet (ch1) are
    // zeroed so this layer's audio output goes silent. Cached mix values
    // are preserved, so setBypass(false) restores the exact program state.
    //
    // JPFX still runs on the audio thread (the Teensy library has no per-
    // object bypass hook), but its input is silent in SINGLE mode because
    // SynthEngine's voice output is muted upstream — so it processes zeros
    // at near-zero CPU cost in practice.
    //
    // While bypassed, setDryMix / setJPFXMix update the cached values but
    // do NOT write through to the mixer gains. Preset loads under bypass
    // therefore apply cleanly on unbypass (applyCachedMixGains).
    //
    // Reverb is no longer our concern. GlobalFX has its own bypass logic
    // driven by master wet level and its own manual bypass override.
    // =========================================================================
    void setBypass(bool bypass);
    bool getBypass() const { return _fullBypass; }

private:
    // =========================================================================
    // AUDIO OBJECTS
    // =========================================================================
    AudioEffectJPFX _jpfx;           // Tone / mod / delay engine (per layer)

    // 4-channel stereo output mixers.
    //   Pre-Phase-3:  ch0=dry, ch1=JPFX wet, ch2=reverb wet, ch3=spare.
    //   Post-Phase-3: ch0=dry, ch1=JPFX wet, ch2 and ch3 unused.
    AudioMixer4 _mixerOutL;
    AudioMixer4 _mixerOutR;

    // =========================================================================
    // AUDIO PATCH CORDS — heap-allocated so the graph is wired at run time.
    // =========================================================================
    AudioConnection* _patchJPFXtoMixerL = nullptr;    // JPFX L → mixer ch1 L
    AudioConnection* _patchJPFXtoMixerR = nullptr;    // JPFX R → mixer ch1 R
    // Note: dry path (ch0) is wired from SynthEngine, not here.
    // Note: reverb patch cords removed — reverb moved to GlobalFX (Phase 3).

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

    // -- Output mix levels (reverb removed, now in GlobalFX) --
    float _dryMixL    = 1.0f;   // ch0 left
    float _dryMixR    = 1.0f;   // ch0 right
    float _jpfxMixL   = 0.0f;   // ch1 left
    float _jpfxMixR   = 0.0f;   // ch1 right

    // -- Full-chain bypass state --
    // When true, setDryMix / setJPFXMix update the cache only; the mixer
    // gains stay at 0 until setBypass(false) calls applyCachedMixGains().
    bool  _fullBypass = false;

    // =========================================================================
    // PRIVATE HELPERS
    // =========================================================================

    // Write the cached mix values through to the live mixer gains.
    // Called from setBypass(false) to restore state after a bypass.
    void applyCachedMixGains();
};
