#pragma once
// =============================================================================
// VoiceBlock.h — JT-8000 complete synthesis voice
//
// Combines dual oscillators, sub osc, noise, ring modulators, resonant filter,
// amp & filter envelopes, pitch envelope, and feedback oscillation.
//
// PITCH ARCHITECTURE (Delivery 1):
//   All pitch sources now route through OscillatorBlock's FM mixer.
//   VoiceBlock exposes setPitchBend() (was setPitchModulation) which writes
//   a DC value into the pre-mixer. No software pitch loop.
// =============================================================================

#include "synth_pinknoise.h"
#include "effect_multiply.h"

#include "Audio.h"
#include "OscillatorBlock.h"
#include "EnvelopeBlock.h"
#include "FilterBlock.h"
#include "LFOBlock.h"
#include "SubOscillatorBlock.h"
#include "DebugTrace.h"

class VoiceBlock {
public:
    // =========================================================================
    // LIFECYCLE
    // =========================================================================

    VoiceBlock();
    void update();
    void noteOn(float frequency, float velocity);
    void noteOff();
    void setAmplitude(float amplitude);

    // =========================================================================
    // OSCILLATOR CONFIGURATION
    // =========================================================================

    void setOsc1Waveform(int waveform);
    void setOsc2Waveform(int waveform);
    void setOscMix(float osc1Level, float osc2Level);
    void setOsc1Mix(float level);
    void setOsc2Mix(float level);
    void setOsc1PitchOffset(float semitones);
    void setOsc2PitchOffset(float semitones);

    /** Pitch bend — writes DC into pre-mixer for both oscillators.
     *  Called per-frame by SynthEngine when the pitch wheel moves. */
    void setOsc1PitchBend(float semitones);
    void setOsc2PitchBend(float semitones);

    void setOsc1Detune(float hertz);
    void setOsc2Detune(float hertz);
    void setOsc1FineTune(float cents);
    void setOsc2FineTune(float cents);
    void setSubMix(float level);
    void setNoiseMix(float level);
    void setOsc1SupersawDetune(float amount);
    void setOsc2SupersawDetune(float amount);
    void setOsc1SupersawMix(float amount);
    void setOsc2SupersawMix(float amount);
    void setOsc1ShapeDcAmp(float amplitude);
    void setOsc2ShapeDcAmp(float amplitude);
    void setOsc1FrequencyDcAmp(float amplitude);
    void setOsc2FrequencyDcAmp(float amplitude);
    void setRing1Mix(float level);
    void setRing2Mix(float level);
    void setBaseFrequency(float frequency);

    // =========================================================================
    // ARBITRARY WAVEFORM SELECTION
    // =========================================================================

    void setOsc1ArbBank(ArbBank bank);
    void setOsc2ArbBank(ArbBank bank);
    void setOsc1ArbIndex(uint16_t index);
    void setOsc2ArbIndex(uint16_t index);

    // =========================================================================
    // GLIDE (PORTAMENTO)
    // =========================================================================

    void setGlideEnabled(bool enabled);
    void setGlideTime(float milliseconds);

    // =========================================================================
    // FEEDBACK OSCILLATION
    // =========================================================================

    void setOsc1FeedbackAmount(float amount);
    void setOsc2FeedbackAmount(float amount);
    void setOsc1FeedbackMix(float mix);
    void setOsc2FeedbackMix(float mix);

    float getOsc1FeedbackMix()    const;
    float getOsc2FeedbackMix()    const;
    float getOsc1FeedbackAmount() const;
    float getOsc2FeedbackAmount() const;

    // =========================================================================
    // FILTER
    // =========================================================================

    void setFilterCutoff(float hertz);
    void setFilterResonance(float amount);
    void setFilterOctaveControl(float octaves);
    void setFilterEnvAmount(float amount);
    void setFilterKeyTrackAmount(float amount);
    void setMultimode(float amount);
    void setTwoPole(bool enabled);
    void setXpander4Pole(bool enabled);
    void setXpanderMode(uint8_t mode);
    void setBPBlend2Pole(bool enabled);
    void setPush2Pole(bool enabled);
    void setResonanceModDepth(float depth);

    // =========================================================================
    // ENVELOPES
    // =========================================================================

    void setAmpAttack(float milliseconds);
    void setAmpDecay(float milliseconds);
    void setAmpSustain(float level);
    void setAmpRelease(float milliseconds);
    void setAmpADSR(float a, float d, float s, float r);

    void setFilterAttack(float milliseconds);
    void setFilterDecay(float milliseconds);
    void setFilterSustain(float level);
    void setFilterRelease(float milliseconds);
    void setFilterADSR(float a, float d, float s, float r);

    // =========================================================================
    // PITCH ENVELOPE
    // =========================================================================

    void  setPitchEnvAttack(float milliseconds);
    void  setPitchEnvDecay(float milliseconds);
    void  setPitchEnvSustain(float level);
    void  setPitchEnvRelease(float milliseconds);
    void  setPitchEnvDepth(float semitones);
    float getPitchEnvDepth() const { return _pitchEnvDepth; }

    AudioStream&          pitchEnvOutput();
    AudioSynthWaveformDc& pitchEnvDcRef();

    // =========================================================================
    // VELOCITY SENSITIVITY
    // =========================================================================

    void setVelocityAmpSens(float sensitivity)    { _velAmpSens    = sensitivity; }
    void setVelocityFilterSens(float sensitivity) { _velFilterSens = sensitivity; }
    void setVelocityEnvSens(float sensitivity)    { _velEnvSens    = sensitivity; }

    // =========================================================================
    // GETTERS
    // =========================================================================

    int   getOsc1Waveform()       const;
    int   getOsc2Waveform()       const;
    float getOsc1PitchOffset()    const;
    float getOsc2PitchOffset()    const;
    float getOsc1Detune()         const;
    float getOsc2Detune()         const;
    float getOsc1FineTune()       const;
    float getOsc2FineTune()       const;
    float getOscMix1()            const;
    float getOscMix2()            const;
    float getSubMix()             const;
    float getNoiseMix()           const;
    float getOsc1SupersawDetune() const;
    float getOsc2SupersawDetune() const;
    float getOsc1SupersawMix()    const;
    float getOsc2SupersawMix()    const;
    bool  getGlideEnabled()       const;
    float getGlideTime()          const;
    float getOsc1ShapeDc()        const;
    float getOsc2ShapeDc()        const;
    float getOsc1FrequencyDc()    const;
    float getOsc2FrequencyDc()    const;
    float getRing1Mix()           const;
    float getRing2Mix()           const;

    float getFilterCutoff()       const;
    float getFilterResonance()    const;
    float getFilterOctaveControl() const;
    float getFilterEnvAmount()    const;
    float getFilterKeyTrackAmount() const;
    float getMultimode()          const { return _multimode; }
    bool  getTwoPole()            const { return _useTwoPole; }
    bool  getXpander4Pole()       const { return _xpander4Pole; }
    uint8_t getXpanderMode()      const { return _xpanderMode; }
    bool  getBPBlend2Pole()       const { return _bpBlend2Pole; }
    bool  getPush2Pole()          const { return _push2Pole; }
    float getResonanceModDepth()  const { return _resonanceModDepth; }

    float getAmpAttack()          const;
    float getAmpDecay()           const;
    float getAmpSustain()         const;
    float getAmpRelease()         const;
    float getFilterEnvAttack()    const;
    float getFilterEnvDecay()     const;
    float getFilterEnvSustain()   const;
    float getFilterEnvRelease()   const;

    // =========================================================================
    // AUDIO OUTPUTS & MODULATION MIXERS
    // =========================================================================

    AudioStream& output();
    AudioMixer4& frequencyModMixerOsc1();
    AudioMixer4& frequencyModMixerOsc2();
    AudioMixer4& shapeModMixerOsc1();
    AudioMixer4& shapeModMixerOsc2();
    AudioMixer4& filterModMixer();

    // SynthEngine needs access to pitch envelope patches for wiring
    friend class SynthEngine;

private:
    // Audio components
    OscillatorBlock _osc1{true};   // OSC1: supersaw capable
    OscillatorBlock _osc2{false};  // OSC2: no supersaw (CPU saving)
    AudioEffectMultiply _ring1, _ring2;
    SubOscillatorBlock _subOsc;
    AudioSynthNoisePink _noise;
    AudioMixer4 _oscMixer;
    AudioMixer4 _voiceMixer;

    FilterBlock _filter;
    EnvelopeBlock _filterEnvelope;
    EnvelopeBlock _ampEnvelope;

    // State variables
    float _osc1Level = 1.0f;
    float _osc2Level = 0.0f;
    float _ring1Level = 0.0f;
    float _ring2Level = 0.0f;

    float _baseCutoff = 10000.0f;
    float _keyTrackVal = 0.0f;
    float _filterEnvAmount = 0.0f;
    float _filterKeyTrackAmount = 0.5f;
    float _multimode = 0.0f;
    float _resonanceModDepth = 0.0f;
    bool    _useTwoPole   = false;
    bool    _xpander4Pole = false;
    uint8_t _xpanderMode  = 0;
    bool    _bpBlend2Pole = false;
    bool    _push2Pole    = false;

    bool _isActive = false;
    float _currentFreq = 0.0f;
    float _subMix = 0.0f;
    float _noiseMix = 0.0f;

    // Headroom limiter
    static constexpr float _kMaxMixerGain = 0.9f;
    float _clampedLevel(float level);

    AudioConnection* _patchCables[16];

    // -----------------------------------------------------------------------
    // Pitch envelope — DC source → EnvelopeBlock → FM mixer slot 3
    // -----------------------------------------------------------------------
    AudioSynthWaveformDc _pitchEnvDc;
    EnvelopeBlock _pitchEnvelope;
    AudioConnection* _pitchEnvPatch1 = nullptr;  // → osc1 freqMod slot 3
    AudioConnection* _pitchEnvPatch2 = nullptr;  // → osc2 freqMod slot 3
    float _pitchEnvDepth = 0.0f;

    // Velocity sensitivity scalars (0..1)
    float _velAmpSens    = 0.0f;
    float _velFilterSens = 0.0f;
    float _velEnvSens    = 0.0f;

    // Base filter env amount (before velocity scaling)
    float _baseFilterEnvAmount = 0.0f;
};
