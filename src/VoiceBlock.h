#pragma once

// =============================================================================
// VoiceBlock.h — JT-8000 complete synthesis voice
//
// Combines dual oscillators, sub osc, noise, ring modulators, resonant filter,
// amp & filter envelopes, pitch envelope, and feedback oscillation.
//
// VOICE ACTIVITY MODEL:
//   A voice is "audio-active" when its amp envelope is in any non-idle phase
//   (Attack, Decay, Sustain, or Release).  This is queried from the hardware
//   envelope — no manual boolean is maintained.
//
//   isAudioActive() == true  → voice is producing audio (including release tail)
//   isAudioActive() == false → voice is silent, safe to reallocate
//
// PITCH ARCHITECTURE:
//   All pitch sources route through OscillatorBlock's FM mixer.
//   VoiceBlock exposes setPitchBend() which writes a DC value into the
//   pre-mixer.  No software pitch loop.
//
// CROSS MODULATION (JT_OPT_CROSS_MOD):
//   OSC2 audio → OSC1 _crossModPreMixer slot 1 at a gain derived from
//   crossModDepthFromCC() (CrossModSync.h).
//
//   The _crossModPreMixer (inside OscillatorBlock) sits between
//   _combinedPitchDc and _frequencyModMixer slot 0:
//     _combinedPitchDc ──► pre-mixer slot 0 (gain 1.0, always)
//     OSC2 audio       ──► pre-mixer slot 1 (gain = depth)
//                              │
//                              ▼
//                       FM mixer slot 0
//
//   This keeps all four _frequencyModMixer slots for LFO1/LFO2/pitch-env
//   with no conflicts.  The cable from OSC2 output into the pre-mixer is a
//   permanent connection built in VoiceBlock's constructor.
//   setCrossModDepth() calls _osc1.setCrossModGain() unconditionally.
//
//   Works with ANY waveform combination.  Does NOT require sync to be active.
//   When depth == 0.0 the pre-mixer slot 1 gain is 0 — no modulation, no
//   audio cost beyond the unconditional AudioMixer4 update (~1 µs/voice).
//
//   When sync IS active, OscillatorBlock outputs are removed from the graph
//   so the pre-mixer slot 1 carries no signal.  The sync engine handles
//   cross-mod internally (per-sample) via its own _crossModDepth — VoiceBlock
//   forwards the depth to the sync engine on enable.
//
// OSCILLATOR HARD SYNC (JT_OPT_OSC_SYNC):
//   Sample-accurate phase reset: when OSC2 (master) phase wraps, OSC1 (slave)
//   phase resets to zero at that exact sample.
//   Works with ANY waveform combination.  Does NOT require cross-mod.
//   When sync is enabled, VoiceBlock swaps audio connections to route through
//   AudioSynthOscSync.  When sync is disabled, normal OscillatorBlock paths
//   are restored.
//
// =============================================================================

#include "synth_pinknoise.h"
#include "effect_multiply.h"

#include "Audio.h"
#include "OscillatorBlock.h"
#include "EnvelopeBlock.h"
#include "FilterBlock.h"
#include "LFOBlock.h"
#include "SubOscillatorBlock.h"
#include "CrossModSync.h"
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
    void silence();           // Force voice to idle state (boot / panic)
    void setAmplitude(float amplitude);

    /// Returns velocity of the most recent noteOn (0.0–1.0).
    /// Used by SynthEngine MONO mode to re-trigger at the same velocity
    /// when returning to a previously held note (legato return).
    float getLastVelocity() const;

    // =========================================================================
    // VOICE ACTIVITY — query the hardware, don't maintain a flag
    // =========================================================================

    /// True when the amp envelope is in any active phase (A/D/S/R).
    /// False only when the envelope has fully completed its release.
    /// Single source of truth for "is this voice producing audio?"
    bool isAudioActive() const { return _ampEnvelope.isActive(); }

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

    /// Pitch bend — writes DC into pre-mixer for both oscillators.
    /// Called per-frame by SynthEngine when the pitch wheel moves.
    void setOsc1PitchBend(float semitones);
    void setOsc2PitchBend(float semitones);

    void setOsc1Detune(float semitones);
    void setOsc2Detune(float semitones);
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

    /// Set step sequencer pitch offset on both oscillators (FM-scaled).
    void setSeqPitchOffset(float fmScaledOffset);
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

    void  setOsc1FeedbackAmount(float amount);
    void  setOsc2FeedbackAmount(float amount);
    void  setOsc1FeedbackMix(float mix);
    void  setOsc2FeedbackMix(float mix);
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

    /// Set step sequencer filter modulation offset (bipolar).
    void setSeqFilterOffset(float offset);
    void setMultimode(float amount);
    void setTwoPole(bool enabled);
    void setXpander4Pole(bool enabled);
    void setXpanderMode(uint8_t mode);
    void setBPBlend2Pole(bool enabled);
    void setPush2Pole(bool enabled);
    void setResonanceModDepth(float depth);

    // Engine switching — routed to FilterBlock
    void setFilterEngine(uint8_t engine);
    void setVAFilterType(VAFilterType type);

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

    int   getOsc1Waveform()        const;
    int   getOsc2Waveform()        const;
    float getOsc1PitchOffset()     const;
    float getOsc2PitchOffset()     const;
    float getOsc1Detune()          const;
    float getOsc2Detune()          const;
    float getOsc1FineTune()        const;
    float getOsc2FineTune()        const;
    float getOscMix1()             const;
    float getOscMix2()             const;
    float getSubMix()              const;
    float getNoiseMix()            const;
    float getOsc1SupersawDetune()  const;
    float getOsc2SupersawDetune()  const;
    float getOsc1SupersawMix()     const;
    float getOsc2SupersawMix()     const;
    bool  getGlideEnabled()        const;
    float getGlideTime()           const;
    float getOsc1ShapeDc()         const;
    float getOsc2ShapeDc()         const;
    float getOsc1FrequencyDc()     const;
    float getOsc2FrequencyDc()     const;
    float getRing1Mix()            const;
    float getRing2Mix()            const;

    float getFilterCutoff()        const;
    float getFilterResonance()     const;
    float getFilterOctaveControl() const;
    float getFilterEnvAmount()     const;
    float getFilterKeyTrackAmount() const;
    float getMultimode()           const { return _multimode; }
    bool  getTwoPole()             const { return _useTwoPole; }
    bool  getXpander4Pole()        const { return _xpander4Pole; }
    uint8_t getXpanderMode()       const { return _xpanderMode; }
    bool  getBPBlend2Pole()        const { return _bpBlend2Pole; }
    bool  getPush2Pole()           const { return _push2Pole; }
    float getResonanceModDepth()   const { return _resonanceModDepth; }

    float getAmpAttack()           const;
    float getAmpDecay()            const;
    float getAmpSustain()          const;
    float getAmpRelease()          const;
    float getFilterEnvAttack()     const;
    float getFilterEnvDecay()      const;
    float getFilterEnvSustain()    const;
    float getFilterEnvRelease()    const;

    // =========================================================================
    // CROSS MODULATION & OSCILLATOR SYNC — two independent features
    // =========================================================================

    /// Enable or disable oscillator hard sync (JT_OPT_OSC_SYNC).
    /// Swaps audio graph connections; no-op and returns false if not compiled.
    void setSyncEnabled(bool enabled);
    bool getSyncEnabled() const;

    /// Set cross-modulation depth (OSC2 audio → OSC1 FM pitch).
    /// Works with any waveform, independent of sync state.
    /// depth is in 0.0–1.0 normalised units; use crossModDepthFromCC() to
    /// convert from a CC value.  Requires JT_OPT_CROSS_MOD; stored only otherwise.
    void  setCrossModDepth(float depth);
    float getCrossModDepth() const;

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
    // =========================================================================
    // AUDIO COMPONENTS
    // =========================================================================

    OscillatorBlock     _osc1{true};    // OSC1: supersaw + cross-mod pre-mixer
    OscillatorBlock     _osc2{false};   // OSC2: no supersaw (saves CPU)
    AudioEffectMultiply _ring1, _ring2;
    SubOscillatorBlock  _subOsc;
    AudioSynthNoisePink _noise;
    AudioMixer4         _oscMixer;      // Sums OSC1, OSC2, ring1, ring2
    AudioMixer4         _voiceMixer;    // Sums osc mixer, sub osc, noise

    FilterBlock   _filter;
    EnvelopeBlock _filterEnvelope;
    EnvelopeBlock _ampEnvelope;

    // =========================================================================
    // OSC MIX STATE
    // =========================================================================

    float _osc1Level  = 1.0f;
    float _osc2Level  = 0.0f;
    float _ring1Level = 0.0f;
    float _ring2Level = 0.0f;
    float _subMix     = 0.0f;
    float _noiseMix   = 0.0f;

    // =========================================================================
    // FILTER STATE
    // =========================================================================

    float   _baseCutoff           = 10000.0f;
    float   _keyTrackVal          = 0.0f;
    float   _filterEnvAmount      = 0.0f;
    float   _filterKeyTrackAmount = 0.5f;
    float   _multimode            = 0.0f;
    float   _resonanceModDepth    = 0.0f;
    bool    _useTwoPole           = false;
    bool    _xpander4Pole         = false;
    uint8_t _xpanderMode          = 0;
    bool    _bpBlend2Pole         = false;
    bool    _push2Pole            = false;

    // =========================================================================
    // VOICE STATE
    // =========================================================================

    float _currentFreq  = 0.0f;
    float _lastVelocity = 1.0f;

    static constexpr float _kMaxMixerGain = 0.9f;
    float _clampedLevel(float level);

    // =========================================================================
    // STATIC PATCH CABLES (built once in constructor, never deleted)
    //
    // [0]  osc1 → oscMixer 0
    // [1]  osc2 → oscMixer 1
    // [2]  osc1 → ring1 in0
    // [3]  osc2 → ring1 in1
    // [4]  osc1 → ring2 in0
    // [5]  osc2 → ring2 in1
    // [6]  ring1 → oscMixer 2
    // [7]  ring2 → oscMixer 3
    // [8]  oscMixer → voiceMixer 0
    // [9]  subOsc → voiceMixer 2
    // [10] noise → voiceMixer 3
    // [11] voiceMixer → filter in
    // [12] filter out → ampEnvelope in
    // [13] filter envmod → filterEnvelope in
    // [14] filterEnvelope out → filter modMixer 1
    // [15] pitchEnvDc → pitchEnvelope in
    // =========================================================================
    AudioConnection* _patchCables[16];

    // =========================================================================
    // PITCH ENVELOPE — DC source → EnvelopeBlock → FM mixer slot 3
    // =========================================================================

    AudioSynthWaveformDc _pitchEnvDc;
    EnvelopeBlock        _pitchEnvelope;
    AudioConnection*     _pitchEnvPatch1 = nullptr;   // → osc1 freqMod slot 3
    AudioConnection*     _pitchEnvPatch2 = nullptr;   // → osc2 freqMod slot 3
    float                _pitchEnvDepth  = 0.0f;

    // =========================================================================
    // VELOCITY SENSITIVITY
    // =========================================================================

    float _velAmpSens    = 0.0f;
    float _velFilterSens = 0.0f;
    float _velEnvSens    = 0.0f;

    float _baseFilterEnvAmount = 0.0f;

    // =========================================================================
    // CROSS MODULATION (JT_OPT_CROSS_MOD)
    //
    // _crossModDepth is always declared (outside all #if guards) so that
    // setCrossModDepth() and getCrossModDepth() compile in all configurations.
    //
    // When JT_OPT_CROSS_MOD is enabled:
    //   - Constructor builds a permanent cable:
    //       _osc2.output() → _osc1.crossModPreMixerRef() slot 1
    //   - setCrossModDepth() calls _osc1.setCrossModGain(scaledGain)
    //   - No audio flows through slot 1 when gain == 0.0
    //
    // When JT_OPT_OSC_SYNC is also active:
    //   - Sync-on removes _osc2 from the audio graph; pre-mixer slot 1 goes
    //     silent automatically — no special handling needed here.
    //   - Sync engine handles cross-mod internally at the requested depth.
    // =========================================================================

    float _crossModDepth = 0.0f;

#if JT_OPT_CROSS_MOD
    // Permanent cable: _osc2.output() → _osc1._crossModPreMixer slot 1.
    // Gain on slot 1 controlled by _osc1.setCrossModGain() in setCrossModDepth().
    AudioConnection* _patchCrossModFM = nullptr;
#endif

    // =========================================================================
    // OSCILLATOR HARD SYNC (JT_OPT_OSC_SYNC)
    //
    // Dynamic cables created on enable, destroyed on disable.
    // All nullptr when sync is off.
    // =========================================================================

#if JT_OPT_OSC_SYNC
    AudioSynthOscSync _syncEngine;
    bool              _syncActive = false;

    AudioConnection* _patchSyncSlaveToMix    = nullptr;   // sync ch0 → oscMixer 0
    AudioConnection* _patchSyncMasterToMix   = nullptr;   // sync ch1 → oscMixer 1
    AudioConnection* _patchSyncSlaveFM       = nullptr;   // osc1 FM mixer → sync in0
    AudioConnection* _patchSyncMasterFM      = nullptr;   // osc2 FM mixer → sync in1
    AudioConnection* _patchSyncSlaveShape    = nullptr;   // osc1 shape mixer → sync in2
    AudioConnection* _patchSyncMasterShape   = nullptr;   // osc2 shape mixer → sync in3
    AudioConnection* _patchSyncSlaveToRing1  = nullptr;   // sync ch0 → ring1 in0
    AudioConnection* _patchSyncMasterToRing1 = nullptr;   // sync ch1 → ring1 in1
    AudioConnection* _patchSyncSlaveToRing2  = nullptr;   // sync ch0 → ring2 in0
    AudioConnection* _patchSyncMasterToRing2 = nullptr;   // sync ch1 → ring2 in1
#endif
};
