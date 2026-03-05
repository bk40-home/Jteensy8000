/*
 * FXChainBlock.h - OPTIMIZED HYBRID VERSION (JPFX + hexefx Reverb)
 *
 * This combines:
 * - JPFX for tone control, modulation (chorus/flanger/phaser), and delay
 * - hexefx PlateReverb for reverb
 *
 * SIGNAL FLOW (Flexible Routing):
 *   Input (stereo) → JPFX → [optionally] → Reverb
 *                         ↓                    ↓
 *                    Direct to Mixer ← Reverb Wet
 *                         ↓
 *   Dry Signal ────────→ Mixer → Output (stereo)
 *
 * KEY IMPROVEMENTS:
 * - Smart reverb bypass: CPU saved when reverb mix = 0
 * - Flexible routing: JPFX can bypass or feed reverb
 * - Independent dry/wet mixing for each stage
 *
 * This gives you the complete JP-8080 effects suite:
 * - Tone control (JPFX)
 * - 11 modulation variations (JPFX)
 * - 5 delay variations (JPFX)
 * - High-quality reverb (hexefx) with smart bypass
 */

#pragma once

#include <Arduino.h>
#include <Audio.h>
#include "AudioEffectJPFX.h"
#include "effect_platereverb_i16.h"  // hexefx reverb

class FXChainBlock {
public:
    FXChainBlock();
    ~FXChainBlock();

    // =========================================================================
    // JPFX CONTROLS (Tone, Modulation, Delay)
    // =========================================================================
    
    // Tone control
    void setBassGain(float dB);         // Low-shelf: -12dB to +12dB
    void setTrebleGain(float dB);       // High-shelf: -12dB to +12dB
    float getBassGain() const;
    float getTrebleGain() const;

    // Modulation effects (11 variations)
    void setModEffect(int8_t variation);     // -1=off, 0-10=preset
    void setModMix(float mix);               // 0..1 (wet amount)
    void setModRate(float hz);               // 0..20 Hz (0=use preset)
    void setModFeedback(float fb);           // -1=preset, 0..0.99
    int8_t getModEffect() const;
    float getModMix() const;
    float getModRate() const;
    float getModFeedback() const;
    const char* getModEffectName() const;

    // ADD to public methods:
    void updateFromBPMClock(const BPMClockManager& bpmClock);
    void setDelayTimingMode(TimingMode mode);
    TimingMode getDelayTimingMode() const;

    // Delay effects (5 variations)
    void setDelayEffect(int8_t variation);   // -1=off, 0-4=preset
    void setDelayMix(float mix);             // 0..1 (wet amount)
    void setDelayFeedback(float fb);         // -1=preset, 0..0.99
    void setDelayTime(float ms);             // 0=preset, 5..1500ms
    int8_t getDelayEffect() const;
    float getDelayMix() const;
    float getDelayFeedback() const;
    float getDelayTime() const;
    const char* getDelayEffectName() const;

    // =========================================================================
    // HEXEFX REVERB CONTROLS
    // =========================================================================
    
    void setReverbRoomSize(float size);      // 0..1 (room size)
    void setReverbHiDamping(float damp);     // 0..1 (high frequency damping)
    void setReverbLoDamping(float damp);     // 0..1 (low frequency damping)
    float getReverbRoomSize() const;
    float getReverbHiDamping() const;
    float getReverbLoDamping() const;
    
    // Reverb bypass control (CPU optimization)
    void setReverbBypass(bool bypass);       // Manual bypass override
    bool getReverbBypass() const;

    // =========================================================================
    // MIX CONTROLS (dry + JPFX + reverb)
    // =========================================================================
    
    void setDryMix(float left, float right);       // 0..1 per channel (dry signal)
    void setJPFXMix(float left, float right);      // 0..1 per channel (JPFX wet)
    void setReverbMix(float left, float right);    // 0..1 per channel (reverb wet)
    
    float getDryMixL() const;
    float getDryMixR() const;
    float getJPFXMixL() const;
    float getJPFXMixR() const;
    float getReverbMixL() const;
    float getReverbMixR() const;

    // =========================================================================
    // AUDIO INTERFACE
    // =========================================================================
    
    AudioMixer4& getOutputLeft()  { return _mixerOutL; }  // Final output left
    AudioMixer4& getOutputRight() { return _mixerOutR; }  // Final output right
    AudioEffectJPFX& getJPFXInput() { return _jpfx; }     // JPFX input stage

private:
    // =========================================================================
    // AUDIO OBJECTS
    // =========================================================================
    
    // Effects engines
    AudioEffectJPFX _jpfx;                    // JP-8000 tone/mod/delay
    AudioEffectPlateReverb_i16 _plateReverb;  // High-quality reverb

    // Output mixers (4 channels each: dry, JPFX wet, reverb wet, unused)
    AudioMixer4 _mixerOutL;  // Left output mixer
    AudioMixer4 _mixerOutR;  // Right output mixer

    // =========================================================================
    // AUDIO CONNECTIONS
    // =========================================================================
    
    // JPFX outputs → reverb input (can be bypassed)
    AudioConnection* _patchJPFXtoReverbL;
    AudioConnection* _patchJPFXtoReverbR;
    
    // JPFX outputs → mixer (channel 1 = JPFX wet)
    AudioConnection* _patchJPFXtoMixerL;
    AudioConnection* _patchJPFXtoMixerR;
    
    // Reverb outputs → mixer (channel 2 = reverb wet)
    AudioConnection* _patchReverbToMixerL;
    AudioConnection* _patchReverbToMixerR;
    
    // Note: Dry signal (channel 0) is connected from SynthEngine amp output

    // =========================================================================
    // CACHED PARAMETER STATE
    // =========================================================================
    
    // JPFX tone parameters
    float _bassGain = 0.0f;     // Bass gain in dB
    float _trebleGain = 0.0f;   // Treble gain in dB
    
    // JPFX modulation parameters
    int8_t _modEffect = -1;      // -1=off, 0-10=preset
    float _modMix = 0.5f;        // 0..1
    float _modRate = 0.0f;       // Hz (0=use preset)
    float _modFeedback = -1.0f;  // -1=use preset, 0..0.99
    
    // JPFX delay parameters
    int8_t _delayEffect = -1;      // -1=off, 0-4=preset
    float _delayMix = 0.5f;        // 0..1
    float _delayFeedback = -1.0f;  // -1=use preset, 0..0.99
    float _delayTime = 0.0f;       // ms (0=use preset)
    
    // Reverb parameters
    float _reverbRoomSize = 0.5f;  // 0..1
    float _reverbHiDamp = 0.5f;    // 0..1
    float _reverbLoDamp = 0.5f;    // 0..1
    bool _reverbManualBypass = false;  // Manual bypass override
    
    // Mix levels
    float _dryMixL = 1.0f;      // Dry left gain
    float _dryMixR = 1.0f;      // Dry right gain
    float _jpfxMixL = 0.0f;     // JPFX wet left gain
    float _jpfxMixR = 0.0f;     // JPFX wet right gain
    float _reverbMixL = 0.0f;   // Reverb wet left gain
    float _reverbMixR = 0.0f;   // Reverb wet right gain
    
    // =========================================================================
    // PRIVATE HELPER METHODS
    // =========================================================================
    
    // Update reverb bypass state based on mix levels and manual override
    void updateReverbBypass();
};